/**
 * USB HID (Human Interface Device) Driver for Fern
 *
 * Implements USB HID class support for mice, keyboards, and other input devices.
 * Based on USB HID 1.11 specification and OSDev documentation.
 */

#include "../include/usb/usb_hid.h"
#include "../include/usb/usb.h"
#include "../include/system.h"
#include "../include/memory.h"
#include "../include/debuglog.h"
#include "../include/input_event.h"
#include "../include/input_mux.h"
#include "../include/devfs.h"
#include "../include/hotkey.h"
#include "../include/timer.h"
#include <string.h>

// HID driver state
static usb_hid_device_t* hid_devices = NULL;
static usb_hid_keyboard_callback_t keyboard_callback = NULL;
static usb_hid_mouse_callback_t mouse_callback = NULL;
static bool hid_initialized = false;

// Default mouse bounds
static int32 mouse_screen_width = 1024;
static int32 mouse_screen_height = 768;

// USB HID scancode to ASCII lookup tables
static const char scancode_to_ascii_lower[128] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\n', 27, '\b', '\t',
    ' ', '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '/', '*', '-', '+',
    '\n', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.', '\\', 0, 0, '=',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char scancode_to_ascii_upper[128] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 27, '\b', '\t',
    ' ', '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '/', '*', '-', '+',
    '\n', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.', '|', 0, 0, '=',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint16 hid_usage_to_linux[256] = {
    [0x04] = 30,  [0x05] = 48,  [0x06] = 46,  [0x07] = 32,
    [0x08] = 18,  [0x09] = 33,  [0x0A] = 34,  [0x0B] = 35,
    [0x0C] = 23,  [0x0D] = 36,  [0x0E] = 37,  [0x0F] = 38,
    [0x10] = 50,  [0x11] = 49,  [0x12] = 24,  [0x13] = 25,
    [0x14] = 16,  [0x15] = 19,  [0x16] = 31,  [0x17] = 20,
    [0x18] = 22,  [0x19] = 47,  [0x1A] = 17,  [0x1B] = 45,
    [0x1C] = 21,  [0x1D] = 44,

    [0x1E] = 2,   [0x1F] = 3,   [0x20] = 4,   [0x21] = 5,
    [0x22] = 6,   [0x23] = 7,   [0x24] = 8,   [0x25] = 9,
    [0x26] = 10,  [0x27] = 11,

    [0x28] = 28,  [0x29] = 1,   [0x2A] = 14,  [0x2B] = 15,
    [0x2C] = 57,  [0x2D] = 12,  [0x2E] = 13,  [0x2F] = 26,
    [0x30] = 27,  [0x31] = 43,  [0x32] = 86,  [0x33] = 39,
    [0x34] = 40,  [0x35] = 41,  [0x36] = 51,  [0x37] = 52,
    [0x38] = 53,  [0x39] = 58,

    [0x3A] = 59,  [0x3B] = 60,  [0x3C] = 61,  [0x3D] = 62,
    [0x3E] = 63,  [0x3F] = 64,  [0x40] = 65,  [0x41] = 66,
    [0x42] = 67,  [0x43] = 68,  [0x44] = 87,  [0x45] = 88,

    [0x46] = 99,  [0x47] = 70,  [0x48] = 119, [0x49] = 110,
    [0x4A] = 102, [0x4B] = 104, [0x4C] = 111, [0x4D] = 107,
    [0x4E] = 109, [0x4F] = 106, [0x50] = 105, [0x51] = 108,
    [0x52] = 103,

    [0x53] = 69,  [0x54] = 98,  [0x55] = 55,  [0x56] = 74,
    [0x57] = 78,  [0x58] = 79,  [0x59] = 80,  [0x5A] = 81,
    [0x5B] = 75,  [0x5C] = 76,  [0x5D] = 77,  [0x5E] = 71,
    [0x5F] = 72,  [0x60] = 73,  [0x61] = 82,  [0x62] = 83,

    [0x63] = 125, [0x64] = 126,
};

static uint8 usb_linux_to_hotkey_scancode(uint16 linux_code) {
    switch (linux_code) {
        case 59: return 0x3B;
        case 60: return 0x3C;
        case 61: return 0x3D;
        case 62: return 0x3E;
        case 63: return 0x3F;
        case 64: return 0x40;
        case 65: return 0x41;
        case 66: return 0x42;
        case 67: return 0x43;
        case 68: return 0x44;
        case 87: return 0x57;
        case 88: return 0x58;
        default:
            return 0;
    }
}

