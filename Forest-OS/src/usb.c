#include "include/usb.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"

#define USB_MAX_HOST_CONTROLLERS 16

static usb_host_controller_t* g_usb_hcs[USB_MAX_HOST_CONTROLLERS] = {0};
static usb_device_t* g_usb_devices[USB_MAX_DEVICES] = {0};
static uint32 g_hc_count = 0;
static uint32 g_device_count = 0;
static bool g_usb_initialized = false;

extern bool uhci_init(void);
extern bool ohci_init(void);
extern bool ehci_init(void);
extern bool xhci_init(void);

extern bool usb_hid_init(void);
extern bool usb_hid_probe_device(usb_device_t* device);

extern bool usb_hub_init(void);
extern bool usb_hub_probe_device(usb_device_t* device);

bool usb_register_host_controller(usb_host_controller_t* hc) {
    if (!hc || !hc->init) {
        return false;
    }

    if (g_hc_count >= USB_MAX_HOST_CONTROLLERS) {
        return false;
    }

    g_usb_hcs[g_hc_count++] = hc;

    print("[USB] Host controller registered: ");
    switch (hc->type) {
        case USB_HC_TYPE_UHCI: print("UHCI"); break;
        case USB_HC_TYPE_OHCI: print("OHCI"); break;
        case USB_HC_TYPE_EHCI: print("EHCI"); break;
        case USB_HC_TYPE_XHCI: print("XHCI"); break;
        default: print("Unknown"); break;
    }
    print("\n");

    return true;
}

uint32_t usb_get_hc_count(void) {
    return g_hc_count;
}

usb_host_controller_t* usb_get_hc(uint32_t index) {
    if (index >= g_hc_count) {
        return 0;
    }
    return g_usb_hcs[index];
}

usb_device_t* usb_allocate_device(uint8 port, usb_speed_t speed) {
    if (g_device_count >= USB_MAX_DEVICES) {
        return 0;
    }

    usb_device_t* device = (usb_device_t*)kmalloc(sizeof(usb_device_t));
    if (!device) {
        return 0;
    }

    memory_set((uint8*)device, 0, sizeof(usb_device_t));

    device->port = port;
    device->speed = speed;
    device->address = 0;
    device->initialized = false;

    device->descriptor = (usb_device_descriptor_t*)kmalloc(sizeof(usb_device_descriptor_t));
    if (!device->descriptor) {
        kfree(device);
        return 0;
    }

    return device;
}

void usb_free_device(usb_device_t* device) {
    if (!device) {
        return;
    }

    if (device->descriptor) {
        kfree(device->descriptor);
    }

    kfree(device);
}

int usb_control_transfer(usb_device_t* device, usb_setup_packet_t* setup,
                        void* data, uint32 length) {
    if (!device || !setup) {
        return -1;
    }

    for (uint32 i = 0; i < g_hc_count; i++) {
        usb_host_controller_t* hc = g_usb_hcs[i];
        if (hc && hc->control_transfer) {
            int result = hc->control_transfer(hc, device, setup, data, length);
            if (result >= 0) {
                return result;
            }
        }
    }

    return -1;
}

int usb_get_descriptor(usb_device_t* device, uint8 type, uint8 index,
                     void* buffer, uint32 length) {
    if (!device || !buffer) {
        return -1;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_STANDARD | USB_RECIPIENT_DEVICE | USB_DIRECTION_IN;
    setup.bRequest = USB_SETUP_REQUEST_GET_DESCRIPTOR;
    setup.wValue = (type << 8) | index;
    setup.wIndex = 0;
    setup.wLength = length;

    return usb_control_transfer(device, &setup, buffer, length);
}

int usb_set_address(usb_device_t* device, uint8 address) {
    if (!device) {
        return -1;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_STANDARD | USB_RECIPIENT_DEVICE | USB_DIRECTION_OUT;
    setup.bRequest = USB_SETUP_REQUEST_SET_ADDRESS;
    setup.wValue = address;
    setup.wIndex = 0;
    setup.wLength = 0;

    int result = usb_control_transfer(device, &setup, 0, 0);
    if (result >= 0) {
        device->address = address;
    }

    return result;
}

int usb_set_configuration(usb_device_t* device, uint8 config) {
    if (!device) {
        return -1;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_STANDARD | USB_RECIPIENT_DEVICE | USB_DIRECTION_OUT;
    setup.bRequest = USB_SETUP_REQUEST_SET_CONFIGURATION;
    setup.wValue = config;
    setup.wIndex = 0;
    setup.wLength = 0;

    int result = usb_control_transfer(device, &setup, 0, 0);
    if (result >= 0) {
        device->initialized = true;
    }

    return result;
}

static void usb_enumerate_devices(void) {
    print("[USB] Enumerating devices...\n");

    for (uint32 i = 0; i < g_hc_count; i++) {
        usb_host_controller_t* hc = g_usb_hcs[i];
        if (!hc) {
            continue;
        }

        print("[USB] Scanning host controller ");
        print_dec(i);
        print("\n");
    }
}

static void usb_probe_class_drivers(usb_device_t* device) {
    if (!device || !device->descriptor) {
        return;
    }

    print("[USB] Probing class drivers for device\n");

    if (usb_hub_probe_device(device)) {
        print("[USB] Device claimed by USB Hub driver\n");
        return;
    }

    if (usb_hid_probe_device(device)) {
        print("[USB] Device claimed by USB HID driver\n");
        return;
    }

    print("[USB] Device not claimed by any class driver\n");
}

bool usb_init(void) {
    print("[USB] Initializing USB subsystem...\n");

    memory_set((uint8*)g_usb_hcs, 0, sizeof(g_usb_hcs));
    memory_set((uint8*)g_usb_devices, 0, sizeof(g_usb_devices));
    g_hc_count = 0;
    g_device_count = 0;

    uhci_init();
    ohci_init();
    ehci_init();
    xhci_init();

    usb_hid_init();
    usb_hub_init();

    usb_enumerate_devices();

    g_usb_initialized = true;

    print("[USB] USB subsystem initialized\n");

    /* Surface discovered host controllers to the unified driver model. */
    extern int drv_bus_enumerate_usb(void);
    drv_bus_enumerate_usb();

    return true;
}

void usb_shutdown(void) {
    print("[USB] Shutting down USB subsystem...\n");

    for (uint32 i = 0; i < g_device_count; i++) {
        if (g_usb_devices[i]) {
            usb_free_device(g_usb_devices[i]);
            g_usb_devices[i] = 0;
        }
    }

    for (uint32 i = 0; i < g_hc_count; i++) {
        if (g_usb_hcs[i] && g_usb_hcs[i]->shutdown) {
            g_usb_hcs[i]->shutdown(g_usb_hcs[i]);
        }
    }

    memory_set((uint8*)g_usb_hcs, 0, sizeof(g_usb_hcs));
    memory_set((uint8*)g_usb_devices, 0, sizeof(g_usb_devices));
    g_hc_count = 0;
    g_device_count = 0;
    g_usb_initialized = false;

    print("[USB] USB subsystem shut down\n");
}
