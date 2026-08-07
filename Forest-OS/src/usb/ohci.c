/**
 * OHCI (Open Host Controller Interface) Driver for Fern
 *
 * Implements USB 1.0 support via the OHCI controller specification.
 * Based on OHCI 1.0a specification and OSDev documentation.
 */

#include "../include/usb/ohci.h"
#include "../include/usb/usb.h"
#include "../include/system.h"
#include "../include/memory.h"
#include "../include/pci.h"
#include "../include/debuglog.h"
#include "../include/debug.h"
#include <string.h>

void* kmalloc(size_t size);

// Pool sizes
#define OHCI_ED_POOL_SIZE       64
#define OHCI_TD_POOL_SIZE       256

// Timeouts (in microseconds)
#define OHCI_RESET_TIMEOUT      1000000
#define OHCI_TRANSFER_TIMEOUT   5000000
#define OHCI_PORT_RESET_TIMEOUT 100000

// Default frame interval
#define OHCI_FMINTERVAL_DEFAULT 0x2EDF  // 11999 (12000-1 bit times)
#define OHCI_FSMPS(fi)          (((fi) - 210) * 6 / 7)

// Forward declarations
static bool ohci_reset(ohci_data_t* hc);
static bool ohci_start(ohci_data_t* hc);
static ohci_ed_t* ohci_alloc_ed(ohci_data_t* hc);
static void ohci_free_ed(ohci_data_t* hc, ohci_ed_t* ed);
static ohci_td_t* ohci_alloc_td(ohci_data_t* hc);
static void ohci_free_td(ohci_data_t* hc, ohci_td_t* td);
static int ohci_wait_for_done(ohci_data_t* hc, ohci_td_t* td, uint32 timeout);
static void ohci_process_done_queue(ohci_data_t* hc);

// Controller operations
usb_controller_ops_t ohci_ops = {
    .init = ohci_init,
    .shutdown = ohci_shutdown,
    .reset_port = ohci_reset_port,
    .enable_port = ohci_enable_port,
    .disable_port = ohci_disable_port,
    .get_port_speed = ohci_get_port_speed,
    .port_connected = ohci_port_connected,
    .control_transfer = ohci_control_transfer,
    .bulk_transfer = ohci_bulk_transfer,
    .interrupt_transfer = ohci_interrupt_transfer,
    .poll = ohci_poll
};

/**
 * Read OHCI register
 */
static inline uint32 ohci_read(ohci_data_t* hc, uint32 reg) {
    return mmio_read32((const volatile void*)((uintptr_t)hc->regs + reg));
}

/**
 * Write OHCI register
 */
static inline void ohci_write(ohci_data_t* hc, uint32 reg, uint32 value) {
    mmio_write32((volatile void*)((uintptr_t)hc->regs + reg), value);
}

/**
 * Read port status register
 */
static inline uint32 ohci_read_port(ohci_data_t* hc, uint8 port) {
    return ohci_read(hc, OHCI_REG_RHPORTSTATUS + (port * 4));
}

/**
 * Write port status register
 */
static inline void ohci_write_port(ohci_data_t* hc, uint8 port, uint32 value) {
    ohci_write(hc, OHCI_REG_RHPORTSTATUS + (port * 4), value);
}

/**
 * Allocate an Endpoint Descriptor from the pool
 */
static ohci_ed_t* ohci_alloc_ed(ohci_data_t* hc) {
    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) {
        int word = i / 32;
        int bit = i % 32;
        if (!(hc->ed_bitmap[word] & (1 << bit))) {
            hc->ed_bitmap[word] |= (1 << bit);
            ohci_ed_t* ed = &hc->ed_pool[i];
            memset(ed, 0, sizeof(ohci_ed_t));
            return ed;
        }
    }
    return NULL;
}

/**
 * Free an Endpoint Descriptor back to the pool
 */
static void ohci_free_ed(ohci_data_t* hc, ohci_ed_t* ed) {
    if (!ed || !hc->ed_pool) return;

    int index = ed - hc->ed_pool;
    if (index >= 0 && index < OHCI_ED_POOL_SIZE) {
        int word = index / 32;
        int bit = index % 32;
        hc->ed_bitmap[word] &= ~(1 << bit);
    }
}

/**
 * Allocate a Transfer Descriptor from the pool
 */
