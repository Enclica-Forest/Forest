/**
 * EHCI (Enhanced Host Controller Interface) Driver for Fern
 *
 * Implements USB 2.0 (High-Speed) support via the EHCI controller specification.
 */

#include "../include/usb/ehci.h"
#include "../include/usb/usb.h"
#include "../include/pci.h"
#include "../include/memory.h"
#include "../include/enhanced_heap.h"
#include "../include/string.h"
#include "../include/util.h"
#include "../include/screen.h"
#include "../include/debuglog.h"
#include "../include/timer.h"
#include "../include/system.h"

// QH/QTD pool sizes
#define EHCI_QH_POOL_SIZE       64
#define EHCI_QTD_POOL_SIZE      256

// Forward declarations
static void ehci_reset_controller(usb_controller_t* controller);
static bool ehci_take_ownership(usb_controller_t* controller);
static uint32 ehci_phys_addr(const void* addr);
static void ehci_fill_buffer_ptrs(ehci_qtd_t* qtd, const void* buf, uint32 len);
static void* ehci_alloc_aligned(size_t size, size_t align, const char* tag, void** out_raw);
static void ehci_setup_async_schedule(ehci_data_t* ehci);
static void ehci_setup_periodic_schedule(ehci_data_t* ehci);
static ehci_qh_t* ehci_alloc_qh(ehci_data_t* ehci);
static void ehci_free_qh(ehci_data_t* ehci, ehci_qh_t* qh);
static ehci_qtd_t* ehci_alloc_qtd(ehci_data_t* ehci);
static void ehci_free_qtd(ehci_data_t* ehci, ehci_qtd_t* qtd);
static int ehci_wait_for_transfer(ehci_data_t* ehci, ehci_qtd_t* qtd, uint32 timeout_ms);

/**
 * EHCI controller operations
 */
usb_controller_ops_t ehci_ops = {
    .init = ehci_init,
    .shutdown = ehci_shutdown,
    .reset_port = ehci_reset_port,
    .enable_port = ehci_enable_port,
    .disable_port = ehci_disable_port,
    .get_port_speed = ehci_get_port_speed,
    .port_connected = ehci_port_connected,
    .control_transfer = ehci_control_transfer,
    .bulk_transfer = ehci_bulk_transfer,
    .interrupt_transfer = ehci_interrupt_transfer,
    .poll = ehci_poll
};

/**
 * Read EHCI capability register
 */
static inline uint32 ehci_cap_read32(ehci_data_t* ehci, uint16 reg) {
    return mmio_read32(ehci->cap_regs + reg);
}

/**
 * Read EHCI operational register
 */
static inline uint32 ehci_op_read32(ehci_data_t* ehci, uint16 reg) {
    return ehci->op_regs[reg / 4];
}

/**
 * Write EHCI operational register
 */
static inline void ehci_op_write32(ehci_data_t* ehci, uint16 reg, uint32 value) {
    ehci->op_regs[reg / 4] = value;
}

/**
 * Initialize EHCI controller
 */
