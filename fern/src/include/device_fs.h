#ifndef DEVICE_FS_H
#define DEVICE_FS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "spinlock.h"

/* Define ssize_t if not available */
#ifndef SSIZE_T_DEFINED
#define SSIZE_T_DEFINED
typedef long ssize_t;
#endif

/*
 * Device File System for Fern
 * Provides Unix-style device files with character/block device support
 */

/* Device File Types */
#define DT_CHR        1    /* Character device */
#define DT_BLK        2    /* Block device */
#define DT_BLOCK      DT_BLK  /* Alias for DT_BLK */
#define DT_DIR        4    /* Directory */
#define DT_REG        8    /* Regular file */
#define DT_FIFO       8    /* FIFO */
#define DT_SOCK       12   /* Socket */
#define DT_LNK        16   /* Symbolic link */

/* Device File Modes */
#define S_IFMT        00170000    /* File type mask */
#define S_IFCHR       0020000    /* Character device */
#define S_IFBLK        0060000    /* Block device */
#define S_IFIFO        0010000    /* FIFO */
#define S_ISUID       0004000    /* Set user ID */
#define S_ISGID       0002000    /* Set group ID */
#define S_ISVTX       0001000    /* Sticky bit */

#define S_IRWXU        0000700    /* User permissions */
#define S_IRUSR        0000400    /* User read */
#define S_IWUSR        0000200    /* User write */
#define S_IXUSR        0000100    /* User execute */

#define S_IRWXG        0000070    /* Group permissions */
#define S_IRGRP        0000040    /* Group read */
#define S_IWGRP        0000020    /* Group write */
#define S_IXGRP        0000010    /* Group execute */

#define S_IRWXO        0000007    /* Other permissions */
#define S_IROTH        0000004    /* Other read */
#define S_IWOTH        0000002    /* Other write */
#define S_IXOTH        0000001    /* Other execute */

/* Standard Device Numbers */
#define UNNAMED_MAJOR      0
#define MEM_MAJOR          1       /* /dev/mem, /dev/kmem, /dev/null, /dev/zero, etc. */
#define HD_MAJOR           3       /* Old MFM/RLL hard disk driver */
#define TTY_MAJOR          4       /* TTY devices */
#define CONSOLE_MAJOR      5       /* Console devices */
#define LP_MAJOR           6       /* Parallel printer devices */
#define VCS_MAJOR          7       /* Virtual console memory */
#define BLOCK_MAJOR         8       /* SCSI block devices */
#define MISC_MAJOR        10      /* Miscellaneous devices */
#define INPUT_MAJOR        13      /* Input devices */
#define RTC_MAJOR          13      /* Real-time clock */

/* Fern Specific Device Numbers */
#define FOREST_MAJOR_BASE   200     /* Base for Fern specific devices */
#define FOREST_CHAR_MAJOR   201     /* Character devices */
#define FOREST_BLOCK_MAJOR  202     /* Block devices */
#define FOREST_MISC_MAJOR   203     /* Miscellaneous devices */

/* Device Node Structure */
typedef struct device_node {
    char name[256];                    /* Device name */
    uint32_t device_id;                 /* Device identifier (major:minor) */
    uint16_t major;                      /* Major device number */
    uint16_t minor;                      /* Minor device number */
    uint8_t type;                       /* Device type (DT_CHR, DT_BLK, etc.) */
    uint16_t mode;                       /* File mode and permissions */
    uint32_t uid;                        /* User ID */
    uint32_t gid;                        /* Group ID */
    uint64_t size;                       /* Device size */
    uint64_t blocks;                     /* Number of blocks (for block devices) */
    uint32_t block_size;                  /* Block size in bytes */
    uint64_t inode;                      /* Inode number */
    void *private_data;                  /* Driver-specific data */
    
    /* Device operations */
    struct device_operations *ops;
    
    /* File system integration */
    struct vfs_node *vfs_node;          /* Associated VFS node */
    
    /* Device management */
    struct device_node *next;            /* Linked list for device enumeration */
    int ref_count;                      /* Reference count */
    bool active;                         /* Device is active */

    /* Device statistics */
    struct {
        uint64_t bytes_read;
        uint64_t bytes_written;
        uint64_t read_count;
        uint64_t write_count;
    } stats;
} device_node_t;