static ohci_td_t* ohci_alloc_td(ohci_data_t* hc) {
    for (int i = 0; i < OHCI_TD_POOL_SIZE; i++) {
        int word = i / 32;
        int bit = i % 32;
        if (!(hc->td_bitmap[word] & (1 << bit))) {
            hc->td_bitmap[word] |= (1 << bit);
            ohci_td_t* td = &hc->td_pool[i];
            memset(td, 0, sizeof(ohci_td_t));
            return td;
        }
    }
    return NULL;
}

/**
 * Free a Transfer Descriptor back to the pool
 */
static void ohci_free_td(ohci_data_t* hc, ohci_td_t* td) {
    if (!td || !hc->td_pool) return;

    int index = td - hc->td_pool;
    if (index >= 0 && index < OHCI_TD_POOL_SIZE) {
        int word = index / 32;
        int bit = index % 32;
        hc->td_bitmap[word] &= ~(1 << bit);
    }
}

/**
 * Reset the OHCI controller
 */
static bool ohci_reset(ohci_data_t* hc) {
    // Check if we need to take ownership from SMM
    uint32 control = ohci_read(hc, OHCI_REG_CONTROL);
    if (control & OHCI_CTRL_IR) {
        debug_print("OHCI: Requesting ownership from SMM\n");

        // Set ownership change request
        ohci_write(hc, OHCI_REG_CMDSTATUS, OHCI_CMDSTS_OCR);

        // Wait for ownership
        uint32 timeout = 500000;
        while (timeout > 0) {
            control = ohci_read(hc, OHCI_REG_CONTROL);
            if (!(control & OHCI_CTRL_IR)) {
                break;
            }
            for (volatile int i = 0; i < 1000; i++);
            timeout -= 100;
        }

        if (timeout == 0) {
            debug_print("OHCI: Failed to get ownership from SMM\n");
            return false;
        }
    }

    // Put controller in reset state
    control = ohci_read(hc, OHCI_REG_CONTROL);
    control &= ~OHCI_CTRL_HCFS_MASK;
    control |= OHCI_CTRL_HCFS_RESET;
    ohci_write(hc, OHCI_REG_CONTROL, control);

    // Wait a bit for reset
    for (volatile int i = 0; i < 100000; i++);

    // Issue software reset
    ohci_write(hc, OHCI_REG_CMDSTATUS, OHCI_CMDSTS_HCR);

    // Wait for reset to complete
    uint32 timeout = OHCI_RESET_TIMEOUT;
    while (timeout > 0) {
        uint32 status = ohci_read(hc, OHCI_REG_CMDSTATUS);
        if (!(status & OHCI_CMDSTS_HCR)) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    if (timeout == 0) {
        debug_print("OHCI: Reset timeout\n");
        return false;
    }

    debug_print("OHCI: Controller reset complete\n");
    return true;
}

/**
 * Start the OHCI controller
 */
static bool ohci_start(ohci_data_t* hc) {
    // Set HCCA
    ohci_write(hc, OHCI_REG_HCCA, (uint32)(uintptr_t)hc->hcca);

    // Set frame interval
    uint32 fminterval = OHCI_FMINTERVAL_DEFAULT;
    uint32 fsmps = OHCI_FSMPS(fminterval);
    ohci_write(hc, OHCI_REG_FMINTERVAL, fminterval | (fsmps << 16) | (1 << 31));

    // Set periodic start (90% of frame interval)
    ohci_write(hc, OHCI_REG_PERIODICSTART, (fminterval * 9) / 10);

    // Clear control and bulk ED heads
    ohci_write(hc, OHCI_REG_CONTROLHEADED, 0);
    ohci_write(hc, OHCI_REG_BULKHEADED, 0);

    // Enable interrupts
    ohci_write(hc, OHCI_REG_INTDISABLE, OHCI_INT_MIE);  // Disable all first
    ohci_write(hc, OHCI_REG_INTSTATUS, 0xFFFFFFFF);     // Clear all status

    uint32 int_enable = OHCI_INT_WDH |   // Writeback Done Head
                        OHCI_INT_RHSC |  // Root Hub Status Change
                        OHCI_INT_UE |    // Unrecoverable Error
                        OHCI_INT_MIE;    // Master Interrupt Enable

    ohci_write(hc, OHCI_REG_INTENABLE, int_enable);

    // Set control register to operational state
    uint32 control = OHCI_CTRL_HCFS_OPERATIONAL |
                     OHCI_CTRL_CLE |  // Control List Enable
                     OHCI_CTRL_BLE |  // Bulk List Enable
                     OHCI_CTRL_PLE |  // Periodic List Enable
                     (3 << 0);        // CBSR = 3:1

    ohci_write(hc, OHCI_REG_CONTROL, control);

    // Power on root hub ports
    uint32 rh_status = ohci_read(hc, OHCI_REG_RHDESCRIPTORA);
    if (!(rh_status & OHCI_RHA_NPS)) {
        // Set global power
        ohci_write(hc, OHCI_REG_RHSTATUS, OHCI_RHS_LPSC);

        // Wait for power good
        uint32 potpgt = (rh_status >> OHCI_RHA_POTPGT_SHIFT) & 0xFF;
        for (volatile int i = 0; i < (potpgt * 2000 + 10000); i++);
    }

    debug_print("OHCI: Controller started\n");
    return true;
}

/**
 * Initialize OHCI controller
 */
bool ohci_init(usb_controller_t* controller) {
    debug_print("OHCI: Initializing controller\n");

    // Allocate controller-specific data
    ohci_data_t* hc = (ohci_data_t*)kmalloc(sizeof(ohci_data_t));
    if (!hc) {
        debug_print("OHCI: Failed to allocate controller data\n");
        return false;
    }

    memset(hc, 0, sizeof(ohci_data_t));
    controller->hcd_data = hc;

    // Map MMIO registers
    hc->regs = (volatile uint32*)(uintptr_t)controller->base_address;

    // Read revision
    uint32 revision = ohci_read(hc, OHCI_REG_REVISION);
    uint8 rev_major = (revision >> 4) & 0xF;
    uint8 rev_minor = revision & 0xF;

    debug_print("OHCI: Revision ");
    debug_print_dec(rev_major);
    debug_print(".");
    debug_print_dec(rev_minor);
    debug_print("\n");

    if (rev_major != 1 || rev_minor != 0) {
        debug_print("OHCI: Warning - unsupported revision\n");
    }

    // Allocate HCCA (256-byte aligned)
    hc->hcca = (ohci_hcca_t*)kmalloc_aligned(sizeof(ohci_hcca_t), 256);
    if (!hc->hcca) {
        debug_print("OHCI: Failed to allocate HCCA\n");
        kfree(hc);
        return false;
    }
    memset(hc->hcca, 0, sizeof(ohci_hcca_t));

    // Allocate ED pool (16-byte aligned)
    hc->ed_pool = (ohci_ed_t*)kmalloc_aligned(
        OHCI_ED_POOL_SIZE * sizeof(ohci_ed_t), 16);
    if (!hc->ed_pool) {
        debug_print("OHCI: Failed to allocate ED pool\n");
        kfree(hc->hcca);
        kfree(hc);
        return false;
    }
    memset(hc->ed_pool, 0, OHCI_ED_POOL_SIZE * sizeof(ohci_ed_t));
    memset(hc->ed_bitmap, 0, sizeof(hc->ed_bitmap));

    // Allocate TD pool (16-byte aligned)
    hc->td_pool = (ohci_td_t*)kmalloc_aligned(
        OHCI_TD_POOL_SIZE * sizeof(ohci_td_t), 16);
    if (!hc->td_pool) {
        debug_print("OHCI: Failed to allocate TD pool\n");
        kfree(hc->ed_pool);
        kfree(hc->hcca);
        kfree(hc);
        return false;
    }
    memset(hc->td_pool, 0, OHCI_TD_POOL_SIZE * sizeof(ohci_td_t));
    memset(hc->td_bitmap, 0, sizeof(hc->td_bitmap));

    // Reset the controller
    if (!ohci_reset(hc)) {
        kfree(hc->td_pool);
        kfree(hc->ed_pool);
        kfree(hc->hcca);
        kfree(hc);
        return false;
    }

    // Start the controller
    if (!ohci_start(hc)) {
        kfree(hc->td_pool);
        kfree(hc->ed_pool);
        kfree(hc->hcca);
        kfree(hc);
        return false;
    }

    // Get number of ports
    uint32 rhdesca = ohci_read(hc, OHCI_REG_RHDESCRIPTORA);
    hc->num_ports = rhdesca & OHCI_RHA_NDP_MASK;

    controller->num_ports = hc->num_ports;
    controller->initialized = true;

    debug_print("OHCI: Controller initialized with ");
    debug_print_dec(hc->num_ports);
    debug_print(" ports\n");

    return true;
}

/**
 * Shutdown OHCI controller
 */
void ohci_shutdown(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) return;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    debug_print("OHCI: Shutting down controller\n");

    // Disable interrupts
    ohci_write(hc, OHCI_REG_INTDISABLE, OHCI_INT_MIE);

    // Put controller in reset state
    uint32 control = ohci_read(hc, OHCI_REG_CONTROL);
    control &= ~OHCI_CTRL_HCFS_MASK;
    control |= OHCI_CTRL_HCFS_RESET;
    ohci_write(hc, OHCI_REG_CONTROL, control);

    // Free memory
    if (hc->td_pool) kfree(hc->td_pool);
    if (hc->ed_pool) kfree(hc->ed_pool);
    if (hc->hcca) kfree(hc->hcca);

    kfree(hc);
    controller->hcd_data = NULL;
    controller->initialized = false;

    debug_print("OHCI: Shutdown complete\n");
}

