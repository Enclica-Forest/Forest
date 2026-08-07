/**
 * Device Filesystem (devfs) for Fern
 *
 * Provides UNIX-like /dev interface for character and block devices.
 * Integrates with the input event subsystem for keyboard and mouse devices.
 */

#ifndef DEVFS_H
#define DEVFS_H

#include "types.h"
#include "vfs.h"
#include "input_event.h"
#include <stdbool.h>

/*
 * Device types
 */
typedef enum {
    DEV_TYPE_CHAR,      /* Character device (mice, keyboards, serial ports) */
    DEV_TYPE_BLOCK      /* Block device (disks, flash drives) */
} dev_type_t;

/*
 * Device bus types for dynamic registration
 */
typedef enum {
    DEV_BUS_UNKNOWN,    /* Unknown or static device */
    DEV_BUS_PCI,        /* PCI device */
    DEV_BUS_USB         /* USB device */
} dev_bus_t;

/*
 * Device major numbers (UNIX-like)
 */
#define DEV_MAJOR_MEM           1       /* /dev/null, /dev/zero, /dev/random */
#define DEV_MAJOR_TTY           4       /* TTY devices */
#define DEV_MAJOR_CONSOLE       5       /* Console */
#define DEV_MAJOR_INPUT         13      /* Input devices (mice, keyboards) */
#define DEV_MAJOR_FB            29      /* Framebuffer */
#define DEV_MAJOR_CURSOR        30      /* Cursor devices */
#define DEV_MAJOR_USB           180     /* USB devices */
#define DEV_MAJOR_SCSI          8       /* SCSI/USB storage */
#define DEV_MAJOR_TIMER         10      /* /dev/timer, /dev/pit (misc devices) */
#define DEV_MAJOR_RTC           253     /* /dev/rtc */
#define DEV_MAJOR_HPET          254     /* /dev/hpet */

/*
 * Input device minor numbers
 */
#define DEV_MINOR_KBD           0       /* /dev/kbd - keyboard (evdev style) */
#define DEV_MINOR_MOUSE         1       /* /dev/mouse - mouse (evdev style) */
#define DEV_MINOR_PS2KBD        64      /* /dev/ps2kbd - PS2 keyboard */
#define DEV_MINOR_PS2MOUSE      65      /* /dev/ps2mouse - PS2 mouse */
#define DEV_MINOR_PSAUX         66      /* /dev/psaux - PS2 auxiliary (mouse) */
#define DEV_MINOR_MICE          32      /* /dev/input/mice (combined mouse, raw) */
#define DEV_MINOR_MOUSE0        33      /* /dev/input/mouse0 (raw) */
#define DEV_MINOR_EVENT0        64      /* /dev/input/event0 (keyboard evdev) */
#define DEV_MINOR_EVENT1        65      /* /dev/input/event1 (mouse evdev) */

/*
 * Maximum devices
 */
#define DEVFS_MAX_DEVICES       64
#define DEVFS_UEVENT_QUEUE_SIZE 64

/*
 * Mouse packet format (ImPS/2 compatible, like /dev/input/mice on Linux)
 * Used for legacy /dev/input/mice interface
 */
typedef struct __attribute__((packed)) {
    uint8 buttons;      /* Bit 0: left, Bit 1: right, Bit 2: middle */
    int8  dx;           /* X movement (-127 to 127) */
    int8  dy;           /* Y movement (-127 to 127) */
    int8  dz;           /* Scroll wheel (-127 to 127, optional) */
} mouse_packet_t;

/*
 * Buffer sizes for input devices
 */
#define MOUSE_BUFFER_SIZE       64
#define KEYBOARD_BUFFER_SIZE    128

/*
 * Mouse buffer structure for legacy /dev/input/mice interface
 */
typedef struct {
    mouse_packet_t packets[MOUSE_BUFFER_SIZE];
    uint32 head;
    uint32 tail;
    uint32 count;
    bool overflow;
} mouse_buffer_t;

