/**
 * UHCI (Universal Host Controller Interface) Driver for Fern
 *
 * Implements USB 1.0/1.1 support via the UHCI controller specification.
 */

#include "../include/usb/uhci.h"
#include "../include/usb/usb.h"
#include "../include/pci.h"
#include "../include/enhanced_heap.h"
#include "../include/string.h"
#include "../include/util.h"
#include "../include/screen.h"
#include "../include/debuglog.h"
#include "../include/timer.h"
#include "../include/io_ports.h"
#include "../include/system.h"

// TD/QH pool size
#define UHCI_TD_POOL_SIZE       256
#define UHCI_QH_POOL_SIZE       32

// Forward declarations
static void uhci_reset_controller(usb_controller_t* controller);
static uhci_td_t* uhci_alloc_td(uhci_data_t* uhci);
static void uhci_free_td(uhci_data_t* uhci, uhci_td_t* td);
static uhci_qh_t* uhci_alloc_qh(uhci_data_t* uhci);
static void uhci_free_qh(uhci_data_t* uhci, uhci_qh_t* qh);
static void uhci_setup_frame_list(uhci_data_t* uhci);
static int uhci_wait_for_transfer(uhci_data_t* uhci, uhci_td_t* td, uint32 timeout_ms);

/**
 * UHCI controller operations
 */
usb_controller_ops_t uhci_ops = {
    .init = uhci_init,
    .shutdown = uhci_shutdown,
    .reset_port = uhci_reset_port,
    .enable_port = uhci_enable_port,
    .disable_port = uhci_disable_port,
    .get_port_speed = uhci_get_port_speed,
    .port_connected = uhci_port_connected,
    .control_transfer = uhci_control_transfer,
    .bulk_transfer = uhci_bulk_transfer,
    .interrupt_transfer = uhci_interrupt_transfer,
    .poll = uhci_poll
};

/**
 * Read UHCI register (16-bit)
 */
static inline uint16 uhci_read16(uhci_data_t* uhci, uint16 reg) {
    return inportw(uhci->io_base + reg);
}

/**
 * Write UHCI register (16-bit)
 */
static inline void uhci_write16(uhci_data_t* uhci, uint16 reg, uint16 value) {
    outportw(uhci->io_base + reg, value);
}

/**
 * Read UHCI register (32-bit)
 */
static inline uint32 uhci_read32(uhci_data_t* uhci, uint16 reg) {
    return inportd(uhci->io_base + reg);
}

/**
 * Write UHCI register (32-bit)
 */
static inline void uhci_write32(uhci_data_t* uhci, uint16 reg, uint32 value) {
    outportd(uhci->io_base + reg, value);
}

/**
 * Initialize UHCI controller
 */
