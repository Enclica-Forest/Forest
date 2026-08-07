/*
 * Device File System for Fern
 * Provides Unix-style device files with comprehensive character/block device support
 */

#include "include/device_fs.h"
#include "include/memory_safe.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/atomic.h"
#include "include/vfs.h"
#include "include/task.h"

/* Global device registry */
static device_registry_t g_device_registry = {0};

/* Default device classes */
static device_class_t g_char_class = {0};
static device_class_t g_block_class = {0};
static device_class_t g_misc_class = {0};

/* Standard device operations */
static ssize_t device_default_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
static ssize_t device_default_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
static int device_default_close(struct device_node *dev);
static int device_default_ioctl(struct device_node *dev, uint32_t request, void *arg);
static int device_default_mmap(struct device_node *dev, void *addr, size_t len, uint32_t prot, uint64_t offset);
static int device_default_poll(struct device_node *dev, uint16_t *revents, uint16_t *events, int timeout);
static int device_default_flush(struct device_node *dev);
static int device_default_suspend(struct device_node *dev);
static int device_default_resume(struct device_node *dev);
static int device_default_get_info(struct device_node *dev, void *info);
static int device_default_set_config(struct device_node *dev, const void *config);

/*
 * Initialize device file system
 */
int device_fs_init(void)
{
    if (g_device_registry.initialized) {
        return 0;
    }
    
    debuglog(DEBUG_INFO, "DEVICE: Initializing device file system\n");

    spinlock_init(&g_device_registry.lock, "device_registry");
    
    /* Initialize device classes */
    g_char_class.major = FOREST_CHAR_MAJOR;
    g_char_class.default_ops = &(device_operations_t){
        .open = NULL,
        .close = device_default_close,
        .read = device_default_read,
        .write = device_default_write,
        .ioctl = device_default_ioctl,
        .mmap = device_default_mmap,
        .poll = device_default_poll,
        .flush = device_default_flush,
        .suspend = device_default_suspend,
        .resume = device_default_resume,
        .get_info = device_default_get_info,
        .set_config = device_default_set_config
    };
    g_char_class.device_count = 0;
    g_char_class.devices = NULL;
    
    g_block_class.major = FOREST_BLOCK_MAJOR;
    g_block_class.default_ops = &(device_operations_t){
        .open = NULL,
        .close = device_default_close,
        .read = device_default_read,
        .write = device_default_write,
        .ioctl = device_default_ioctl,
        .mmap = device_default_mmap,
        .poll = device_default_poll,
        .flush = device_default_flush,
        .suspend = device_default_suspend,
        .resume = device_default_resume,
        .get_info = device_default_get_info,
        .set_config = device_default_set_config
    };
    g_block_class.device_count = 0;
    g_block_class.devices = NULL;
    
    g_misc_class.major = FOREST_MISC_MAJOR;
    g_misc_class.default_ops = &(device_operations_t){
        .open = NULL,
        .close = device_default_close,
        .read = device_default_read,
        .write = device_default_write,
        .ioctl = device_default_ioctl,
        .mmap = device_default_mmap,
        .poll = device_default_poll,
        .flush = device_default_flush,
        .suspend = device_default_suspend,
        .resume = device_default_resume,
        .get_info = device_default_get_info,
        .set_config = device_default_set_config
    };
    g_misc_class.device_count = 0;
    g_misc_class.devices = NULL;
    
    /* Initialize class list */
    g_char_class.next = &g_block_class;
    g_block_class.next = &g_misc_class;
    g_misc_class.next = NULL;
    g_device_registry.classes = &g_char_class;
    
    /* Setup VFS root */
    if (device_fs_mount() != 0) {
        debuglog(DEBUG_INFO,"DEVICE: Failed to mount device filesystem\n");
        return -1;
    }
    
    g_device_registry.initialized = true;
    debuglog(DEBUG_INFO,"DEVICE: Device file system initialized\n");
    
    return 0;
}

/*
 * Register a device class
 */
