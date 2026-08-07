/**
 * Forest-OS sysfs Filesystem Implementation
 * Provides kernel object information to userspace applications
 * 
 * This is required for Linux/Unix application compatibility as many
 * programs access /sys for device and kernel information.
 */

#include "include/vfs.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/screen.h"
#include "include/devfs.h"
#include <stdio.h>

void* kmalloc(size_t size);

// Sysfs node types
typedef enum {
    SYSFS_DIR,
    SYSFS_FILE,
    SYSFS_LINK
} sysfs_type_t;

typedef struct sysfs_entry {
    sysfs_type_t type;
    char name[64];
    char data[256];
    uint32 size;
} sysfs_entry_t;

// Forward declarations
static uint32 sysfs_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 sysfs_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static bool sysfs_readdir(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent);
static vfs_node_t* sysfs_finddir(vfs_node_t* parent, const char* name);
static vfs_node_t* sysfs_vfs_get_root(void* sb);
static vfs_node_t* sysfs_create_node(sysfs_entry_t* entry, uint32 inode);
static bool sysfs_get_children(vfs_node_t* node, sysfs_entry_t** entries, uint32* count);
static bool sysfs_entry_in_table(sysfs_entry_t* entry, sysfs_entry_t* table, uint32 count);

// Global sysfs data
static bool g_sysfs_initialized = false;
static vfs_node_t* g_sysfs_root = NULL;

