/**
 * USB HID (Human Interface Device) Driver for Fern
 *
 * Implements USB HID class support for mice, keyboards, and other input devices.
 * Based on USB HID 1.11 specification and OSDev documentation.
 */

#ifndef USB_HID_H
#define USB_HID_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// HID Class-Specific Requests
#define USB_HID_GET_REPORT          0x01
#define USB_HID_GET_IDLE            0x02
#define USB_HID_GET_PROTOCOL        0x03
#define USB_HID_SET_REPORT          0x09
#define USB_HID_SET_IDLE            0x0A
#define USB_HID_SET_PROTOCOL        0x0B

// HID Report Types
#define USB_HID_REPORT_INPUT        0x01
#define USB_HID_REPORT_OUTPUT       0x02
#define USB_HID_REPORT_FEATURE      0x03

// HID Protocols
#define USB_HID_PROTOCOL_BOOT       0x00
#define USB_HID_PROTOCOL_REPORT     0x01

// HID Country Codes
#define USB_HID_COUNTRY_NOT_SUPPORTED   0
#define USB_HID_COUNTRY_US              33

// Boot Protocol Keyboard Modifier Keys
#define USB_HID_MOD_LEFT_CTRL       (1 << 0)
#define USB_HID_MOD_LEFT_SHIFT      (1 << 1)
#define USB_HID_MOD_LEFT_ALT        (1 << 2)
#define USB_HID_MOD_LEFT_GUI        (1 << 3)
#define USB_HID_MOD_RIGHT_CTRL      (1 << 4)
#define USB_HID_MOD_RIGHT_SHIFT     (1 << 5)
#define USB_HID_MOD_RIGHT_ALT       (1 << 6)
#define USB_HID_MOD_RIGHT_GUI       (1 << 7)

// Boot Protocol Keyboard LEDs
#define USB_HID_LED_NUM_LOCK        (1 << 0)
#define USB_HID_LED_CAPS_LOCK       (1 << 1)
#define USB_HID_LED_SCROLL_LOCK     (1 << 2)
#define USB_HID_LED_COMPOSE         (1 << 3)
#define USB_HID_LED_KANA            (1 << 4)

// Boot Protocol Mouse Button Bits
#define USB_HID_MOUSE_BUTTON_LEFT   (1 << 0)
#define USB_HID_MOUSE_BUTTON_RIGHT  (1 << 1)
#define USB_HID_MOUSE_BUTTON_MIDDLE (1 << 2)

// HID Usage Pages
#define USB_HID_USAGE_PAGE_GENERIC_DESKTOP  0x01
#define USB_HID_USAGE_PAGE_SIMULATION       0x02
#define USB_HID_USAGE_PAGE_VR               0x03
#define USB_HID_USAGE_PAGE_SPORT            0x04
#define USB_HID_USAGE_PAGE_GAME             0x05
#define USB_HID_USAGE_PAGE_GENERIC_DEVICE   0x06
#define USB_HID_USAGE_PAGE_KEYBOARD         0x07
#define USB_HID_USAGE_PAGE_LEDS             0x08
#define USB_HID_USAGE_PAGE_BUTTON           0x09
#define USB_HID_USAGE_PAGE_ORDINAL          0x0A
#define USB_HID_USAGE_PAGE_TELEPHONY        0x0B
#define USB_HID_USAGE_PAGE_CONSUMER         0x0C

// Generic Desktop Page Usages
#define USB_HID_USAGE_POINTER               0x01
#define USB_HID_USAGE_MOUSE                 0x02
#define USB_HID_USAGE_JOYSTICK              0x04
#define USB_HID_USAGE_GAMEPAD               0x05
#define USB_HID_USAGE_KEYBOARD              0x06
#define USB_HID_USAGE_KEYPAD                0x07
#define USB_HID_USAGE_MULTI_AXIS            0x08
#define USB_HID_USAGE_TABLET                0x09
#define USB_HID_USAGE_X                     0x30
#define USB_HID_USAGE_Y                     0x31
#define USB_HID_USAGE_Z                     0x32
#define USB_HID_USAGE_RX                    0x33
#define USB_HID_USAGE_RY                    0x34
#define USB_HID_USAGE_RZ                    0x35
#define USB_HID_USAGE_SLIDER                0x36
#define USB_HID_USAGE_DIAL                  0x37
#define USB_HID_USAGE_WHEEL                 0x38