/**
 * Reset a port
 */
bool ohci_reset_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    if (port >= hc->num_ports) return false;

    // Check if device is connected
    uint32 portsc = ohci_read_port(hc, port);
    if (!(portsc & OHCI_PORT_CCS)) {
        return false;
    }

    // Set port reset
    ohci_write_port(hc, port, OHCI_PORT_PRS);

    // Wait for reset to complete
    uint32 timeout = OHCI_PORT_RESET_TIMEOUT;
    while (timeout > 0) {
        portsc = ohci_read_port(hc, port);
        if (portsc & OHCI_PORT_PRSC) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    if (timeout == 0) {
        debug_print("OHCI: Port reset timeout\n");
        return false;
    }

    // Clear reset change status
    ohci_write_port(hc, port, OHCI_PORT_PRSC);

    // Wait a bit for device to settle
    for (volatile int i = 0; i < 20000; i++);

    // Check if port is enabled
    portsc = ohci_read_port(hc, port);
    if (!(portsc & OHCI_PORT_PES)) {
        debug_print("OHCI: Port not enabled after reset\n");
        return false;
    }

    debug_print("OHCI: Port ");
    debug_print_dec(port);
    debug_print(" reset complete\n");

    return true;
}

/**
 * Enable a port
 */
bool ohci_enable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    if (port >= hc->num_ports) return false;

    // Set port enable
    ohci_write_port(hc, port, OHCI_PORT_PES);

    return true;
}