/* Device Operations Structure */
typedef struct device_operations {
    /* File operations */
    int (*open)(struct device_node *dev, uint32_t flags);
    int (*close)(struct device_node *dev);
    ssize_t (*read)(struct device_node *dev, void *buffer, size_t count, uint64_t offset);
    ssize_t (*write)(struct device_node *dev, const void *buffer, size_t count, uint64_t offset);
    int (*ioctl)(struct device_node *dev, uint32_t request, void *arg);
    
    /* Device-specific operations */
    int (*mmap)(struct device_node *dev, void *addr, size_t len, uint32_t prot, uint64_t offset);
    int (*poll)(struct device_node *dev, uint16_t *revents, uint16_t *events, int timeout);
    int (*flush)(struct device_node *dev);
    
    /* Power management */
    int (*suspend)(struct device_node *dev);
    int (*resume)(struct device_node *dev);
    
    /* Device control */
    int (*get_info)(struct device_node *dev, void *info);
    int (*set_config)(struct device_node *dev, const void *config);
} device_operations_t;

/* Device Class Structure */
typedef struct device_class {
    char name[64];                     /* Class name */
    uint16_t major;                      /* Major number for this class */
    device_operations_t *default_ops;    /* Default operations for class */
    
    /* Class management */
    struct device_class *next;
    uint32_t device_count;                /* Number of devices in this class */
    struct device_node *devices;           /* List of devices in this class */
} device_class_t;

/* Device Registry */
typedef struct device_registry {
    device_class_t *classes;              /* List of device classes */
    device_node_t *all_devices;           /* List of all devices */
    uint32_t total_devices;               /* Total number of devices */
    uint32_t next_device_id;              /* Next available device ID */
    bool initialized;                     /* Registry is initialized */
    
    /* Device file system */
    struct vfs_node *dev_root;            /* /dev directory VFS node */
    
    /* Synchronization */
    spinlock_t lock;                     /* Registry lock */
} device_registry_t;

/* Device File Operations */
typedef struct device_file {
    device_node_t *device;               /* Associated device */
    uint32_t flags;                      /* Open flags */
    uint64_t position;                    /* Current file position */
    uint32_t ref_count;                   /* Reference count */
    
    /* Buffering for character devices */
    uint8_t *read_buffer;
    size_t read_buffer_size;
    size_t read_buffer_pos;
    bool non_blocking;
} device_file_t;

/* Device Creation Parameters */
typedef struct device_params {
    const char *name;                     /* Device name */
    uint16_t major;                      /* Major number */
    uint16_t minor;                      /* Minor number */
    uint8_t type;                       /* Device type */
    uint16_t mode;                       /* File mode */
    uint32_t uid;                        /* User ID */
    uint32_t gid;                        /* Group ID */
    device_operations_t *ops;               /* Device operations */
    void *private_data;                  /* Private data */
} device_params_t;

/* Device Information Structure */
typedef struct device_info {
    char name[256];                     /* Device name */
    char driver_name[64];                /* Driver name */
    char version[32];                   /* Driver version */
    uint32_t device_id;                  /* Device ID */
    uint16_t major;                      /* Major number */
    uint16_t minor;                      /* Minor number */
    uint8_t type;                       /* Device type */
    uint64_t size;                       /* Device size */
    uint32_t block_size;                  /* Block size */
    uint64_t features;                    /* Device features */
    bool readable;                      /* Device is readable */
    bool writable;                      /* Device is writable */
    bool seekable;                      /* Device supports seeking */
    bool mmapable;                      /* Device supports mmap */
} device_info_t;

/* Device Statistics */
typedef struct device_stats {
    uint64_t read_count;                 /* Number of read operations */
    uint64_t write_count;                /* Number of write operations */
    uint64_t bytes_read;                 /* Total bytes read */
    uint64_t bytes_written;               /* Total bytes written */
    uint64_t seek_count;                 /* Number of seek operations */
    uint64_t open_count;                 /* Number of open operations */
    uint64_t close_count;                /* Number of close operations */
    uint64_t error_count;                /* Number of errors */
    uint64_t last_access_time;            /* Last access timestamp */
} device_stats_t;

