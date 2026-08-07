/**
 * Device Filesystem (devfs) Implementation for Fern
 *
 * Provides UNIX-like /dev interface for character and block devices.
 * Supports PS/2 and USB input devices via /dev/mouse, /dev/keyboard, etc.
 */

#include "include/devfs.h"
#include "include/vfs.h"
#include "include/ps2_mouse.h"
#include "include/kb.h"
#include "include/enhanced_heap.h"
#include "include/string.h"
#include "include/util.h"
#include "include/screen.h"
#include "include/debuglog.h"
#include "include/interrupt.h"
#include "include/input_event.h"
#include "include/input_ring.h"
#include "include/ioctl.h"
#include "include/pci.h"
#include "include/framebuffer.h"
#include "include/spinlock.h"
#include <stdio.h>

// Device list
static dev_node_t* g_device_list = NULL;
static bool g_devfs_initialized = false;

// Mouse buffer
static mouse_buffer_t g_mouse_buffer;
static bool g_mouse_device_ready = false;

// Keyboard buffer
static keyboard_buffer_t g_keyboard_buffer;
static bool g_keyboard_device_ready = false;

// Udev-like event queue skeleton
typedef struct {
    devfs_uevent_t events[DEVFS_UEVENT_QUEUE_SIZE];
    uint32 head;
    uint32 tail;
    uint32 count;
    uint64 seqnum;
    bool overflow;
} devfs_uevent_queue_t;

static devfs_uevent_queue_t g_uevent_queue = {0};

// Performance optimization: Cache framebuffer info to avoid repeated graphics_get_framebuffer() calls
typedef struct {
    bool valid;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uintptr_t virtual_addr;
    uintptr_t physical_addr;
    size_t size;
    bool double_buffered;
} fb_cache_entry_t;

static fb_cache_entry_t g_fb_cache[MAX_FB_DEVICES] = {0};
static spinlock_t g_fb_cache_lock = SPINLOCK_INIT("devfs_fb_cache");

static bool snapshot_fb_cache(int fb_index, fb_cache_entry_t* out_entry);
static bool parse_u32_control_write(uint32 size, const uint8* buffer, uint32* out_value);

// Forward declarations
static void mouse_dev_open(vfs_node_t* node, uint32 flags);
static void mouse_dev_close(vfs_node_t* node);
static uint32 mouse_dev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static int mouse_dev_poll(vfs_node_t* node, uint32 events);

static void keyboard_dev_open(vfs_node_t* node, uint32 flags);
static void keyboard_dev_close(vfs_node_t* node);
static uint32 keyboard_dev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static int keyboard_dev_poll(vfs_node_t* node, uint32 events);

// Framebuffer cache forward declaration
static void update_fb_cache(int fb_index);
static int get_fb_index(uint16 major, uint16 minor);
static const char* devfs_infer_subsystem(const char* devname, dev_type_t type, uint16 major);

static bool snapshot_fb_cache(int fb_index, fb_cache_entry_t* out_entry) {
    if (!out_entry || fb_index < 0 || fb_index >= MAX_FB_DEVICES) {
        return false;
    }

    spinlock_acquire(&g_fb_cache_lock);
    update_fb_cache(fb_index);
    *out_entry = g_fb_cache[fb_index];
    bool valid = out_entry->valid;
    spinlock_release(&g_fb_cache_lock);
    return valid;
}

static bool parse_u32_control_write(uint32 size, const uint8* buffer, uint32* out_value) {
    if (!buffer || !out_value || size < sizeof(uint32)) {
        return false;
    }

    memcpy(out_value, buffer, sizeof(uint32));
    return true;
}

/**
 * Get framebuffer cache index based on device major/minor numbers
 */
static int get_fb_index(uint16 major, uint16 minor) {
    (void)minor;
    if (major == DEV_MAJOR_FB) {
        return 0;  // /dev/fb0
    } else if (major == DEV_MAJOR_FB1) {
        return 1;  // /dev/fb1
    } else if (major == DEV_MAJOR_FB2) {
        return 2;  // /dev/fb2
    }
    return -1; // Invalid framebuffer device
}

static const char* devfs_infer_subsystem(const char* devname, dev_type_t type, uint16 major) {
    if (!devname) {
        return "misc";
    }

    if (strncmp(devname, "input/", 6) == 0 || strcmp(devname, "kbd") == 0 ||
        strcmp(devname, "keyboard") == 0 || strcmp(devname, "mouse") == 0 ||
        strcmp(devname, "ps2kbd") == 0 || strcmp(devname, "ps2mouse") == 0 ||
        strcmp(devname, "psaux") == 0 || major == DEV_MAJOR_INPUT) {
        return "input";
    }

    if (strncmp(devname, "tty", 3) == 0 || strcmp(devname, "console") == 0 ||
        major == DEV_MAJOR_TTY || major == DEV_MAJOR_CONSOLE) {
        return "tty";
    }

    if (strncmp(devname, "fb", 2) == 0 || strncmp(devname, "cursor", 6) == 0 ||
        major == DEV_MAJOR_FB || major == DEV_MAJOR_FB1 || major == DEV_MAJOR_FB2) {
        return "graphics";
    }

    if (strncmp(devname, "usb/", 4) == 0 || major == DEV_MAJOR_USB) {
        return "usb";
    }

    if (type == DEV_TYPE_BLOCK || major == DEV_MAJOR_SCSI) {
        return "block";
    }

    if (major == DEV_MAJOR_MEM) {
        return "mem";
    }

    return "misc";
}

bool devfs_uevent_emit(const char* action, const char* subsystem,
                       const char* devname, uint16 major, uint16 minor) {
    if (!action || !devname || !g_devfs_initialized) {
        return false;
    }

    bool interrupts_enabled = irq_save_and_disable_safe();

    if (g_uevent_queue.count >= DEVFS_UEVENT_QUEUE_SIZE) {
        g_uevent_queue.head = (g_uevent_queue.head + 1) % DEVFS_UEVENT_QUEUE_SIZE;
        g_uevent_queue.count--;
        g_uevent_queue.overflow = true;
    }

    devfs_uevent_t* event = &g_uevent_queue.events[g_uevent_queue.tail];
    memory_set((uint8*)event, 0, sizeof(*event));

    g_uevent_queue.seqnum++;
    event->seqnum = g_uevent_queue.seqnum;
    strncpy(event->action, action, sizeof(event->action) - 1);
    strncpy(event->subsystem, subsystem ? subsystem : "misc", sizeof(event->subsystem) - 1);
    strncpy(event->devname, devname, sizeof(event->devname) - 1);
    event->major = major;
    event->minor = minor;

    snprintf(event->devpath, sizeof(event->devpath), "/devices/virtual/%s/%s",
             event->subsystem, event->devname);

    g_uevent_queue.tail = (g_uevent_queue.tail + 1) % DEVFS_UEVENT_QUEUE_SIZE;
    g_uevent_queue.count++;

    irq_restore_safe(interrupts_enabled);
    return true;
}

bool devfs_uevent_pop(devfs_uevent_t* out_event) {
    if (!out_event) {
        return false;
    }

    bool interrupts_enabled = irq_save_and_disable_safe();
    if (g_uevent_queue.count == 0) {
        irq_restore_safe(interrupts_enabled);
        return false;
    }

    *out_event = g_uevent_queue.events[g_uevent_queue.head];
    g_uevent_queue.head = (g_uevent_queue.head + 1) % DEVFS_UEVENT_QUEUE_SIZE;
    g_uevent_queue.count--;

    irq_restore_safe(interrupts_enabled);
    return true;
}

uint32 devfs_uevent_pending_count(void) {
    uint32 count;
    bool interrupts_enabled = irq_save_and_disable_safe();
    count = g_uevent_queue.count;
    irq_restore_safe(interrupts_enabled);
    return count;
}

uint64 devfs_uevent_last_seqnum(void) {
    uint64 seq;
    bool interrupts_enabled = irq_save_and_disable_safe();
    seq = g_uevent_queue.seqnum;
    irq_restore_safe(interrupts_enabled);
    return seq;
}

int devfs_uevent_format(const devfs_uevent_t* event, char* buffer, uint32 size) {
    if (!event || !buffer || size == 0) {
        return -1;
    }

    int len = snprintf(buffer, size,
        "ACTION=%s\n"
        "DEVPATH=%s\n"
        "SUBSYSTEM=%s\n"
        "DEVNAME=%s\n"
        "MAJOR=%u\n"
        "MINOR=%u\n"
        "SEQNUM=%llu\n",
        event->action,
        event->devpath,
        event->subsystem,
        event->devname,
        event->major,
        event->minor,
        event->seqnum);

    return (len < 0) ? -1 : len;
}

// Device operations for mouse
static dev_ops_t g_mouse_ops = {
    .open = mouse_dev_open,
    .close = mouse_dev_close,
    .read = mouse_dev_read,
    .write = NULL,
    .ioctl = NULL,
    .poll = mouse_dev_poll
};

// Device operations for keyboard
static dev_ops_t g_keyboard_ops = {
    .open = keyboard_dev_open,
    .close = keyboard_dev_close,
    .read = keyboard_dev_read,
    .write = NULL,
    .ioctl = NULL,
    .poll = keyboard_dev_poll
};