// Boot Protocol Keyboard Report (8 bytes)
typedef struct __attribute__((packed)) {
    uint8 modifiers;        // Modifier keys
    uint8 reserved;         // Reserved (always 0)
    uint8 keycodes[6];      // Keycode array
} usb_hid_keyboard_report_t;

// Boot Protocol Mouse Report (3+ bytes)
typedef struct __attribute__((packed)) {
    uint8 buttons;          // Button status
    int8  x;                // X movement (-127 to 127)
    int8  y;                // Y movement (-127 to 127)
    int8  wheel;            // Scroll wheel (optional, if IntelliMouse)
} usb_hid_mouse_report_t;

// HID Report Descriptor Item Types
#define USB_HID_ITEM_TYPE_MAIN      0
#define USB_HID_ITEM_TYPE_GLOBAL    1
#define USB_HID_ITEM_TYPE_LOCAL     2
#define USB_HID_ITEM_TYPE_RESERVED  3

// HID Main Item Tags
#define USB_HID_MAIN_INPUT          0x08
#define USB_HID_MAIN_OUTPUT         0x09
#define USB_HID_MAIN_FEATURE        0x0B
#define USB_HID_MAIN_COLLECTION     0x0A
#define USB_HID_MAIN_END_COLLECTION 0x0C

// HID Global Item Tags
#define USB_HID_GLOBAL_USAGE_PAGE   0x00
#define USB_HID_GLOBAL_LOG_MIN      0x01
#define USB_HID_GLOBAL_LOG_MAX      0x02
#define USB_HID_GLOBAL_PHY_MIN      0x03
#define USB_HID_GLOBAL_PHY_MAX      0x04
#define USB_HID_GLOBAL_UNIT_EXP     0x05
#define USB_HID_GLOBAL_UNIT         0x06
#define USB_HID_GLOBAL_REPORT_SIZE  0x07
#define USB_HID_GLOBAL_REPORT_ID    0x08
#define USB_HID_GLOBAL_REPORT_COUNT 0x09
#define USB_HID_GLOBAL_PUSH         0x0A
#define USB_HID_GLOBAL_POP          0x0B

// HID Local Item Tags
#define USB_HID_LOCAL_USAGE         0x00
#define USB_HID_LOCAL_USAGE_MIN     0x01
#define USB_HID_LOCAL_USAGE_MAX     0x02
#define USB_HID_LOCAL_DESIG_INDEX   0x03
#define USB_HID_LOCAL_DESIG_MIN     0x04
#define USB_HID_LOCAL_DESIG_MAX     0x05
#define USB_HID_LOCAL_STRING_INDEX  0x07
#define USB_HID_LOCAL_STRING_MIN    0x08
#define USB_HID_LOCAL_STRING_MAX    0x09
#define USB_HID_LOCAL_DELIMITER     0x0A

// Collection Types
#define USB_HID_COLLECTION_PHYSICAL     0x00
#define USB_HID_COLLECTION_APPLICATION  0x01
#define USB_HID_COLLECTION_LOGICAL      0x02
#define USB_HID_COLLECTION_REPORT       0x03
#define USB_HID_COLLECTION_NAMED_ARRAY  0x04
#define USB_HID_COLLECTION_USAGE_SWITCH 0x05
#define USB_HID_COLLECTION_USAGE_MOD    0x06