static void usb_hid_dispatch_key_event(uint16 linux_code, bool pressed, uint8 modifiers) {
    if (linux_code == 0) {
        return;
    }

    input_event_t input_ev;
    uint32 ticks = timer_get_ticks();
    input_ev.tv_sec = ticks / 1000;
    input_ev.tv_usec = (ticks % 1000) * 1000;
    input_ev.type = EV_KEY;
    input_ev.code = linux_code;
    input_ev.value = pressed ? KEY_PRESS : KEY_RELEASE;

    if (devfs_is_initialized()) {
        devfs_kbd_queue_event(&input_ev);
    }
    if (input_mux_is_initialized()) {
        input_mux_dispatch_event(&input_ev);
    }

    input_ev.type = EV_SYN;
    input_ev.code = SYN_REPORT;
    input_ev.value = 0;
    if (devfs_is_initialized()) {
        devfs_kbd_queue_event(&input_ev);
    }
    if (input_mux_is_initialized()) {
        input_mux_dispatch_event(&input_ev);
    }

    uint16 hotkey_mods = 0;
    if (modifiers & (USB_HID_MOD_LEFT_CTRL | USB_HID_MOD_RIGHT_CTRL)) {
        hotkey_mods |= HOTKEY_MOD_CTRL;
    }
    if (modifiers & (USB_HID_MOD_LEFT_ALT | USB_HID_MOD_RIGHT_ALT)) {
        hotkey_mods |= HOTKEY_MOD_ALT;
    }
    if (modifiers & (USB_HID_MOD_LEFT_SHIFT | USB_HID_MOD_RIGHT_SHIFT)) {
        hotkey_mods |= HOTKEY_MOD_SHIFT;
    }
    if (modifiers & (USB_HID_MOD_LEFT_GUI | USB_HID_MOD_RIGHT_GUI)) {
        hotkey_mods |= HOTKEY_MOD_SUPER;
    }

    uint8 hotkey_scancode = usb_linux_to_hotkey_scancode(linux_code);
    if (hotkey_scancode) {
        hotkey_process_key_event(hotkey_scancode, pressed, hotkey_mods);
    }
}

static void usb_hid_emit_modifier_changes(uint8 prev, uint8 now) {
    struct mod_map {
        uint8 mask;
        uint16 code;
    } mods[] = {
        { USB_HID_MOD_LEFT_CTRL, 29 },
        { USB_HID_MOD_LEFT_SHIFT, 42 },
        { USB_HID_MOD_LEFT_ALT, 56 },
        { USB_HID_MOD_LEFT_GUI, 125 },
        { USB_HID_MOD_RIGHT_CTRL, 97 },
        { USB_HID_MOD_RIGHT_SHIFT, 54 },
        { USB_HID_MOD_RIGHT_ALT, 100 },
        { USB_HID_MOD_RIGHT_GUI, 126 },
    };

    for (uint32 i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        bool was_set = (prev & mods[i].mask) != 0;
        bool now_set = (now & mods[i].mask) != 0;
        if (was_set != now_set) {
            usb_hid_dispatch_key_event(mods[i].code, now_set, now);
        }
    }
}

// Forward declarations
static void usb_hid_process_keyboard_report(usb_hid_device_t* device, usb_hid_keyboard_report_t* report);
static void usb_hid_process_mouse_report(usb_hid_device_t* device, usb_hid_mouse_report_t* report);
static usb_hid_device_t* usb_hid_alloc_device(void);
static void usb_hid_free_device(usb_hid_device_t* device);
static usb_hid_device_type_t usb_hid_detect_type(usb_interface_t* interface);

// USB HID class driver
usb_class_driver_t usb_hid_driver = {
    .name = "usb_hid",
    .class_code = USB_CLASS_HID,
    .subclass = 0,
    .protocol = 0,
    .probe = usb_hid_probe,
    .disconnect = usb_hid_disconnect,
    .next = NULL
};

/**
 * Initialize USB HID subsystem
 */