/**
 * Initialize the device filesystem
 */
bool devfs_init(void) {
    if (g_devfs_initialized) {
        return true;
    }

    g_device_list = NULL;

    // Initialize mouse buffer
    memory_set((uint8*)&g_mouse_buffer, 0, sizeof(g_mouse_buffer));

    // Initialize keyboard buffer
    memory_set((uint8*)&g_keyboard_buffer, 0, sizeof(g_keyboard_buffer));
    memory_set((uint8*)&g_uevent_queue, 0, sizeof(g_uevent_queue));

    g_devfs_initialized = true;

    debuglog(DEBUG_INFO, "[DEVFS] Device filesystem initialized\n");
    print("[DEVFS] Device filesystem initialized\n");

    return true;
}

/**
 * Shutdown the device filesystem
 */
void devfs_shutdown(void) {
    // Free all device nodes
    dev_node_t* current = g_device_list;
    while (current) {
        dev_node_t* next = current->next;
        enhanced_heap_free(current, "devfs_node");
        current = next;
    }

    g_device_list = NULL;
    g_devfs_initialized = false;
    g_mouse_device_ready = false;
    g_keyboard_device_ready = false;
    memory_set((uint8*)&g_uevent_queue, 0, sizeof(g_uevent_queue));
}

/**
 * Register a device
 */
bool devfs_register_device(const char* name, dev_type_t type,
                           uint16 major, uint16 minor, dev_ops_t* ops, void* private_data) {
    if (!name || !g_devfs_initialized) {
        return false;
    }

    // Check for duplicate
    if (devfs_find_device(name)) {
        debuglog(DEBUG_WARN, "[DEVFS] Device '%s' already registered\n", name);
        return false;
    }

    // Allocate device node
    dev_node_t* node = (dev_node_t*)enhanced_heap_alloc(sizeof(dev_node_t), "devfs_node");
    if (!node) {
        return false;
    }

    // Initialize node
    memory_set((uint8*)node, 0, sizeof(dev_node_t));
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->type = type;
    node->major = major;
    node->minor = minor;
    node->ops = ops;
    node->private_data = private_data;
    node->bus_type = DEV_BUS_UNKNOWN;

    // Add to list
    node->next = g_device_list;
    g_device_list = node;

    debuglog(DEBUG_INFO, "[DEVFS] Registered device: %s (major=%d, minor=%d)\n",
             name, major, minor);
    devfs_uevent_emit("add", devfs_infer_subsystem(name, type, major), name, major, minor);

    return true;
}

/**
 * Unregister a device
 */
bool devfs_unregister_device(const char* name) {
    if (!name || !g_devfs_initialized) {
        return false;
    }

    dev_node_t* prev = NULL;
    dev_node_t* current = g_device_list;

    while (current) {
        if (strcmp(current->name, name) == 0) {
            uint16 major = current->major;
            uint16 minor = current->minor;
            dev_type_t type = current->type;
            char dev_name[sizeof(current->name)];
            strncpy(dev_name, current->name, sizeof(dev_name) - 1);
            dev_name[sizeof(dev_name) - 1] = '\0';

            if (prev) {
                prev->next = current->next;
            } else {
                g_device_list = current->next;
            }
            enhanced_heap_free(current, "devfs_node");
            debuglog(DEBUG_INFO, "[DEVFS] Unregistered device: %s\n", name);
            devfs_uevent_emit("remove",
                devfs_infer_subsystem(dev_name, type, major),
                dev_name, major, minor);
            return true;
        }
        prev = current;
        current = current->next;
    }

    return false;
}

/**
 * Find a device by name
 */
dev_node_t* devfs_find_device(const char* name) {
    if (!name) {
        return NULL;
    }

    dev_node_t* current = g_device_list;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}

/**
 * Register a dynamic device with bus information
 */
bool devfs_register_dynamic_device(dev_bus_t bus_type, const char* bus_id,
                                   dev_type_t type, uint16 major, uint16 minor,
                                   dev_ops_t* ops, void* private_data) {
    if (!bus_id || !g_devfs_initialized) {
        return false;
    }

    // Build full device name based on bus type
    char full_name[128];
    if (bus_type == DEV_BUS_PCI) {
        strcpy(full_name, "pci/");
        strncat(full_name, bus_id, sizeof(full_name) - 5);
    } else if (bus_type == DEV_BUS_USB) {
        strcpy(full_name, "usb/");
        strncat(full_name, bus_id, sizeof(full_name) - 5);
    } else {
        return false;  // Unsupported bus type
    }

    // Check for duplicate
    if (devfs_find_device(full_name)) {
        debuglog(DEBUG_WARN, "[DEVFS] Dynamic device '%s' already registered\n", full_name);
        return false;
    }

    // Allocate device node
    dev_node_t* node = (dev_node_t*)enhanced_heap_alloc(sizeof(dev_node_t), "devfs_node");
    if (!node) {
        return false;
    }

    // Initialize node
    memory_set((uint8*)node, 0, sizeof(dev_node_t));
    strncpy(node->name, full_name, sizeof(node->name) - 1);
    node->type = type;
    node->major = major;
    node->minor = minor;
    node->ops = ops;
    node->private_data = private_data;
    node->bus_type = bus_type;
    strncpy(node->bus_id, bus_id, sizeof(node->bus_id) - 1);

    // Add to list
    node->next = g_device_list;
    g_device_list = node;

    debuglog(DEBUG_INFO, "[DEVFS] Registered dynamic device: %s (bus=%d, major=%d, minor=%d)\n",
             full_name, bus_type, major, minor);
    devfs_uevent_emit("add", (bus_type == DEV_BUS_USB) ? "usb" : "pci",
                      full_name, major, minor);

    return true;
}

/**
 * Unregister a dynamic device by bus information
 */
bool devfs_unregister_dynamic_device(dev_bus_t bus_type, const char* bus_id) {
    if (!bus_id || !g_devfs_initialized) {
        return false;
    }

    dev_node_t* prev = NULL;
    dev_node_t* current = g_device_list;

    while (current) {
        if (current->bus_type == bus_type && strcmp(current->bus_id, bus_id) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                g_device_list = current->next;
            }
            debuglog(DEBUG_INFO, "[DEVFS] Unregistered dynamic device: %s\n", current->name);
            devfs_uevent_emit("remove", (bus_type == DEV_BUS_USB) ? "usb" : "pci",
                              current->name, current->major, current->minor);
            enhanced_heap_free(current, "devfs_node");
            return true;
        }
        prev = current;
        current = current->next;
    }

    return false;
}

// ============================================================================
// Dynamic USB Device Registration
// ============================================================================

/**
 * Register a USB device
 */
bool devfs_register_usb_device(const char* bus_id, dev_type_t type, dev_ops_t* ops, void* private_data) {
    if (!bus_id || !g_devfs_initialized) {
        return false;
    }

    // Use USB major, minor based on bus_id or generic
    uint16 major = DEV_MAJOR_USB;
    uint16 minor = 0;  // TODO: Assign unique minor

    return devfs_register_dynamic_device(DEV_BUS_USB, bus_id, type, major, minor, ops, private_data);
}

/**
 * Unregister a USB device
 */
bool devfs_unregister_usb_device(const char* bus_id) {
    if (!bus_id) {
        return false;
    }

    return devfs_unregister_dynamic_device(DEV_BUS_USB, bus_id);
}

// ============================================================================
// Utility Functions
// ============================================================================
// Mouse Device Implementation (UNIX-like /dev/input/mice)
// ============================================================================

/**
 * Initialize the mouse device
 */
bool devfs_mouse_init(void) {
    if (!g_devfs_initialized) {
        if (!devfs_init()) {
            return false;
        }
    }

    // Clear buffer
    memory_set((uint8*)&g_mouse_buffer, 0, sizeof(g_mouse_buffer));

    // Register /dev/mouse device
    if (!devfs_register_device("mouse", DEV_TYPE_CHAR,
                                DEV_MAJOR_INPUT, DEV_MINOR_MICE,
                                &g_mouse_ops, &g_mouse_buffer)) {
        return false;
    }

    // Also register /dev/input/mice for compatibility
    devfs_register_device("input/mice", DEV_TYPE_CHAR,
                          DEV_MAJOR_INPUT, DEV_MINOR_MICE,
                          &g_mouse_ops, &g_mouse_buffer);

    g_mouse_device_ready = true;
    debuglog(DEBUG_INFO, "[DEVFS] Mouse device initialized (/dev/mouse)\n");
    print("[DEVFS] Mouse device initialized (/dev/mouse)\n");

    return true;
}

/**
 * Queue a mouse packet (called from PS/2 or USB mouse driver)
 */
void devfs_mouse_queue_packet(const mouse_packet_t* packet) {
    if (!packet || !g_mouse_device_ready) {
        return;
    }

    bool interrupts_enabled = irq_save_and_disable_safe();

    // Check for overflow
    if (g_mouse_buffer.count >= MOUSE_BUFFER_SIZE) {
        // Drop oldest packet
        g_mouse_buffer.head = (g_mouse_buffer.head + 1) % MOUSE_BUFFER_SIZE;
        g_mouse_buffer.count--;
        g_mouse_buffer.overflow = true;
    }

    // Add packet to buffer
    g_mouse_buffer.packets[g_mouse_buffer.tail] = *packet;
    g_mouse_buffer.tail = (g_mouse_buffer.tail + 1) % MOUSE_BUFFER_SIZE;
    g_mouse_buffer.count++;

    irq_restore_safe(interrupts_enabled);
}