/**
 * Disable a port
 */
bool ohci_disable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    if (port >= hc->num_ports) return false;

    // Clear port enable
    ohci_write_port(hc, port, OHCI_PORT_CCS);

    return true;
}

/**
 * Get port speed
 */
usb_speed_t ohci_get_port_speed(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return USB_SPEED_FULL;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    if (port >= hc->num_ports) return USB_SPEED_FULL;

    uint32 portsc = ohci_read_port(hc, port);

    // OHCI only supports full and low speed
    if (portsc & OHCI_PORT_LSDA) {
        return USB_SPEED_LOW;
    }

    return USB_SPEED_FULL;
}

/**
 * Check if device is connected to port
 */
bool ohci_port_connected(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    if (port >= hc->num_ports) return false;

    uint32 portsc = ohci_read_port(hc, port);
    return (portsc & OHCI_PORT_CCS) != 0;
}

/**
 * Wait for a TD to complete
 */
static int ohci_wait_for_done(ohci_data_t* hc, ohci_td_t* td, uint32 timeout) {
    while (timeout > 0) {
        // Check done head
        uint32 done_head = hc->hcca->done_head & ~1;

        if (done_head != 0) {
            // Process done queue
            ohci_process_done_queue(hc);

            // Check if our TD completed
            uint32 cc = (td->flags >> OHCI_TD_CC_SHIFT) & 0xF;
            if (cc != OHCI_CC_NOTACCESSED) {
                if (cc == OHCI_CC_NOERROR) {
                    return 0;  // Success
                }
                return -1;  // Error
            }
        }

        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    return -1;  // Timeout
}

/**
 * Process the done queue
 */
static void ohci_process_done_queue(ohci_data_t* hc) {
    uint32 done_head = hc->hcca->done_head;
    hc->hcca->done_head = 0;

    // Clear done head interrupt
    ohci_write(hc, OHCI_REG_INTSTATUS, OHCI_INT_WDH);

    // Process done TDs (they're linked via next_td)
    while (done_head & ~1) {
        ohci_td_t* td = (ohci_td_t*)(uintptr_t)(done_head & ~1);
        done_head = td->next_td;

        // TD processing is complete - the transfer functions will check the CC
    }
}

/**
 * Perform a control transfer
 */
int ohci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length) {
    if (!controller || !controller->hcd_data || !device || !setup) return -1;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    // Allocate ED
    ohci_ed_t* ed = ohci_alloc_ed(hc);
    if (!ed) {
        debug_print("OHCI: Failed to allocate ED\n");
        return -1;
    }

    // Allocate TDs (setup + optional data + status)
    ohci_td_t* setup_td = ohci_alloc_td(hc);
    ohci_td_t* data_td = (length > 0) ? ohci_alloc_td(hc) : NULL;
    ohci_td_t* status_td = ohci_alloc_td(hc);
    ohci_td_t* dummy_td = ohci_alloc_td(hc);  // Dummy TD for tail

    if (!setup_td || !status_td || !dummy_td || (length > 0 && !data_td)) {
        debug_print("OHCI: Failed to allocate TDs\n");
        if (setup_td) ohci_free_td(hc, setup_td);
        if (data_td) ohci_free_td(hc, data_td);
        if (status_td) ohci_free_td(hc, status_td);
        if (dummy_td) ohci_free_td(hc, dummy_td);
        ohci_free_ed(hc, ed);
        return -1;
    }

    // Determine speed
    bool low_speed = (device->speed == USB_SPEED_LOW);

    // Set up ED
    uint16 max_packet = device->max_packet_size0;
    if (max_packet == 0) max_packet = 8;

    ed->flags = (device->address & OHCI_ED_FA_MASK) |
                ((0 << OHCI_ED_EN_SHIFT) & OHCI_ED_EN_MASK) |
                OHCI_ED_D_TD |
                (low_speed ? OHCI_ED_S : 0) |
                ((max_packet << OHCI_ED_MPS_SHIFT) & OHCI_ED_MPS_MASK);

    // Build TD chain

    // Setup TD
    setup_td->flags = OHCI_TD_DP_SETUP | OHCI_TD_T_DATA0 | OHCI_TD_DI_NONE;
    setup_td->cbp = (uint32)(uintptr_t)setup;
    setup_td->be = (uint32)(uintptr_t)setup + 7;
    setup_td->next_td = data_td ? (uint32)(uintptr_t)data_td : (uint32)(uintptr_t)status_td;

    // Data TD (if any)
    if (length > 0 && data) {
        bool data_in = (setup->bmRequestType & 0x80) != 0;
        data_td->flags = (data_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT) |
                         OHCI_TD_T_DATA1 | OHCI_TD_DI_NONE | OHCI_TD_R;
        data_td->cbp = (uint32)(uintptr_t)data;
        data_td->be = (uint32)(uintptr_t)data + length - 1;
        data_td->next_td = (uint32)(uintptr_t)status_td;
    }

    // Status TD
    bool status_in = (length == 0) || !(setup->bmRequestType & 0x80);
    status_td->flags = (status_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT) |
                       OHCI_TD_T_DATA1 | (0 << 21);  // DI = 0 (interrupt immediately)
    status_td->cbp = 0;
    status_td->be = 0;
    status_td->next_td = (uint32)(uintptr_t)dummy_td;

    // Set up ED pointers
    ed->head_td = (uint32)(uintptr_t)setup_td;
    ed->tail_td = (uint32)(uintptr_t)dummy_td;
    ed->next_ed = 0;

    // Add ED to control list
    ohci_write(hc, OHCI_REG_CONTROLHEADED, (uint32)(uintptr_t)ed);

    // Set Control List Filled
    ohci_write(hc, OHCI_REG_CMDSTATUS, OHCI_CMDSTS_CLF);

    // Wait for completion
    int result = ohci_wait_for_done(hc, status_td, OHCI_TRANSFER_TIMEOUT);

    // Calculate actual transfer length
    int transferred = 0;
    if (result == 0 && length > 0 && data_td) {
        // CBP is 0 if transfer completed, otherwise points to next byte
        if (data_td->cbp == 0) {
            transferred = length;
        } else {
            transferred = length - ((data_td->be - data_td->cbp) + 1);
        }
    }

    // Clear control head
    ohci_write(hc, OHCI_REG_CONTROLHEADED, 0);

    // Free resources
    ohci_free_td(hc, setup_td);
    if (data_td) ohci_free_td(hc, data_td);
    ohci_free_td(hc, status_td);
    ohci_free_td(hc, dummy_td);
    ohci_free_ed(hc, ed);

    return (result == 0) ? (length > 0 ? transferred : 0) : -1;
}