bool ehci_init(usb_controller_t* controller) {
    if (!controller) {
        return false;
    }

    debuglog(DEBUG_INFO, "[EHCI] Initializing controller at MMIO base 0x%08x\n",
             controller->base_address);

    // Allocate controller-specific data
    ehci_data_t* ehci = (ehci_data_t*)enhanced_heap_alloc(sizeof(ehci_data_t), "ehci_data");
    if (!ehci) {
        debuglog(DEBUG_ERROR, "[EHCI] Failed to allocate controller data\n");
        return false;
    }

    memory_set((uint8*)ehci, 0, sizeof(ehci_data_t));
    ehci->cap_regs = (volatile uint8*)(uintptr_t)controller->base_address;
    controller->hcd_data = ehci;

    // Read capability registers
    uint8 cap_length = mmio_read8(ehci->cap_regs + EHCI_CAP_CAPLENGTH);
    ehci->cap_length = cap_length;
    ehci->op_regs = (volatile uint32*)(ehci->cap_regs + cap_length);

    uint32 hcsparams = ehci_cap_read32(ehci, EHCI_CAP_HCSPARAMS);
    uint32 hccparams = ehci_cap_read32(ehci, EHCI_CAP_HCCPARAMS);

    ehci->num_ports = hcsparams & EHCI_HCS_N_PORTS_MASK;
    ehci->num_companions = (hcsparams & EHCI_HCS_N_CC_MASK) >> EHCI_HCS_N_CC_SHIFT;
    ehci->ports_per_companion = (hcsparams & EHCI_HCS_N_PCC_MASK) >> EHCI_HCS_N_PCC_SHIFT;
    ehci->has_64bit = (hccparams & EHCI_HCC_64BIT) != 0;
    ehci->eecp = (hccparams & EHCI_HCC_EECP_MASK) >> EHCI_HCC_EECP_SHIFT;

    controller->num_ports = ehci->num_ports;

    debuglog(DEBUG_INFO, "[EHCI] %d ports, %d companions, EECP=0x%02x, 64-bit=%s\n",
             ehci->num_ports, ehci->num_companions, ehci->eecp,
             ehci->has_64bit ? "yes" : "no");

    // Take ownership from BIOS
    if (!ehci_take_ownership(controller)) {
        debuglog(DEBUG_WARN, "[EHCI] Failed to take ownership from BIOS\n");
        // Continue anyway
    }

    // Reset the controller
    ehci_reset_controller(controller);

    // Allocate frame list (periodic schedule) - requires 4K alignment
    ehci->frame_list = (uint32*)ehci_alloc_aligned(
        EHCI_FRAME_LIST_SIZE * sizeof(uint32), 4096, "ehci_frame_list", &ehci->frame_list_raw);
    if (!ehci->frame_list) {
        debuglog(DEBUG_ERROR, "[EHCI] Failed to allocate frame list\n");
        enhanced_heap_free(ehci, "ehci_data");
        return false;
    }

    // Allocate QH pool - requires 32-byte alignment
    ehci->qh_pool = (ehci_qh_t*)ehci_alloc_aligned(
        EHCI_QH_POOL_SIZE * sizeof(ehci_qh_t), 32, "ehci_qh_pool", &ehci->qh_pool_raw);
    if (!ehci->qh_pool) {
        if (ehci->frame_list_raw) {
            enhanced_heap_free(ehci->frame_list_raw, "ehci_frame_list");
        }
        enhanced_heap_free(ehci, "ehci_data");
        return false;
    }
    memory_set((uint8*)ehci->qh_pool, 0, EHCI_QH_POOL_SIZE * sizeof(ehci_qh_t));
    memory_set((uint8*)ehci->qh_bitmap, 0, sizeof(ehci->qh_bitmap));

    // Allocate QTD pool - requires 32-byte alignment
    ehci->qtd_pool = (ehci_qtd_t*)ehci_alloc_aligned(
        EHCI_QTD_POOL_SIZE * sizeof(ehci_qtd_t), 32, "ehci_qtd_pool", &ehci->qtd_pool_raw);
    if (!ehci->qtd_pool) {
        if (ehci->qh_pool_raw) {
            enhanced_heap_free(ehci->qh_pool_raw, "ehci_qh_pool");
        }
        if (ehci->frame_list_raw) {
            enhanced_heap_free(ehci->frame_list_raw, "ehci_frame_list");
        }
        enhanced_heap_free(ehci, "ehci_data");
        return false;
    }
    memory_set((uint8*)ehci->qtd_pool, 0, EHCI_QTD_POOL_SIZE * sizeof(ehci_qtd_t));
    memory_set((uint8*)ehci->qtd_bitmap, 0, sizeof(ehci->qtd_bitmap));

    // Setup async schedule (control and bulk transfers)
    ehci_setup_async_schedule(ehci);

    // Setup periodic schedule (interrupt and isochronous transfers)
    ehci_setup_periodic_schedule(ehci);

    // Set segment selector to 0 (for 32-bit addressing)
    ehci_op_write32(ehci, EHCI_OP_CTRLDSSEGMENT, 0);

    // Set periodic and async list addresses
    ehci_op_write32(ehci, EHCI_OP_PERIODICLISTBASE, ehci_phys_addr(ehci->frame_list));
    ehci_op_write32(ehci, EHCI_OP_ASYNCLISTADDR, ehci_phys_addr(ehci->async_head));

    // Enable interrupts
    ehci_op_write32(ehci, EHCI_OP_USBINTR,
                    EHCI_INTR_INT | EHCI_INTR_ERR | EHCI_INTR_PCD | EHCI_INTR_IAA);

    // Set interrupt threshold to 1 micro-frame
    uint32 cmd = ehci_op_read32(ehci, EHCI_OP_USBCMD);
    cmd &= ~EHCI_CMD_ITC_MASK;
    cmd |= (1 << EHCI_CMD_ITC_SHIFT);

    // Enable periodic and async schedules
    cmd |= EHCI_CMD_PSE | EHCI_CMD_ASE;

    // Start the controller
    cmd |= EHCI_CMD_RS;
    ehci_op_write32(ehci, EHCI_OP_USBCMD, cmd);

    // Wait for controller to start
    timer_sleep_ms(10);

    // Check if controller is running
    uint32 status = ehci_op_read32(ehci, EHCI_OP_USBSTS);
    if (status & EHCI_STS_HALT) {
        debuglog(DEBUG_ERROR, "[EHCI] Controller failed to start (halted)\n");
        return false;
    }

    // Set configure flag to route ports to EHCI
    ehci_op_write32(ehci, EHCI_OP_CONFIGFLAG, EHCI_CF_FLAG);
    timer_sleep_ms(5);

    // Power on all ports
    for (uint8 port = 0; port < ehci->num_ports; port++) {
        uint32 portsc = ehci_op_read32(ehci, EHCI_OP_PORTSC + (port * 4));
        if (!(portsc & EHCI_PORT_PP)) {
            portsc |= EHCI_PORT_PP;
            ehci_op_write32(ehci, EHCI_OP_PORTSC + (port * 4), portsc);
        }
    }

    timer_sleep_ms(20);  // Wait for power to stabilize

    debuglog(DEBUG_INFO, "[EHCI] Controller initialized\n");

    return true;
}