/**
 * Read mouse data (ImPS/2 format)
 */
int devfs_mouse_read(void* buffer, uint32 size) {
    if (!buffer || size == 0 || !g_mouse_device_ready) {
        return -1;
    }

    bool interrupts_enabled = irq_save_and_disable_safe();

    uint32 bytes_read = 0;
    uint8* out = (uint8*)buffer;

    // Read packets (each is 3 or 4 bytes in ImPS/2 format)
    while (g_mouse_buffer.count > 0 && bytes_read + 3 <= size) {
        mouse_packet_t* pkt = &g_mouse_buffer.packets[g_mouse_buffer.head];

        // Output in ImPS/2 format (3 bytes basic, 4 with wheel)
        out[bytes_read++] = pkt->buttons;
        out[bytes_read++] = (uint8)pkt->dx;
        out[bytes_read++] = (uint8)pkt->dy;

        // Include wheel if there's room
        if (bytes_read < size) {
            out[bytes_read++] = (uint8)pkt->dz;
        }

        g_mouse_buffer.head = (g_mouse_buffer.head + 1) % MOUSE_BUFFER_SIZE;
        g_mouse_buffer.count--;
    }

    irq_restore_safe(interrupts_enabled);

    return (int)bytes_read;
}

/**
 * Check if mouse data is available
 */
bool devfs_mouse_data_available(void) {
    return g_mouse_device_ready && g_mouse_buffer.count > 0;
}

// Mouse device operations
static void mouse_dev_open(vfs_node_t* node, uint32 flags) {
    (void)node;
    (void)flags;
    // Nothing to do on open
}

static void mouse_dev_close(vfs_node_t* node) {
    (void)node;
    // Nothing to do on close
}

static uint32 mouse_dev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;
    int result = devfs_mouse_read(buffer, size);
    return (result < 0) ? 0 : (uint32)result;
}

static int mouse_dev_poll(vfs_node_t* node, uint32 events) {
    (void)node;
    int revents = 0;
    if ((events & POLLIN) && devfs_mouse_data_available()) {
        revents |= POLLIN;
    }
    return revents;
}

// ============================================================================
// Keyboard Device Implementation
// ============================================================================

/**
 * Initialize the keyboard device
 */
bool devfs_keyboard_init(void) {
    if (!g_devfs_initialized) {
        if (!devfs_init()) {
            return false;
        }
    }

    // Clear buffer
    memory_set((uint8*)&g_keyboard_buffer, 0, sizeof(g_keyboard_buffer));

    // Register /dev/keyboard device
    if (!devfs_register_device("keyboard", DEV_TYPE_CHAR,
                                DEV_MAJOR_INPUT, DEV_MINOR_KEYBOARD,
                                &g_keyboard_ops, &g_keyboard_buffer)) {
        return false;
    }

    g_keyboard_device_ready = true;
    debuglog(DEBUG_INFO, "[DEVFS] Keyboard device initialized (/dev/keyboard)\n");

    return true;
}

/**
 * Queue a keyboard scancode
 */
void devfs_keyboard_queue_scancode(uint8 scancode) {
    if (!g_keyboard_device_ready) {
        return;
    }

    bool interrupts_enabled = irq_save_and_disable_safe();

    // Check for overflow
    if (g_keyboard_buffer.count >= KEYBOARD_BUFFER_SIZE) {
        // Drop oldest scancode
        g_keyboard_buffer.head = (g_keyboard_buffer.head + 1) % KEYBOARD_BUFFER_SIZE;
        g_keyboard_buffer.count--;
        g_keyboard_buffer.overflow = true;
    }

    // Add scancode to buffer
    g_keyboard_buffer.scancodes[g_keyboard_buffer.tail] = scancode;
    g_keyboard_buffer.tail = (g_keyboard_buffer.tail + 1) % KEYBOARD_BUFFER_SIZE;
    g_keyboard_buffer.count++;

    irq_restore_safe(interrupts_enabled);
}

/**
 * Read keyboard data
 */
int devfs_keyboard_read(void* buffer, uint32 size) {
    if (!buffer || size == 0 || !g_keyboard_device_ready) {
        return -1;
    }

    bool interrupts_enabled = irq_save_and_disable_safe();

    uint32 bytes_read = 0;
    uint8* out = (uint8*)buffer;

    while (g_keyboard_buffer.count > 0 && bytes_read < size) {
        out[bytes_read++] = g_keyboard_buffer.scancodes[g_keyboard_buffer.head];
        g_keyboard_buffer.head = (g_keyboard_buffer.head + 1) % KEYBOARD_BUFFER_SIZE;
        g_keyboard_buffer.count--;
    }

    irq_restore_safe(interrupts_enabled);

    return (int)bytes_read;
}

/**
 * Check if keyboard data is available
 */
bool devfs_keyboard_data_available(void) {
    return g_keyboard_device_ready && g_keyboard_buffer.count > 0;
}

// Keyboard device operations
static void keyboard_dev_open(vfs_node_t* node, uint32 flags) {
    (void)node;
    (void)flags;
    // Nothing to do on open
}

static void keyboard_dev_close(vfs_node_t* node) {
    (void)node;
    // Nothing to do on close
}

static uint32 keyboard_dev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;
    int result = devfs_keyboard_read(buffer, size);
    return (result < 0) ? 0 : (uint32)result;
}

static int keyboard_dev_poll(vfs_node_t* node, uint32 events) {
    (void)node;
    int revents = 0;
    if ((events & POLLIN) && devfs_keyboard_data_available()) {
        revents |= POLLIN;
    }
    return revents;
}

// ============================================================================
// PS/2 Mouse Integration - Bridge to devfs
// ============================================================================

/**
 * PS/2 mouse callback to queue packets to devfs
 */
void devfs_ps2_mouse_callback(const ps2_mouse_event_t* event) {
    if (!event || !g_mouse_device_ready) {
        return;
    }

    mouse_packet_t packet;

    // Build packet in ImPS/2 format
    packet.buttons = 0;
    if (event->left_button) packet.buttons |= 0x01;
    if (event->right_button) packet.buttons |= 0x02;
    if (event->middle_button) packet.buttons |= 0x04;

    // Clamp deltas to signed 8-bit range
    packet.dx = (int8)(event->dx > 127 ? 127 : (event->dx < -127 ? -127 : event->dx));
    packet.dy = (int8)(event->dy > 127 ? 127 : (event->dy < -127 ? -127 : event->dy));
    packet.dz = 0;  // PS/2 scroll wheel would go here

    devfs_mouse_queue_packet(&packet);
}

// ============================================================================
// VFS Integration
// ============================================================================

/**
 * Build a VFS node wrapping a registered device (shared by devfs_open()
 * and the /dev directory's finddir()).
 */
static vfs_node_t* devfs_make_device_node(dev_node_t* dev, uint32 flags) {
    if (!dev) {
        return NULL;
    }

    // Create VFS node
    vfs_node_t* node = (vfs_node_t*)enhanced_heap_alloc(sizeof(vfs_node_t), "vfs_dev_node");
    if (!node) {
        return NULL;
    }

    memory_set((uint8*)node, 0, sizeof(vfs_node_t));
    strncpy(node->name, dev->name, sizeof(node->name) - 1);
    node->flags = (dev->type == DEV_TYPE_CHAR) ? VFS_CHARDEVICE : VFS_BLOCKDEVICE;
    node->major = dev->major;
    node->minor = dev->minor;
    node->internal_data = dev;

    // Set VFS operations to device operations
    if (dev->ops) {
        node->read = dev->ops->read;
        node->write = dev->ops->write;
        node->open = dev->ops->open;
        node->close = dev->ops->close;
        node->ioctl = dev->ops->ioctl;
        node->poll = dev->ops->poll;
    }

    // Call device open
    if (dev->ops && dev->ops->open) {
        dev->ops->open(node, flags);
    }

    return node;
}

/**
 * readdir() for the bare /dev directory node: enumerates every currently
 * registered device (name + type), mirroring initrd_readdir()'s contract
 * in vfs.c.
 */
static bool devfs_dir_readdir(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent) {
    (void)node;
    if (!dirent) {
        return false;
    }

    uint32 i = 0;
    for (dev_node_t* current = g_device_list; current; current = current->next, i++) {
        if (i != index) {
            continue;
        }

        strncpy(dirent->name, current->name, sizeof(dirent->name) - 1);
        dirent->name[sizeof(dirent->name) - 1] = '\0';
        dirent->inode = i;
        dirent->type = (current->type == DEV_TYPE_CHAR) ? VFS_CHARDEVICE : VFS_BLOCKDEVICE;
        return true;
    }

    return false;
}

/**
 * finddir() for the bare /dev directory node: look up a device by name
 * directly under /dev (e.g. "tty0", "null").
 */
