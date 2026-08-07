/**
 * USB Core Subsystem for Fern
 *
 * Implements USB device enumeration, driver management, and core functionality.
 */

#include "../include/usb/usb.h"
#include "../include/usb/usb_hid.h"
#include "../include/usb/uhci.h"
#include "../include/usb/ohci.h"
#include "../include/usb/ehci.h"
#include "../include/usb/xhci.h"
#include "../include/pci.h"
#include "../include/enhanced_heap.h"
#include "../include/string.h"
#include "../include/util.h"
#include "../include/screen.h"
#include "../include/debuglog.h"
#include "../include/timer.h"
#include "../include/memory.h"

// Maximum number of controllers
#define USB_MAX_CONTROLLERS     8
#define USB_MAX_DEVICES         128

// Controller list
static usb_controller_t* g_controllers = NULL;
static uint32 g_controller_count = 0;

// Class driver list
static usb_class_driver_t* g_class_drivers = NULL;

// USB initialized flag
static bool g_usb_initialized = false;
static page_directory_t* g_usb_last_synced_pd = NULL;
static page_directory_t* g_usb_last_mmio_map_pd = NULL;
static uint32 g_usb_ops_repair_count = 0;
static bool g_usb_poll_quarantined = false;

// Forward declarations
static bool usb_probe_controller(const pci_device_t* pci_dev);
static void usb_enumerate_ports(usb_controller_t* controller);
static bool usb_probe_device(usb_controller_t* controller, uint8 port);
static bool usb_match_class_driver(usb_device_t* device, usb_interface_t* interface);
static usb_controller_ops_t* usb_resolve_controller_ops(usb_controller_t* controller,
                                                        const char* context);

static usb_controller_ops_t* usb_ops_for_type(usb_controller_type_t type) {
    switch (type) {
        case USB_CONTROLLER_UHCI:
            return &uhci_ops;
        case USB_CONTROLLER_OHCI:
            return &ohci_ops;
        case USB_CONTROLLER_EHCI:
            return &ehci_ops;
        case USB_CONTROLLER_XHCI:
            return &xhci_ops;
        default:
            return NULL;
    }
}

static bool usb_ops_is_known(const usb_controller_ops_t* ops) {
    return ops == &uhci_ops || ops == &ohci_ops || ops == &ehci_ops || ops == &xhci_ops;
}

static bool usb_controller_ptr_plausible(const usb_controller_t* controller) {
    if (!controller) {
        return false;
    }

    uintptr_t addr = (uintptr_t)controller;
    uintptr_t heap_start = (uintptr_t)memory_get_kernel_heap_start();
    uintptr_t heap_end = heap_start + MEMORY_KERNEL_HEAP_MAX_SIZE;
    if (addr < heap_start || (addr + sizeof(usb_controller_t)) > heap_end) {
        return false;
    }

    if ((addr & (sizeof(uintptr_t) - 1)) != 0) {
        return false;
    }

    if (controller->type < USB_CONTROLLER_UHCI || controller->type > USB_CONTROLLER_XHCI) {
        return false;
    }

    if (controller->num_ports > 64) {
        return false;
    }

    return true;
}

static usb_controller_ops_t* usb_resolve_controller_ops(usb_controller_t* controller,
                                                        const char* context) {
    if (!controller) {
        return NULL;
    }

    usb_controller_ops_t* expected_ops = usb_ops_for_type(controller->type);
    if (!expected_ops) {
        if (g_usb_ops_repair_count < 32) {
            debuglog(DEBUG_WARN,
                     "[USB] Invalid controller type in %s: type=%d ctrl=%p\n",
                     context ? context : "unknown", (int)controller->type, controller);
        }
        return NULL;
    }

    if (!usb_ops_is_known(controller->ops) || controller->ops != expected_ops) {
        if (g_usb_ops_repair_count < 32) {
            debuglog(DEBUG_WARN,
                     "[USB] Repaired controller ops in %s: ctrl=%p old=%p new=%p type=%d\n",
                     context ? context : "unknown",
                     controller,
                     controller->ops,
                     expected_ops,
                     (int)controller->type);
        }
        controller->ops = expected_ops;
        g_usb_ops_repair_count++;
    }

    return expected_ops;
}