// Kernel version callback
static uint32 sysfs_read_kernel_release(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "3.0.0-forest\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Kernel version callback
static uint32 sysfs_read_kernel_version(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[256];
    int len = snprintf(buf, sizeof(buf), 
        "#1 SMP " __DATE__ " " __TIME__ "\n"
        "Fern version 1.0 (thornedge)\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// OS release info
static uint32 sysfs_read_os_release(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "NAME=\"Fern\"\n"
        "VERSION=\"1.0 (thornedge)\"\n"
        "ID=forestos\n"
        "PRETTY_NAME=\"Fern 1.0 (thornedge)\"\n"
        "VERSION_ID=\"1.0\"\n"
        "HOME_URL=\"https://github.com/bluethefoxofficial/Forest-OS\"\n"
        "BUG_REPORT_URL=\"https://github.com/bluethefoxofficial/Forest-OS/issues\"\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Hardware info
static uint32 sysfs_read_machine_id(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "forestos\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

static uint32 sysfs_read_uevent_seqnum(uint8* buffer, uint32 size, uint32 offset) {
    if (!buffer || size == 0 || offset > 0) {
        return 0;
    }

    char buf[64];
    uint64 seq = devfs_uevent_last_seqnum();
    int len = snprintf(buf, sizeof(buf), "%llu\n", seq);
    if (len < 0) {
        return 0;
    }

    uint32 copy_len = ((uint32)len > size) ? size : (uint32)len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

static uint32 sysfs_read_uevent_pending(uint8* buffer, uint32 size, uint32 offset) {
    if (!buffer || size == 0 || offset > 0) {
        return 0;
    }

    char buf[64];
    uint32 pending = devfs_uevent_pending_count();
    int len = snprintf(buf, sizeof(buf), "%u\n", pending);
    if (len < 0) {
        return 0;
    }

    uint32 copy_len = ((uint32)len > size) ? size : (uint32)len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Static sysfs entries
static sysfs_entry_t sysfs_root_entries[] = {
    {SYSFS_DIR, "block", "", 0},
    {SYSFS_DIR, "bus", "", 0},
    {SYSFS_DIR, "class", "", 0},
    {SYSFS_DIR, "dev", "", 0},
    {SYSFS_DIR, "devices", "", 0},
    {SYSFS_DIR, "firmware", "", 0},
    {SYSFS_DIR, "kernel", "", 0},
    {SYSFS_DIR, "module", "", 0},
    {SYSFS_DIR, "power", "", 0},
};

enum {
    SYSFS_ROOT_BLOCK = 0,
    SYSFS_ROOT_BUS,
    SYSFS_ROOT_CLASS,
    SYSFS_ROOT_DEV,
    SYSFS_ROOT_DEVICES,
    SYSFS_ROOT_FIRMWARE,
    SYSFS_ROOT_KERNEL,
    SYSFS_ROOT_MODULE,
    SYSFS_ROOT_POWER
};

static sysfs_entry_t kernel_entries[] = {
    {SYSFS_FILE, "kernel_release", "3.0.0-forest\n", 0},
    {SYSFS_FILE, "kernel_version", "#1 SMP " __DATE__ " " __TIME__ "\n", 0},
    {SYSFS_FILE, "osrelease", "3.0.0-forest\n", 0},
    {SYSFS_FILE, "ostype", "forestos\n", 0},
    {SYSFS_FILE, "hardware_platform", "i386\n", 0},
    {SYSFS_FILE, "machine", "i386\n", 0},
    {SYSFS_FILE, "uevent_seqnum", "", 0},
    {SYSFS_FILE, "uevent_pending", "", 0},
    {SYSFS_FILE, "uevent_helper", "\n", 0},
};

// /sys/block
static sysfs_entry_t block_entries[] = {
    {SYSFS_DIR, "loop0", "", 0},
    {SYSFS_DIR, "sda", "", 0},
};

static sysfs_entry_t block_loop0_entries[] = {
    {SYSFS_FILE, "dev", "7:0\n", 0},
    {SYSFS_FILE, "size", "0\n", 0},
    {SYSFS_FILE, "removable", "0\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=7\nMINOR=0\nDEVNAME=loop0\nDEVTYPE=disk\n", 0},
};

static sysfs_entry_t block_sda_entries[] = {
    {SYSFS_FILE, "dev", "8:0\n", 0},
    {SYSFS_FILE, "size", "2097152\n", 0},
    {SYSFS_FILE, "ro", "0\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=8\nMINOR=0\nDEVNAME=sda\nDEVTYPE=disk\n", 0},
};

// /sys/class
static sysfs_entry_t class_entries[] = {
    {SYSFS_DIR, "block", "", 0},
    {SYSFS_DIR, "input", "", 0},
    {SYSFS_DIR, "net", "", 0},
    {SYSFS_DIR, "tty", "", 0},
};

static sysfs_entry_t class_block_entries[] = {
    {SYSFS_DIR, "loop0", "", 0},
};

static sysfs_entry_t class_block_loop0_entries[] = {
    {SYSFS_FILE, "dev", "7:0\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=7\nMINOR=0\nDEVNAME=loop0\n", 0},
};

static sysfs_entry_t class_input_entries[] = {
    {SYSFS_DIR, "event0", "", 0},
    {SYSFS_DIR, "event1", "", 0},
    {SYSFS_DIR, "mouse0", "", 0},
};

static sysfs_entry_t class_input_event0_entries[] = {
    {SYSFS_FILE, "dev", "13:64\n", 0},
    {SYSFS_FILE, "name", "Forest Keyboard\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=13\nMINOR=64\nDEVNAME=input/event0\n", 0},
};

static sysfs_entry_t class_input_event1_entries[] = {
    {SYSFS_FILE, "dev", "13:65\n", 0},
    {SYSFS_FILE, "name", "Forest Mouse\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=13\nMINOR=65\nDEVNAME=input/event1\n", 0},
};

static sysfs_entry_t class_input_mouse0_entries[] = {
    {SYSFS_FILE, "dev", "13:33\n", 0},
    {SYSFS_FILE, "name", "Forest PS2 Mouse\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=13\nMINOR=33\nDEVNAME=input/mouse0\n", 0},
};

static sysfs_entry_t class_net_entries[] = {
    {SYSFS_DIR, "lo", "", 0},
};

static sysfs_entry_t class_net_lo_entries[] = {
    {SYSFS_FILE, "address", "00:00:00:00:00:00\n", 0},
    {SYSFS_FILE, "operstate", "unknown\n", 0},
    {SYSFS_FILE, "mtu", "65536\n", 0},
};

static sysfs_entry_t class_tty_entries[] = {
    {SYSFS_DIR, "tty0", "", 0},
};

static sysfs_entry_t class_tty0_entries[] = {
    {SYSFS_FILE, "dev", "4:0\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=4\nMINOR=0\nDEVNAME=tty0\n", 0},
};

// /sys/devices
static sysfs_entry_t devices_entries[] = {
    {SYSFS_DIR, "system", "", 0},
    {SYSFS_DIR, "virtual", "", 0},
    {SYSFS_DIR, "platform", "", 0},
};

static sysfs_entry_t devices_system_entries[] = {
    {SYSFS_DIR, "cpu", "", 0},
};

static sysfs_entry_t devices_system_cpu_entries[] = {
    {SYSFS_FILE, "online", "0\n", 0},
    {SYSFS_FILE, "possible", "0\n", 0},
};

static sysfs_entry_t devices_virtual_entries[] = {
    {SYSFS_DIR, "block", "", 0},
    {SYSFS_DIR, "input", "", 0},
    {SYSFS_DIR, "net", "", 0},
    {SYSFS_DIR, "tty", "", 0},
};

static sysfs_entry_t devices_virtual_block_entries[] = {
    {SYSFS_DIR, "loop0", "", 0},
    {SYSFS_DIR, "sda", "", 0},
};

static sysfs_entry_t devices_virtual_input_entries[] = {
    {SYSFS_DIR, "event0", "", 0},
    {SYSFS_DIR, "event1", "", 0},
    {SYSFS_DIR, "mouse0", "", 0},
};

static sysfs_entry_t devices_virtual_net_entries[] = {
    {SYSFS_DIR, "lo", "", 0},
};

static sysfs_entry_t devices_virtual_net_lo_entries[] = {
    {SYSFS_FILE, "ifindex", "1\n", 0},
    {SYSFS_FILE, "operstate", "unknown\n", 0},
};

static sysfs_entry_t devices_virtual_tty_entries[] = {
    {SYSFS_DIR, "tty0", "", 0},
    {SYSFS_DIR, "console", "", 0},
};

static sysfs_entry_t devices_virtual_tty_tty0_entries[] = {
    {SYSFS_FILE, "dev", "4:0\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=4\nMINOR=0\nDEVNAME=tty0\n", 0},
};

static sysfs_entry_t devices_virtual_tty_console_entries[] = {
    {SYSFS_FILE, "dev", "5:0\n", 0},
    {SYSFS_FILE, "uevent", "MAJOR=5\nMINOR=0\nDEVNAME=console\n", 0},
};

// /sys/dev
static sysfs_entry_t dev_entries[] = {
    {SYSFS_DIR, "block", "", 0},
    {SYSFS_DIR, "char", "", 0},
};

static sysfs_entry_t dev_block_entries[] = {
    {SYSFS_LINK, "7:0", "../../devices/virtual/block/loop0\n", 0},
    {SYSFS_LINK, "8:0", "../../devices/virtual/block/sda\n", 0},
};

static sysfs_entry_t dev_char_entries[] = {
    {SYSFS_LINK, "4:0", "../../devices/virtual/tty/tty0\n", 0},
    {SYSFS_LINK, "5:0", "../../devices/virtual/tty/console\n", 0},
    {SYSFS_LINK, "13:64", "../../devices/virtual/input/event0\n", 0},
    {SYSFS_LINK, "13:65", "../../devices/virtual/input/event1\n", 0},
};

// /sys/bus
static sysfs_entry_t bus_entries[] = {
    {SYSFS_DIR, "pci", "", 0},
    {SYSFS_DIR, "usb", "", 0},
    {SYSFS_DIR, "platform", "", 0},
};

static sysfs_entry_t bus_pci_entries[] = {
    {SYSFS_DIR, "devices", "", 0},
    {SYSFS_DIR, "drivers", "", 0},
    {SYSFS_FILE, "uevent", "DRIVER=pci\n", 0},
};

static sysfs_entry_t bus_usb_entries[] = {
    {SYSFS_DIR, "devices", "", 0},
    {SYSFS_DIR, "drivers", "", 0},
    {SYSFS_FILE, "uevent", "DRIVER=usb\n", 0},
};

static sysfs_entry_t bus_platform_entries[] = {
    {SYSFS_DIR, "devices", "", 0},
    {SYSFS_DIR, "drivers", "", 0},
};

// /sys/module
static sysfs_entry_t module_entries[] = {
    {SYSFS_DIR, "kernel", "", 0},
    {SYSFS_DIR, "vfs", "", 0},
    {SYSFS_DIR, "sysfs", "", 0},
};

static sysfs_entry_t module_kernel_entries[] = {
    {SYSFS_FILE, "initstate", "live\n", 0},
    {SYSFS_FILE, "refcnt", "1\n", 0},
    {SYSFS_FILE, "uevent", "MODALIAS=kernel\n", 0},
};

static sysfs_entry_t module_vfs_entries[] = {
    {SYSFS_FILE, "initstate", "live\n", 0},
    {SYSFS_FILE, "refcnt", "1\n", 0},
    {SYSFS_FILE, "uevent", "MODALIAS=vfs\n", 0},
};

static sysfs_entry_t module_sysfs_entries[] = {
    {SYSFS_FILE, "initstate", "live\n", 0},
    {SYSFS_FILE, "refcnt", "1\n", 0},
    {SYSFS_FILE, "uevent", "MODALIAS=sysfs\n", 0},
};

// Sysfs operations
static uint32 sysfs_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!node || !buffer) {
        return 0;
    }
    
    sysfs_entry_t* entry = (sysfs_entry_t*)node->internal_data;
    if (!entry) {
        return 0;
    }
    
    // Check for callback-based entries
    if (strcmp(entry->name, "kernel_release") == 0) {
        return sysfs_read_kernel_release(buffer, size, offset);
    }
    if (strcmp(entry->name, "kernel_version") == 0) {
        return sysfs_read_kernel_version(buffer, size, offset);
    }
    if (strcmp(entry->name, "uevent_seqnum") == 0) {
        return sysfs_read_uevent_seqnum(buffer, size, offset);
    }
    if (strcmp(entry->name, "uevent_pending") == 0) {
        return sysfs_read_uevent_pending(buffer, size, offset);
    }
    
    // Static data
    uint32 data_len = entry->size;
    if (data_len == 0) {
        data_len = strlen(entry->data);
    }
    if (data_len == 0) {
        return 0;
    }
    if (offset >= data_len) {
        return 0;
    }
    
    uint32 remaining = data_len - offset;
    uint32 copy = remaining < size ? remaining : size;
    memory_copy(entry->data + offset, buffer, copy);
    return copy;
}

static uint32 sysfs_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    // Sysfs is mostly read-only
    return 0;
}

static bool sysfs_entry_in_table(sysfs_entry_t* entry, sysfs_entry_t* table, uint32 count) {
    return entry >= table && entry < (table + count);
}

static bool sysfs_get_children(vfs_node_t* node, sysfs_entry_t** entries, uint32* count) {
    if (!entries || !count) {
        return false;
    }

    if (!node || node == g_sysfs_root) {
        *entries = sysfs_root_entries;
        *count = sizeof(sysfs_root_entries) / sizeof(sysfs_root_entries[0]);
        return true;
    }

    sysfs_entry_t* entry = (sysfs_entry_t*)node->internal_data;
    if (!entry || entry->type != SYSFS_DIR) {
        return false;
    }

    if (entry == &sysfs_root_entries[SYSFS_ROOT_KERNEL]) {
        *entries = kernel_entries;
        *count = sizeof(kernel_entries) / sizeof(kernel_entries[0]);
        return true;
    }
    if (entry == &sysfs_root_entries[SYSFS_ROOT_BLOCK]) {
        *entries = block_entries;
        *count = sizeof(block_entries) / sizeof(block_entries[0]);
        return true;
    }
    if (entry == &sysfs_root_entries[SYSFS_ROOT_CLASS]) {
        *entries = class_entries;
        *count = sizeof(class_entries) / sizeof(class_entries[0]);
        return true;
    }
    if (entry == &sysfs_root_entries[SYSFS_ROOT_DEV]) {
        *entries = dev_entries;
        *count = sizeof(dev_entries) / sizeof(dev_entries[0]);
        return true;
    }
    if (entry == &sysfs_root_entries[SYSFS_ROOT_DEVICES]) {
        *entries = devices_entries;
        *count = sizeof(devices_entries) / sizeof(devices_entries[0]);
        return true;
    }
    if (entry == &sysfs_root_entries[SYSFS_ROOT_BUS]) {
        *entries = bus_entries;
        *count = sizeof(bus_entries) / sizeof(bus_entries[0]);
        return true;
    }
    if (entry == &sysfs_root_entries[SYSFS_ROOT_MODULE]) {
        *entries = module_entries;
        *count = sizeof(module_entries) / sizeof(module_entries[0]);
        return true;
    }

    if (entry == &block_entries[0]) {
        *entries = block_loop0_entries;
        *count = sizeof(block_loop0_entries) / sizeof(block_loop0_entries[0]);
        return true;
    }
    if (entry == &block_entries[1]) {
        *entries = block_sda_entries;
        *count = sizeof(block_sda_entries) / sizeof(block_sda_entries[0]);
        return true;
    }

    if (entry == &class_entries[0]) {
        *entries = class_block_entries;
        *count = sizeof(class_block_entries) / sizeof(class_block_entries[0]);
        return true;
    }
    if (entry == &class_entries[1]) {
        *entries = class_input_entries;
        *count = sizeof(class_input_entries) / sizeof(class_input_entries[0]);
        return true;
    }
    if (entry == &class_entries[2]) {
        *entries = class_net_entries;
        *count = sizeof(class_net_entries) / sizeof(class_net_entries[0]);
        return true;
    }
    if (entry == &class_entries[3]) {
        *entries = class_tty_entries;
        *count = sizeof(class_tty_entries) / sizeof(class_tty_entries[0]);
        return true;
    }

    if (entry == &class_block_entries[0]) {
        *entries = class_block_loop0_entries;
        *count = sizeof(class_block_loop0_entries) / sizeof(class_block_loop0_entries[0]);
        return true;
    }
    if (entry == &class_net_entries[0]) {
        *entries = class_net_lo_entries;
        *count = sizeof(class_net_lo_entries) / sizeof(class_net_lo_entries[0]);
        return true;
    }
    if (entry == &class_input_entries[0]) {
        *entries = class_input_event0_entries;
        *count = sizeof(class_input_event0_entries) / sizeof(class_input_event0_entries[0]);
        return true;
    }
    if (entry == &class_input_entries[1]) {
        *entries = class_input_event1_entries;
        *count = sizeof(class_input_event1_entries) / sizeof(class_input_event1_entries[0]);
        return true;
    }
    if (entry == &class_input_entries[2]) {
        *entries = class_input_mouse0_entries;
        *count = sizeof(class_input_mouse0_entries) / sizeof(class_input_mouse0_entries[0]);
        return true;
    }
    if (entry == &class_tty_entries[0]) {
        *entries = class_tty0_entries;
        *count = sizeof(class_tty0_entries) / sizeof(class_tty0_entries[0]);
        return true;
    }

    if (entry == &dev_entries[0]) {
        *entries = dev_block_entries;
        *count = sizeof(dev_block_entries) / sizeof(dev_block_entries[0]);
        return true;
    }
    if (entry == &dev_entries[1]) {
        *entries = dev_char_entries;
        *count = sizeof(dev_char_entries) / sizeof(dev_char_entries[0]);
        return true;
    }

    if (entry == &devices_entries[0]) {
        *entries = devices_system_entries;
        *count = sizeof(devices_system_entries) / sizeof(devices_system_entries[0]);
        return true;
    }
    if (entry == &devices_entries[1]) {
        *entries = devices_virtual_entries;
        *count = sizeof(devices_virtual_entries) / sizeof(devices_virtual_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_entries[0]) {
        *entries = devices_virtual_block_entries;
        *count = sizeof(devices_virtual_block_entries) / sizeof(devices_virtual_block_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_entries[1]) {
        *entries = devices_virtual_input_entries;
        *count = sizeof(devices_virtual_input_entries) / sizeof(devices_virtual_input_entries[0]);
        return true;
    }
    if (entry == &devices_system_entries[0]) {
        *entries = devices_system_cpu_entries;
        *count = sizeof(devices_system_cpu_entries) / sizeof(devices_system_cpu_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_entries[2]) {
        *entries = devices_virtual_net_entries;
        *count = sizeof(devices_virtual_net_entries) / sizeof(devices_virtual_net_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_entries[3]) {
        *entries = devices_virtual_tty_entries;
        *count = sizeof(devices_virtual_tty_entries) / sizeof(devices_virtual_tty_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_net_entries[0]) {
        *entries = devices_virtual_net_lo_entries;
        *count = sizeof(devices_virtual_net_lo_entries) / sizeof(devices_virtual_net_lo_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_tty_entries[0]) {
        *entries = devices_virtual_tty_tty0_entries;
        *count = sizeof(devices_virtual_tty_tty0_entries) / sizeof(devices_virtual_tty_tty0_entries[0]);
        return true;
    }
    if (entry == &devices_virtual_tty_entries[1]) {
        *entries = devices_virtual_tty_console_entries;
        *count = sizeof(devices_virtual_tty_console_entries) / sizeof(devices_virtual_tty_console_entries[0]);
        return true;
    }

    if (entry == &bus_entries[0]) {
        *entries = bus_pci_entries;
        *count = sizeof(bus_pci_entries) / sizeof(bus_pci_entries[0]);
        return true;
    }
    if (entry == &bus_entries[1]) {
        *entries = bus_usb_entries;
        *count = sizeof(bus_usb_entries) / sizeof(bus_usb_entries[0]);
        return true;
    }
    if (entry == &bus_entries[2]) {
        *entries = bus_platform_entries;
        *count = sizeof(bus_platform_entries) / sizeof(bus_platform_entries[0]);
        return true;
    }

    if (entry == &module_entries[0]) {
        *entries = module_kernel_entries;
        *count = sizeof(module_kernel_entries) / sizeof(module_kernel_entries[0]);
        return true;
    }
    if (entry == &module_entries[1]) {
        *entries = module_vfs_entries;
        *count = sizeof(module_vfs_entries) / sizeof(module_vfs_entries[0]);
        return true;
    }
    if (entry == &module_entries[2]) {
        *entries = module_sysfs_entries;
        *count = sizeof(module_sysfs_entries) / sizeof(module_sysfs_entries[0]);
        return true;
    }

    // Known empty directories.
    if (sysfs_entry_in_table(entry, bus_pci_entries, sizeof(bus_pci_entries) / sizeof(bus_pci_entries[0])) ||
        sysfs_entry_in_table(entry, bus_usb_entries, sizeof(bus_usb_entries) / sizeof(bus_usb_entries[0])) ||
        sysfs_entry_in_table(entry, bus_platform_entries, sizeof(bus_platform_entries) / sizeof(bus_platform_entries[0])) ||
        entry == &sysfs_root_entries[SYSFS_ROOT_FIRMWARE] ||
        entry == &sysfs_root_entries[SYSFS_ROOT_POWER] ||
        entry == &devices_entries[2]) {
        *entries = NULL;
        *count = 0;
        return true;
    }

    return false;
}

static vfs_node_t* sysfs_create_node(sysfs_entry_t* entry, uint32 inode) {
    if (!entry) {
        return NULL;
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        return NULL;
    }

    memory_set((uint8*)node, 0, sizeof(vfs_node_t));
    strncpy(node->name, entry->name, sizeof(node->name) - 1);
    node->inode = inode;
    if (entry->type == SYSFS_DIR) {
        node->flags = VFS_DIRECTORY;
    } else if (entry->type == SYSFS_LINK) {
        node->flags = VFS_SYMLINK;
    } else {
        node->flags = VFS_FILE;
    }
    node->length = (entry->size != 0) ? entry->size : strlen(entry->data);
    node->read = sysfs_read;
    node->write = sysfs_write;
    node->readdir = (entry->type == SYSFS_DIR) ? sysfs_readdir : NULL;
    node->finddir = (entry->type == SYSFS_DIR) ? sysfs_finddir : NULL;
    node->internal_data = entry;
    return node;
}

static bool sysfs_readdir(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent) {
    if (!dirent) {
        return false;
    }
    memory_set((uint8*)dirent, 0, sizeof(*dirent));

    sysfs_entry_t* entries = NULL;
    uint32 entry_count = 0;
    if (!sysfs_get_children(node, &entries, &entry_count)) {
        return false;
    }
    
    if (index == 0) {
        dirent->inode = 1;
        strncpy(dirent->name, ".", sizeof(dirent->name) - 1);
        return true;
    }
    
    if (index == 1) {
        dirent->inode = 2;
        strncpy(dirent->name, "..", sizeof(dirent->name) - 1);
        return true;
    }
    
    uint32 child_index = index - 2;
    
    if (child_index < entry_count && entries) {
        sysfs_entry_t* entry = &entries[child_index];
        dirent->inode = 3 + child_index;
        strncpy(dirent->name, entry->name, sizeof(dirent->name) - 1);
        return true;
    }

    return false;
}

static vfs_node_t* sysfs_finddir(vfs_node_t* parent, const char* name) {
    if (!parent || !name) {
        return NULL;
    }
 
    sysfs_entry_t* entries = NULL;
    uint32 entry_count = 0;
    if (!sysfs_get_children(parent, &entries, &entry_count)) {
        return NULL;
    }

    for (uint32 i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return sysfs_create_node(&entries[i], 3 + i);
        }
    }
    
    return NULL;
}

static vfs_node_t* sysfs_vfs_get_root(void* sb) {
    (void)sb;
    return g_sysfs_root;
}

// Initialize sysfs
bool sysfs_init(void) {
    debuglog(DEBUG_INFO, "[SYSFS] Initializing /sys filesystem...\n");
    
    // Create root sysfs node
    g_sysfs_root = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!g_sysfs_root) {
        debuglog(DEBUG_ERROR, "[SYSFS] Failed to allocate root node\n");
        return false;
    }
    
    memory_set((uint8*)g_sysfs_root, 0, sizeof(vfs_node_t));
    g_sysfs_root->inode = 1;
    g_sysfs_root->flags = VFS_DIRECTORY;
    g_sysfs_root->readdir = sysfs_readdir;
    g_sysfs_root->finddir = sysfs_finddir;
    g_sysfs_root->name[0] = '\0'; // Root has no name
    
    // Register sysfs as a filesystem
    vfs_filesystem_t* sysfs_fs = (vfs_filesystem_t*)kmalloc(sizeof(vfs_filesystem_t));
    if (!sysfs_fs) {
        debuglog(DEBUG_ERROR, "[SYSFS] Failed to allocate filesystem struct\n");
        return false;
    }
    
    memory_set((uint8*)sysfs_fs, 0, sizeof(vfs_filesystem_t));
    sysfs_fs->name = "sysfs";
    sysfs_fs->mount = NULL;
    sysfs_fs->get_root = sysfs_vfs_get_root;
    
    // Register the filesystem
    if (vfs_register_filesystem(sysfs_fs) != 0) {
        debuglog(DEBUG_WARN, "[SYSFS] Failed to register sysfs (may already exist)\n");
    }
    
    // Mount sysfs at /sys
    if (vfs_mount("sysfs", "/sys", "sysfs", NULL, NULL, NULL, 0) != 0) {
        debuglog(DEBUG_WARN, "[SYSFS] Failed to mount at /sys\n");
    }
    
    g_sysfs_initialized = true;
    debuglog(DEBUG_INFO, "[SYSFS] /sys filesystem initialized\n");
    
    return true;
}

// Get sysfs root node
vfs_node_t* sysfs_get_root(void) {
    return g_sysfs_root;
}