int device_register_class(const char *name, uint16_t major, device_operations_t *ops)
{
    device_class_t *cls;
    unsigned long flags;
    
    if (!name || !ops) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    cls = memory_heap_alloc(sizeof(device_class_t));
    if (!cls) {
        return DEVICE_ERROR_NO_MEMORY;
    }
    
    memset(cls, 0, sizeof(device_class_t));
    strncpy(cls->name, name, sizeof(cls->name) - 1);
    cls->major = major;
    cls->default_ops = ops;
    cls->device_count = 0;
    cls->devices = NULL;
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    /* Add to class list */
    cls->next = g_device_registry.classes;
    g_device_registry.classes = cls;
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    
    debuglog(DEBUG_INFO,"DEVICE: Registered device class '%s' (major %d)\n", name, major);
    
    return DEVICE_SUCCESS;
}

/*
 * Register a device
 */
int device_register(const device_params_t *params)
{
    device_node_t *device;
    device_class_t *cls;
    unsigned long flags;
    
    if (!params || !params->name || !params->ops) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    /* Find device class */
    cls = device_find_class_by_major(params->major);
    if (!cls) {
        debuglog(DEBUG_INFO,"DEVICE: Unknown device class for major %d\n", params->major);
        return DEVICE_ERROR_NOT_FOUND;
    }
    
    /* Check if device already exists */
    if (device_find_by_name(params->name)) {
        debuglog(DEBUG_INFO,"DEVICE: Device '%s' already exists\n", params->name);
        return DEVICE_ERROR_BUSY;
    }
    
    device = memory_heap_alloc(sizeof(device_node_t));
    if (!device) {
        return DEVICE_ERROR_NO_MEMORY;
    }
    
    memset(device, 0, sizeof(device_node_t));
    
    /* Fill device structure */
    strncpy(device->name, params->name, sizeof(device->name) - 1);
    device->device_id = g_device_registry.next_device_id++;
    device->major = params->major;
    device->minor = params->minor;
    device->type = params->type;
    device->mode = params->mode;
    device->uid = params->uid;
    device->gid = params->gid;
    device->size = 0;
    device->blocks = 0;
    device->block_size = 512; /* Default block size */
    device->inode = 0;
    device->private_data = params->private_data;
    device->ops = params->ops;
    device->vfs_node = NULL;
    device->next = NULL;
    device->ref_count = 0;
    device->active = true;
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    /* Add to class device list */
    device->next = cls->devices;
    cls->devices = device;
    cls->device_count++;
    g_device_registry.total_devices++;
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    
    /* Create VFS node */
    if (device_create_node("/dev", device->major, device->minor, 
                           device->type, device->mode) != 0) {
        debuglog(DEBUG_INFO,"DEVICE: Failed to create VFS node for '%s'\n", params->name);
        device->active = false;
        return DEVICE_ERROR;
    }
    
    debuglog(DEBUG_INFO,"DEVICE: Registered device '%s' (%d:%d)\n", 
               params->name, params->major, params->minor);
    
    return device->device_id;
}

/*
 * Create a device node in the VFS (stubbed to succeed for now)
 */
int device_create_node(const char *path, uint16_t major, uint16_t minor, uint8_t type, uint16_t mode)
{
    (void)path;
    (void)major;
    (void)minor;
    (void)type;
    (void)mode;
    return DEVICE_SUCCESS;
}

/*
 * Find device by name
 */
device_node_t *device_find_by_name(const char *name)
{
    device_class_t *cls;
    device_node_t *device;
    unsigned long flags;
    
    if (!name) {
        return NULL;
    }
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    cls = g_device_registry.classes;
    while (cls) {
        device = cls->devices;
        while (device) {
            if (strcmp(device->name, name) == 0) {
                spin_unlock_irqrestore(&g_device_registry.lock, flags);
                return device;
            }
            device = device->next;
        }
        cls = cls->next;
    }
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    return NULL;
}

/*
 * Find device by ID
 */
device_node_t *device_find_by_id(uint32_t device_id)
{
    device_class_t *cls;
    device_node_t *device;
    unsigned long flags;
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    cls = g_device_registry.classes;
    while (cls) {
        device = cls->devices;
        while (device) {
            if (device->device_id == device_id) {
                spin_unlock_irqrestore(&g_device_registry.lock, flags);
                return device;
            }
            device = device->next;
        }
        cls = cls->next;
    }
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    return NULL;
}