bool uhci_init(usb_controller_t* controller) {
    if (!controller) {
        return false;
    }

    debuglog(DEBUG_INFO, "[UHCI] Initializing controller at I/O base 0x%04x\n",
             controller->base_address);

    // Allocate controller-specific data
    uhci_data_t* uhci = (uhci_data_t*)enhanced_heap_alloc(sizeof(uhci_data_t), "uhci_data");
    if (!uhci) {
        debuglog(DEBUG_ERROR, "[UHCI] Failed to allocate controller data\n");
        return false;
    }

    memory_set((uint8*)uhci, 0, sizeof(uhci_data_t));
    uhci->io_base = (uint16)controller->base_address;
    controller->hcd_data = uhci;

    // Disable BIOS legacy support
    // Write to PCI register 0xC0 to disable legacy USB support
    pci_config_write16(controller->pci_device.segment,
                       controller->pci_device.bus,
                       controller->pci_device.device,
                       controller->pci_device.function,
                       0xC0, 0x2000);
    uhci->legacy_support = true;

    // Reset the controller
    uhci_reset_controller(controller);

    // Allocate frame list (1024 entries, 4KB aligned)
    uhci->frame_list = (uint32*)enhanced_heap_alloc(UHCI_FRAME_LIST_SIZE * sizeof(uint32),
                                                    "uhci_frame_list");
    if (!uhci->frame_list) {
        debuglog(DEBUG_ERROR, "[UHCI] Failed to allocate frame list\n");
        enhanced_heap_free(uhci, "uhci_data");
        return false;
    }

    // Allocate TD pool
    uhci->td_pool = (uhci_td_t*)enhanced_heap_alloc(UHCI_TD_POOL_SIZE * sizeof(uhci_td_t),
                                                    "uhci_td_pool");
    if (!uhci->td_pool) {
        enhanced_heap_free(uhci->frame_list, "uhci_frame_list");
        enhanced_heap_free(uhci, "uhci_data");
        return false;
    }
    memory_set((uint8*)uhci->td_pool, 0, UHCI_TD_POOL_SIZE * sizeof(uhci_td_t));
    memory_set((uint8*)uhci->td_pool_bitmap, 0, sizeof(uhci->td_pool_bitmap));

    // Setup frame list with empty entries
    uhci_setup_frame_list(uhci);

    // Set frame list base address
    uhci_write32(uhci, UHCI_REG_FRBASEADD, (uint32)(uintptr_t)uhci->frame_list);

    // Set frame number to 0
    uhci_write16(uhci, UHCI_REG_FRNUM, 0);

    // Set SOF timing
    outportb(uhci->io_base + UHCI_REG_SOFMOD, 0x40);

    // Enable interrupts
    uhci_write16(uhci, UHCI_REG_USBINTR,
                 UHCI_INTR_IOC | UHCI_INTR_SP | UHCI_INTR_TIMEOUT_CRC);

    // Start the controller
    uhci_write16(uhci, UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP | UHCI_CMD_CF);

    // Wait for controller to start
    timer_sleep_ms(10);

    // Check if controller is running
    uint16 status = uhci_read16(uhci, UHCI_REG_USBSTS);
    if (status & UHCI_STS_HCH) {
        debuglog(DEBUG_ERROR, "[UHCI] Controller failed to start (halted)\n");
        return false;
    }

    // Set number of ports (UHCI typically has 2 ports)
    controller->num_ports = 2;

    // Check for actual ports by looking for valid port registers
    for (uint8 port = 0; port < 8; port++) {
        uint16 portsc = uhci_read16(uhci, UHCI_REG_PORTSC1 + (port * 2));
        if ((portsc & 0x80) == 0 || portsc == 0xFFFF) {
            controller->num_ports = port;
            break;
        }
    }

    debuglog(DEBUG_INFO, "[UHCI] Controller initialized: %d ports\n", controller->num_ports);

    return true;
}

/**
 * Shutdown UHCI controller
 */
void uhci_shutdown(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) {
        return;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;

    // Stop the controller
    uhci_write16(uhci, UHCI_REG_USBCMD, 0);

    // Wait for halt
    timer_sleep_ms(10);

    // Free allocated memory
    if (uhci->frame_list) {
        enhanced_heap_free(uhci->frame_list, "uhci_frame_list");
    }
    if (uhci->td_pool) {
        enhanced_heap_free(uhci->td_pool, "uhci_td_pool");
    }

    enhanced_heap_free(uhci, "uhci_data");
    controller->hcd_data = NULL;

    debuglog(DEBUG_INFO, "[UHCI] Controller shutdown\n");
}

/**
 * Reset the UHCI controller
 */
static void uhci_reset_controller(usb_controller_t* controller) {
    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;

    // Global reset
    uhci_write16(uhci, UHCI_REG_USBCMD, UHCI_CMD_GRESET);
    timer_sleep_ms(50);
    uhci_write16(uhci, UHCI_REG_USBCMD, 0);
    timer_sleep_ms(10);

    // Host controller reset
    uhci_write16(uhci, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);

    // Wait for reset to complete (bit should self-clear)
    for (int i = 0; i < 100; i++) {
        if (!(uhci_read16(uhci, UHCI_REG_USBCMD) & UHCI_CMD_HCRESET)) {
            break;
        }
        timer_sleep_ms(1);
    }

    // Clear status
    uhci_write16(uhci, UHCI_REG_USBSTS, 0xFFFF);

    debuglog(DEBUG_INFO, "[UHCI] Controller reset complete\n");
}

/**
 * Setup the frame list
 */