bool usb_hid_init(void) {
    if (hid_initialized) {
        return true;
    }

    debug_print("USB HID: Initializing\n");

    hid_devices = NULL;
    keyboard_callback = NULL;
    mouse_callback = NULL;

    // Register with USB core
    if (!usb_register_class_driver(&usb_hid_driver)) {
        debug_print("USB HID: Failed to register class driver\n");
        return false;
    }

    hid_initialized = true;
    debug_print("USB HID: Initialized\n");

    return true;
}

/**
 * Shutdown USB HID subsystem
 */
void usb_hid_shutdown(void) {
    if (!hid_initialized) {
        return;
    }

    debug_print("USB HID: Shutting down\n");

    // Free all HID devices
    usb_hid_device_t* device = hid_devices;
    while (device) {
        usb_hid_device_t* next = device->next;
        usb_hid_free_device(device);
        device = next;
    }

    hid_devices = NULL;
    keyboard_callback = NULL;
    mouse_callback = NULL;

    usb_unregister_class_driver(&usb_hid_driver);

    hid_initialized = false;
    debug_print("USB HID: Shutdown complete\n");
}

/**
 * Allocate a HID device structure
 */
static usb_hid_device_t* usb_hid_alloc_device(void) {
    usb_hid_device_t* device = (usb_hid_device_t*)kmalloc(sizeof(usb_hid_device_t));
    if (!device) {
        return NULL;
    }

    memset(device, 0, sizeof(usb_hid_device_t));
    return device;
}

/**
 * Free a HID device structure
 */
static void usb_hid_free_device(usb_hid_device_t* device) {
    if (!device) return;

    if (device->report_descriptor) {
        kfree(device->report_descriptor);
    }

    kfree(device);
}

/**
 * Detect HID device type from interface descriptor
 */
static usb_hid_device_type_t usb_hid_detect_type(usb_interface_t* interface) {
    if (!interface) {
        return USB_HID_TYPE_UNKNOWN;
    }

    // Check for boot interface subclass
    if (interface->subclass == USB_HID_SUBCLASS_BOOT) {
        switch (interface->protocol) {
            case USB_HID_PROTOCOL_KEYBOARD:
                return USB_HID_TYPE_KEYBOARD;
            case USB_HID_PROTOCOL_MOUSE:
                return USB_HID_TYPE_MOUSE;
        }
    }

    // Default to unknown for non-boot devices
    return USB_HID_TYPE_UNKNOWN;
}

/**
 * Probe a USB device for HID support
 */
bool usb_hid_probe(usb_device_t* device, usb_interface_t* interface) {
    if (!device || !interface) {
        return false;
    }

    // Check if this is an HID interface
    if (interface->class_code != USB_CLASS_HID) {
        return false;
    }

    debug_print("USB HID: Probing device VID=0x");
    debug_print_hex(device->vendor_id);
    debug_print(" PID=0x");
    debug_print_hex(device->product_id);
    debug_print("\n");

    // Detect device type
    usb_hid_device_type_t type = usb_hid_detect_type(interface);

    if (type == USB_HID_TYPE_UNKNOWN) {
        debug_print("USB HID: Unknown HID device type\n");
        // We could still try to handle it, but for now skip
        return false;
    }

    // Allocate HID device
    usb_hid_device_t* hid_device = usb_hid_alloc_device();
    if (!hid_device) {
        debug_print("USB HID: Failed to allocate device\n");
        return false;
    }

    hid_device->usb_device = device;
    hid_device->interface = interface;
    hid_device->type = type;
    hid_device->protocol = USB_HID_PROTOCOL_BOOT;

    // Find interrupt IN endpoint
    for (uint8 i = 0; i < interface->num_endpoints; i++) {
        usb_endpoint_t* ep = &interface->endpoints[i];
        if (ep->type == USB_TRANSFER_INTERRUPT) {
            if (ep->direction == USB_DIR_IN) {
                hid_device->interrupt_in = ep;
                hid_device->poll_interval = ep->interval;
            } else {
                hid_device->interrupt_out = ep;
            }
        }
    }

    if (!hid_device->interrupt_in) {
        debug_print("USB HID: No interrupt IN endpoint found\n");
        usb_hid_free_device(hid_device);
        return false;
    }

    // Set boot protocol for keyboard/mouse
    if (interface->subclass == USB_HID_SUBCLASS_BOOT) {
        if (!usb_hid_set_protocol(hid_device, USB_HID_PROTOCOL_BOOT)) {
            debug_print("USB HID: Warning - Failed to set boot protocol\n");
        }
    }

    // Set idle rate to 0 (report only on change)
    usb_hid_set_idle(hid_device, 0, 0);

    // Initialize device-specific state
    switch (type) {
        case USB_HID_TYPE_KEYBOARD:
            memset(&hid_device->state.keyboard, 0, sizeof(usb_hid_keyboard_state_t));
            debug_print("USB HID: Keyboard detected\n");
            break;

        case USB_HID_TYPE_MOUSE:
            memset(&hid_device->state.mouse, 0, sizeof(usb_hid_mouse_state_t));
            hid_device->state.mouse.screen_width = mouse_screen_width;
            hid_device->state.mouse.screen_height = mouse_screen_height;
            // Start mouse in center of screen
            hid_device->state.mouse.x = mouse_screen_width / 2;
            hid_device->state.mouse.y = mouse_screen_height / 2;
            debug_print("USB HID: Mouse detected\n");
            break;

        default:
            break;
    }

    // Link to device list
    hid_device->next = hid_devices;
    hid_devices = hid_device;

    // Store driver data in interface
    interface->driver_data = hid_device;

    debug_print("USB HID: Device probed successfully\n");
    return true;
}