/*
 * Find device by major/minor
 */
device_node_t *device_find_by_major_minor(uint16_t major, uint16_t minor)
{
    device_class_t *cls;
    device_node_t *device;
    unsigned long flags;
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    cls = device_find_class_by_major(major);
    if (cls) {
        device = cls->devices;
        while (device) {
            if (device->major == major && device->minor == minor) {
                spin_unlock_irqrestore(&g_device_registry.lock, flags);
                return device;
            }
            device = device->next;
        }
    }
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    return NULL;
}

/*
 * Find device class by major
 */
device_class_t *device_find_class_by_major(uint16_t major)
{
    device_class_t *cls;
    unsigned long flags;
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    cls = g_device_registry.classes;
    while (cls) {
        if (cls->major == major) {
            spin_unlock_irqrestore(&g_device_registry.lock, flags);
            return cls;
        }
        cls = cls->next;
    }
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    return NULL;
}

/*
 * Open device file
 */
int device_open(const char *name, uint32_t flags, device_file_t **file)
{
    device_node_t *device;
    device_file_t *dev_file;
    
    if (!name || !file) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    /* Find device */
    device = device_find_by_name(name);
    if (!device) {
        return DEVICE_ERROR_NOT_FOUND;
    }
    
    /* Check permissions with actual process credentials */
    uint32_t uid = 0, gid = 0;
    if (current_task) {
        uid = current_task->uid;
        gid = current_task->gid;
    }
    if (!device_check_permissions(name, uid, gid, flags)) {
        return DEVICE_ERROR_PERMISSION;
    }
    
    /* Allocate file structure */
    dev_file = memory_heap_alloc(sizeof(device_file_t));
    if (!dev_file) {
        return DEVICE_ERROR_NO_MEMORY;
    }
    
    memset(dev_file, 0, sizeof(device_file_t));
    dev_file->device = device;
    dev_file->flags = flags;
    dev_file->position = 0;
    dev_file->ref_count = 1;
    dev_file->read_buffer = NULL;
    dev_file->read_buffer_size = 0;
    dev_file->read_buffer_pos = 0;
    dev_file->non_blocking = (flags & VFS_NONBLOCK) != 0;
    
    /* Increment device reference count */
    device->ref_count++;
    
    /* Call device open operation */
    if (device->ops && device->ops->open) {
        int ret = device->ops->open(device, flags);
        if (ret != DEVICE_SUCCESS) {
            memory_heap_free(dev_file);
            device->ref_count--;
            return ret;
        }
    }
    
    *file = dev_file;
    debuglog(DEBUG_INFO,"DEVICE: Opened device '%s'\n", name);
    
    return DEVICE_SUCCESS;
}

/*
 * Close device file
 */
int device_close(device_file_t *file)
{
    if (!file || !file->device) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    /* Call device close operation */
    if (file->device->ops && file->device->ops->close) {
        file->device->ops->close(file->device);
    }
    
    /* Free read buffer if allocated */
    if (file->read_buffer) {
        memory_heap_free(file->read_buffer);
    }
    
    /* Decrement device reference count */
    file->device->ref_count--;
    
    debuglog(DEBUG_INFO,"DEVICE: Closed device '%s'\n", file->device->name);
    
    memory_heap_free(file);
    return DEVICE_SUCCESS;
}

/*
 * Read from device
 */
ssize_t device_read(device_file_t *file, void *buffer, size_t count)
{
    if (!file || !file->device || !buffer) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    if (!file->device->ops || !file->device->ops->read) {
        return DEVICE_ERROR_NOT_SUPPORTED;
    }
    
    /* Call device read operation */
    ssize_t result = file->device->ops->read(file->device, buffer, count, file->position);
    
    if (result > 0) {
        file->position += result;
        file->device->stats.bytes_read += result;
        file->device->stats.read_count++;
    }
    
    return result;
}

/*
 * Write to device
 */