/**
 * Shutdown EHCI controller
 */
void ehci_shutdown(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) {
        return;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;

    // Stop the controller
    uint32 cmd = ehci_op_read32(ehci, EHCI_OP_USBCMD);
    cmd &= ~(EHCI_CMD_RS | EHCI_CMD_PSE | EHCI_CMD_ASE);
    ehci_op_write32(ehci, EHCI_OP_USBCMD, cmd);

    // Wait for halt
    for (int i = 0; i < 100; i++) {
        if (ehci_op_read32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HALT) {
            break;
        }
        timer_sleep_ms(1);
    }

    // Free allocated memory
    if (ehci->frame_list_raw) {
        enhanced_heap_free(ehci->frame_list_raw, "ehci_frame_list");
        ehci->frame_list_raw = NULL;
        ehci->frame_list = NULL;
    }
    if (ehci->qh_pool_raw) {
        enhanced_heap_free(ehci->qh_pool_raw, "ehci_qh_pool");
        ehci->qh_pool_raw = NULL;
        ehci->qh_pool = NULL;
    }
    if (ehci->qtd_pool_raw) {
        enhanced_heap_free(ehci->qtd_pool_raw, "ehci_qtd_pool");
        ehci->qtd_pool_raw = NULL;
        ehci->qtd_pool = NULL;
    }

    enhanced_heap_free(ehci, "ehci_data");
    controller->hcd_data = NULL;

    debuglog(DEBUG_INFO, "[EHCI] Controller shutdown\n");
}

/**
 * Take ownership from BIOS
 */
static bool ehci_take_ownership(usb_controller_t* controller) {
    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;

    if (ehci->eecp < 0x40) {
        return true;  // No extended capabilities
    }

    // Read legacy support register
    uint32 legacy = pci_config_read32(controller->pci_device.segment,
                                       controller->pci_device.bus,
                                       controller->pci_device.device,
                                       controller->pci_device.function,
                                       ehci->eecp);

    // Check if BIOS owns the controller
    if (legacy & EHCI_LEGACY_BIOS_OWNED) {
        debuglog(DEBUG_INFO, "[EHCI] Taking ownership from BIOS\n");

        // Request ownership
        legacy |= EHCI_LEGACY_OS_OWNED;
        pci_config_write32(controller->pci_device.segment,
                           controller->pci_device.bus,
                           controller->pci_device.device,
                           controller->pci_device.function,
                           ehci->eecp, legacy);

        // Wait for BIOS to release
        for (int i = 0; i < 100; i++) {
            legacy = pci_config_read32(controller->pci_device.segment,
                                        controller->pci_device.bus,
                                        controller->pci_device.device,
                                        controller->pci_device.function,
                                        ehci->eecp);

            if (!(legacy & EHCI_LEGACY_BIOS_OWNED)) {
                return true;
            }
            timer_sleep_ms(10);
        }

        debuglog(DEBUG_WARN, "[EHCI] BIOS ownership timeout\n");
        return false;
    }

    return true;
}