/**
 * PCI enumeration callback for USB controllers
 */
static bool usb_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    // Check for USB Serial Bus Controller
    if (device->class_code == USB_CLASS_SERIAL_BUS &&
        device->subclass == USB_SUBCLASS_USB) {

        const char* type_str = "Unknown";
        switch (device->prog_if) {
            case USB_PROGIF_UHCI: type_str = "UHCI"; break;
            case USB_PROGIF_OHCI: type_str = "OHCI"; break;
            case USB_PROGIF_EHCI: type_str = "EHCI"; break;
            case USB_PROGIF_XHCI: type_str = "xHCI"; break;
        }

        debuglog(DEBUG_INFO, "[USB] Found %s controller at %02x:%02x.%x\n",
                 type_str, device->bus, device->device, device->function);

        usb_probe_controller(device);
    }

    return true;  // Continue enumeration
}

/**
 * Initialize the USB subsystem
 */
bool usb_init(void) {
    if (g_usb_initialized) {
        return true;
    }

    print("[USB] Initializing USB subsystem...\n");
    debuglog(DEBUG_INFO, "[USB] Initializing USB subsystem\n");

    // Initialize PCI first
    if (!pci_init()) {
        print("[USB] PCI initialization failed\n");
        return false;
    }

    // Scan PCI bus for USB controllers
    pci_enumerate(usb_pci_callback, NULL);

    if (g_controller_count == 0) {
        print("[USB] No USB controllers found\n");
        debuglog(DEBUG_WARN, "[USB] No USB controllers found\n");
        // Not a fatal error - system can work without USB
    } else {
        print("[USB] Found ");
        print(int_to_string(g_controller_count));
        print(" USB controller(s)\n");
    }

    g_usb_initialized = true;

    // Enumerate ports on all controllers
    usb_controller_t* controller = g_controllers;
    uint32 visited = 0;
    while (controller) {
        if (!usb_controller_ptr_plausible(controller)) {
            debuglog(DEBUG_WARN, "[USB] Invalid controller pointer during init enumerate: %p\n", controller);
            g_usb_poll_quarantined = true;
            break;
        }
        if (visited++ >= USB_MAX_CONTROLLERS) {
            debuglog(DEBUG_WARN, "[USB] Controller list overrun during init enumerate\n");
            g_usb_poll_quarantined = true;
            break;
        }
        usb_enumerate_ports(controller);
        controller = controller->next;
    }

    return true;
}

/**
 * Shutdown the USB subsystem
 */
void usb_shutdown(void) {
    if (!g_usb_initialized) {
        return;
    }

    debuglog(DEBUG_INFO, "[USB] Shutting down USB subsystem\n");

    // Shutdown all controllers
    usb_controller_t* controller = g_controllers;
    while (controller) {
        if (!usb_controller_ptr_plausible(controller)) {
            debuglog(DEBUG_WARN, "[USB] Invalid controller pointer during shutdown: %p\n", controller);
            break;
        }
        usb_controller_t* next = controller->next;

        // Free all devices
        for (int i = 0; i < 128; i++) {
            if (controller->devices[i]) {
                usb_free_device(controller->devices[i]);
                controller->devices[i] = NULL;
            }
        }

        // Call controller shutdown
        usb_controller_ops_t* ops = usb_resolve_controller_ops(controller, "shutdown");
        if (ops && ops->shutdown) {
            ops->shutdown(controller);
        }

        // Free controller-specific data
        if (controller->hcd_data) {
            enhanced_heap_free(controller->hcd_data, "usb_hcd_data");
        }

        enhanced_heap_free(controller, "usb_controller");
        controller = next;
    }

    g_controllers = NULL;
    g_controller_count = 0;
    g_usb_initialized = false;
    g_usb_last_synced_pd = NULL;
    g_usb_last_mmio_map_pd = NULL;
}