/**
 * Disconnect a HID device
 */
void usb_hid_disconnect(usb_device_t* device, usb_interface_t* interface) {
    if (!device || !interface) return;

    usb_hid_device_t* hid_device = (usb_hid_device_t*)interface->driver_data;
    if (!hid_device) return;

    debug_print("USB HID: Disconnecting device\n");

    if (hid_device->type == USB_HID_TYPE_KEYBOARD) {
        usb_hid_keyboard_state_t* state = &hid_device->state.keyboard;
        for (uint16 key = 0; key < 256; key++) {
            if (state->keys_pressed[key]) {
                usb_hid_dispatch_key_event(hid_usage_to_linux[key], false, state->modifiers);
                state->keys_pressed[key] = false;
            }
        }
        if (state->modifiers) {
            usb_hid_emit_modifier_changes(state->modifiers, 0);
            state->modifiers = 0;
        }
    }

    // Remove from device list
    if (hid_devices == hid_device) {
        hid_devices = hid_device->next;
    } else {
        usb_hid_device_t* prev = hid_devices;
        while (prev && prev->next != hid_device) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = hid_device->next;
        }
    }

    interface->driver_data = NULL;
    usb_hid_free_device(hid_device);

    debug_print("USB HID: Device disconnected\n");
}

/**
 * Set HID protocol (boot or report)
 */
bool usb_hid_set_protocol(usb_hid_device_t* device, uint8 protocol) {
    if (!device || !device->usb_device) return false;

    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_HID_SET_PROTOCOL,
        protocol,
        device->interface->number,
        NULL, 0);

    if (result >= 0) {
        device->protocol = protocol;
        return true;
    }

    return false;
}

/**
 * Get HID protocol
 */
bool usb_hid_get_protocol(usb_hid_device_t* device, uint8* protocol) {
    if (!device || !device->usb_device || !protocol) return false;

    uint8 buf;
    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_HID_GET_PROTOCOL,
        0,
        device->interface->number,
        &buf, 1);

    if (result >= 0) {
        *protocol = buf;
        return true;
    }

    return false;
}

/**
 * Set idle rate
 */
bool usb_hid_set_idle(usb_hid_device_t* device, uint8 duration, uint8 report_id) {
    if (!device || !device->usb_device) return false;

    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_HID_SET_IDLE,
        (duration << 8) | report_id,
        device->interface->number,
        NULL, 0);

    return result >= 0;
}

/**
 * Get a report
 */
bool usb_hid_get_report(usb_hid_device_t* device, uint8 type, uint8 id,
                        void* buffer, uint16 length) {
    if (!device || !device->usb_device || !buffer) return false;

    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_HID_GET_REPORT,
        (type << 8) | id,
        device->interface->number,
        buffer, length);

    return result >= 0;
}

/**
 * Set a report
 */