/**
 * Reset the EHCI controller
 */
static void ehci_reset_controller(usb_controller_t* controller) {
    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;

    // Stop the controller first
    uint32 cmd = ehci_op_read32(ehci, EHCI_OP_USBCMD);
    cmd &= ~EHCI_CMD_RS;
    ehci_op_write32(ehci, EHCI_OP_USBCMD, cmd);

    // Wait for halt
    for (int i = 0; i < 100; i++) {
        if (ehci_op_read32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HALT) {
            break;
        }
        timer_sleep_ms(1);
    }

    // Reset the controller
    cmd = ehci_op_read32(ehci, EHCI_OP_USBCMD);
    cmd |= EHCI_CMD_HCRESET;
    ehci_op_write32(ehci, EHCI_OP_USBCMD, cmd);

    // Wait for reset to complete
    for (int i = 0; i < 100; i++) {
        if (!(ehci_op_read32(ehci, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) {
            break;
        }
        timer_sleep_ms(1);
    }

    // Clear status
    ehci_op_write32(ehci, EHCI_OP_USBSTS, 0x3F);

    debuglog(DEBUG_INFO, "[EHCI] Controller reset complete\n");
}

/**
 * Setup async schedule
 */
static void ehci_setup_async_schedule(ehci_data_t* ehci) {
    // Create a dummy QH as the head of the async list
    ehci->async_head = ehci_alloc_qh(ehci);
    if (!ehci->async_head) {
        return;
    }

    // Configure as head of reclamation list, pointing to itself
    ehci->async_head->horizontal_link = ehci_phys_addr(ehci->async_head) |
                                        (EHCI_QH_TYPE_QH << 1);
    ehci->async_head->endpoint_ch = EHCI_QH_EC_H;  // Head of reclamation
    ehci->async_head->endpoint_cap = (1 << EHCI_QH_CAP_MULT_SHIFT);
    ehci->async_head->next_td = EHCI_QH_LP_TERMINATE;
    ehci->async_head->alt_td = EHCI_QH_LP_TERMINATE;
    ehci->async_head->token = EHCI_TD_TOKEN_STATUS_HALTED;
}

/**
 * Setup periodic schedule
 */
static void ehci_setup_periodic_schedule(ehci_data_t* ehci) {
    // Initialize frame list with terminated entries
    for (int i = 0; i < EHCI_FRAME_LIST_SIZE; i++) {
        ehci->frame_list[i] = EHCI_QH_LP_TERMINATE;
    }

    // TODO: Setup interrupt queue heads for different intervals
}

static uint32 ehci_phys_addr(const void* addr) {
    if (!addr) {
        return 0;
    }

    uintptr_t va = (uintptr_t)addr;
    uintptr_t page_va = va & ~(uintptr_t)0xFFF;
    uintptr_t phys_page = vmm_get_physical_addr(vmm_get_current_page_directory(), (uint32)page_va);
    if (!phys_page) {
        debuglog(DEBUG_WARN, "[EHCI] Failed to translate VA 0x%08x\n", (uint32)va);
        return (uint32)va;
    }

    return (uint32)(phys_page | (va & 0xFFF));
}

static void ehci_fill_buffer_ptrs(ehci_qtd_t* qtd, const void* buf, uint32 len) {
    if (!qtd || !buf || len == 0) {
        return;
    }

    uintptr_t va = (uintptr_t)buf;
    uintptr_t end = va + len - 1;

    for (int i = 0; i < 5; i++) {
        if (va > end) {
            qtd->buffer[i] = 0;
            continue;
        }

        uintptr_t page_va = va & ~(uintptr_t)0xFFF;
        uintptr_t phys_page = vmm_get_physical_addr(
            vmm_get_current_page_directory(), (uint32)page_va);
        if (!phys_page) {
            debuglog(DEBUG_WARN, "[EHCI] Failed to translate buffer VA 0x%08x\n", (uint32)va);
            qtd->buffer[i] = (uint32)va;
        } else {
            qtd->buffer[i] = (uint32)(phys_page | (va & 0xFFF));
        }

        va = page_va + 0x1000;
    }
}

static void* ehci_alloc_aligned(size_t size, size_t align, const char* tag, void** out_raw) {
    if (align == 0 || (align & (align - 1)) != 0) {
        return NULL;
    }

    size_t total = size + align - 1;
    void* raw = enhanced_heap_alloc(total, tag);
    if (!raw) {
        return NULL;
    }

    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + (align - 1)) & ~(uintptr_t)(align - 1);
    if (out_raw) {
        *out_raw = raw;
    }
    return (void*)aligned;
}

/**
 * Allocate a Queue Head
 */
static ehci_qh_t* ehci_alloc_qh(ehci_data_t* ehci) {
    for (int i = 0; i < EHCI_QH_POOL_SIZE; i++) {
        uint32 word_index = i / 32;
        uint32 bit_index = i % 32;

        if (!(ehci->qh_bitmap[word_index] & (1 << bit_index))) {
            ehci->qh_bitmap[word_index] |= (1 << bit_index);
            ehci_qh_t* qh = &ehci->qh_pool[i];
            memory_set((uint8*)qh, 0, sizeof(ehci_qh_t));
            return qh;
        }
    }
    return NULL;
}

/**
 * Free a Queue Head
 */
static void ehci_free_qh(ehci_data_t* ehci, ehci_qh_t* qh) {
    if (!qh || qh == ehci->async_head) return;

    int index = (int)(qh - ehci->qh_pool);
    if (index >= 0 && index < EHCI_QH_POOL_SIZE) {
        uint32 word_index = index / 32;
        uint32 bit_index = index % 32;
        ehci->qh_bitmap[word_index] &= ~(1 << bit_index);
    }
}

/**
 * Allocate a Queue Transfer Descriptor
 */
static ehci_qtd_t* ehci_alloc_qtd(ehci_data_t* ehci) {
    for (int i = 0; i < EHCI_QTD_POOL_SIZE; i++) {
        uint32 word_index = i / 32;
        uint32 bit_index = i % 32;

        if (!(ehci->qtd_bitmap[word_index] & (1 << bit_index))) {
            ehci->qtd_bitmap[word_index] |= (1 << bit_index);
            ehci_qtd_t* qtd = &ehci->qtd_pool[i];
            memory_set((uint8*)qtd, 0, sizeof(ehci_qtd_t));
            return qtd;
        }
    }
    return NULL;
}

/**
 * Free a Queue Transfer Descriptor
 */
static void ehci_free_qtd(ehci_data_t* ehci, ehci_qtd_t* qtd) {
    if (!qtd) return;

    int index = (int)(qtd - ehci->qtd_pool);
    if (index >= 0 && index < EHCI_QTD_POOL_SIZE) {
        uint32 word_index = index / 32;
        uint32 bit_index = index % 32;
        ehci->qtd_bitmap[word_index] &= ~(1 << bit_index);
    }
}

/**
 * Wait for QTD to complete
 */
static int ehci_wait_for_transfer(ehci_data_t* ehci, ehci_qtd_t* qtd, uint32 timeout_ms) {
    (void)ehci;

    for (uint32 i = 0; i < timeout_ms; i++) {
        if (!(qtd->token & EHCI_TD_TOKEN_STATUS_ACTIVE)) {
            // Check for errors
            if (qtd->token & (EHCI_TD_TOKEN_STATUS_HALTED |
                              EHCI_TD_TOKEN_STATUS_DBUFFER |
                              EHCI_TD_TOKEN_STATUS_BABBLE |
                              EHCI_TD_TOKEN_STATUS_XACT)) {
                return -1;
            }
            uint32 bytes = (qtd->token & EHCI_TD_TOKEN_BYTES_MASK) >> EHCI_TD_TOKEN_BYTES_SHIFT;
            return (int)(qtd->sw_length - bytes);
        }
        timer_sleep_ms(1);
    }

    return -1;  // Timeout
}

/**
 * Reset a port
 */
bool ehci_reset_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;
    uint16 port_reg = EHCI_OP_PORTSC + (port * 4);

    // Check if device is connected
    uint32 portsc = ehci_op_read32(ehci, port_reg);
    if (!(portsc & EHCI_PORT_CCS)) {
        return false;
    }

    // Check if it's a low/full speed device (needs companion controller)
    if (!(portsc & EHCI_PORT_PED) && (portsc & EHCI_PORT_LS_MASK) == 0x400) {
        // K-state detected (low-speed device), hand off to companion
        portsc |= EHCI_PORT_OWNER;
        ehci_op_write32(ehci, port_reg, portsc);
        debuglog(DEBUG_INFO, "[EHCI] Port %d: Low-speed device, handed to companion\n", port);
        return false;
    }

    // Set port reset
    portsc &= ~EHCI_PORT_PED;  // Disable first
    portsc |= EHCI_PORT_RESET;
    ehci_op_write32(ehci, port_reg, portsc);

    timer_sleep_ms(50);  // USB spec says at least 10ms

    // Clear reset
    portsc = ehci_op_read32(ehci, port_reg);
    portsc &= ~EHCI_PORT_RESET;
    ehci_op_write32(ehci, port_reg, portsc);

    // Wait for reset to complete and port to enable
    for (int i = 0; i < 100; i++) {
        timer_sleep_ms(1);
        portsc = ehci_op_read32(ehci, port_reg);

        if (!(portsc & EHCI_PORT_RESET)) {
            // Check if high-speed device enabled
            if (portsc & EHCI_PORT_PED) {
                // Clear change bits
                ehci_op_write32(ehci, port_reg, portsc |
                                EHCI_PORT_CSC | EHCI_PORT_PEDC | EHCI_PORT_OCC);
                return true;
            } else {
                // Not high-speed, hand off to companion
                portsc |= EHCI_PORT_OWNER;
                ehci_op_write32(ehci, port_reg, portsc);
                debuglog(DEBUG_INFO, "[EHCI] Port %d: Full-speed device, handed to companion\n", port);
                return false;
            }
        }
    }

    debuglog(DEBUG_WARN, "[EHCI] Port %d reset timeout\n", port);
    return false;
}