static vfs_node_t* devfs_dir_finddir(vfs_node_t* node, const char* name) {
    (void)node;
    if (!name || !name[0]) {
        return NULL;
    }

    dev_node_t* dev = devfs_find_device(name);
    if (!dev) {
        return NULL;
    }

    return devfs_make_device_node(dev, 0);
}

/**
 * Build the directory VFS node representing /dev itself, so it can be
 * opened, listed (ls /dev), and stat'd like any other directory.
 */
static vfs_node_t* devfs_open_root_dir(void) {
    vfs_node_t* node = (vfs_node_t*)enhanced_heap_alloc(sizeof(vfs_node_t), "vfs_dev_node");
    if (!node) {
        return NULL;
    }

    memory_set((uint8*)node, 0, sizeof(vfs_node_t));
    strncpy(node->name, "dev", sizeof(node->name) - 1);
    node->flags = VFS_DIRECTORY;
    node->readdir = devfs_dir_readdir;
    node->finddir = devfs_dir_finddir;

    return node;
}

/**
 * Open a device file
 */
vfs_node_t* devfs_open(const char* path, uint32 flags) {
    if (!path || !g_devfs_initialized) {
        return NULL;
    }

    // Skip leading /dev/ if present
    const char* name = path;
    if (strcmp(path, "/dev") == 0 || strcmp(path, "dev") == 0) {
        name = "";
    } else if (strncmp(path, "/dev/", 5) == 0) {
        name = path + 5;
    } else if (strncmp(path, "dev/", 4) == 0) {
        name = path + 4;
    }

    // Bare /dev (no further path component): return a listable directory
    // node instead of failing, so `ls /dev` / `cd /dev` work like any
    // other directory.
    if (name[0] == '\0') {
        return devfs_open_root_dir();
    }

    // Find device
    dev_node_t* dev = devfs_find_device(name);
    if (!dev) {
        return NULL;
    }

    return devfs_make_device_node(dev, flags);
}

/**
 * Close a device file
 */
void devfs_close(vfs_node_t* node) {
    if (!node) {
        return;
    }

    dev_node_t* dev = (dev_node_t*)node->internal_data;
    if (dev && dev->ops && dev->ops->close) {
        dev->ops->close(node);
    }

    enhanced_heap_free(node, "vfs_dev_node");
}

// ============================================================================
// Character Device Registration (convenience wrapper)
// ============================================================================

/**
 * Register a character device using the chardev_t wrapper structure
 */
bool devfs_register_chardev(chardev_t* dev) {
    if (!dev || !dev->name || !g_devfs_initialized) {
        return false;
    }

    // Convert chardev_t to dev_ops_t pointer
    static dev_ops_t ops_storage[DEVFS_MAX_DEVICES];
    static uint32 ops_count = 0;

    if (ops_count >= DEVFS_MAX_DEVICES) {
        debuglog(DEBUG_ERROR, "[DEVFS] Too many chardev registrations\n");
        return false;
    }

    // Copy operations
    dev_ops_t* ops = &ops_storage[ops_count++];
    ops->read = dev->ops.read;
    ops->write = dev->ops.write;
    ops->open = dev->ops.open;
    ops->close = dev->ops.close;
    ops->ioctl = dev->ops.ioctl;
    ops->poll = dev->ops.poll;

    // Register using existing function
    if (!devfs_register_device(dev->name, DEV_TYPE_CHAR,
                                dev->major, dev->minor,
                                ops, dev->driver_data)) {
        ops_count--;
        return false;
    }

    dev->registered = true;
    return true;
}

/**
 * Unregister a character device
 */
bool devfs_unregister_chardev(chardev_t* dev) {
    if (!dev || !dev->name || !dev->registered) {
        return false;
    }

    if (devfs_unregister_device(dev->name)) {
        dev->registered = false;
        return true;
    }

    return false;
}

// ============================================================================
// Input Device Integration (evdev-style /dev/kbd and /dev/mouse)
// ============================================================================

/* Input event ring buffers for /dev/kbd and /dev/mouse */
static input_ring_t g_kbd_event_ring;
static input_ring_t g_mouse_event_ring;
static bool g_input_devices_initialized = false;

/* Forward declarations for input device operations */
static uint32 kbd_evdev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 kbd_evdev_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static void kbd_evdev_open(vfs_node_t* node, uint32 flags);
static void kbd_evdev_close(vfs_node_t* node);
static int kbd_evdev_ioctl(vfs_node_t* node, uint32 request, void* arg);
static int kbd_evdev_poll(vfs_node_t* node, uint32 events);

static uint32 mouse_evdev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 mouse_evdev_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static void mouse_evdev_open(vfs_node_t* node, uint32 flags);
static void mouse_evdev_close(vfs_node_t* node);
static int mouse_evdev_ioctl(vfs_node_t* node, uint32 request, void* arg);
static int mouse_evdev_poll(vfs_node_t* node, uint32 events);

/* Device operations for evdev-style input devices */
static dev_ops_t g_kbd_evdev_ops = {
    .read = kbd_evdev_read,
    .write = kbd_evdev_write,
    .open = kbd_evdev_open,
    .close = kbd_evdev_close,
    .ioctl = kbd_evdev_ioctl,
    .poll = kbd_evdev_poll
};

static dev_ops_t g_mouse_evdev_ops = {
    .read = mouse_evdev_read,
    .write = mouse_evdev_write,
    .open = mouse_evdev_open,
    .close = mouse_evdev_close,
    .ioctl = mouse_evdev_ioctl,
    .poll = mouse_evdev_poll
};

/**
 * Initialize input devices (/dev/kbd, /dev/mouse) with evdev-style interface
 */
bool devfs_input_init(void) {
    if (!g_devfs_initialized) {
        if (!devfs_init()) {
            return false;
        }
    }

    if (g_input_devices_initialized) {
        return true;
    }

    /* Initialize event ring buffers */
    input_ring_init(&g_kbd_event_ring, "kbd_events");
    input_ring_init(&g_mouse_event_ring, "mouse_events");

    /* Register /dev/kbd - keyboard event device */
    if (!devfs_register_device(DEV_PATH_KBD, DEV_TYPE_CHAR,
                                DEV_MAJOR_INPUT, DEV_MINOR_KBD,
                                &g_kbd_evdev_ops, &g_kbd_event_ring)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/kbd\n");
        return false;
    }

    /* Register /dev/mouse - mouse event device */
    if (!devfs_register_device(DEV_PATH_MOUSE, DEV_TYPE_CHAR,
                                DEV_MAJOR_INPUT, DEV_MINOR_MOUSE,
                                &g_mouse_evdev_ops, &g_mouse_event_ring)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/mouse\n");
        return false;
    }

    /* Also register /dev/input/event0 and /dev/input/event1 for compatibility */
    devfs_register_device(DEV_PATH_INPUT_EVENT0, DEV_TYPE_CHAR,
                          DEV_MAJOR_INPUT, DEV_MINOR_EVENT0,
                          &g_kbd_evdev_ops, &g_kbd_event_ring);
    devfs_register_device(DEV_PATH_INPUT_EVENT1, DEV_TYPE_CHAR,
                          DEV_MAJOR_INPUT, DEV_MINOR_EVENT1,
                          &g_mouse_evdev_ops, &g_mouse_event_ring);

    /* Register PS2-specific device files */
    if (!devfs_register_device(DEV_PATH_PS2KBD, DEV_TYPE_CHAR,
                               DEV_MAJOR_INPUT, DEV_MINOR_PS2KBD,
                               &g_kbd_evdev_ops, &g_kbd_event_ring)) {
        debuglog(DEBUG_WARN, "[DEVFS] Failed to register /dev/ps2kbd\n");
    }

    if (!devfs_register_device(DEV_PATH_PS2MOUSE, DEV_TYPE_CHAR,
                               DEV_MAJOR_INPUT, DEV_MINOR_PS2MOUSE,
                               &g_mouse_evdev_ops, &g_mouse_event_ring)) {
        debuglog(DEBUG_WARN, "[DEVFS] Failed to register /dev/ps2mouse\n");
    }

    /* Register /dev/psaux (PS2 auxiliary device - traditional mouse device) */
    if (!devfs_register_device(DEV_PATH_PSAUX, DEV_TYPE_CHAR,
                               DEV_MAJOR_INPUT, DEV_MINOR_PSAUX,
                               &g_mouse_evdev_ops, &g_mouse_event_ring)) {
        debuglog(DEBUG_WARN, "[DEVFS] Failed to register /dev/psaux\n");
    }

    g_input_devices_initialized = true;
    debuglog(DEBUG_INFO, "[DEVFS] Input devices initialized (/dev/kbd, /dev/mouse, /dev/ps2kbd, /dev/ps2mouse, /dev/psaux)\n");
    print("[DEVFS] Input devices initialized (/dev/kbd, /dev/mouse, /dev/ps2kbd, /dev/ps2mouse, /dev/psaux)\n");

    return true;
}

/**
 * Shutdown input devices
 */
void devfs_input_shutdown(void) {
    if (!g_input_devices_initialized) return;

    devfs_unregister_device(DEV_PATH_KBD);
    devfs_unregister_device(DEV_PATH_MOUSE);
    devfs_unregister_device(DEV_PATH_PS2KBD);
    devfs_unregister_device(DEV_PATH_PS2MOUSE);
    devfs_unregister_device(DEV_PATH_PSAUX);
    devfs_unregister_device(DEV_PATH_INPUT_EVENT0);
    devfs_unregister_device(DEV_PATH_INPUT_EVENT1);

    g_input_devices_initialized = false;
}