bool usb_hid_set_report(usb_hid_device_t* device, uint8 type, uint8 id,
                        void* data, uint16 length) {
    if (!device || !device->usb_device || !data) return false;

    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_HID_SET_REPORT,
        (type << 8) | id,
        device->interface->number,
        data, length);

    return result >= 0;
}

/**
 * Process a keyboard report
 */
static void usb_hid_process_keyboard_report(usb_hid_device_t* device, usb_hid_keyboard_report_t* report) {
    if (!device || !report) return;

    usb_hid_keyboard_state_t* state = &device->state.keyboard;

    // Update modifiers
    uint8 prev_modifiers = state->modifiers;
    state->modifiers = report->modifiers;

    if (prev_modifiers != state->modifiers) {
        usb_hid_emit_modifier_changes(prev_modifiers, state->modifiers);
    }

    // Find released keys
    for (int i = 0; i < 6; i++) {
        uint8 key = state->last_keycodes[i];
        if (key == 0) continue;

        // Check if key is still pressed
        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (report->keycodes[j] == key) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed) {
            state->keys_pressed[key] = false;

            if (keyboard_callback) {
                usb_hid_keyboard_event_t event;
                event.scancode = key;
                event.pressed = false;
                event.modifiers = state->modifiers;
                event.ascii = 0;
                keyboard_callback(&event);
            }

            usb_hid_dispatch_key_event(hid_usage_to_linux[key], false, state->modifiers);
        }
    }

    // Find newly pressed keys
    for (int i = 0; i < 6; i++) {
        uint8 key = report->keycodes[i];
        if (key == 0) continue;

        // Check if key was already pressed
        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (state->last_keycodes[j] == key) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed) {
            state->keys_pressed[key] = true;

            // Handle LED keys
            if (key == 0x39) {  // Caps Lock
                state->caps_lock = !state->caps_lock;
                uint8 leds = (state->num_lock ? USB_HID_LED_NUM_LOCK : 0) |
                             (state->caps_lock ? USB_HID_LED_CAPS_LOCK : 0) |
                             (state->scroll_lock ? USB_HID_LED_SCROLL_LOCK : 0);
                usb_hid_keyboard_set_leds(device, leds);
            } else if (key == 0x53) {  // Num Lock
                state->num_lock = !state->num_lock;
                uint8 leds = (state->num_lock ? USB_HID_LED_NUM_LOCK : 0) |
                             (state->caps_lock ? USB_HID_LED_CAPS_LOCK : 0) |
                             (state->scroll_lock ? USB_HID_LED_SCROLL_LOCK : 0);
                usb_hid_keyboard_set_leds(device, leds);
            } else if (key == 0x47) {  // Scroll Lock
                state->scroll_lock = !state->scroll_lock;
                uint8 leds = (state->num_lock ? USB_HID_LED_NUM_LOCK : 0) |
                             (state->caps_lock ? USB_HID_LED_CAPS_LOCK : 0) |
                             (state->scroll_lock ? USB_HID_LED_SCROLL_LOCK : 0);
                usb_hid_keyboard_set_leds(device, leds);
            }

            if (keyboard_callback) {
                usb_hid_keyboard_event_t event;
                event.scancode = key;
                event.pressed = true;
                event.modifiers = state->modifiers;

                // Convert to ASCII
                bool shift = (state->modifiers & (USB_HID_MOD_LEFT_SHIFT | USB_HID_MOD_RIGHT_SHIFT)) != 0;
                event.ascii = usb_hid_scancode_to_ascii(key, shift, state->caps_lock);

                keyboard_callback(&event);
            }

            usb_hid_dispatch_key_event(hid_usage_to_linux[key], true, state->modifiers);
        }
    }

    // Save current keycodes for next comparison
    memcpy(state->last_keycodes, report->keycodes, 6);
}

/**
 * Process a mouse report
 */