/**
 * Enable a port
 */
bool ehci_enable_port(usb_controller_t* controller, uint8 port) {
    // EHCI ports are enabled automatically after reset
    (void)controller;
    (void)port;
    return true;
}

/**
 * Disable a port
 */
bool ehci_disable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;
    uint16 port_reg = EHCI_OP_PORTSC + (port * 4);

    uint32 portsc = ehci_op_read32(ehci, port_reg);
    portsc &= ~EHCI_PORT_PED;
    ehci_op_write32(ehci, port_reg, portsc);

    return true;
}

/**
 * Get port speed
 */
usb_speed_t ehci_get_port_speed(usb_controller_t* controller, uint8 port) {
    (void)controller;
    (void)port;
    // EHCI only handles high-speed devices
    return USB_SPEED_HIGH;
}

/**
 * Check if port has device connected
 */
bool ehci_port_connected(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data || port >= controller->num_ports) {
        return false;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;
    uint16 port_reg = EHCI_OP_PORTSC + (port * 4);

    uint32 portsc = ehci_op_read32(ehci, port_reg);

    // Check if connected and not owned by companion
    return (portsc & EHCI_PORT_CCS) && !(portsc & EHCI_PORT_OWNER);
}

/**
 * Control transfer
 */
int ehci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length) {
    if (!controller || !controller->hcd_data || !device || !setup) {
        return -1;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;
    bool is_in = (setup->bmRequestType & USB_REQTYPE_DIR_IN) != 0;
    uint16 max_packet = device->max_packet_size0 ? device->max_packet_size0 : 64;

    // Allocate QH for this transfer
    ehci_qh_t* qh = ehci_alloc_qh(ehci);
    if (!qh) return -1;

    // Setup QH endpoint characteristics
    qh->endpoint_ch = (device->address & EHCI_QH_EC_ADDR_MASK) |
                      ((0 << EHCI_QH_EC_ENDPOINT_SHIFT) & EHCI_QH_EC_ENDPOINT_MASK) |
                      EHCI_QH_EC_EPS_HIGH |
                      EHCI_QH_EC_DTC |
                      ((max_packet << EHCI_QH_EC_MPL_SHIFT) & EHCI_QH_EC_MPL_MASK) |
                      (device->address == 0 ? EHCI_QH_EC_C : 0) |
                      (3 << EHCI_QH_EC_RL_SHIFT);

    qh->endpoint_cap = (1 << EHCI_QH_CAP_MULT_SHIFT);

    // Allocate SETUP QTD
    ehci_qtd_t* qtd_setup = ehci_alloc_qtd(ehci);
    if (!qtd_setup) {
        ehci_free_qh(ehci, qh);
        return -1;
    }

    qtd_setup->next_td = EHCI_QH_LP_TERMINATE;
    qtd_setup->alt_td = EHCI_QH_LP_TERMINATE;
    qtd_setup->token = EHCI_TD_TOKEN_STATUS_ACTIVE |
                       EHCI_TD_TOKEN_PID_SETUP |
                       (3 << EHCI_TD_TOKEN_CERR_SHIFT) |
                       (8 << EHCI_TD_TOKEN_BYTES_SHIFT);
    ehci_fill_buffer_ptrs(qtd_setup, setup, 8);
    qtd_setup->sw_length = 8;

    // Link QTD to QH
    qh->next_td = ehci_phys_addr(qtd_setup);
    qh->token = 0;

    // Link QH into async schedule
    qh->horizontal_link = ehci->async_head->horizontal_link;
    ehci->async_head->horizontal_link = ehci_phys_addr(qh) | (EHCI_QH_TYPE_QH << 1);

    // Wait for SETUP to complete
    int result = ehci_wait_for_transfer(ehci, qtd_setup, 500);

    // Unlink QH
    ehci->async_head->horizontal_link = qh->horizontal_link;
    ehci_free_qtd(ehci, qtd_setup);

    if (result < 0) {
        ehci_free_qh(ehci, qh);
        return -1;
    }

    // DATA stage (if needed)
    int total_transferred = 0;
    if (length > 0 && data) {
        ehci_qtd_t* qtd_data = ehci_alloc_qtd(ehci);
        if (!qtd_data) {
            ehci_free_qh(ehci, qh);
            return -1;
        }

        qtd_data->next_td = EHCI_QH_LP_TERMINATE;
        qtd_data->alt_td = EHCI_QH_LP_TERMINATE;
        qtd_data->token = EHCI_TD_TOKEN_STATUS_ACTIVE |
                          (is_in ? EHCI_TD_TOKEN_PID_IN : EHCI_TD_TOKEN_PID_OUT) |
                          (3 << EHCI_TD_TOKEN_CERR_SHIFT) |
                          (length << EHCI_TD_TOKEN_BYTES_SHIFT) |
                          EHCI_TD_TOKEN_DT;  // DATA1
        ehci_fill_buffer_ptrs(qtd_data, data, length);
        qtd_data->sw_length = length;

        qh->next_td = ehci_phys_addr(qtd_data);
        qh->token = 0;

        // Link QH
        qh->horizontal_link = ehci->async_head->horizontal_link;
        ehci->async_head->horizontal_link = ehci_phys_addr(qh) | (EHCI_QH_TYPE_QH << 1);

        result = ehci_wait_for_transfer(ehci, qtd_data, 500);

        ehci->async_head->horizontal_link = qh->horizontal_link;
        ehci_free_qtd(ehci, qtd_data);

        if (result < 0) {
            ehci_free_qh(ehci, qh);
            return -1;
        }
        total_transferred = result;
    }

    // STATUS stage
    ehci_qtd_t* qtd_status = ehci_alloc_qtd(ehci);
    if (!qtd_status) {
        ehci_free_qh(ehci, qh);
        return total_transferred > 0 ? total_transferred : -1;
    }

    qtd_status->next_td = EHCI_QH_LP_TERMINATE;
    qtd_status->alt_td = EHCI_QH_LP_TERMINATE;
    qtd_status->token = EHCI_TD_TOKEN_STATUS_ACTIVE |
                        ((!is_in || length == 0) ? EHCI_TD_TOKEN_PID_IN : EHCI_TD_TOKEN_PID_OUT) |
                        (3 << EHCI_TD_TOKEN_CERR_SHIFT) |
                        EHCI_TD_TOKEN_IOC |
                        EHCI_TD_TOKEN_DT;
    qtd_status->sw_length = 0;

    qh->next_td = ehci_phys_addr(qtd_status);
    qh->token = 0;

    qh->horizontal_link = ehci->async_head->horizontal_link;
    ehci->async_head->horizontal_link = ehci_phys_addr(qh) | (EHCI_QH_TYPE_QH << 1);

    result = ehci_wait_for_transfer(ehci, qtd_status, 500);

    ehci->async_head->horizontal_link = qh->horizontal_link;
    ehci_free_qtd(ehci, qtd_status);
    ehci_free_qh(ehci, qh);

    return (result >= 0) ? total_transferred : -1;
}

/**
 * Bulk transfer
 */
int ehci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length) {
    if (!controller || !controller->hcd_data || !device || !endpoint || !data) {
        return -1;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;
    bool is_in = (endpoint->direction == USB_DIR_IN);
    uint16 max_packet = endpoint->max_packet_size;

    // Allocate QH
    ehci_qh_t* qh = ehci_alloc_qh(ehci);
    if (!qh) return -1;

    qh->endpoint_ch = (device->address & EHCI_QH_EC_ADDR_MASK) |
                      (((endpoint->address & 0x0F) << EHCI_QH_EC_ENDPOINT_SHIFT) & EHCI_QH_EC_ENDPOINT_MASK) |
                      EHCI_QH_EC_EPS_HIGH |
                      ((max_packet << EHCI_QH_EC_MPL_SHIFT) & EHCI_QH_EC_MPL_MASK) |
                      (3 << EHCI_QH_EC_RL_SHIFT);

    qh->endpoint_cap = (1 << EHCI_QH_CAP_MULT_SHIFT);

    int total_transferred = 0;
    uint8* buf_ptr = (uint8*)data;
    uint32 remaining = length;

    while (remaining > 0) {
        uint32 transfer_len = remaining > 16384 ? 16384 : remaining;

        ehci_qtd_t* qtd = ehci_alloc_qtd(ehci);
        if (!qtd) break;

        qtd->next_td = EHCI_QH_LP_TERMINATE;
        qtd->alt_td = EHCI_QH_LP_TERMINATE;
        qtd->token = EHCI_TD_TOKEN_STATUS_ACTIVE |
                     (is_in ? EHCI_TD_TOKEN_PID_IN : EHCI_TD_TOKEN_PID_OUT) |
                     (3 << EHCI_TD_TOKEN_CERR_SHIFT) |
                     (transfer_len << EHCI_TD_TOKEN_BYTES_SHIFT);
        ehci_fill_buffer_ptrs(qtd, buf_ptr, transfer_len);
        qtd->sw_length = transfer_len;

        qh->next_td = ehci_phys_addr(qtd);
        qh->token = 0;

        qh->horizontal_link = ehci->async_head->horizontal_link;
        ehci->async_head->horizontal_link = ehci_phys_addr(qh) | (EHCI_QH_TYPE_QH << 1);

        int result = ehci_wait_for_transfer(ehci, qtd, 5000);

        ehci->async_head->horizontal_link = qh->horizontal_link;
        ehci_free_qtd(ehci, qtd);

        if (result < 0) break;

        total_transferred += result;
        buf_ptr += result;
        remaining -= result;

        if (is_in && (uint32)result < transfer_len) break;  // Short packet
    }

    ehci_free_qh(ehci, qh);

    return total_transferred > 0 ? total_transferred : -1;
}

/**
 * Interrupt transfer
 */
int ehci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length) {
    // For now, implement as bulk transfer
    // TODO: Proper interrupt transfer using periodic schedule
    return ehci_bulk_transfer(controller, device, endpoint, data, length);
}

/**
 * Poll controller for events
 */
void ehci_poll(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) {
        return;
    }

    ehci_data_t* ehci = (ehci_data_t*)controller->hcd_data;

    // Read and clear status
    uint32 status = ehci_op_read32(ehci, EHCI_OP_USBSTS);
    if (status & (EHCI_STS_INT | EHCI_STS_ERR | EHCI_STS_PCD | EHCI_STS_IAA)) {
        ehci_op_write32(ehci, EHCI_OP_USBSTS, status);
    }

    // Check for port changes
    if (status & EHCI_STS_PCD) {
        for (uint8 port = 0; port < ehci->num_ports; port++) {
            uint32 portsc = ehci_op_read32(ehci, EHCI_OP_PORTSC + (port * 4));

            if (portsc & EHCI_PORT_CSC) {
                ehci_op_write32(ehci, EHCI_OP_PORTSC + (port * 4), portsc);

                if (portsc & EHCI_PORT_CCS) {
                    debuglog(DEBUG_INFO, "[EHCI] Device connected on port %d\n", port);
                } else {
                    debuglog(DEBUG_INFO, "[EHCI] Device disconnected from port %d\n", port);
                }
            }
        }
    }
}