/**
 * Queue an input event to the keyboard device
 * Called from PS/2 keyboard driver IRQ handler
 */
void devfs_kbd_queue_event(const input_event_t* event) {
    if (!event || !g_input_devices_initialized) return;
    input_ring_push(&g_kbd_event_ring, event);
}

/**
 * Queue an input event to the mouse device
 * Called from PS/2 mouse driver IRQ handler
 */
void devfs_mouse_queue_event(const input_event_t* event) {
    if (!event || !g_input_devices_initialized) return;
    input_ring_push(&g_mouse_event_ring, event);
}

/**
 * Get keyboard event ring buffer (for direct access)
 */
input_ring_t* devfs_get_kbd_ring(void) {
    return g_input_devices_initialized ? &g_kbd_event_ring : NULL;
}

/**
 * Get mouse event ring buffer (for direct access)
 */
input_ring_t* devfs_get_mouse_ring(void) {
    return g_input_devices_initialized ? &g_mouse_event_ring : NULL;
}

/* Keyboard evdev device operations */
static uint32 kbd_evdev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0 || !g_input_devices_initialized) {
        return 0;
    }

    uint32 bytes_read = 0;
    input_event_t event;

    /* Read as many complete events as will fit */
    while (bytes_read + sizeof(input_event_t) <= size) {
        if (!input_ring_pop(&g_kbd_event_ring, &event)) {
            break;  /* No more events */
        }

        memcpy(buffer + bytes_read, &event, sizeof(input_event_t));
        bytes_read += sizeof(input_event_t);
    }

    return bytes_read;
}

static uint32 kbd_evdev_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    /* Writing to keyboard is not supported (could be used for LED control) */
    return 0;
}

static void kbd_evdev_open(vfs_node_t* node, uint32 flags) {
    (void)node;
    (void)flags;
}

static void kbd_evdev_close(vfs_node_t* node) {
    (void)node;
}

static int kbd_evdev_ioctl(vfs_node_t* node, uint32 request, void* arg) {
    (void)node;

    switch (request) {
        case EVIOCGVERSION:
            if (arg) *(int*)arg = 0x010001;  /* Version 1.0.1 */
            return IOCTL_SUCCESS;

        case KDGETLED:
            /* TODO: Get actual LED state from PS/2 keyboard driver */
            if (arg) *(int*)arg = 0;
            return IOCTL_SUCCESS;

        case KDSETLED:
            /* TODO: Set LED state via PS/2 keyboard driver */
            return IOCTL_SUCCESS;

        default:
            return IOCTL_ENOTTY;
    }
}

static int kbd_evdev_poll(vfs_node_t* node, uint32 events) {
    (void)node;
    int revents = 0;

    if (events & POLLIN) {
        if (!input_ring_is_empty(&g_kbd_event_ring)) {
            revents |= POLLIN;
        }
    }

    return revents;
}

/* Mouse evdev device operations */
static uint32 mouse_evdev_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0 || !g_input_devices_initialized) {
        return 0;
    }

    uint32 bytes_read = 0;
    input_event_t event;

    /* Read as many complete events as will fit */
    while (bytes_read + sizeof(input_event_t) <= size) {
        if (!input_ring_pop(&g_mouse_event_ring, &event)) {
            break;  /* No more events */
        }

        memcpy(buffer + bytes_read, &event, sizeof(input_event_t));
        bytes_read += sizeof(input_event_t);
    }

    return bytes_read;
}

static uint32 mouse_evdev_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    /* Writing to mouse is not supported */
    return 0;
}

static void mouse_evdev_open(vfs_node_t* node, uint32 flags) {
    (void)node;
    (void)flags;
}

static void mouse_evdev_close(vfs_node_t* node) {
    (void)node;
}

static int mouse_evdev_ioctl(vfs_node_t* node, uint32 request, void* arg) {
    (void)node;

    switch (request) {
        case EVIOCGVERSION:
            if (arg) *(int*)arg = 0x010001;  /* Version 1.0.1 */
            return IOCTL_SUCCESS;

        case MOUSEGETID:
            /* TODO: Get actual device ID from PS/2 mouse driver */
            if (arg) *(int*)arg = 0;  /* Standard PS/2 mouse */
            return IOCTL_SUCCESS;

        default:
            return IOCTL_ENOTTY;
    }
}

static int mouse_evdev_poll(vfs_node_t* node, uint32 events) {
    (void)node;
    int revents = 0;

    if (events & POLLIN) {
        if (!input_ring_is_empty(&g_mouse_event_ring)) {
            revents |= POLLIN;
        }
    }

    return revents;
}

// ============================================================================
// Framebuffer Device Implementation (/dev/fb_width, /dev/fb_height, /dev/fb_pitch)
// ============================================================================

#include "include/graphics/graphics_manager.h"

static bool g_fb_devices_initialized = false;

/* Forward declarations for FB device operations */
static uint32 fb_width_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_height_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_pitch_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_bpp_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_addr_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_size_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);

// Cursor device function declarations
static uint32 cursor_pos_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 cursor_pos_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 cursor_visible_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 cursor_visible_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);

// Framebuffer memory mapping device function declarations
static uint32 fb_mmap_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_mmap_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);


static dev_ops_t g_fb_width_ops = {
    .read = fb_width_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_height_ops = {
    .read = fb_height_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_pitch_ops = {
    .read = fb_pitch_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_bpp_ops = {
    .read = fb_bpp_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_addr_ops = {
    .read = fb_addr_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_size_ops = {
    .read = fb_size_read,
    .write = NULL,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

/* Forward declarations for FB control device operations */
static uint32 fb_double_buffer_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_double_buffer_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_swap_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb_swap_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);

static dev_ops_t g_fb_double_buffer_ops = {
    .read = fb_double_buffer_read,
    .write = fb_double_buffer_write,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_swap_ops = {
    .read = fb_swap_read,
    .write = fb_swap_write,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};



/**
 * Read framebuffer width
 * Returns width as 4-byte little-endian uint32
 */
static uint32 fb_width_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for these control devices

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    // Always refresh to avoid stale metadata after mode/buffer changes.
    if (!snapshot_fb_cache(fb_index, &fb_cache)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_width_read: No framebuffer available\n");
        return 0;
    }

    // Validate framebuffer dimensions
    if (fb_cache.width == 0 || fb_cache.width > 16384) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_width_read: Invalid width %u\n", fb_cache.width);
        return 0;
    }

    uint32 bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(buffer, &fb_cache.width, bytes_to_copy);

    return bytes_to_copy;
}

// ============================================================================
// Framebuffer Memory Mapping Device (/dev/fb_mmap)
// ============================================================================

/**
 * Read from framebuffer memory directly
 * Provides direct memory access to the framebuffer
 */
static uint32 fb_mmap_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    // Validate parameters
    if (!buffer || size == 0) return 0;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_mmap_read: No framebuffer available\n");
        return 0;
    }
    if (!fb_cache.virtual_addr || fb_cache.size == 0) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_mmap_read: Invalid framebuffer cache state\n");
        return 0;
    }

    // Fast bounds check using cached values
    if (offset >= fb_cache.size) {
        return 0;  // Read beyond framebuffer
    }

    // Optimize for common case: read within bounds
    size_t available = fb_cache.size - offset;
    uint32 to_read = (size > available) ? (uint32)available : size;

    // Use optimized memory copy for large transfers
    if (to_read >= 1024) {
        // For large transfers, use optimized copy if available
        memcpy(buffer, (void*)(fb_cache.virtual_addr + offset), to_read);
    } else {
        // For small transfers, use standard copy
        memcpy(buffer, (void*)(fb_cache.virtual_addr + offset), to_read);
    }

    return to_read;
}

/**
 * Write to framebuffer memory directly
 * Provides direct memory access to the framebuffer
 */
static uint32 fb_mmap_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    // Validate parameters
    if (!buffer || size == 0) return 0;

    if (framebuffer_has_userspace_mapping()) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_mmap_write: denied while SYS_MMAP_FB mapping is active\n");
        return 0;
    }

    if (!snapshot_fb_cache(fb_index, &fb_cache)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_mmap_write: No framebuffer available\n");
        return 0;
    }
    if (!fb_cache.virtual_addr || fb_cache.size == 0) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_mmap_write: Invalid framebuffer cache state\n");
        return 0;
    }

    // Fast bounds check using cached values
    if (offset >= fb_cache.size) {
        return 0;  // Write beyond framebuffer
    }

    // Optimize for common case: write within bounds
    size_t available = fb_cache.size - offset;
    uint32 to_write = (size > available) ? (uint32)available : size;

    // Use optimized memory copy for large transfers
    if (to_write >= 1024) {
        // For large transfers, use optimized copy if available
        // (could be replaced with SIMD operations in the future)
        memcpy((void*)(fb_cache.virtual_addr + offset), buffer, to_write);
    } else {
        // For small transfers, direct copy is fine
        memcpy((void*)(fb_cache.virtual_addr + offset), buffer, to_write);
    }

    __asm__ volatile("mfence" ::: "memory");
    return to_write;
}