// USB HID Device Types
typedef enum {
    USB_HID_TYPE_UNKNOWN,
    USB_HID_TYPE_KEYBOARD,
    USB_HID_TYPE_MOUSE,
    USB_HID_TYPE_JOYSTICK,
    USB_HID_TYPE_GAMEPAD,
    USB_HID_TYPE_TABLET
} usb_hid_device_type_t;

// USB HID Keyboard State
typedef struct {
    bool num_lock;
    bool caps_lock;
    bool scroll_lock;
    uint8 modifiers;
    uint8 last_keycodes[6];
    bool keys_pressed[256];
} usb_hid_keyboard_state_t;

// USB HID Mouse State
typedef struct {
    int32 x;
    int32 y;
    int32 wheel;
    bool left_button;
    bool right_button;
    bool middle_button;
    bool button4;
    bool button5;
    int32 screen_width;
    int32 screen_height;
} usb_hid_mouse_state_t;

// USB HID Device
typedef struct usb_hid_device {
    usb_device_t* usb_device;
    usb_interface_t* interface;
    usb_endpoint_t* interrupt_in;
    usb_endpoint_t* interrupt_out;
    usb_hid_device_type_t type;
    uint8 protocol;             // Boot or report protocol
    uint16 report_descriptor_length;
    uint8* report_descriptor;
    uint8 poll_interval;        // Polling interval in ms
    union {
        usb_hid_keyboard_state_t keyboard;
        usb_hid_mouse_state_t mouse;
    } state;
    struct usb_hid_device* next;
} usb_hid_device_t;

// USB HID Keyboard Event
typedef struct {
    uint8 scancode;
    bool pressed;
    uint8 modifiers;
    char ascii;
} usb_hid_keyboard_event_t;

// USB HID Mouse Event
typedef struct {
    int32 dx;
    int32 dy;
    int32 dwheel;
    int32 x;
    int32 y;
    bool left_button;
    bool right_button;
    bool middle_button;
    bool button4;
    bool button5;
} usb_hid_mouse_event_t;

// Callback types
typedef void (*usb_hid_keyboard_callback_t)(const usb_hid_keyboard_event_t* event);
typedef void (*usb_hid_mouse_callback_t)(const usb_hid_mouse_event_t* event);

// USB HID Functions
bool usb_hid_init(void);
void usb_hid_shutdown(void);
void usb_hid_poll(void);

// Device management
bool usb_hid_probe(usb_device_t* device, usb_interface_t* interface);
void usb_hid_disconnect(usb_device_t* device, usb_interface_t* interface);

// Protocol control
bool usb_hid_set_protocol(usb_hid_device_t* device, uint8 protocol);
bool usb_hid_get_protocol(usb_hid_device_t* device, uint8* protocol);
bool usb_hid_set_idle(usb_hid_device_t* device, uint8 duration, uint8 report_id);
bool usb_hid_get_report(usb_hid_device_t* device, uint8 type, uint8 id,
                        void* buffer, uint16 length);
bool usb_hid_set_report(usb_hid_device_t* device, uint8 type, uint8 id,
                        void* data, uint16 length);

// Keyboard functions
void usb_hid_keyboard_register_callback(usb_hid_keyboard_callback_t callback);
bool usb_hid_keyboard_set_leds(usb_hid_device_t* device, uint8 leds);
usb_hid_keyboard_state_t* usb_hid_keyboard_get_state(usb_hid_device_t* device);

// Mouse functions
void usb_hid_mouse_register_callback(usb_hid_mouse_callback_t callback);
void usb_hid_mouse_set_bounds(int32 width, int32 height);
void usb_hid_mouse_set_position(int32 x, int32 y);
usb_hid_mouse_state_t* usb_hid_mouse_get_state(usb_hid_device_t* device);

// Scancode to ASCII conversion
char usb_hid_scancode_to_ascii(uint8 scancode, bool shift, bool caps_lock);

// USB HID class driver
extern usb_class_driver_t usb_hid_driver;

#endif // USB_HID_H