/**
 * Perform a bulk transfer
 */
int ohci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length) {
    if (!controller || !controller->hcd_data || !device || !endpoint || !data) {
        return -1;
    }

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    // Allocate ED
    ohci_ed_t* ed = ohci_alloc_ed(hc);
    if (!ed) {
        debug_print("OHCI: Failed to allocate ED\n");
        return -1;
    }

    // Allocate TDs
    ohci_td_t* data_td = ohci_alloc_td(hc);
    ohci_td_t* dummy_td = ohci_alloc_td(hc);

    if (!data_td || !dummy_td) {
        debug_print("OHCI: Failed to allocate TDs\n");
        if (data_td) ohci_free_td(hc, data_td);
        if (dummy_td) ohci_free_td(hc, dummy_td);
        ohci_free_ed(hc, ed);
        return -1;
    }

    // Set up ED
    uint8 ep_num = endpoint->address & 0x0F;
    bool is_in = (endpoint->direction == USB_DIR_IN);
    uint16 max_packet = endpoint->max_packet_size;

    ed->flags = (device->address & OHCI_ED_FA_MASK) |
                ((ep_num << OHCI_ED_EN_SHIFT) & OHCI_ED_EN_MASK) |
                (is_in ? OHCI_ED_D_IN : OHCI_ED_D_OUT) |
                ((max_packet << OHCI_ED_MPS_SHIFT) & OHCI_ED_MPS_MASK);

    // Set up data TD
    data_td->flags = (is_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT) | OHCI_TD_R | (0 << 21);
    data_td->cbp = (uint32)(uintptr_t)data;
    data_td->be = (uint32)(uintptr_t)data + length - 1;
    data_td->next_td = (uint32)(uintptr_t)dummy_td;

    // Set up ED pointers
    ed->head_td = (uint32)(uintptr_t)data_td;
    ed->tail_td = (uint32)(uintptr_t)dummy_td;
    ed->next_ed = 0;

    // Add ED to bulk list
    ohci_write(hc, OHCI_REG_BULKHEADED, (uint32)(uintptr_t)ed);

    // Set Bulk List Filled
    ohci_write(hc, OHCI_REG_CMDSTATUS, OHCI_CMDSTS_BLF);

    // Wait for completion
    int result = ohci_wait_for_done(hc, data_td, OHCI_TRANSFER_TIMEOUT);

    // Calculate actual transfer length
    int transferred = 0;
    if (result == 0) {
        if (data_td->cbp == 0) {
            transferred = length;
        } else {
            transferred = length - ((data_td->be - data_td->cbp) + 1);
        }
    }

    // Clear bulk head
    ohci_write(hc, OHCI_REG_BULKHEADED, 0);

    // Free resources
    ohci_free_td(hc, data_td);
    ohci_free_td(hc, dummy_td);
    ohci_free_ed(hc, ed);

    return (result == 0) ? transferred : -1;
}

