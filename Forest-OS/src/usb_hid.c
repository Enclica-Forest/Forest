#include "include/usb.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"

#define USB_HID_BOOT_PROTOCOL_KEYBOARD 0
#define USB_HID_BOOT_PROTOCOL_MOUSE 1
#define USB_HID_REPORT_PROTOCOL 1
#define USB_HID_BOOT_PROTOCOL 0

#define USB_HID_REQUEST_SET_PROTOCOL 0x0B
#define USB_HID_REQUEST_GET_REPORT 0x01
#define USB_HID_REQUEST_SET_REPORT 0x09

#define USB_HID_BOOT_KEYBOARD_REPORT_SIZE 8
#define USB_HID_BOOT_MOUSE_REPORT_SIZE 3

typedef struct {
    uint8 modifiers;
    uint8 reserved;
    uint8 keycodes[6];
} __attribute__((packed)) usb_hid_keyboard_report_t;

typedef struct {
    uint8 buttons;
    int8 x;
    int8 y;
} __attribute__((packed)) usb_hid_mouse_report_t;

typedef struct {
    usb_device_t* device;
    uint8 interface;
    uint8 report_protocol;
    uint8 boot_protocol;
    uint8 endpoint_in;
    uint8 endpoint_out;
    uint8 interval;
    uint8 leds;
    bool initialized;
} usb_hid_device_t;

static usb_hid_device_t g_hid_devices[32];
static uint32 g_hid_count = 0;

static const char* usb_hid_scancode_to_ascii(uint8 scancode, uint8 modifiers) {
    static const char* keymap_no_shift[] = {
        "", "", "", "", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
        "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "1", "2", "3", "4",
        "5", "6", "7", "8", "9", "0", "\n", "\x1b", "\b", "\t", " ", "-", "=", "[", "]", "\\",
        "#", ";", "'", "`", ",", ".", "/", ""
    };
    
    static const char* keymap_shift[] = {
        "", "", "", "", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "!", "@", "#", "$",
        "%", "^", "&", "*", "(", ")", "\n", "\x1b", "\b", "\t", " ", "_", "+", "{", "}", "|",
        "~", ":", "\"", "~", "<", ">", "?", ""
    };
    
    if (modifiers & 0x22) {
        if (scancode < sizeof(keymap_shift) / sizeof(char*)) {
            return keymap_shift[scancode];
        }
    } else {
        if (scancode < sizeof(keymap_no_shift) / sizeof(char*)) {
            return keymap_no_shift[scancode];
        }
    }
    
    return "";
}

static bool usb_hid_set_protocol(usb_hid_device_t* hid, uint8 protocol) {
    if (!hid || !hid->device) {
        return false;
    }

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE | USB_DIRECTION_OUT;
    setup.bRequest = USB_HID_REQUEST_SET_PROTOCOL;
    setup.wValue = protocol;
    setup.wIndex = hid->interface;
    setup.wLength = 0;

    int result = usb_control_transfer(hid->device, &setup, 0, 0);
    return result >= 0;
}

static bool usb_hid_set_leds(usb_hid_device_t* hid, uint8 leds) {
    if (!hid || !hid->device) {
        return false;
    }

    hid->leds = leds;

    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE | USB_DIRECTION_OUT;
    setup.bRequest = USB_HID_REQUEST_SET_REPORT;
    setup.wValue = (0x02 << 8) | 0x00;
    setup.wIndex = hid->interface;
    setup.wLength = 1;

    uint8 led_data = leds;
    int result = usb_control_transfer(hid->device, &setup, &led_data, 1);
    return result >= 0;
}

static int usb_hid_keyboard_poll(usb_hid_device_t* hid) {
    if (!hid || !hid->device) {
        return -1;
    }

    usb_hid_keyboard_report_t report;
    memory_set((uint8*)&report, 0, sizeof(report));

    usb_endpoint_t* endpoint = &hid->device->endpoints[hid->endpoint_in];
    if (!endpoint) {
        return -1;
    }

    usb_host_controller_t* hc = 0;
    int result = hc->interrupt_transfer(hc, endpoint, &report, sizeof(report), USB_DIRECTION_IN);
    
    if (result > 0) {
        if (report.keycodes[0] != 0) {
            const char* key = usb_hid_scancode_to_ascii(report.keycodes[0], report.modifiers);
            if (key[0] != 0) {
                print(key);
            }
        }
    }

    return result;
}

static int usb_hid_mouse_poll(usb_hid_device_t* hid) {
    if (!hid || !hid->device) {
        return -1;
    }

    usb_hid_mouse_report_t report;
    memory_set((uint8*)&report, 0, sizeof(report));

    usb_endpoint_t* endpoint = &hid->device->endpoints[hid->endpoint_in];
    if (!endpoint) {
        return -1;
    }

    usb_host_controller_t* hc = 0;
    int result = hc->interrupt_transfer(hc, endpoint, &report, sizeof(report), USB_DIRECTION_IN);
    
    if (result > 0) {
        if (report.buttons & 0x01) {
            print("[HID] Left button\n");
        }
        if (report.buttons & 0x02) {
            print("[HID] Right button\n");
        }
        if (report.buttons & 0x04) {
            print("[HID] Middle button\n");
        }
        
        if (report.x != 0 || report.y != 0) {
            print("[HID] Mouse movement: X=");
            print_dec(report.x);
            print(" Y=");
            print_dec(report.y);
            print("\n");
        }
    }

    return result;
}

bool usb_hid_probe_device(usb_device_t* device) {
    if (!device || !device->descriptor) {
        return false;
    }

    print("[HID] Probing USB device: VID=");
    print_hex(device->vendor_id);
    print(" PID=");
    print_hex(device->product_id);
    print("\n");

    return true;
}

bool usb_hid_init(void) {
    print("[HID] Initializing USB HID driver...\n");

    memory_set((uint8*)g_hid_devices, 0, sizeof(g_hid_devices));
    g_hid_count = 0;

    print("[HID] USB HID driver initialized\n");
    return true;
}

void usb_hid_shutdown(void) {
    print("[HID] Shutting down USB HID driver...\n");

    for (uint32 i = 0; i < g_hid_count; i++) {
        if (g_hid_devices[i].initialized) {
            g_hid_devices[i].initialized = false;
        }
    }

    g_hid_count = 0;
}
