#include "include/usb.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"

#define USB_HUB_DESCRIPTOR_TYPE 0x29
#define USB_HUB_CLASS_REQUEST_GET_STATUS 0x00
#define USB_HUB_CLASS_REQUEST_CLEAR_FEATURE 0x01
#define USB_HUB_CLASS_REQUEST_SET_FEATURE 0x03
#define USB_HUB_CLASS_REQUEST_GET_DESCRIPTOR 0x06
#define USB_HUB_CLASS_REQUEST_SET_DESCRIPTOR 0x07

#define USB_HUB_FEATURE_PORT_CONNECTION 0x00
#define USB_HUB_FEATURE_PORT_ENABLE 0x01
#define USB_HUB_FEATURE_PORT_SUSPEND 0x02
#define USB_HUB_FEATURE_PORT_OVER_CURRENT 0x03
#define USB_HUB_FEATURE_PORT_RESET 0x04
#define USB_HUB_FEATURE_PORT_POWER 0x08
#define USB_HUB_FEATURE_PORT_LOW_SPEED 0x09
#define USB_HUB_FEATURE_C_PORT_CONNECTION 0x10
#define USB_HUB_FEATURE_C_PORT_ENABLE 0x11
#define USB_HUB_FEATURE_C_PORT_SUSPEND 0x12
#define USB_HUB_FEATURE_C_PORT_OVER_CURRENT 0x13
#define USB_HUB_FEATURE_C_PORT_RESET 0x14

typedef struct {
    uint8 bLength;
    uint8 bDescriptorType;
    uint8 bNbrPorts;
    uint16 wHubCharacteristics;
    uint8 bPwrOn2PwrGood;
    uint8 bHubContrCurrent;
    uint8 device_removable;
    uint8 port_pwr_ctrl_mask;
} __attribute__((packed)) usb_hub_descriptor_t;

typedef struct {
    uint16 wPortStatus;
    uint16 wPortChange;
} __attribute__((packed)) usb_hub_port_status_t;

#define USB_HUB_PORT_STATUS_CONNECTION 0x0001
#define USB_HUB_PORT_STATUS_ENABLE 0x0002
#define USB_HUB_PORT_STATUS_SUSPEND 0x0004
#define USB_HUB_PORT_STATUS_OVER_CURRENT 0x0008
#define USB_HUB_PORT_STATUS_RESET 0x0010
#define USB_HUB_PORT_STATUS_POWER 0x0100
#define USB_HUB_PORT_STATUS_LOW_SPEED 0x0200
#define USB_HUB_PORT_STATUS_HIGH_SPEED 0x0400
#define USB_HUB_PORT_STATUS_TEST 0x0800
#define USB_HUB_PORT_STATUS_INDICATOR 0x1000

#define USB_HUB_PORT_CHANGE_CONNECTION 0x0001
#define USB_HUB_PORT_CHANGE_ENABLE 0x0002
#define USB_HUB_PORT_CHANGE_SUSPEND 0x0004
#define USB_HUB_PORT_CHANGE_OVER_CURRENT 0x0008
#define USB_HUB_PORT_CHANGE_RESET 0x0010

typedef struct {
    usb_device_t* device;
    usb_hub_descriptor_t descriptor;
    usb_hub_port_status_t* port_status;
    uint8 num_ports;
    uint8 interface;
    uint8 status_endpoint;
    uint8 interval;
    bool initialized;
} usb_hub_t;

static usb_hub_t g_usb_hubs[16];
static uint32 g_hub_count = 0;