static void uhci_setup_frame_list(uhci_data_t* uhci) {
    // Initialize all frame list entries as terminated (empty)
    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) {
        uhci->frame_list[i] = UHCI_PTR_TERMINATE;
    }

    // TODO: Set up periodic queue heads for interrupt transfers
}

/**
 * Allocate a Transfer Descriptor
 */
static uhci_td_t* uhci_alloc_td(uhci_data_t* uhci) {
    for (int i = 0; i < UHCI_TD_POOL_SIZE; i++) {
        uint32 word_index = i / 32;
        uint32 bit_index = i % 32;

        if (!(uhci->td_pool_bitmap[word_index] & (1 << bit_index))) {
            uhci->td_pool_bitmap[word_index] |= (1 << bit_index);
            uhci_td_t* td = &uhci->td_pool[i];
            memory_set((uint8*)td, 0, sizeof(uhci_td_t));
            return td;
        }
    }
    return NULL;
}

/**
 * Free a Transfer Descriptor
 */
static void uhci_free_td(uhci_data_t* uhci, uhci_td_t* td) {
    if (!td) return;

    int index = (int)(td - uhci->td_pool);
    if (index >= 0 && index < UHCI_TD_POOL_SIZE) {
        uint32 word_index = index / 32;
        uint32 bit_index = index % 32;
        uhci->td_pool_bitmap[word_index] &= ~(1 << bit_index);
    }
}

/**
 * Reset a port
 */
bool uhci_reset_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    uint16 port_reg = UHCI_REG_PORTSC1 + (port * 2);

    // Set port reset
    uhci_write16(uhci, port_reg, UHCI_PORTSC_PR);
    timer_sleep_ms(50);  // USB spec says at least 10ms

    // Clear port reset
    uhci_write16(uhci, port_reg, 0);
    timer_sleep_ms(10);

    // Enable port
    uint16 portsc = uhci_read16(uhci, port_reg);
    portsc |= UHCI_PORTSC_PED;
    uhci_write16(uhci, port_reg, portsc);

    // Wait for port to enable
    for (int i = 0; i < 100; i++) {
        timer_sleep_ms(1);
        portsc = uhci_read16(uhci, port_reg);
        if (portsc & UHCI_PORTSC_PED) {
            // Clear change bits
            uhci_write16(uhci, port_reg, portsc | UHCI_PORTSC_CSC | UHCI_PORTSC_PEDC);
            return true;
        }
    }

    debuglog(DEBUG_WARN, "[UHCI] Port %d reset timeout\n", port);
    return false;
}

/**
 * Enable a port
 */
bool uhci_enable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    uint16 port_reg = UHCI_REG_PORTSC1 + (port * 2);

    uint16 portsc = uhci_read16(uhci, port_reg);
    portsc |= UHCI_PORTSC_PED;
    uhci_write16(uhci, port_reg, portsc);

    return true;
}

/**
 * Disable a port
 */
bool uhci_disable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    uint16 port_reg = UHCI_REG_PORTSC1 + (port * 2);

    uint16 portsc = uhci_read16(uhci, port_reg);
    portsc &= ~UHCI_PORTSC_PED;
    uhci_write16(uhci, port_reg, portsc);

    return true;
}

/**
 * Get port speed
 */
usb_speed_t uhci_get_port_speed(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return USB_SPEED_FULL;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    uint16 port_reg = UHCI_REG_PORTSC1 + (port * 2);

    uint16 portsc = uhci_read16(uhci, port_reg);

    // UHCI only supports low and full speed
    if (portsc & UHCI_PORTSC_LSDA) {
        return USB_SPEED_LOW;
    }

    return USB_SPEED_FULL;
}

/**
 * Check if port has device connected
 */
bool uhci_port_connected(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    uint16 port_reg = UHCI_REG_PORTSC1 + (port * 2);

    uint16 portsc = uhci_read16(uhci, port_reg);

    return (portsc & UHCI_PORTSC_CCS) != 0;
}

/**
 * Wait for a TD to complete
 */