/**
 * Initialize framebuffer info devices
 */
static uint32 fb_height_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for these control devices

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_height_read: No framebuffer available\n");
        return 0;
    }

    // Validate framebuffer dimensions
    if (fb_cache.height == 0 || fb_cache.height > 16384) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_height_read: Invalid height %u\n", fb_cache.height);
        return 0;
    }

    uint32 bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(buffer, &fb_cache.height, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Read framebuffer pitch
 * Returns pitch (bytes per row) as 4-byte little-endian uint32
 */
static uint32 fb_pitch_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) return 0;

    uint32 bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(buffer, &fb_cache.pitch, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Read framebuffer bits per pixel
 * Returns bpp as 4-byte little-endian uint32
 */
static uint32 fb_bpp_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) return 0;

    uint32 bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(buffer, &fb_cache.bpp, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Read framebuffer address
 * Returns address as 8-byte little-endian uint64
 */
static uint32 fb_addr_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    // Validate parameters
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for these control devices

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_addr_read: No framebuffer available\n");
        return 0;
    }

    /* CRITICAL FIX: Return PHYSICAL address for userspace mmap
     * Userspace cannot use kernel virtual addresses directly */
    if (fb_cache.physical_addr == 0) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_addr_read: Invalid framebuffer physical address\n");
        return 0;
    }

    uint64 addr = (uint64)fb_cache.physical_addr;
    uint32 bytes_to_copy = (size < sizeof(uint64)) ? size : sizeof(uint64);
    memcpy(buffer, &addr, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Read framebuffer size in bytes
 * Returns size as 8-byte little-endian uint64
 */
static uint32 fb_size_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) return 0;

    uint64 fb_size = (uint64)fb_cache.size;
    uint32 bytes_to_copy = (size < sizeof(uint64)) ? size : sizeof(uint64);
    memcpy(buffer, &fb_size, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Read double buffering state (1 = enabled, 0 = disabled)
 */
static uint32 fb_double_buffer_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    bool valid = snapshot_fb_cache(fb_index, &fb_cache);
    uint32 state = (valid && fb_cache.double_buffered) ? 1 : 0;
    uint32 bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(buffer, &state, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Write double buffering state (1 = enable, 0 = disable)
 */
static uint32 fb_double_buffer_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    // Validate parameters
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for control devices

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;

    uint32 state = 0;
    if (!parse_u32_control_write(size, buffer, &state)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_double_buffer_write: Need %u bytes\n", (uint32)sizeof(uint32));
        return 0;
    }

    // Only allow 0 or 1 as valid states
    if (state > 1) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_double_buffer_write: Invalid state %u, must be 0 or 1\n", state);
        return 0;
    }

    if (framebuffer_has_userspace_mapping()) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_double_buffer_write: denied while SYS_MMAP_FB mapping is active\n");
        return 0;
    }

    fb_cache_entry_t before_cache = {0};
    snapshot_fb_cache(fb_index, &before_cache);

    graphics_result_t result = graphics_enable_double_buffering(state != 0);
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_double_buffer_write: Failed to %s double buffering\n",
                 state ? "enable" : "disable");
        return 0;
    }

    if (state != 0) {
        // Enabling double buffering - record the owner
        framebuffer_mmap_set_double_buffer_owner(current_task ? current_task->id : 0);
    } else {
        // Disabling double buffering - clear the owner
        framebuffer_mmap_set_double_buffer_owner(0);
    }

    framebuffer_mmap_refresh();

    // Invalidate cache since framebuffer properties may have changed
    spinlock_acquire(&g_fb_cache_lock);
    for (int i = 0; i < MAX_FB_DEVICES; i++) {
        g_fb_cache[i].valid = false;
    }
    spinlock_release(&g_fb_cache_lock);

    fb_cache_entry_t after_cache = {0};
    if (snapshot_fb_cache(fb_index, &after_cache)) {
        bool requested = (state != 0);
        if (after_cache.double_buffered != requested &&
            before_cache.valid &&
            before_cache.double_buffered != requested) {
            debuglog(DEBUG_WARN,
                     "[DEVFS] fb_double_buffer_write: backend did not apply requested state=%u\n",
                     state);
            return 0;
        }
    }

    debuglog(DEBUG_INFO, "[DEVFS] fb_double_buffer_write: %s double buffering\n", 
             state ? "Enabled" : "Disabled");
    
    return sizeof(uint32);
}

/**
 * Read swap buffer (not really readable, but for consistency)
 */
static uint32 fb_swap_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;
    (void)buffer;
    (void)size;
    return 0;
}

/**
 * Write to swap buffer (any write triggers swap)
 */
static uint32 fb_swap_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)buffer;
    fb_cache_entry_t fb_cache;

    // Validate parameters
    if (size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for control devices

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) {
        debuglog(DEBUG_WARN, "[DEVFS] fb_swap_write: No framebuffer available\n");
        return 0;
    }
    
    /*
     * /dev/fb_mmap and /dev/fb0 writes target the front framebuffer directly.
     * Triggering kernel swap here can overwrite userspace output with a stale
     * kernel-managed back buffer. Treat /dev/fb_swap as a flush barrier only.
     */
    __asm__ volatile("mfence" ::: "memory");
    return size;
}



// ============================================================================
// Cursor Device Operations
// ============================================================================