static bool usb_hub_get_descriptor(usb_hub_t* hub) {
    if (!hub || !hub->device) {
        return false;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_DEVICE | USB_DIRECTION_IN;
    setup.bRequest = USB_HUB_CLASS_REQUEST_GET_DESCRIPTOR;
    setup.wValue = (USB_HUB_DESCRIPTOR_TYPE << 8) | 0x00;
    setup.wIndex = 0;
    setup.wLength = sizeof(usb_hub_descriptor_t);

    int result = usb_control_transfer(hub->device, &setup, &hub->descriptor, sizeof(usb_hub_descriptor_t));
    return result > 0 && hub->descriptor.bDescriptorType == USB_HUB_DESCRIPTOR_TYPE;
}

static bool usb_hub_get_port_status(usb_hub_t* hub, uint8 port, usb_hub_port_status_t* status) {
    if (!hub || !hub->device || !status) {
        return false;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_OTHER | USB_DIRECTION_IN;
    setup.bRequest = USB_HUB_CLASS_REQUEST_GET_STATUS;
    setup.wValue = 0;
    setup.wIndex = port;
    setup.wLength = sizeof(usb_hub_port_status_t);

    int result = usb_control_transfer(hub->device, &setup, status, sizeof(usb_hub_port_status_t));
    return result > 0;
}

static bool usb_hub_set_port_feature(usb_hub_t* hub, uint8 port, uint16 feature) {
    if (!hub || !hub->device) {
        return false;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_OTHER | USB_DIRECTION_OUT;
    setup.bRequest = USB_HUB_CLASS_REQUEST_SET_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;

    int result = usb_control_transfer(hub->device, &setup, 0, 0);
    return result >= 0;
}

static bool usb_hub_clear_port_feature(usb_hub_t* hub, uint8 port, uint16 feature) {
    if (!hub || !hub->device) {
        return false;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_OTHER | USB_DIRECTION_OUT;
    setup.bRequest = USB_HUB_CLASS_REQUEST_CLEAR_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;

    int result = usb_control_transfer(hub->device, &setup, 0, 0);
    return result >= 0;
}

static bool usb_hub_reset_port(usb_hub_t* hub, uint8 port) {
    if (!usb_hub_set_port_feature(hub, port, USB_HUB_FEATURE_PORT_RESET)) {
        return false;
    }

    timer_sleep_ms(50);

    usb_hub_port_status_t status;
    if (!usb_hub_get_port_status(hub, port, &status)) {
        return false;
    }

    return (status.wPortStatus & USB_HUB_PORT_STATUS_ENABLE) != 0;
}

static bool usb_hub_power_on_ports(usb_hub_t* hub) {
    if (!hub) {
        return false;
    }

    for (uint8 i = 1; i <= hub->num_ports; i++) {
        if (!usb_hub_set_port_feature(hub, i, USB_HUB_FEATURE_PORT_POWER)) {
            return false;
        }
    }

    timer_sleep_ms(hub->descriptor.bPwrOn2PwrGood * 2);
    return true;
}

bool usb_hub_probe_device(usb_device_t* device) {
    if (!device || !device->descriptor) {
        return false;
    }

    if (g_hub_count >= 16) {
        return false;
    }

    usb_hub_t* hub = &g_usb_hubs[g_hub_count];
    memory_set((uint8*)hub, 0, sizeof(usb_hub_t));
    
    hub->device = device;
    
    if (!usb_hub_get_descriptor(hub)) {
        return false;
    }

    hub->num_ports = hub->descriptor.bNbrPorts;
    hub->interface = 0;

    hub->port_status = (usb_hub_port_status_t*)kmalloc(sizeof(usb_hub_port_status_t) * hub->num_ports);
    if (!hub->port_status) {
        return false;
    }
    memory_set((uint8*)hub->port_status, 0, sizeof(usb_hub_port_status_t) * hub->num_ports);

    if (!usb_hub_power_on_ports(hub)) {
        kfree(hub->port_status);
        hub->port_status = 0;
        return false;
    }

    hub->initialized = true;

    print("[HUB] Hub initialized with ");
    print_dec(hub->num_ports);
    print(" ports\n");

    for (uint8 i = 1; i <= hub->num_ports; i++) {
        usb_hub_port_status_t status;
        if (usb_hub_get_port_status(hub, i, &status)) {
            if (status.wPortStatus & USB_HUB_PORT_STATUS_CONNECTION) {
                print("[HUB] Port ");
                print_dec(i);
                print(": Device connected\n");

                if (usb_hub_reset_port(hub, i)) {
                    print("[HUB] Port ");
                    print_dec(i);
                    print(": Device enabled\n");
                }
            }
        }
    }

    g_hub_count++;
    return true;
}

bool usb_hub_init(void) {
    print("[HUB] Initializing USB Hub driver...\n");

    memory_set((uint8*)g_usb_hubs, 0, sizeof(g_usb_hubs));
    g_hub_count = 0;

    print("[HUB] USB Hub driver initialized\n");
    return true;
}

void usb_hub_shutdown(void) {
    print("[HUB] Shutting down USB Hub driver...\n");

    for (uint32 i = 0; i < g_hub_count; i++) {
        if (g_usb_hubs[i].port_status) {
            kfree(g_usb_hubs[i].port_status);
            g_usb_hubs[i].port_status = 0;
        }
        g_usb_hubs[i].initialized = false;
    }

    g_hub_count = 0;
}