/**
 * Poll all USB controllers
 */
void usb_poll(void) {
    if (!g_usb_initialized) {
        return;
    }
    if (g_usb_poll_quarantined) {
        return;
    }

    page_directory_t* active_pd = vmm_get_current_page_directory();

    if (active_pd && active_pd != g_usb_last_synced_pd) {
        /*
         * USB poll runs from timer IRQ context. Ensure kernel PDEs (including
         * MMIO ranges like EHCI BARs) are visible in the active task CR3.
         */
        vmm_sync_kernel_pdes(active_pd);
        g_usb_last_synced_pd = active_pd;
    }

    usb_controller_t* controller = g_controllers;
    uint32 visited = 0;
    while (controller) {
        if (!usb_controller_ptr_plausible(controller)) {
            debuglog(DEBUG_WARN, "[USB] Invalid controller pointer in poll: %p\n", controller);
            g_usb_poll_quarantined = true;
            break;
        }

        uint32 controller_addr = (uint32)(uintptr_t)controller;
        uint32 controller_page = controller_addr & ~MEMORY_PAGE_MASK;
        if (active_pd && !vmm_is_mapped(active_pd, controller_page)) {
            debuglog(DEBUG_WARN,
                     "[USB] Controller pointer left mapped space in poll: ctrl=%p page=0x%08x\n",
                     controller, controller_page);
            g_usb_poll_quarantined = true;
            break;
        }

        if (visited++ >= USB_MAX_CONTROLLERS) {
            debuglog(DEBUG_WARN,
                     "[USB] Controller list appears corrupted (visited=%u count=%u)\n",
                     visited, g_controller_count);
            break;
        }

        usb_controller_t* next = controller->next;
        if (next) {
            if (!usb_controller_ptr_plausible(next)) {
                debuglog(DEBUG_WARN,
                         "[USB] Invalid controller->next in poll: curr=%p next=%p (plausibility)\n",
                         controller, next);
                next = NULL;
                g_usb_poll_quarantined = true;
            }

            uint32 next_addr = (uint32)(uintptr_t)next;
            uint32 next_page = next_addr & ~MEMORY_PAGE_MASK;
            if (next && active_pd && !vmm_is_mapped(active_pd, next_page)) {
                debuglog(DEBUG_WARN,
                         "[USB] Invalid controller->next in poll: curr=%p next=%p page=0x%08x\n",
                         controller, next, next_page);
                next = NULL;
                g_usb_poll_quarantined = true;
            }
        }

        usb_controller_ops_t* ops = usb_resolve_controller_ops(controller, "poll");
        if (!ops) {
            g_usb_poll_quarantined = true;
            controller = next;
            continue;
        }

        bool can_poll = true;
        if (controller->is_mmio && active_pd) {
            uint32 base = controller->base_address & ~0xFFFu;
            uint32 end = base + 0x10000u; /* cover common HC register windows */
            if (end < base) {
                end = base + 0x1000u;
            }

            if (!vmm_is_mapped(active_pd, base) || active_pd != g_usb_last_mmio_map_pd) {
                memory_result_t map_res = vmm_identity_map_range(
                    active_pd,
                    base,
                    end,
                    PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
                if (map_res != MEMORY_OK && map_res != MEMORY_ERROR_ALREADY_MAPPED) {
                    debuglog(DEBUG_WARN,
                             "[USB] MMIO remap failed in active PD: base=0x%08x end=0x%08x res=%d\n",
                             base, end, (int)map_res);
                    can_poll = false;
                } else {
                    g_usb_last_mmio_map_pd = active_pd;
                }
            }

            if (can_poll) {
                can_poll = vmm_is_mapped(active_pd, base);
            }
        }

        if (can_poll && ops && ops->poll) {
            ops->poll(controller);
        }
        controller = next;
    }

    usb_hid_poll();
}

/**
 * Probe and initialize a USB controller
 */
static bool usb_probe_controller(const pci_device_t* pci_dev) {
    if (g_controller_count >= USB_MAX_CONTROLLERS) {
        debuglog(DEBUG_WARN, "[USB] Too many controllers, skipping\n");
        return false;
    }

    // Allocate controller structure
    usb_controller_t* controller = (usb_controller_t*)enhanced_heap_alloc(
        sizeof(usb_controller_t), "usb_controller");
    if (!controller) {
        return false;
    }

    memory_set((uint8*)controller, 0, sizeof(usb_controller_t));
    controller->pci_device = *pci_dev;

    // Determine controller type and set up operations
    switch (pci_dev->prog_if) {
        case USB_PROGIF_UHCI:
            controller->type = USB_CONTROLLER_UHCI;
            controller->base_address = pci_dev->bar[4] & ~0x03;  // I/O space
            controller->is_mmio = false;
            controller->ops = &uhci_ops;
            break;

        case USB_PROGIF_OHCI:
            controller->type = USB_CONTROLLER_OHCI;
            controller->base_address = pci_dev->bar[0] & ~0x0F;  // Memory space
            controller->is_mmio = true;
            controller->ops = &ohci_ops;
            break;

        case USB_PROGIF_EHCI:
            controller->type = USB_CONTROLLER_EHCI;
            controller->base_address = pci_dev->bar[0] & ~0x0F;  // Memory space
            controller->is_mmio = true;
            controller->ops = &ehci_ops;
            break;

        case USB_PROGIF_XHCI:
            controller->type = USB_CONTROLLER_XHCI;
            controller->base_address = pci_dev->bar[0] & ~0x0F;  // Memory space
            controller->is_mmio = true;
            controller->ops = &xhci_ops;
            break;

        default:
            debuglog(DEBUG_WARN, "[USB] Unknown controller type: 0x%02x\n", pci_dev->prog_if);
            enhanced_heap_free(controller, "usb_controller");
            return false;
    }

    // Enable PCI bus mastering and memory/IO access
    uint16 cmd = pci_config_read16(pci_dev->segment, pci_dev->bus,
                                    pci_dev->device, pci_dev->function, 0x04);
    cmd |= 0x07;  // I/O, Memory, Bus Master
    pci_config_write16(pci_dev->segment, pci_dev->bus,
                       pci_dev->device, pci_dev->function, 0x04, cmd);

    if (controller->is_mmio) {
        uint32 base = controller->base_address & ~0xFFFu;
        uint32 end = base + 0x10000u;
        if (end < base) {
            end = base + 0x1000u;
        }

        page_directory_t* kernel_pd = vmm_get_kernel_page_directory();
        page_directory_t* active_pd = vmm_get_current_page_directory();

        if (kernel_pd) {
            memory_result_t kmap_res = vmm_identity_map_range(
                kernel_pd,
                base,
                end,
                PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
            if (kmap_res != MEMORY_OK && kmap_res != MEMORY_ERROR_ALREADY_MAPPED) {
                debuglog(DEBUG_ERROR,
                         "[USB] Failed to map MMIO in kernel PD 0x%08x-0x%08x (res=%d)\n",
                         base, end, (int)kmap_res);
            }
        }

        if (active_pd && active_pd != kernel_pd) {
            memory_result_t amap_res = vmm_identity_map_range(
                active_pd,
                base,
                end,
                PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
            if (amap_res != MEMORY_OK && amap_res != MEMORY_ERROR_ALREADY_MAPPED) {
                debuglog(DEBUG_ERROR,
                         "[USB] Failed to map MMIO in active PD 0x%08x-0x%08x (res=%d)\n",
                         base, end, (int)amap_res);
            }
        }

        if (kernel_pd || active_pd) {
            debuglog(DEBUG_INFO,
                     "[USB] MMIO mapped range 0x%08x-0x%08x (kernel_pd=%p active_pd=%p)\n",
                     base, end, kernel_pd, active_pd);
        }
    }

    // Initialize the controller
    usb_controller_ops_t* controller_ops = usb_resolve_controller_ops(controller, "probe_init");
    if (controller_ops && controller_ops->init) {
        if (!controller_ops->init(controller)) {
            debuglog(DEBUG_ERROR, "[USB] Controller initialization failed\n");
            enhanced_heap_free(controller, "usb_controller");
            return false;
        }
    }

    controller->initialized = true;

    // Add to controller list
    controller->next = g_controllers;
    g_controllers = controller;
    g_controller_count++;

    debuglog(DEBUG_INFO, "[USB] Controller initialized: %s (base=0x%08x, ports=%d)\n",
             usb_speed_string(USB_SPEED_HIGH), controller->base_address, controller->num_ports);

    return true;
}

/**
 * Enumerate ports on a controller
 */
static void usb_enumerate_ports(usb_controller_t* controller) {
    if (!controller || !controller->initialized) {
        return;
    }

    debuglog(DEBUG_INFO, "[USB] Enumerating %d ports on controller\n", controller->num_ports);

    usb_controller_ops_t* ops = usb_resolve_controller_ops(controller, "enumerate_ports");
    for (uint8 port = 0; port < controller->num_ports; port++) {
        if (ops && ops->port_connected) {
            if (ops->port_connected(controller, port)) {
                debuglog(DEBUG_INFO, "[USB] Device detected on port %d\n", port);
                usb_probe_device(controller, port);
            }
        }
    }
}

/**
 * Probe a device on a port
 */
static bool usb_probe_device(usb_controller_t* controller, uint8 port) {
    if (!controller) {
        return false;
    }

    usb_controller_ops_t* ops = usb_resolve_controller_ops(controller, "probe_device");
    if (!ops) {
        return false;
    }

    // Reset the port
    if (ops->reset_port) {
        if (!ops->reset_port(controller, port)) {
            debuglog(DEBUG_WARN, "[USB] Port %d reset failed\n", port);
            return false;
        }
    }

    // Wait for port to settle
    timer_sleep_ms(100);

    // Check if device is still connected
    if (ops->port_connected) {
        if (!ops->port_connected(controller, port)) {
            debuglog(DEBUG_WARN, "[USB] Device disconnected after reset\n");
            return false;
        }
    }

    // Allocate device structure
    usb_device_t* device = usb_alloc_device(controller);
    if (!device) {
        return false;
    }

    device->port = port;

    // Get port speed
    if (ops->get_port_speed) {
        device->speed = ops->get_port_speed(controller, port);
    } else {
        device->speed = USB_SPEED_FULL;
    }

    debuglog(DEBUG_INFO, "[USB] Device speed: %s\n", usb_speed_string(device->speed));

    // Enumerate the device
    if (!usb_enumerate_device(device)) {
        usb_free_device(device);
        return false;
    }

    return true;
}

/**
 * Allocate a USB device structure
 */
usb_device_t* usb_alloc_device(usb_controller_t* controller) {
    if (!controller) {
        return NULL;
    }

    usb_device_t* device = (usb_device_t*)enhanced_heap_alloc(
        sizeof(usb_device_t), "usb_device");
    if (!device) {
        return NULL;
    }

    memory_set((uint8*)device, 0, sizeof(usb_device_t));
    device->controller = controller;
    device->address = 0;  // Default address before enumeration

    return device;
}

/**
 * Free a USB device structure
 */
void usb_free_device(usb_device_t* device) {
    if (!device) {
        return;
    }

    // Free configurations
    if (device->configurations) {
        for (uint8 i = 0; i < device->num_configurations; i++) {
            usb_configuration_t* config = &device->configurations[i];
            if (config->interfaces) {
                for (uint8 j = 0; j < config->num_interfaces; j++) {
                    usb_interface_t* iface = &config->interfaces[j];
                    if (iface->endpoints) {
                        enhanced_heap_free(iface->endpoints, "usb_endpoints");
                    }
                }
                enhanced_heap_free(config->interfaces, "usb_interfaces");
            }
        }
        enhanced_heap_free(device->configurations, "usb_configs");
    }

    // Free controller-specific data
    if (device->hcd_data) {
        enhanced_heap_free(device->hcd_data, "usb_hcd_device");
    }

    enhanced_heap_free(device, "usb_device");
}

/**
 * Allocate a USB address
 */
uint8 usb_alloc_address(usb_controller_t* controller) {
    if (!controller) {
        return 0;
    }

    // Find first available address (1-127)
    for (uint8 addr = 1; addr < 128; addr++) {
        if (!controller->devices[addr]) {
            return addr;
        }
    }

    return 0;  // No address available
}

/**
 * Enumerate a USB device
 */
bool usb_enumerate_device(usb_device_t* device) {
    if (!device || !device->controller) {
        return false;
    }

    debuglog(DEBUG_INFO, "[USB] Enumerating device...\n");

    // Read first 8 bytes of device descriptor to get max packet size
    usb_device_descriptor_t desc;
    int result = usb_get_descriptor(device, USB_DESC_DEVICE, 0, &desc, 8);
    if (result < 0) {
        debuglog(DEBUG_ERROR, "[USB] Failed to read device descriptor (8 bytes)\n");
        return false;
    }

    device->max_packet_size0 = desc.bMaxPacketSize0;

    // Assign an address
    uint8 new_addr = usb_alloc_address(device->controller);
    if (new_addr == 0) {
        debuglog(DEBUG_ERROR, "[USB] No USB addresses available\n");
        return false;
    }

    result = usb_set_address(device, new_addr);
    if (result < 0) {
        debuglog(DEBUG_ERROR, "[USB] Failed to set address %d\n", new_addr);
        return false;
    }

    device->address = new_addr;
    device->controller->devices[new_addr] = device;

    timer_sleep_ms(10);  // Wait for address to settle

    // Read full device descriptor
    result = usb_get_descriptor(device, USB_DESC_DEVICE, 0, &desc, sizeof(desc));
    if (result < 0) {
        debuglog(DEBUG_ERROR, "[USB] Failed to read full device descriptor\n");
        return false;
    }

    device->vendor_id = desc.idVendor;
    device->product_id = desc.idProduct;
    device->device_class = desc.bDeviceClass;
    device->device_subclass = desc.bDeviceSubClass;
    device->device_protocol = desc.bDeviceProtocol;
    device->num_configurations = desc.bNumConfigurations;

    debuglog(DEBUG_INFO, "[USB] Device: VID=%04x PID=%04x Class=%02x Subclass=%02x\n",
             device->vendor_id, device->product_id,
             device->device_class, device->device_subclass);

    // Read string descriptors (manufacturer, product, serial)
    // TODO: Implement string descriptor reading

    // Read and parse configuration descriptors
    if (device->num_configurations > 0) {
        device->configurations = (usb_configuration_t*)enhanced_heap_alloc(
            sizeof(usb_configuration_t) * device->num_configurations, "usb_configs");
        if (!device->configurations) {
            return false;
        }
        memory_set((uint8*)device->configurations, 0,
                   sizeof(usb_configuration_t) * device->num_configurations);

        // Read first configuration
        usb_config_descriptor_t config_desc;
        result = usb_get_descriptor(device, USB_DESC_CONFIGURATION, 0,
                                    &config_desc, sizeof(config_desc));
        if (result < 0) {
            debuglog(DEBUG_WARN, "[USB] Failed to read configuration descriptor\n");
        } else {
            device->configurations[0].value = config_desc.bConfigurationValue;
            device->configurations[0].num_interfaces = config_desc.bNumInterfaces;
            device->configurations[0].attributes = config_desc.bmAttributes;
            device->configurations[0].max_power = config_desc.bMaxPower * 2;  // In mA

            debuglog(DEBUG_INFO, "[USB] Configuration: %d interfaces, %dmA\n",
                     config_desc.bNumInterfaces, config_desc.bMaxPower * 2);

            // Read full configuration with all descriptors
            uint8* config_data = (uint8*)enhanced_heap_alloc(config_desc.wTotalLength, "usb_config_data");
            if (config_data) {
                result = usb_get_descriptor(device, USB_DESC_CONFIGURATION, 0,
                                            config_data, config_desc.wTotalLength);
                if (result >= 0) {
                    // Parse interface and endpoint descriptors
                    // TODO: Implement full descriptor parsing
                }
                enhanced_heap_free(config_data, "usb_config_data");
            }
        }
    }

    // Set configuration (use first configuration)
    if (device->num_configurations > 0) {
        result = usb_set_configuration(device, device->configurations[0].value);
        if (result < 0) {
            debuglog(DEBUG_WARN, "[USB] Failed to set configuration\n");
        } else {
            device->configured = true;
            device->active_config = &device->configurations[0];
        }
    }

    // Try to match class drivers
    if (device->active_config) {
        for (uint8 i = 0; i < device->active_config->num_interfaces; i++) {
            usb_interface_t* iface = &device->active_config->interfaces[i];
            usb_match_class_driver(device, iface);
        }
    }

    debuglog(DEBUG_INFO, "[USB] Device enumerated successfully (addr=%d)\n", device->address);

    return true;
}

/**
 * Configure a USB device
 */
bool usb_configure_device(usb_device_t* device, uint8 config) {
    if (!device) {
        return false;
    }

    int result = usb_set_configuration(device, config);
    if (result < 0) {
        return false;
    }

    device->configured = true;
    return true;
}

/**
 * Send a control message
 */
int usb_control_msg(usb_device_t* device, uint8 request_type, uint8 request,
                    uint16 value, uint16 index, void* data, uint16 length) {
    if (!device || !device->controller) {
        return -1;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = request_type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = length;

    usb_controller_ops_t* ops = usb_resolve_controller_ops(device->controller, "control_msg");
    if (ops && ops->control_transfer) {
        return ops->control_transfer(
            device->controller, device, &setup, data, length);
    }

    return -1;
}

/**
 * Get a descriptor
 */
int usb_get_descriptor(usb_device_t* device, uint8 type, uint8 index,
                       void* buffer, uint16 length) {
    return usb_control_msg(device,
                           USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE,
                           USB_REQ_GET_DESCRIPTOR,
                           (type << 8) | index,
                           0,
                           buffer, length);
}

/**
 * Set device address
 */
int usb_set_address(usb_device_t* device, uint8 address) {
    return usb_control_msg(device,
                           USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE,
                           USB_REQ_SET_ADDRESS,
                           address,
                           0,
                           NULL, 0);
}

/**
 * Set device configuration
 */
int usb_set_configuration(usb_device_t* device, uint8 config) {
    return usb_control_msg(device,
                           USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE,
                           USB_REQ_SET_CONFIGURATION,
                           config,
                           0,
                           NULL, 0);
}

/**
 * Register a USB controller
 */
bool usb_register_controller(usb_controller_t* controller) {
    if (!controller) {
        return false;
    }

    controller->next = g_controllers;
    g_controllers = controller;
    g_controller_count++;

    return true;
}

/**
 * Unregister a USB controller
 */
void usb_unregister_controller(usb_controller_t* controller) {
    if (!controller) {
        return;
    }

    usb_controller_t* prev = NULL;
    usb_controller_t* curr = g_controllers;

    while (curr) {
        if (curr == controller) {
            if (prev) {
                prev->next = curr->next;
            } else {
                g_controllers = curr->next;
            }
            g_controller_count--;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/**
 * Find a controller by type
 */
usb_controller_t* usb_find_controller(usb_controller_type_t type) {
    usb_controller_t* controller = g_controllers;
    while (controller) {
        if (controller->type == type) {
            return controller;
        }
        controller = controller->next;
    }
    return NULL;
}

/**
 * Register a class driver
 */
bool usb_register_class_driver(usb_class_driver_t* driver) {
    if (!driver) {
        return false;
    }

    driver->next = g_class_drivers;
    g_class_drivers = driver;

    debuglog(DEBUG_INFO, "[USB] Registered class driver: %s\n", driver->name);

    return true;
}

/**
 * Unregister a class driver
 */
void usb_unregister_class_driver(usb_class_driver_t* driver) {
    if (!driver) {
        return;
    }

    usb_class_driver_t* prev = NULL;
    usb_class_driver_t* curr = g_class_drivers;

    while (curr) {
        if (curr == driver) {
            if (prev) {
                prev->next = curr->next;
            } else {
                g_class_drivers = curr->next;
            }
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/**
 * Match a class driver to an interface
 */
static bool usb_match_class_driver(usb_device_t* device, usb_interface_t* interface) {
    if (!device || !interface) {
        return false;
    }

    usb_class_driver_t* driver = g_class_drivers;
    while (driver) {
        // Check class match
        if (driver->class_code != 0xFF &&
            driver->class_code != interface->class_code) {
            driver = driver->next;
            continue;
        }

        // Check subclass match
        if (driver->subclass != 0xFF &&
            driver->subclass != interface->subclass) {
            driver = driver->next;
            continue;
        }

        // Check protocol match
        if (driver->protocol != 0xFF &&
            driver->protocol != interface->protocol) {
            driver = driver->next;
            continue;
        }

        // Try to probe
        if (driver->probe && driver->probe(device, interface)) {
            debuglog(DEBUG_INFO, "[USB] Device claimed by driver: %s\n", driver->name);
            return true;
        }

        driver = driver->next;
    }

    return false;
}

/**
 * Get USB speed string
 */
const char* usb_speed_string(usb_speed_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:         return "Low Speed (1.5 Mbps)";
        case USB_SPEED_FULL:        return "Full Speed (12 Mbps)";
        case USB_SPEED_HIGH:        return "High Speed (480 Mbps)";
        case USB_SPEED_SUPER:       return "SuperSpeed (5 Gbps)";
        case USB_SPEED_SUPER_PLUS:  return "SuperSpeed+ (10 Gbps)";
        default:                    return "Unknown";
    }
}

/**
 * Get USB class string
 */
const char* usb_class_string(uint8 class_code) {
    switch (class_code) {
        case USB_CLASS_PER_INTERFACE:   return "Per Interface";
        case USB_CLASS_AUDIO:           return "Audio";
        case USB_CLASS_COMM:            return "Communications";
        case USB_CLASS_HID:             return "HID";
        case USB_CLASS_PHYSICAL:        return "Physical";
        case USB_CLASS_IMAGE:           return "Image";
        case USB_CLASS_PRINTER:         return "Printer";
        case USB_CLASS_MASS_STORAGE:    return "Mass Storage";
        case USB_CLASS_HUB:             return "Hub";
        case USB_CLASS_CDC_DATA:        return "CDC Data";
        case USB_CLASS_SMART_CARD:      return "Smart Card";
        case USB_CLASS_VIDEO:           return "Video";
        case USB_CLASS_WIRELESS:        return "Wireless";
        case USB_CLASS_MISC:            return "Miscellaneous";
        case USB_CLASS_VENDOR_SPECIFIC: return "Vendor Specific";
        default:                        return "Unknown";
    }
}