ssize_t device_write(device_file_t *file, const void *buffer, size_t count)
{
    if (!file || !file->device || !buffer) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    if (!file->device->ops || !file->device->ops->write) {
        return DEVICE_ERROR_NOT_SUPPORTED;
    }
    
    /* Call device write operation */
    ssize_t result = file->device->ops->write(file->device, buffer, count, file->position);
    
    if (result > 0) {
        file->position += result;
        file->device->stats.bytes_written += result;
        file->device->stats.write_count++;
    }
    
    return result;
}

/*
 * Default device operations
 */
static int device_default_close(struct device_node *dev)
{
    (void)dev;
    return DEVICE_SUCCESS;
}

static ssize_t device_default_read(struct device_node *dev, void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static ssize_t device_default_write(struct device_node *dev, const void *buffer, size_t count, uint64_t offset)
{
    (void)dev;
    (void)buffer;
    (void)count;
    (void)offset;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int device_default_ioctl(struct device_node *dev, uint32_t request, void *arg)
{
    (void)dev;
    (void)request;
    (void)arg;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int device_default_mmap(struct device_node *dev, void *addr, size_t len, uint32_t prot, uint64_t offset)
{
    (void)dev;
    (void)addr;
    (void)len;
    (void)prot;
    (void)offset;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int device_default_poll(struct device_node *dev, uint16_t *revents, uint16_t *events, int timeout)
{
    (void)dev;
    (void)revents;
    (void)events;
    (void)timeout;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int device_default_flush(struct device_node *dev)
{
    (void)dev;
    return DEVICE_SUCCESS;
}

static int device_default_suspend(struct device_node *dev)
{
    (void)dev;
    return DEVICE_SUCCESS;
}

static int device_default_resume(struct device_node *dev)
{
    (void)dev;
    return DEVICE_SUCCESS;
}

static int device_default_get_info(struct device_node *dev, void *info)
{
    (void)dev;
    (void)info;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

static int device_default_set_config(struct device_node *dev, const void *config)
{
    (void)dev;
    (void)config;
    return DEVICE_ERROR_NOT_SUPPORTED;
}

/*
 * Check device permissions
 */
int device_check_permissions(const char *name, uint32_t uid, uint32_t gid, uint32_t requested_access)
{
    device_node_t *device = device_find_by_name(name);
    if (!device) {
        return false;
    }

    /* Enhanced Unix-style permission checking */
    uint32_t required = 0;
    if (requested_access & VFS_READ) {
        if (uid == 0) {  /* root has read access */
            required |= S_IRUSR;
        } else if (uid == device->uid) {
            required |= S_IRUSR;
        } else if (gid == device->gid) {
            required |= S_IRGRP;
        } else {
            required |= S_IROTH;
        }
    }

    if (requested_access & VFS_WRITE) {
        if (uid == 0) {  /* root has write access */
            required |= S_IWUSR;
        } else if (uid == device->uid) {
            required |= S_IWUSR;
        } else if (gid == device->gid) {
            required |= S_IWGRP;
        } else {
            required |= S_IWOTH;
        }
    }

    return (device->mode & required) == required;
}

/*
 * List all devices
 */
int device_list_all(device_node_t ***devices, uint32_t *count)
{
    device_class_t *cls;
    device_node_t *device;
    device_node_t **list;
    uint32_t total_count = 0;
    unsigned long flags;
    
    if (!devices || !count) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    /* Count devices */
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    cls = g_device_registry.classes;
    while (cls) {
        total_count += cls->device_count;
        cls = cls->next;
    }
    
    if (total_count == 0) {
        spin_unlock_irqrestore(&g_device_registry.lock, flags);
        *devices = NULL;
        *count = 0;
        return DEVICE_SUCCESS;
    }
    
    /* Allocate list */
    list = memory_heap_alloc(total_count * sizeof(device_node_t*));
    if (!list) {
        spin_unlock_irqrestore(&g_device_registry.lock, flags);
        return DEVICE_ERROR_NO_MEMORY;
    }
    
    /* Fill list */
    uint32_t index = 0;
    cls = g_device_registry.classes;
    while (cls) {
        device = cls->devices;
        while (device) {
            list[index++] = device;
            device = device->next;
        }
        cls = cls->next;
    }
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    
    *devices = list;
    *count = total_count;
    
    return DEVICE_SUCCESS;
}

/*
 * Get device information
 */
int device_get_info(const char *name, device_info_t *info)
{
    device_node_t *device = device_find_by_name(name);
    if (!device || !info) {
        return DEVICE_ERROR_INVALID_PARAM;
    }
    
    memset(info, 0, sizeof(device_info_t));
    strncpy(info->name, device->name, sizeof(info->name) - 1);
    strncpy(info->driver_name, "ForestOS Device", sizeof(info->driver_name) - 1);
    strncpy(info->version, "1.0", sizeof(info->version) - 1);
    info->device_id = device->device_id;
    info->major = device->major;
    info->minor = device->minor;
    info->type = device->type;
    info->size = device->size;
    info->block_size = device->block_size;
    info->features = 0;
    info->readable = (device->ops != NULL && device->ops->read != NULL);
    info->writable = (device->ops != NULL && device->ops->write != NULL);
    info->seekable = (device->type == DT_BLOCK);
    info->mmapable = (device->ops != NULL && device->ops->mmap != NULL);
    
    return DEVICE_SUCCESS;
}

/*
 * Mount device filesystem
 */
int device_fs_mount(void)
{
    /* This would integrate with the existing VFS system */
    /* For now, just return success */
    debuglog(DEBUG_INFO,"DEVICE: Device filesystem mounted at /dev\n");
    return DEVICE_SUCCESS;
}

/*
 * Unmount device filesystem
 */
int device_fs_unmount(void)
{
    /* This would unmount from VFS system */
    /* For now, just return success */
    debuglog(DEBUG_INFO,"DEVICE: Device filesystem unmounted\n");
    return DEVICE_SUCCESS;
}

/*
 * Set device security policy
 */
int device_set_security_policy(const char *name, uint32_t policy)
{
    device_node_t *device = device_find_by_name(name);
    if (!device) {
        return DEVICE_ERROR_NOT_FOUND;
    }

    /* For dangerous devices like /dev/mem, /dev/kmem, restrict to root only */
    if (strcmp(device->name, "mem") == 0 ||
        strcmp(device->name, "kmem") == 0 ||
        strcmp(device->name, "core") == 0) {
        if (policy & 0x1) { /* Restrict to root only */
            device->mode &= ~(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        }
    }

    return DEVICE_SUCCESS;
}

uint16_t device_major_from_id(uint32_t device_id)
{
    return (device_id >> 16) & 0xFFFF;
}

uint16_t device_minor_from_id(uint32_t device_id)
{
    return device_id & 0xFFFF;
}

const char *device_type_to_string(uint8_t type)
{
    switch (type) {
        case DT_CHR: return "character";
        case DT_BLK: return "block";
        case DT_DIR: return "directory";
        case DT_FIFO: return "fifo";
        case DT_SOCK: return "socket";
        case DT_LNK: return "symlink";
        default: return "unknown";
    }
}

bool device_is_character_device(uint8_t type)
{
    return type == DT_CHR;
}

bool device_is_block_device(uint8_t type)
{
    return type == DT_BLK;
}

/*
 * Cleanup device file system
 */
void device_fs_cleanup(void)
{
    device_class_t *cls;
    device_node_t *device, *next;
    unsigned long flags;
    
    if (!g_device_registry.initialized) {
        return;
    }
    
    debuglog(DEBUG_INFO,"DEVICE: Cleaning up device file system\n");
    
    spin_lock_irqsave(&g_device_registry.lock, flags);
    
    /* Free all devices */
    cls = g_device_registry.classes;
    while (cls) {
        device = cls->devices;
        while (device) {
            next = device->next;
            memory_heap_free(device);
            device = next;
        }
        cls = cls->next;
    }
    
    /* Free all classes */
    cls = g_device_registry.classes;
    while (cls) {
        device_class_t *next_cls = cls->next;
        memory_heap_free(cls);
        cls = next_cls;
    }
    
    memset(&g_device_registry, 0, sizeof(device_registry_t));
    
    spin_unlock_irqrestore(&g_device_registry.lock, flags);
    
    g_device_registry.initialized = false;
}