static dev_ops_t g_cursor_pos_ops = {
    .read = cursor_pos_read,
    .write = cursor_pos_write,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_cursor_visible_ops = {
    .read = cursor_visible_read,
    .write = cursor_visible_write,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

static dev_ops_t g_fb_mmap_ops = {
    .read = fb_mmap_read,
    .write = fb_mmap_write,
    .open = NULL,
    .close = NULL,
    .ioctl = NULL,
    .poll = NULL
};

/**
 * Initialize framebuffer info devices
 */
bool devfs_fb_init(void) {
    if (!g_devfs_initialized) {
        if (!devfs_init()) {
            return false;
        }
    }

    if (g_fb_devices_initialized) {
        return true;
    }

    /* Register /dev/fb_width */
    if (!devfs_register_device(DEV_PATH_FB_WIDTH, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_WIDTH,
                                &g_fb_width_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_width\n");
        return false;
    }

    /* Register /dev/fb_height */
    if (!devfs_register_device(DEV_PATH_FB_HEIGHT, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_HEIGHT,
                                &g_fb_height_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_height\n");
        return false;
    }

    /* Register /dev/fb_pitch */
    if (!devfs_register_device(DEV_PATH_FB_PITCH, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_PITCH,
                                &g_fb_pitch_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_pitch\n");
        return false;
    }

    /* Register /dev/fb_bpp */
    if (!devfs_register_device(DEV_PATH_FB_BPP, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_BPP,
                                &g_fb_bpp_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_bpp\n");
        return false;
    }

    /* Register /dev/fb_addr */
    if (!devfs_register_device(DEV_PATH_FB_ADDR, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_ADDR,
                                &g_fb_addr_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_addr\n");
        return false;
    }

    /* Register /dev/fb_size */
    if (!devfs_register_device(DEV_PATH_FB_SIZE, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_SIZE,
                                &g_fb_size_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_size\n");
        return false;
    }

    /* Register /dev/fb_double_buffer */
    if (!devfs_register_device(DEV_PATH_FB_DOUBLE_BUFFER, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_DOUBLE_BUFFER,
                                &g_fb_double_buffer_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_double_buffer\n");
        return false;
    }

/* Register /dev/fb_swap */
    if (!devfs_register_device(DEV_PATH_FB_SWAP, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_SWAP,
                                &g_fb_swap_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_swap\n");
        return false;
    }

    /* Register /dev/cursor_pos */
    if (!devfs_register_device(DEV_PATH_CURSOR_POS, DEV_TYPE_CHAR,
                                DEV_MAJOR_CURSOR, DEV_MINOR_CURSOR_POS,
                                &g_cursor_pos_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/cursor_pos\n");
        return false;
    }

    /* Register /dev/cursor_visible */
    if (!devfs_register_device(DEV_PATH_CURSOR_VISIBLE, DEV_TYPE_CHAR,
                                DEV_MAJOR_CURSOR, DEV_MINOR_CURSOR_VISIBLE,
                                &g_cursor_visible_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/cursor_visible\n");
        return false;
    }

    /* Register /dev/fb_mmap */
    if (!devfs_register_device(DEV_PATH_FB_MMAP, DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB_MMAP,
                                &g_fb_mmap_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb_mmap\n");
        return false;
    }

    g_fb_devices_initialized = true;
    debuglog(DEBUG_INFO, "[DEVFS] Framebuffer, cursor, and memory mapping devices initialized (/dev/fb_width, /dev/fb_height, /dev/fb_pitch, /dev/fb_bpp, /dev/fb_addr, /dev/fb_size, /dev/fb_double_buffer, /dev/fb_swap, /dev/cursor_pos, /dev/cursor_visible, /dev/fb_mmap)\n");

    return true;
}

/**
 * Shutdown framebuffer info devices
 */
void devfs_fb_shutdown(void) {
    if (!g_fb_devices_initialized) return;

    devfs_unregister_device(DEV_PATH_FB_WIDTH);
    devfs_unregister_device(DEV_PATH_FB_HEIGHT);
    devfs_unregister_device(DEV_PATH_FB_PITCH);
    devfs_unregister_device(DEV_PATH_FB_BPP);
    devfs_unregister_device(DEV_PATH_FB_ADDR);
    devfs_unregister_device(DEV_PATH_FB_SIZE);
    devfs_unregister_device(DEV_PATH_SCRX);
    devfs_unregister_device(DEV_PATH_SCRY);
    devfs_unregister_device(DEV_PATH_FB_DOUBLE_BUFFER);
    devfs_unregister_device(DEV_PATH_FB_SWAP);
    devfs_unregister_device(DEV_PATH_CURSOR_POS);
    devfs_unregister_device(DEV_PATH_CURSOR_VISIBLE);
    devfs_unregister_device(DEV_PATH_FB_MMAP);

    g_fb_devices_initialized = false;
}

// ============================================================================
// Framebuffer Device Implementation (/dev/fb0)
// ============================================================================

static bool g_fb0_initialized = false;

/* Forward declarations for FB0 device operations */
static void fb0_open(vfs_node_t* node, uint32 flags);
static void fb0_close(vfs_node_t* node);
static uint32 fb0_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 fb0_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static int fb0_ioctl(vfs_node_t* node, uint32 request, void* arg);
static int fb0_mmap(vfs_node_t* node, void* addr, size_t len, uint32_t prot, uint64_t offset);

static dev_ops_t g_fb0_ops = {
    .open = fb0_open,
    .close = fb0_close,
    .read = fb0_read,
    .write = fb0_write,
    .ioctl = fb0_ioctl,
    .poll = NULL
};

/**
 * Open framebuffer device
 */
static void fb0_open(vfs_node_t* node, uint32 flags) {
    (void)flags;
    (void)node;

    // Framebuffer is always available once graphics is initialized
}



// ============================================================================
// Framebuffer Device Support (/dev/fb*)
// ============================================================================

/**
 * Update framebuffer cache - called when framebuffer properties might have changed
 */
static void update_fb_cache(int fb_index) {
    if (fb_index < 0 || fb_index >= MAX_FB_DEVICES) {
        return;
    }

    framebuffer_t* fb = graphics_get_framebuffer();  // TODO: Support multiple framebuffers
    if (!fb) {
        g_fb_cache[fb_index].valid = false;
        return;
    }

    fb_cache_entry_t entry = {0};
    entry.width = fb->width;
    entry.height = fb->height;
    entry.pitch = fb->pitch;
    entry.bpp = fb->bpp;
    entry.virtual_addr = fb->virtual_addr;
    entry.physical_addr = fb->physical_addr;

    /* Keep exported bpp consistent with stride when firmware metadata is wrong. */
    if (entry.width != 0 &&
        entry.pitch >= entry.width &&
        (entry.pitch % entry.width) == 0) {
        uint32_t stride_bpp = entry.pitch / entry.width;
        if (stride_bpp >= 1 && stride_bpp <= 4) {
            uint32_t declared_bpp = (entry.bpp + 7) / 8;
            if (declared_bpp != stride_bpp) {
                debuglog(DEBUG_WARN,
                         "[DEVFS] FB bpp mismatch: reported=%u (%u Bpp), pitch/width=%u Bpp; normalizing\n",
                         entry.bpp, declared_bpp, stride_bpp);
                entry.bpp = stride_bpp * 8;
            }
        }
    }

    /*
     * Expose only visible framebuffer bytes, not full VRAM, to prevent
     * /dev/fb* reads/writes from touching memory outside the active surface.
     */
    size_t visible_size = (size_t)fb->pitch * (size_t)fb->height;
    if (fb->pitch != 0 && visible_size / fb->pitch != fb->height) {
        visible_size = 0;  // overflow guard
    }
    if (visible_size == 0) {
        visible_size = fb->size;
    }
    if (fb->size == 0 || fb->size > visible_size) {
        entry.size = visible_size;
    } else {
        entry.size = fb->size;
    }
    entry.double_buffered = fb->double_buffered;
    entry.valid = true;
    g_fb_cache[fb_index] = entry;
}

/**
 * Read framebuffer width (cached for performance)
 * Returns width as 4-byte little-endian uint32
 */
static uint32 cursor_pos_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;

    // Validate parameters
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for position devices

    // Get current mouse position from input system
    int32_t mouse_x = 0, mouse_y = 0;
    // TODO: Interface with input subsystem to get current mouse position
    // For now, return 0,0 as placeholder
    
    // Return x,y as two consecutive uint32 values
    if (size >= sizeof(uint32) * 2) {
        uint32_t pos[2] = {(uint32_t)mouse_x, (uint32_t)mouse_y};
        uint32_t bytes_to_copy = (size < sizeof(pos)) ? size : sizeof(pos);
        memcpy(buffer, pos, bytes_to_copy);
        return bytes_to_copy;
    } else if (size >= sizeof(uint32)) {
        // Only return x coordinate if buffer too small for both
        uint32_t x = (uint32_t)mouse_x;
        memcpy(buffer, &x, sizeof(x));
        return sizeof(x);
    }
    
    return 0;
}

/**
 * Write cursor position (set x, y from uint32 values)
 */
static uint32 cursor_pos_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;

    // Validate parameters
    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for position devices

    // Expect at least 2 uint32 values for x,y
    if (size < sizeof(uint32) * 2) {
        debuglog(DEBUG_WARN, "[DEVFS] cursor_pos_write: Insufficient data, need 8 bytes for x,y\n");
        return 0;
    }

    uint32_t pos[2];
    memcpy(pos, buffer, sizeof(pos));
    
    // TODO: Interface with input subsystem to set mouse position
    // For now, just log the position
    debuglog(DEBUG_INFO, "[DEVFS] cursor_pos_write: Set cursor position to (%u, %u)\n", pos[0], pos[1]);
    
    return sizeof(pos);
}

/**
 * Read cursor visibility state
 */
static uint32 cursor_visible_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0) return 0;

    // Check if cursor is visible
    bool visible = true; // TODO: Interface with input subsystem
    uint32_t state = visible ? 1 : 0;
    uint32_t bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(buffer, &state, bytes_to_copy);

    return bytes_to_copy;
}

/**
 * Write cursor visibility state (1 = show, 0 = hide)
 */
static uint32 cursor_visible_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0) return 0;
    if (offset != 0) return 0;  // Only allow offset 0 for control devices

    uint32_t state;
    uint32_t bytes_to_copy = (size < sizeof(uint32)) ? size : sizeof(uint32);
    memcpy(&state, buffer, bytes_to_copy);

    // Only allow 0 or 1 as valid states
    if (state > 1) {
        debuglog(DEBUG_WARN, "[DEVFS] cursor_visible_write: Invalid state %u, must be 0 or 1\n", state);
        return 0;
    }

    // TODO: Interface with graphics subsystem to show/hide cursor
    debuglog(DEBUG_INFO, "[DEVFS] cursor_visible_write: %s cursor\n", state ? "Show" : "Hide");
    
    return bytes_to_copy;
}



/**
 * Read from framebuffer device
 */
static uint32 fb0_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache) || !fb_cache.virtual_addr || fb_cache.size == 0) {
        return 0;
    }

    // Limit read to framebuffer size
    if (offset >= fb_cache.size) {
        return 0;
    }

    size_t available = fb_cache.size - offset;
    uint32 to_read = (size > available) ? (uint32)available : size;

    memcpy(buffer, (void*)(fb_cache.virtual_addr + offset), to_read);
    return to_read;
}

/**
 * Write to framebuffer device
 */
static uint32 fb0_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!buffer || size == 0) return 0;

    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return 0;
    fb_cache_entry_t fb_cache;

    if (framebuffer_has_userspace_mapping()) {
        return 0;
    }

    if (!snapshot_fb_cache(fb_index, &fb_cache) || !fb_cache.virtual_addr || fb_cache.size == 0) {
        return 0;
    }

    // Limit write to framebuffer size
    if (offset >= fb_cache.size) {
        return 0;
    }

    size_t available = fb_cache.size - offset;
    uint32 to_write = (size > available) ? (uint32)available : size;

    memcpy((void*)(fb_cache.virtual_addr + offset), buffer, to_write);
    __asm__ volatile("mfence" ::: "memory");
    return to_write;
}

/**
 * IOCTL for framebuffer device
 */
/* Defined in syscall.c: shared implementation for SYS_SET_FB_MODE, so the
 * ioctl entry point below goes through the exact same DM/root permission
 * check, graphics_set_mode() hot-swap, and TTY cell-buffer resize as the
 * syscall does. */
extern long sys_set_fb_mode(uint32_t width, uint32_t height, uint32_t bpp);

static int fb0_ioctl(vfs_node_t* node, uint32 request, void* arg) {
    int fb_index = get_fb_index(node->major, node->minor);
    if (fb_index < 0) return -1;
    fb_cache_entry_t fb_cache;

    if (!snapshot_fb_cache(fb_index, &fb_cache)) return -1;

    switch (request) {
        case 0x4600: // FBIOGET_VSCREENINFO (get variable screen info)
            if (arg) {
                // Simplified: just return basic info
                struct fb_var_screeninfo {
                    uint32 xres;
                    uint32 yres;
                    uint32 xres_virtual;
                    uint32 yres_virtual;
                    uint32 bits_per_pixel;
                    uint32 pitch;
                } *var = arg;

                memory_set((uint8*)var, 0, sizeof(*var));
                var->xres = fb_cache.width;
                var->yres = fb_cache.height;
                var->xres_virtual = fb_cache.width;
                var->yres_virtual = fb_cache.height;
                var->bits_per_pixel = fb_cache.bpp;
                var->pitch = fb_cache.pitch;
                return 0;
            }
            break;

        case 0x4601: // FBIOPUT_VSCREENINFO (set variable screen info -> runtime mode change)
            if (arg) {
                struct fb_var_screeninfo {
                    uint32 xres;
                    uint32 yres;
                    uint32 xres_virtual;
                    uint32 yres_virtual;
                    uint32 bits_per_pixel;
                    uint32 pitch;
                } *var = arg;

                /* Same permission check, mode-set, and TTY resize as
                 * SYS_SET_FB_MODE -- see syscall.c for why this only
                 * actually succeeds against a BGA-capable (QEMU/Bochs/
                 * VirtualBox) device and -ENOTSUP's on bare VESA-only
                 * hardware. */
                long result = sys_set_fb_mode(var->xres, var->yres, var->bits_per_pixel);
                return (result == 0) ? 0 : -1;
            }
            break;

        case 0x4602: // FBIOGET_FSCREENINFO (get fixed screen info)
            if (arg) {
                struct fb_fix_screeninfo {
                    uintptr_t smem_start;
                    size_t smem_len;
                } *fix = arg;

                /* CRITICAL FIX: Return PHYSICAL address, not virtual address
                 * Userspace needs physical address for proper mmap */
                memory_set((uint8*)fix, 0, sizeof(*fix));
                fix->smem_start = fb_cache.physical_addr;
                fix->smem_len = fb_cache.size;
                return 0;
            }
            break;

        case 0x4603: // FBIOMMAP (mmap framebuffer into userspace)
            (void)arg;
            debuglog(DEBUG_WARN, "[DEVFS] fb0_ioctl: FBIOMMAP unsupported, use SYS_MMAP_FB\n");
            return -1;
    }

    return -1; // Unsupported ioctl
}

/**
 * Close framebuffer device (stub implementation)
 */
static void fb0_close(vfs_node_t* node) {
    (void)node;
    // Nothing to do for framebuffer close
}

/**
 * Memory map framebuffer device (stub implementation)
 */
__attribute__((unused)) static int fb0_mmap(vfs_node_t* node, void* addr, size_t len, uint32_t prot, uint64_t offset) {
    (void)node; (void)addr; (void)len; (void)prot; (void)offset;
    // TODO: Implement proper framebuffer mmap
    return -1;
}

/**
 * Initialize /dev/fb0 framebuffer device
 */
bool devfs_fb0_init(void) {
    if (!g_devfs_initialized) {
        debuglog(DEBUG_INFO, "[DEVFS] Initializing devfs for fb0\n");
        if (!devfs_init()) {
            debuglog(DEBUG_ERROR, "[DEVFS] Failed to initialize devfs for fb0\n");
            return false;
        }
    }

    if (g_fb0_initialized) {
        debuglog(DEBUG_INFO, "[DEVFS] fb0 already initialized\n");
        return true;
    }

    debuglog(DEBUG_INFO, "[DEVFS] Registering /dev/fb0 device\n");

    /* Register /dev/fb0 */
    if (!devfs_register_device("fb0", DEV_TYPE_CHAR,
                                DEV_MAJOR_FB, DEV_MINOR_FB0,
                                &g_fb0_ops, NULL)) {
        debuglog(DEBUG_ERROR, "[DEVFS] Failed to register /dev/fb0\n");
        return false;
    }

    /* Register /dev/fb1 (secondary framebuffer - currently points to same framebuffer as fb0) */
    if (!devfs_register_device("fb1", DEV_TYPE_CHAR,
                                DEV_MAJOR_FB1, DEV_MINOR_FB1_0,
                                &g_fb0_ops, NULL)) {
        debuglog(DEBUG_WARN, "[DEVFS] Failed to register /dev/fb1 (continuing)\n");
        // Continue anyway as this is optional
    }

    /* Register /dev/fb2 (tertiary framebuffer - currently points to same framebuffer as fb0) */
    if (!devfs_register_device("fb2", DEV_TYPE_CHAR,
                                DEV_MAJOR_FB2, DEV_MINOR_FB2_0,
                                &g_fb0_ops, NULL)) {
        debuglog(DEBUG_WARN, "[DEVFS] Failed to register /dev/fb2 (continuing)\n");
        // Continue anyway as this is optional
    }

    g_fb0_initialized = true;
    debuglog(DEBUG_INFO, "[DEVFS] Framebuffer devices initialized: /dev/fb0, /dev/fb1, /dev/fb2\n");

    return true;
}

/**
 * Shutdown /dev/fb0 framebuffer device
 */
void devfs_fb0_shutdown(void) {
    if (!g_fb0_initialized) return;

    devfs_unregister_device("fb0");
    g_fb0_initialized = false;
}

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Check if devfs is initialized
 */
bool devfs_is_initialized(void) {
    return g_devfs_initialized;
}

// ============================================================================
// Dynamic PCI Device Registration
// ============================================================================

/**
 * Simple hex conversion helper
 */
static void uint8_to_hex_str(uint8 value, char* buffer) {
    const char* hex_digits = "0123456789abcdef";
    buffer[0] = hex_digits[(value >> 4) & 0xF];
    buffer[1] = hex_digits[value & 0xF];
    buffer[2] = '\0';
}

static void uint16_to_hex_str(uint16 value, char* buffer) {
    uint8_to_hex_str((uint8)(value >> 8), buffer);
    uint8_to_hex_str((uint8)value, buffer + 2);
}

/**
 * PCI enumeration callback for device registration
 */
static bool pci_register_callback(const pci_device_t* device, void* context) {
    (void)context;

    // Build bus ID string (domain:bus:device.function)
    char bus_id[32];
    char temp[8];

    strcpy(bus_id, "");

    // Domain (4 hex digits)
    uint16_to_hex_str(device->segment, temp);
    if (temp[0] == '0' && temp[1] == '0') {
        if (temp[2] == '0') {
            strncat(bus_id, "000", 3);
            strncat(bus_id, temp + 3, 1);
        } else {
            strncat(bus_id, "00", 2);
            strncat(bus_id, temp + 2, 2);
        }
    } else {
        strcat(bus_id, temp);
    }
    strcat(bus_id, ":");

    // Bus (2 hex)
    uint8_to_hex_str(device->bus, temp);
    if (temp[0] == '0') {
        strncat(bus_id, temp + 1, 1);
    } else {
        strcat(bus_id, temp);
    }
    strcat(bus_id, ":");

    // Device (2 hex)
    uint8_to_hex_str(device->device, temp);
    if (temp[0] == '0') {
        strncat(bus_id, temp + 1, 1);
    } else {
        strcat(bus_id, temp);
    }
    strcat(bus_id, ".");

    // Function (1 hex)
    uint8_to_hex_str(device->function, temp);
    strncat(bus_id, temp + 1, 1);  // Take last char for single digit

    // Use a generic major/minor for dynamic devices
    uint16 major = DEV_MAJOR_MEM;  // Reuse MEM major for now
    uint16 minor = (uint16)((device->bus << 8) | (device->device << 3) | device->function);

    // Create basic read-only ops for PCI config access
    static dev_ops_t pci_ops = {
        .read = NULL,  // TODO: Implement PCI config read
        .write = NULL,
        .open = NULL,
        .close = NULL,
        .ioctl = NULL,
        .poll = NULL
    };

    if (!devfs_register_dynamic_device(DEV_BUS_PCI, bus_id, DEV_TYPE_CHAR,
                                       major, minor, &pci_ops, (void*)device)) {
        debuglog(DEBUG_WARN, "[DEVFS] Failed to register PCI device %s\n", bus_id);
    }

    return true;  // Continue enumeration
}

/**
 * Register all PCI devices
 */
bool devfs_register_pci_devices(void) {
    if (!g_devfs_initialized) {
        if (!devfs_init()) {
            return false;
        }
    }

    debuglog(DEBUG_INFO, "[DEVFS] Registering PCI devices\n");
    pci_enumerate(pci_register_callback, NULL);

    return true;
}

/**
 * Get count of registered devices
 */
uint32 devfs_get_device_count(void) {
    uint32 count = 0;
    dev_node_t* current = g_device_list;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

/**
 * List all registered devices (for debugging)
 */
void devfs_list_devices(void) {
    debuglog(DEBUG_INFO, "[DEVFS] Registered devices:\n");
    dev_node_t* current = g_device_list;
    while (current) {
        debuglog(DEBUG_INFO, "  /dev/%s (major=%d, minor=%d, type=%s)\n",
                 current->name, current->major, current->minor,
                 current->type == DEV_TYPE_CHAR ? "char" : "block");
        current = current->next;
    }
}
