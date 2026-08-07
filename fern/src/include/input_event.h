#ifndef INPUT_EVENT_H
#define INPUT_EVENT_H

#include "types.h"
#include <stdbool.h>

/*
 * Input Event Subsystem
 *
 * This provides a unified event structure for all input devices (keyboard, mouse).
 * The structure is designed to be Linux evdev compatible for familiarity.
 *
 * Events are 16 bytes: timestamp (8), type (2), code (2), value (4)
 */

/* Event types */
#define EV_SYN          0x00    /* Synchronization events */
#define EV_KEY          0x01    /* Key/button press/release */
#define EV_REL          0x02    /* Relative axis movement */
#define EV_ABS          0x03    /* Absolute axis position */
#define EV_MSC          0x04    /* Miscellaneous */
#define EV_SW           0x05    /* Switch events */
#define EV_LED          0x11    /* LED state change */
#define EV_SND          0x12    /* Sound events */
#define EV_REP          0x14    /* Auto-repeat events */
#define EV_MAX          0x1F

/* Synchronization codes */
#define SYN_REPORT      0       /* End of event packet */
#define SYN_CONFIG      1       /* Configuration changed */
#define SYN_MT_REPORT   2       /* Multi-touch report */
#define SYN_DROPPED     3       /* Buffer overrun, events lost */

/* Relative axes */
#define REL_X           0x00    /* X-axis movement */
#define REL_Y           0x01    /* Y-axis movement */
#define REL_Z           0x02    /* Z-axis movement */
#define REL_RX          0x03    /* X rotation */
#define REL_RY          0x04    /* Y rotation */
#define REL_RZ          0x05    /* Z rotation */
#define REL_HWHEEL      0x06    /* Horizontal scroll wheel */
#define REL_DIAL        0x07    /* Dial */
#define REL_WHEEL       0x08    /* Vertical scroll wheel */
#define REL_MISC        0x09    /* Miscellaneous */
#define REL_MAX         0x0F

/* Button/Key codes for mouse buttons (above keyboard range) */
#define BTN_MISC        0x100
#define BTN_0           0x100
#define BTN_1           0x101
#define BTN_2           0x102
#define BTN_3           0x103
#define BTN_4           0x104
#define BTN_5           0x105
#define BTN_6           0x106
#define BTN_7           0x107
#define BTN_8           0x108
#define BTN_9           0x109

#define BTN_MOUSE       0x110
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114
#define BTN_FORWARD     0x115
#define BTN_BACK        0x116
#define BTN_TASK        0x117

/*
 * Keyboard key codes
 *
 * IMPORTANT: Key codes are defined in ps2_keyboard.h, not here.
 * This avoids conflicts between the Linux evdev key codes and the PS/2 driver
 * internal key codes. Include ps2_keyboard.h for key code definitions.
 *
 * The input_event_t structure below uses key codes in its 'code' field.
 * When working with keyboard events, use the KEY_* macros from ps2_keyboard.h.
 */

/* Bus types for device identification */
#define BUS_PCI             0x01
#define BUS_ISAPNP          0x02
#define BUS_USB             0x03
#define BUS_HIL             0x04
#define BUS_BLUETOOTH       0x05
#define BUS_VIRTUAL         0x06
#define BUS_ISA             0x10
#define BUS_I8042           0x11
#define BUS_XTKBD           0x12
#define BUS_RS232           0x13
#define BUS_GAMEPORT        0x14
#define BUS_PARPORT         0x15
#define BUS_AMIGA           0x16
#define BUS_ADB             0x17
#define BUS_I2C             0x18
#define BUS_HOST            0x19
#define BUS_GSC             0x1A
#define BUS_ATARI           0x1B
#define BUS_SPI             0x1C

/*
 * Input event structure (16 bytes)
 * This matches the Linux struct input_event layout
 */
typedef struct input_event {
    uint32 tv_sec;      /* Timestamp: seconds since boot */
    uint32 tv_usec;     /* Timestamp: microseconds */
    uint16 type;        /* Event type (EV_KEY, EV_REL, etc.) */
    uint16 code;        /* Event code (key code, axis, etc.) */
    int32  value;       /* Event value (1=press, 0=release, delta for REL) */
} __attribute__((packed)) input_event_t;

/*
 * Input device identification
 */
typedef struct input_id {
    uint16 bustype;     /* Bus type (BUS_USB, BUS_I8042, etc.) */
    uint16 vendor;      /* Vendor ID */
    uint16 product;     /* Product ID */
    uint16 version;     /* Version */
} input_id_t;

/*
 * Input device information
 */
typedef struct input_device_info {
    input_id_t id;
    char name[64];              /* Device name */
    char phys[64];              /* Physical path */
    uint32 event_types;         /* Bitmask of supported event types */
    uint32 capabilities;        /* Device capabilities */
} input_device_info_t;

/* Capability flags */
#define INPUT_CAP_KEY       (1 << EV_KEY)   /* Has keys/buttons */
#define INPUT_CAP_REL       (1 << EV_REL)   /* Has relative axes */
#define INPUT_CAP_ABS       (1 << EV_ABS)   /* Has absolute axes */
#define INPUT_CAP_MSC       (1 << EV_MSC)   /* Has misc events */
#define INPUT_CAP_LED       (1 << EV_LED)   /* Has LEDs */
#define INPUT_CAP_REP       (1 << EV_REP)   /* Has auto-repeat */

/* Event value meanings for EV_KEY */
#define KEY_RELEASE     0   /* Key released */
#define KEY_PRESS       1   /* Key pressed */
#define KEY_REPEAT      2   /* Key auto-repeat */

/*
 * Helper macros for creating events
 */
#define INPUT_EVENT_INIT(ev, t, c, v) do { \
    (ev)->type = (t); \
    (ev)->code = (c); \
    (ev)->value = (v); \
} while(0)

/* Check if event is a button event */
#define IS_BUTTON(code) ((code) >= BTN_MISC && (code) < BTN_TASK + 1)

/* Check if event is a mouse button */
#define IS_MOUSE_BUTTON(code) ((code) >= BTN_MOUSE && (code) <= BTN_TASK)

/* Check if event is a keyboard key */
#define IS_KEY(code) ((code) > KEY_RESERVED && (code) < BTN_MISC)

#endif /* INPUT_EVENT_H */