static void usb_hid_process_mouse_report(usb_hid_device_t* device, usb_hid_mouse_report_t* report) {
    if (!device || !report) return;

    usb_hid_mouse_state_t* state = &device->state.mouse;

    // Update position
    int32 new_x = state->x + report->x;
    int32 new_y = state->y + report->y;

    // Clamp to screen bounds
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x >= state->screen_width) new_x = state->screen_width - 1;
    if (new_y >= state->screen_height) new_y = state->screen_height - 1;

    // Update state
    bool left_changed = state->left_button != ((report->buttons & USB_HID_MOUSE_BUTTON_LEFT) != 0);
    bool right_changed = state->right_button != ((report->buttons & USB_HID_MOUSE_BUTTON_RIGHT) != 0);
    bool middle_changed = state->middle_button != ((report->buttons & USB_HID_MOUSE_BUTTON_MIDDLE) != 0);

    state->left_button = (report->buttons & USB_HID_MOUSE_BUTTON_LEFT) != 0;
    state->right_button = (report->buttons & USB_HID_MOUSE_BUTTON_RIGHT) != 0;
    state->middle_button = (report->buttons & USB_HID_MOUSE_BUTTON_MIDDLE) != 0;

    int32 dx = new_x - state->x;
    int32 dy = new_y - state->y;
    int32 dwheel = report->wheel;

    state->x = new_x;
    state->y = new_y;
    state->wheel += dwheel;

    // Notify callback
    if (mouse_callback && (dx != 0 || dy != 0 || dwheel != 0 ||
                           left_changed || right_changed || middle_changed)) {
        usb_hid_mouse_event_t event;
        event.dx = dx;
        event.dy = dy;
        event.dwheel = dwheel;
        event.x = state->x;
        event.y = state->y;
        event.left_button = state->left_button;
        event.right_button = state->right_button;
        event.middle_button = state->middle_button;
        event.button4 = state->button4;
        event.button5 = state->button5;
        mouse_callback(&event);
    }

    input_event_t input_ev;
    uint32 ticks = timer_get_ticks();
    input_ev.tv_sec = ticks / 1000;
    input_ev.tv_usec = (ticks % 1000) * 1000;

    if (dx || dy) {
        input_ev.type = EV_REL;
        input_ev.code = REL_X;
        input_ev.value = dx;
        if (devfs_is_initialized()) {
            devfs_mouse_queue_event(&input_ev);
        }
        if (input_mux_is_initialized()) {
            input_mux_dispatch_event(&input_ev);
        }

        input_ev.code = REL_Y;
        input_ev.value = dy;
        if (devfs_is_initialized()) {
            devfs_mouse_queue_event(&input_ev);
        }
        if (input_mux_is_initialized()) {
            input_mux_dispatch_event(&input_ev);
        }
    }

    if (left_changed) {
        input_ev.type = EV_KEY;
        input_ev.code = BTN_LEFT;
        input_ev.value = state->left_button ? KEY_PRESS : KEY_RELEASE;
        if (devfs_is_initialized()) {
            devfs_mouse_queue_event(&input_ev);
        }
        if (input_mux_is_initialized()) {
            input_mux_dispatch_event(&input_ev);
        }
    }
    if (right_changed) {
        input_ev.type = EV_KEY;
        input_ev.code = BTN_RIGHT;
        input_ev.value = state->right_button ? KEY_PRESS : KEY_RELEASE;
        if (devfs_is_initialized()) {
            devfs_mouse_queue_event(&input_ev);
        }
        if (input_mux_is_initialized()) {
            input_mux_dispatch_event(&input_ev);
        }
    }
    if (middle_changed) {
        input_ev.type = EV_KEY;
        input_ev.code = BTN_MIDDLE;
        input_ev.value = state->middle_button ? KEY_PRESS : KEY_RELEASE;
        if (devfs_is_initialized()) {
            devfs_mouse_queue_event(&input_ev);
        }
        if (input_mux_is_initialized()) {
            input_mux_dispatch_event(&input_ev);
        }
    }

    input_ev.type = EV_SYN;
    input_ev.code = SYN_REPORT;
    input_ev.value = 0;
    if (devfs_is_initialized()) {
        devfs_mouse_queue_event(&input_ev);
    }
    if (input_mux_is_initialized()) {
        input_mux_dispatch_event(&input_ev);
    }
}

/**
 * Poll all HID devices for input
 */