/*
 * Keyboard buffer structure for legacy scancode interface
 */
typedef struct {
    uint8 scancodes[KEYBOARD_BUFFER_SIZE];
    uint32 head;
    uint32 tail;
    uint32 count;
    bool overflow;
} keyboard_buffer_t;

/* Alias for backwards compatibility */
#define DEV_MINOR_KEYBOARD DEV_MINOR_KBD

/*
 * Device operations structure
 */
typedef struct dev_ops {
    /* Required operations */
    uint32 (*read)(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
    uint32 (*write)(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);

    /* Optional operations */
    void (*open)(vfs_node_t* node, uint32 flags);
    void (*close)(vfs_node_t* node);
    int  (*ioctl)(vfs_node_t* node, uint32 request, void* arg);
    int  (*poll)(vfs_node_t* node, uint32 events);
} dev_ops_t;

/*
 * Device node structure
 */
typedef struct dev_node {
    char name[64];              /* Device name (e.g., "kbd", "input/event0") */
    dev_type_t type;            /* Character or block device */
    uint16 major;               /* Major device number */
    uint16 minor;               /* Minor device number */
    dev_ops_t* ops;             /* Device operations */
    void* private_data;         /* Driver-specific data */
    vfs_node_t* vfs_node;       /* Associated VFS node */
    uint32 open_count;          /* Number of open handles */
    bool registered;            /* Device is registered */
    dev_bus_t bus_type;         /* Bus type for dynamic devices */
    char bus_id[32];            /* Bus-specific identifier (e.g., "0000:00:1f.3") */
    struct dev_node* next;      /* Linked list for hash collisions */
} dev_node_t;

/*
 * Minimal uevent payload for user-space device event consumers.
 */
typedef struct devfs_uevent {
    uint64 seqnum;
    char action[16];    /* add/remove/change */
    char subsystem[32]; /* input/block/tty/etc */
    char devpath[96];   /* /devices/virtual/... */
    char devname[64];   /* path under /dev */
    uint16 major;
    uint16 minor;
} devfs_uevent_t;

/*
 * Character device wrapper (for cleaner registration)
 */
typedef struct chardev {
    const char* name;
    uint16 major;
    uint16 minor;
    dev_ops_t ops;
    void* driver_data;
    vfs_node_t* node;
    bool registered;
    uint32 open_count;
} chardev_t;

/*
 * Device filesystem state
 */
typedef struct {
    dev_node_t* devices[DEVFS_MAX_DEVICES];
    uint32 num_devices;
    vfs_node_t* dev_root;       /* /dev directory node */
    bool initialized;
} devfs_state_t;

/*
 * Initialization
 */
bool devfs_init(void);
void devfs_shutdown(void);
bool devfs_is_initialized(void);

/*
 * Device registration
 */
bool devfs_register_device(const char* name, dev_type_t type,
                           uint16 major, uint16 minor,
                           dev_ops_t* ops, void* private_data);
bool devfs_unregister_device(const char* name);
dev_node_t* devfs_find_device(const char* name);
dev_node_t* devfs_find_by_major_minor(uint16 major, uint16 minor);

/* Character device registration (convenience wrapper) */
bool devfs_register_chardev(chardev_t* dev);
bool devfs_unregister_chardev(chardev_t* dev);

/* Dynamic device registration for buses */
bool devfs_register_dynamic_device(dev_bus_t bus_type, const char* bus_id,
                                   dev_type_t type, uint16 major, uint16 minor,
                                   dev_ops_t* ops, void* private_data);
bool devfs_unregister_dynamic_device(dev_bus_t bus_type, const char* bus_id);

/*
 * VFS integration
 */
vfs_node_t* devfs_open(const char* path, uint32 flags);
void devfs_close(vfs_node_t* node);
bool devfs_readdir(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent);
vfs_node_t* devfs_finddir(vfs_node_t* node, const char* name);

/*
 * Input device initialization
 * These create the /dev/kbd and /dev/mouse device nodes
 */
bool devfs_input_init(void);
void devfs_input_shutdown(void);

/*
 * Dynamic bus device registration
 */
bool devfs_register_pci_devices(void);
bool devfs_register_usb_device(const char* bus_id, dev_type_t type, dev_ops_t* ops, void* private_data);
bool devfs_unregister_usb_device(const char* bus_id);

/*
 * Input event queueing (called from interrupt handlers)
 */
void devfs_kbd_queue_event(const input_event_t* event);
void devfs_mouse_queue_event(const input_event_t* event);

/*
 * Input ring buffer access (for syscall implementations)
 */
#include "input_ring.h"
input_ring_t* devfs_get_kbd_ring(void);
input_ring_t* devfs_get_mouse_ring(void);

/*
 * Legacy mouse interface
 */
bool devfs_mouse_init(void);
void devfs_mouse_queue_packet(const mouse_packet_t* packet);
int devfs_mouse_read(void* buffer, uint32 size);
bool devfs_mouse_data_available(void);

/*
 * Legacy keyboard interface
 */
bool devfs_keyboard_init(void);
void devfs_keyboard_queue_scancode(uint8 scancode);
int devfs_keyboard_read(void* buffer, uint32 size);
bool devfs_keyboard_data_available(void);

/*
 * Timer device initialization
 * Creates /dev/timer, /dev/rtc, /dev/hpet, /dev/pit device nodes
 */
bool timer_dev_init(void);
void timer_dev_shutdown(void);

/*
 * Utility functions
 */
uint32 devfs_get_device_count(void);
void devfs_list_devices(void);

/*
 * Udev-like event queue (skeleton)
 */
bool devfs_uevent_emit(const char* action, const char* subsystem,
                       const char* devname, uint16 major, uint16 minor);
bool devfs_uevent_pop(devfs_uevent_t* out_event);
uint32 devfs_uevent_pending_count(void);
uint64 devfs_uevent_last_seqnum(void);
int devfs_uevent_format(const devfs_uevent_t* event, char* buffer, uint32 size);

/*
 * Standard device paths
 */
#define DEV_PATH_KBD            "kbd"           /* /dev/kbd */
#define DEV_PATH_MOUSE          "mouse"         /* /dev/mouse */
#define DEV_PATH_PS2KBD         "ps2kbd"        /* /dev/ps2kbd - PS2 keyboard */
#define DEV_PATH_PS2MOUSE       "ps2mouse"      /* /dev/ps2mouse - PS2 mouse */
#define DEV_PATH_PSAUX          "psaux"         /* /dev/psaux - PS2 auxiliary (mouse) */
#define DEV_PATH_TTY            "tty"           /* /dev/tty */
#define DEV_PATH_CONSOLE        "console"       /* /dev/console */
#define DEV_PATH_NULL           "null"          /* /dev/null */
#define DEV_PATH_ZERO           "zero"          /* /dev/zero */
#define DEV_PATH_INPUT_EVENT0   "input/event0"  /* /dev/input/event0 */
#define DEV_PATH_INPUT_EVENT1   "input/event1"  /* /dev/input/event1 */
#define DEV_PATH_INPUT_MICE     "input/mice"    /* /dev/input/mice */
#define DEV_PATH_TIMER          "timer"         /* /dev/timer */
#define DEV_PATH_RTC            "rtc"           /* /dev/rtc */
#define DEV_PATH_RTC0           "rtc0"          /* /dev/rtc0 */
#define DEV_PATH_HPET           "hpet"          /* /dev/hpet */
#define DEV_PATH_PIT            "pit"           /* /dev/pit */
#define DEV_PATH_FB_WIDTH       "fb_width"      /* /dev/fb_width */
#define DEV_PATH_FB_HEIGHT      "fb_height"     /* /dev/fb_height */
#define DEV_PATH_FB_PITCH       "fb_pitch"      /* /dev/fb_pitch */
#define DEV_PATH_FB_BPP         "fb_bpp"        /* /dev/fb_bpp */
#define DEV_PATH_FB_ADDR        "fb_addr"       /* /dev/fb_addr */
#define DEV_PATH_FB_SIZE        "fb_size"       /* /dev/fb_size */
#define DEV_PATH_SCRX           "scrx"          /* /dev/scrx - screen width */
#define DEV_PATH_SCRY           "scry"          /* /dev/scry - screen height */

/* Framebuffer control devices */
#define DEV_PATH_FB_DOUBLE_BUFFER "fb_double_buffer"  /* /dev/fb_double_buffer */
#define DEV_PATH_FB_SWAP         "fb_swap"             /* /dev/fb_swap */

/* Cursor control devices */
#define DEV_PATH_CURSOR_POS      "cursor_pos"          /* /dev/cursor_pos */
#define DEV_PATH_CURSOR_VISIBLE  "cursor_visible"      /* /dev/cursor_visible */

/* Framebuffer memory mapping device */
#define DEV_PATH_FB_MMAP         "fb_mmap"             /* /dev/fb_mmap - direct memory access */

/*
 * Framebuffer device minor numbers
 */
#define DEV_MINOR_FB0           0       /* /dev/fb0 - primary framebuffer */
#define DEV_MINOR_FB_WIDTH      1       /* /dev/fb_width */
#define DEV_MINOR_FB_HEIGHT     2       /* /dev/fb_height */
#define DEV_MINOR_FB_PITCH      3       /* /dev/fb_pitch */
#define DEV_MINOR_FB_BPP        6       /* /dev/fb_bpp */
#define DEV_MINOR_FB_ADDR       7       /* /dev/fb_addr */
#define DEV_MINOR_FB_SIZE       8       /* /dev/fb_size */
#define DEV_MINOR_SCRX          4       /* /dev/scrx */
#define DEV_MINOR_SCRY          5       /* /dev/scry */
#define DEV_MINOR_FB_DOUBLE_BUFFER 9   /* /dev/fb_double_buffer */
#define DEV_MINOR_FB_SWAP      10       /* /dev/fb_swap */

/* Cursor device minor numbers (using different major number) */
#define DEV_MINOR_CURSOR_POS    1       /* /dev/cursor_pos */
#define DEV_MINOR_CURSOR_VISIBLE 2     /* /dev/cursor_visible */

/* Framebuffer memory mapping device minor numbers */
#define DEV_MINOR_FB_MMAP       11      /* /dev/fb_mmap */

/* Multiple framebuffer device support */
#define DEV_MAJOR_FB1           31      /* Secondary framebuffer devices */
#define DEV_MAJOR_FB2           32      /* Tertiary framebuffer devices */
#define DEV_MINOR_FB1_0         0       /* /dev/fb1 */
#define DEV_MINOR_FB2_0         0       /* /dev/fb2 */
#define MAX_FB_DEVICES           3       /* /dev/fb0, /dev/fb1, /dev/fb2 */

/*
 * Framebuffer device initialization
 * Creates /dev/fb_width, /dev/fb_height, /dev/fb_pitch device nodes
 */
bool devfs_fb_init(void);
void devfs_fb_shutdown(void);
bool devfs_fb0_init(void);
void devfs_fb0_shutdown(void);

/*
 * Helper macros
 */
#define CHARDEV_INIT(dev_name, dev_major, dev_minor) { \
    .name = dev_name, \
    .major = dev_major, \
    .minor = dev_minor, \
    .ops = {0}, \
    .driver_data = NULL, \
    .node = NULL, \
    .registered = false, \
    .open_count = 0 \
}

#endif /* DEVFS_H */