/**
 * Perform an interrupt transfer
 */
int ohci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length) {
    // For simplicity, use control list for one-shot interrupt transfers
    return ohci_bulk_transfer(controller, device, endpoint, data, length);
}

/**
 * Poll for controller events
 */
void ohci_poll(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) return;

    ohci_data_t* hc = (ohci_data_t*)controller->hcd_data;

    uint32 status = ohci_read(hc, OHCI_REG_INTSTATUS);

    if (status & OHCI_INT_WDH) {
        // Writeback Done Head
        ohci_process_done_queue(hc);
    }

    if (status & OHCI_INT_RHSC) {
        // Root Hub Status Change
        debug_print("OHCI: Root hub status change\n");

        for (uint8 port = 0; port < hc->num_ports; port++) {
            uint32 portsc = ohci_read_port(hc, port);

            if (portsc & OHCI_PORT_CSC) {
                // Connect status changed
                if (portsc & OHCI_PORT_CCS) {
                    debug_print("OHCI: Device connected on port ");
                    debug_print_dec(port);
                    debug_print("\n");
                } else {
                    debug_print("OHCI: Device disconnected on port ");
                    debug_print_dec(port);
                    debug_print("\n");
                }

                // Clear change bit
                ohci_write_port(hc, port, OHCI_PORT_CSC);
            }

            if (portsc & OHCI_PORT_PESC) {
                // Port enable changed
                ohci_write_port(hc, port, OHCI_PORT_PESC);
            }
        }

        // Clear RHSC
        ohci_write(hc, OHCI_REG_INTSTATUS, OHCI_INT_RHSC);
    }

    if (status & OHCI_INT_UE) {
        // Unrecoverable Error
        debug_print("OHCI: Unrecoverable error!\n");
        ohci_write(hc, OHCI_REG_INTSTATUS, OHCI_INT_UE);
    }
}