/* Core Functions */
int device_fs_init(void);
void device_fs_cleanup(void);
bool device_fs_is_initialized(void);

/* Device Registration */
int device_register(const device_params_t *params);
int device_unregister(uint32_t device_id);
int device_register_class(const char *name, uint16_t major, device_operations_t *ops);
int device_unregister_class(uint16_t major);

/* Device Lookup */
device_node_t *device_find_by_name(const char *name);
device_node_t *device_find_by_id(uint32_t device_id);
device_node_t *device_find_by_major_minor(uint16_t major, uint16_t minor);
device_class_t *device_find_class_by_major(uint16_t major);
int device_list_all(device_node_t ***devices, uint32_t *count);

/* Device File Operations */
int device_open(const char *name, uint32_t flags, device_file_t **file);
int device_close(device_file_t *file);
ssize_t device_read(device_file_t *file, void *buffer, size_t count);
ssize_t device_write(device_file_t *file, const void *buffer, size_t count);
int device_ioctl(device_file_t *file, uint32_t request, void *arg);
int device_mmap(device_file_t *file, void *addr, size_t len, uint32_t prot, uint64_t offset);
int device_poll(device_file_t *file, uint16_t *revents, uint16_t *events, int timeout);

/* Device Node Management */
int device_create_node(const char *path, uint16_t major, uint16_t minor, uint8_t type, uint16_t mode);
int device_remove_node(const char *path);
int device_change_permissions(const char *path, uint16_t mode);
int device_change_owner(const char *path, uint32_t uid, uint32_t gid);

/* Device Class Management */
int device_class_add_device(uint16_t major, const device_params_t *params);
int device_class_remove_device(uint16_t major, uint16_t minor);
device_node_t *device_class_get_device(uint16_t major, uint16_t minor);

/* Statistics and Monitoring */
int device_get_stats(const char *name, device_stats_t *stats);
int device_reset_stats(const char *name);
void device_dump_stats(void);
int device_get_info(const char *name, device_info_t *info);

/* Security and Permissions */
int device_check_permissions(const char *name, uint32_t uid, uint32_t gid, uint32_t requested_access);
int device_set_security_policy(const char *name, uint32_t policy);

/* Utility Functions */
uint32_t device_make_device_id(uint16_t major, uint16_t minor);
uint16_t device_major_from_id(uint32_t device_id);
uint16_t device_minor_from_id(uint32_t device_id);
const char *device_type_to_string(uint8_t type);
bool device_is_character_device(uint8_t type);
bool device_is_block_device(uint8_t type);

/* Integration with VFS */
int device_fs_mount(void);
int device_fs_unmount(void);
struct vfs_node *device_get_vfs_node(const char *name);

/* Debug and Diagnostics */
void device_dump_registry(void);
void device_dump_classes(void);
void device_dump_device(const device_node_t *device);
int device_run_self_test(void);

/* IOCTL Numbers (common ones) */
#define DEVICE_IOCTL_GET_INFO     0x01
#define DEVICE_IOCTL_SET_CONFIG  0x02
#define DEVICE_IOCTL_GET_STATS    0x03
#define DEVICE_IOCTL_RESET_STATS  0x04
#define DEVICE_IOCTL_GET_FEATURES 0x05
#define DEVICE_IOCTL_SET_FEATURES 0x06

/* Error Codes */
#define DEVICE_SUCCESS          0
#define DEVICE_ERROR           -1
#define DEVICE_ERROR_INVALID_PARAM -2
#define DEVICE_ERROR_NOT_FOUND    -3
#define DEVICE_ERROR_PERMISSION  -4
#define DEVICE_ERROR_BUSY       -5
#define DEVICE_ERROR_NO_MEMORY   -6
#define DEVICE_ERROR_NOT_SUPPORTED -7

#endif /* DEVICE_FS_H */