static int uhci_wait_for_transfer(uhci_data_t* uhci, uhci_td_t* td, uint32 timeout_ms) {
    (void)uhci;

    for (uint32 i = 0; i < timeout_ms; i++) {
        // Check if TD is no longer active
        if (!(td->status & UHCI_TD_STATUS_ACTIVE)) {
            // Check for errors
            if (td->status & (UHCI_TD_STATUS_STALLED |
                              UHCI_TD_STATUS_DBUFFER |
                              UHCI_TD_STATUS_BABBLE |
                              UHCI_TD_STATUS_CRC_TIMEOUT |
                              UHCI_TD_STATUS_BITSTUFF)) {
                return -1;
            }
            return (td->status & UHCI_TD_STATUS_ACTLEN_MASK);
        }
        timer_sleep_ms(1);
    }

    return -1;  // Timeout
}

/**
 * Control transfer
 */
int uhci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length) {
    if (!controller || !controller->hcd_data || !device || !setup) {
        return -1;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    bool is_in = (setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0;
    uint8 max_packet = device->max_packet_size0 ? device->max_packet_size0 : 8;

    // Allocate TDs: 1 for SETUP, N for DATA (if any), 1 for STATUS
    uhci_td_t* td_setup = uhci_alloc_td(uhci);
    if (!td_setup) return -1;

    // Build SETUP TD
    uint32 token = UHCI_TD_TOKEN_PID_SETUP |
                   (device->address << 8) |
                   (0 << 15) |  // Endpoint 0
                   (7 << 21);   // 8 bytes (setup packet length - 1)

    td_setup->link = UHCI_PTR_TERMINATE;
    td_setup->status = UHCI_TD_STATUS_ACTIVE |
                       (3 << 27) |  // Error count = 3
                       (device->speed == USB_SPEED_LOW ? UHCI_TD_STATUS_LS : 0);
    td_setup->token = token;
    td_setup->buffer = (uint32)(uintptr_t)setup;

    // Insert into frame list
    uhci->frame_list[0] = (uint32)(uintptr_t)td_setup;

    // Wait for SETUP to complete
    int result = uhci_wait_for_transfer(uhci, td_setup, 500);
    uhci->frame_list[0] = UHCI_PTR_TERMINATE;

    if (result < 0) {
        uhci_free_td(uhci, td_setup);
        return -1;
    }

    uhci_free_td(uhci, td_setup);

    // DATA stage (if needed)
    if (length > 0 && data) {
        uint32 remaining = length;
        uint8* buf_ptr = (uint8*)data;
        uint8 toggle = 1;  // DATA1 for first data packet

        while (remaining > 0) {
            uint16 packet_len = remaining > max_packet ? max_packet : (uint16)remaining;

            uhci_td_t* td_data = uhci_alloc_td(uhci);
            if (!td_data) return -1;

            token = (is_in ? UHCI_TD_TOKEN_PID_IN : UHCI_TD_TOKEN_PID_OUT) |
                    (device->address << 8) |
                    (0 << 15) |  // Endpoint 0
                    ((packet_len - 1) << 21) |
                    (toggle << 19);

            td_data->link = UHCI_PTR_TERMINATE;
            td_data->status = UHCI_TD_STATUS_ACTIVE |
                              (3 << 27) |
                              (device->speed == USB_SPEED_LOW ? UHCI_TD_STATUS_LS : 0);
            td_data->token = token;
            td_data->buffer = (uint32)(uintptr_t)buf_ptr;

            uhci->frame_list[0] = (uint32)(uintptr_t)td_data;

            result = uhci_wait_for_transfer(uhci, td_data, 500);
            uhci->frame_list[0] = UHCI_PTR_TERMINATE;

            uhci_free_td(uhci, td_data);

            if (result < 0) {
                return -1;
            }

            buf_ptr += packet_len;
            remaining -= packet_len;
            toggle ^= 1;
        }
    }

    // STATUS stage
    uhci_td_t* td_status = uhci_alloc_td(uhci);
    if (!td_status) return -1;

    // STATUS is opposite direction of DATA (or IN if no data)
    uint8 status_pid = (length > 0 && is_in) ? UHCI_TD_TOKEN_PID_OUT : UHCI_TD_TOKEN_PID_IN;

    token = status_pid |
            (device->address << 8) |
            (0 << 15) |  // Endpoint 0
            (0x7FF << 21) |  // Max length for zero-length packet
            (1 << 19);  // DATA1

    td_status->link = UHCI_PTR_TERMINATE;
    td_status->status = UHCI_TD_STATUS_ACTIVE |
                        (3 << 27) |
                        (device->speed == USB_SPEED_LOW ? UHCI_TD_STATUS_LS : 0);
    td_status->token = token;
    td_status->buffer = 0;

    uhci->frame_list[0] = (uint32)(uintptr_t)td_status;

    result = uhci_wait_for_transfer(uhci, td_status, 500);
    uhci->frame_list[0] = UHCI_PTR_TERMINATE;

    uhci_free_td(uhci, td_status);

    return (result >= 0) ? (int)length : -1;
}

/**
 * Bulk transfer
 */
int uhci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length) {
    if (!controller || !controller->hcd_data || !device || !endpoint || !data) {
        return -1;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;
    bool is_in = (endpoint->direction == USB_DIR_IN);
    uint16 max_packet = endpoint->max_packet_size;

    uint32 total_transferred = 0;
    uint8* buf_ptr = (uint8*)data;
    uint32 remaining = length;
    uint8 toggle = 0;

    while (remaining > 0) {
        uint16 packet_len = remaining > max_packet ? max_packet : (uint16)remaining;

        uhci_td_t* td = uhci_alloc_td(uhci);
        if (!td) return -1;

        uint32 token = (is_in ? UHCI_TD_TOKEN_PID_IN : UHCI_TD_TOKEN_PID_OUT) |
                       (device->address << 8) |
                       ((endpoint->address & 0x0F) << 15) |
                       ((packet_len - 1) << 21) |
                       (toggle << 19);

        td->link = UHCI_PTR_TERMINATE;
        td->status = UHCI_TD_STATUS_ACTIVE |
                     (3 << 27) |
                     (device->speed == USB_SPEED_LOW ? UHCI_TD_STATUS_LS : 0);
        td->token = token;
        td->buffer = (uint32)(uintptr_t)buf_ptr;

        uhci->frame_list[0] = (uint32)(uintptr_t)td;

        int result = uhci_wait_for_transfer(uhci, td, 5000);
        uhci->frame_list[0] = UHCI_PTR_TERMINATE;

        uhci_free_td(uhci, td);

        if (result < 0) {
            return total_transferred > 0 ? (int)total_transferred : -1;
        }

        uint32 actual = (result == 0x7FF) ? 0 : result + 1;
        total_transferred += actual;
        buf_ptr += actual;
        remaining -= actual;
        toggle ^= 1;

        // Short packet indicates end of transfer
        if (is_in && actual < max_packet) {
            break;
        }
    }

    return (int)total_transferred;
}

/**
 * Interrupt transfer
 */
int uhci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length) {
    // For now, implement as a bulk transfer
    // TODO: Proper interrupt transfer scheduling
    return uhci_bulk_transfer(controller, device, endpoint, data, length);
}

/**
 * Poll controller for events
 */
void uhci_poll(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) {
        return;
    }

    uhci_data_t* uhci = (uhci_data_t*)controller->hcd_data;

    // Read and clear status
    uint16 status = uhci_read16(uhci, UHCI_REG_USBSTS);
    if (status & (UHCI_STS_USBINT | UHCI_STS_ERROR | UHCI_STS_RD | UHCI_STS_HSE | UHCI_STS_HCPE)) {
        uhci_write16(uhci, UHCI_REG_USBSTS, status);
    }

    // Check for port changes
    for (uint8 port = 0; port < controller->num_ports; port++) {
        uint16 port_reg = UHCI_REG_PORTSC1 + (port * 2);
        uint16 portsc = uhci_read16(uhci, port_reg);

        if (portsc & UHCI_PORTSC_CSC) {
            // Clear change bit
            uhci_write16(uhci, port_reg, portsc | UHCI_PORTSC_CSC);

            if (portsc & UHCI_PORTSC_CCS) {
                debuglog(DEBUG_INFO, "[UHCI] Device connected on port %d\n", port);
            } else {
                debuglog(DEBUG_INFO, "[UHCI] Device disconnected from port %d\n", port);
            }
        }
    }
}