void usb_hid_poll(void) {
    if (!hid_initialized) return;

    usb_hid_device_t* device = hid_devices;
    while (device) {
        if (!device->usb_device || !device->interrupt_in) {
            device = device->next;
            continue;
        }

        switch (device->type) {
            case USB_HID_TYPE_KEYBOARD: {
                usb_hid_keyboard_report_t report;
                int result = device->usb_device->controller->ops->interrupt_transfer(
                    device->usb_device->controller,
                    device->usb_device,
                    device->interrupt_in,
                    &report,
                    sizeof(report));

                if (result > 0) {
                    usb_hid_process_keyboard_report(device, &report);
                }
                break;
            }

            case USB_HID_TYPE_MOUSE: {
                usb_hid_mouse_report_t report;
                int result = device->usb_device->controller->ops->interrupt_transfer(
                    device->usb_device->controller,
                    device->usb_device,
                    device->interrupt_in,
                    &report,
                    sizeof(report));

                if (result > 0) {
                    usb_hid_process_mouse_report(device, &report);
                }
                break;
            }

            default:
                break;
        }

        device = device->next;
    }
}

/**
 * Register keyboard event callback
 */
void usb_hid_keyboard_register_callback(usb_hid_keyboard_callback_t callback) {
    keyboard_callback = callback;
}

/**
 * Register mouse event callback
 */
void usb_hid_mouse_register_callback(usb_hid_mouse_callback_t callback) {
    mouse_callback = callback;
}

/**
 * Set keyboard LEDs
 */
bool usb_hid_keyboard_set_leds(usb_hid_device_t* device, uint8 leds) {
    if (!device || device->type != USB_HID_TYPE_KEYBOARD) return false;

    // Send LED report (report type 2 = output, report ID = 0)
    return usb_hid_set_report(device, USB_HID_REPORT_OUTPUT, 0, &leds, 1);
}

/**
 * Get keyboard state
 */
usb_hid_keyboard_state_t* usb_hid_keyboard_get_state(usb_hid_device_t* device) {
    if (!device || device->type != USB_HID_TYPE_KEYBOARD) return NULL;
    return &device->state.keyboard;
}

/**
 * Get mouse state
 */
usb_hid_mouse_state_t* usb_hid_mouse_get_state(usb_hid_device_t* device) {
    if (!device || device->type != USB_HID_TYPE_MOUSE) return NULL;
    return &device->state.mouse;
}

/**
 * Set mouse screen bounds
 */
void usb_hid_mouse_set_bounds(int32 width, int32 height) {
    mouse_screen_width = width;
    mouse_screen_height = height;

    // Update all connected mice
    usb_hid_device_t* device = hid_devices;
    while (device) {
        if (device->type == USB_HID_TYPE_MOUSE) {
            device->state.mouse.screen_width = width;
            device->state.mouse.screen_height = height;

            // Clamp current position
            if (device->state.mouse.x >= width) {
                device->state.mouse.x = width - 1;
            }
            if (device->state.mouse.y >= height) {
                device->state.mouse.y = height - 1;
            }
        }
        device = device->next;
    }
}

/**
 * Set mouse position
 */
void usb_hid_mouse_set_position(int32 x, int32 y) {
    // Set position on first mouse device
    usb_hid_device_t* device = hid_devices;
    while (device) {
        if (device->type == USB_HID_TYPE_MOUSE) {
            device->state.mouse.x = x;
            device->state.mouse.y = y;

            // Clamp to bounds
            if (device->state.mouse.x < 0) device->state.mouse.x = 0;
            if (device->state.mouse.y < 0) device->state.mouse.y = 0;
            if (device->state.mouse.x >= device->state.mouse.screen_width) {
                device->state.mouse.x = device->state.mouse.screen_width - 1;
            }
            if (device->state.mouse.y >= device->state.mouse.screen_height) {
                device->state.mouse.y = device->state.mouse.screen_height - 1;
            }
            break;
        }
        device = device->next;
    }
}

/**
 * Convert USB HID scancode to ASCII character
 */
char usb_hid_scancode_to_ascii(uint8 scancode, bool shift, bool caps_lock) {
    if (scancode >= 128) {
        return 0;
    }

    // Determine if we need uppercase
    bool uppercase = shift;

    // Caps lock only affects letters (scancodes 4-29)
    if (caps_lock && scancode >= 4 && scancode <= 29) {
        uppercase = !uppercase;
    }

    if (uppercase) {
        return scancode_to_ascii_upper[scancode];
    } else {
        return scancode_to_ascii_lower[scancode];
    }
}
