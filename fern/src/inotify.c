/**
 * Forest-OS inotify Implementation
 * Provides Linux-compatible inotify interface for file system monitoring
 * 
 * inotify is used by file managers, IDEs, and many applications to watch for file changes
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/memory_safe.h"
#include "include/util.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/vfs.h"

// Error codes
#ifndef EBADF
#define EBADF 9
#endif

// Maximum number of inotify instances
#define MAX_INOTIFY_INSTANCES 64
#define MAX_INOTIFY_WATCHES 256
#define INOTIFY_EVENT_SIZE 16
#define INOTIFY_BUF_LEN (1024 * (INOTIFY_EVENT_SIZE + 8))
#define INOTIFY_FD_BASE 11000

// inotify event flags
#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800

// inotify event flags (all events)
#define IN_ALL_EVENTS    0x00000FFF

// inotify init flags
#define IN_CLOEXEC       0x200000
#define IN_NONBLOCK      0x800

// inotify watch descriptor structure
typedef struct inotify_watch {
    bool used;
    int wd;                 // Watch descriptor
    char path[256];         // Path being watched
    uint32_t mask;          // Events to watch
    uint32_t cookie;        // Cookie for rename events
    struct inotify_watch* next;
} inotify_watch_t;

// inotify instance structure
typedef struct inotify_instance {
    bool used;
    int fd;                 // File descriptor
    uint32_t owner_pid;     // Creating task's PID, for cleanup on task exit
    inotify_watch_t* watches; // List of watches
    uint32_t watch_count;    // Number of watches
    uint8_t buffer[INOTIFY_BUF_LEN]; // Event buffer
    uint32_t buf_pos;       // Position in buffer
    spinlock_t lock;
} inotify_instance_t;

// Global inotify data
static inotify_instance_t g_inotify_instances[MAX_INOTIFY_INSTANCES];
static spinlock_t g_inotify_lock;
static int g_next_fd = INOTIFY_FD_BASE;
static int g_next_wd = 1;

// Initialize inotify subsystem
void inotify_subsystem_init(void) {
    spinlock_init(&g_inotify_lock, "inotify");
    memory_set((uint8*)g_inotify_instances, 0, sizeof(g_inotify_instances));
    debuglog(DEBUG_INFO, "[INOTIFY] inotify subsystem initialized\n");
}

// Find inotify instance by fd
static inotify_instance_t* find_inotify_by_fd(int fd) {
    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        if (g_inotify_instances[i].used && g_inotify_instances[i].fd == fd) {
            return &g_inotify_instances[i];
        }
    }
    return NULL;
}

// Find watch by wd
__attribute__((unused)) static inotify_watch_t* find_watch_by_wd(inotify_instance_t* instance, int wd) {
    inotify_watch_t* watch = instance->watches;
    while (watch) {
        if (watch->wd == wd) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

// Find watch by path
static inotify_watch_t* find_watch_by_path(inotify_instance_t* instance, const char* path) {
    inotify_watch_t* watch = instance->watches;
    while (watch) {
        if (strcmp(watch->path, path) == 0) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

// inotify_init - Initialize an inotify instance
int inotify_init(void) {
    spinlock_acquire(&g_inotify_lock);
    
    // Find empty slot
    int idx = -1;
    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        if (!g_inotify_instances[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spinlock_release(&g_inotify_lock);
        return -ENOMEM;
    }
    
    // Initialize inotify instance
    g_inotify_instances[idx].used = true;
    g_inotify_instances[idx].fd = g_next_fd++;
    g_inotify_instances[idx].owner_pid = current_task ? current_task->id : 0;
    g_inotify_instances[idx].watches = NULL;
    g_inotify_instances[idx].watch_count = 0;
    g_inotify_instances[idx].buf_pos = 0;
    memory_set(g_inotify_instances[idx].buffer, 0, INOTIFY_BUF_LEN);
    spinlock_init(&g_inotify_instances[idx].lock, "inotify_inst");
    
    int result = g_inotify_instances[idx].fd;
    spinlock_release(&g_inotify_lock);
    
    return result;
}

// inotify_init1 - Initialize an inotify instance with flags
int inotify_init1(int flags) {
    int fd = inotify_init();
    if (fd < 0) {
        return fd;
    }
    
    // Handle flags (CLOEXEC, NONBLOCK)
    // For now, we just return the fd
    (void)flags;
    
    return fd;
}

// Add event to instance buffer
static int add_event_to_buffer(inotify_instance_t* instance, int wd, uint32_t mask, uint32_t cookie, const char* name) {
    if (!instance || wd < 0) {
        return -1;
    }
    
    // Calculate event size
    uint32_t raw_name_len = name ? strlen(name) : 0;
    uint32_t name_len = raw_name_len ? (raw_name_len + 1) : 0; // Include trailing NUL when name is present
    uint32_t padded_name_len = (name_len + 3) & ~3U;
    uint32_t event_size = INOTIFY_EVENT_SIZE + padded_name_len;
    
    // Check if there's room in the buffer
    if (instance->buf_pos + event_size > INOTIFY_BUF_LEN) {
        return -1;
    }
    
    // Structure: wd (4), mask (4), cookie (4), len (4), name (len)
    uint32_t* p = (uint32_t*)(instance->buffer + instance->buf_pos);
    p[0] = wd;
    p[1] = mask;
    p[2] = cookie;
    p[3] = padded_name_len;
    
    if (padded_name_len > 0) {
        uint8_t* name_dst = instance->buffer + instance->buf_pos + INOTIFY_EVENT_SIZE;
        memory_set(name_dst, 0, padded_name_len);
        if (name && raw_name_len > 0) {
            memory_copy((const char*)name, (char*)name_dst, raw_name_len);
        }
    }
    
    instance->buf_pos += event_size;
    
    return 0;
}

// inotify_add_watch - Add a watch to an inotify instance
int inotify_add_watch(int fd, const char* path, uint32_t mask) {
    if (!path) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_inotify_lock);
    
    inotify_instance_t* instance = find_inotify_by_fd(fd);
    if (!instance) {
        spinlock_release(&g_inotify_lock);
        return -EBADF;
    }
    
    // Check if path is already being watched
    inotify_watch_t* existing = find_watch_by_path(instance, path);
    if (existing) {
        // Update mask
        existing->mask = mask;
        spinlock_release(&g_inotify_lock);
        return existing->wd;
    }
    
    // Check if we've hit the watch limit
    if (instance->watch_count >= MAX_INOTIFY_WATCHES) {
        spinlock_release(&g_inotify_lock);
        return -ENOSPC;
    }
    
    // Validate path exists
    vfs_node_t* node = vfs_open(path, 0);
    if (!node) {
        spinlock_release(&g_inotify_lock);
        return -ENOENT;
    }
    vfs_close(node);
    
    // Create new watch
    inotify_watch_t* watch = (inotify_watch_t*)kmalloc(sizeof(inotify_watch_t));
    if (!watch) {
        spinlock_release(&g_inotify_lock);
        return -ENOMEM;
    }
    
    watch->used = true;
    watch->wd = g_next_wd++;
    strncpy(watch->path, path, sizeof(watch->path) - 1);
    watch->path[sizeof(watch->path) - 1] = '\0';
    watch->mask = mask;
    watch->cookie = 0;
    watch->next = NULL;
    
    // Add to list
    if (instance->watches) {
        inotify_watch_t* last = instance->watches;
        while (last->next) {
            last = last->next;
        }
        last->next = watch;
    } else {
        instance->watches = watch;
    }
    
    instance->watch_count++;
    
    int result = watch->wd;
    spinlock_release(&g_inotify_lock);
    
    return result;
}

// inotify_rm_watch - Remove a watch from an inotify instance
int inotify_rm_watch(int fd, int wd) {
    spinlock_acquire(&g_inotify_lock);
    
    inotify_instance_t* instance = find_inotify_by_fd(fd);
    if (!instance) {
        spinlock_release(&g_inotify_lock);
        return -EBADF;
    }
    
    // Find and remove watch
    inotify_watch_t* prev = NULL;
    inotify_watch_t* watch = instance->watches;
    
    while (watch) {
        if (watch->wd == wd) {
            // Remove from list
            if (prev) {
                prev->next = watch->next;
            } else {
                instance->watches = watch->next;
            }
            
            // Free watch
            kfree(watch);
            instance->watch_count--;
            
            spinlock_release(&g_inotify_lock);
            return 0;
        }
        prev = watch;
        watch = watch->next;
    }
    
    spinlock_release(&g_inotify_lock);
    return -EINVAL;
}

// Generate an inotify event (called by VFS when files change)
void inotify_generate_event(const char* path, uint32_t mask) {
    spinlock_acquire(&g_inotify_lock);
    
    // Find all instances watching this path
    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        if (!g_inotify_instances[i].used) {
            continue;
        }
        
        inotify_instance_t* instance = &g_inotify_instances[i];
        
        // Check each watch
        inotify_watch_t* watch = instance->watches;
        while (watch) {
            // Check if path matches or is parent
            bool match = false;
            
            // Exact match
            if (strcmp(watch->path, path) == 0) {
                match = true;
            }
            // Parent directory match
            else if (strncmp(watch->path, path, strlen(watch->path)) == 0) {
                // Check if path is in the watched directory
                const char* rest = path + strlen(watch->path);
                if (*rest == '/' || *rest == '\0') {
                    match = true;
                }
            }
            
            if (match && (watch->mask & mask)) {
                // Add event to buffer
                const char* name = path;
                const char* last_slash = strrchr(path, '/');
                if (last_slash) {
                    name = last_slash + 1;
                }
                
                add_event_to_buffer(instance, watch->wd, mask, 0, name);
            }
            
            watch = watch->next;
        }
    }
    
    spinlock_release(&g_inotify_lock);
}

// Read inotify events (called by userspace read syscall)
int inotify_read(int fd, void* buf, size_t count) {
    if (!buf || count == 0) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_inotify_lock);
    
    inotify_instance_t* instance = find_inotify_by_fd(fd);
    if (!instance) {
        spinlock_release(&g_inotify_lock);
        return -EBADF;
    }
    
    if (count < INOTIFY_EVENT_SIZE) {
        spinlock_release(&g_inotify_lock);
        return -EINVAL;
    }

    if (instance->buf_pos == 0) {
        spinlock_release(&g_inotify_lock);
        return 0;
    }

    // Copy only whole event packets that fit in userspace buffer.
    uint32_t to_copy = 0;
    while (to_copy + INOTIFY_EVENT_SIZE <= instance->buf_pos) {
        uint8_t* event_base = instance->buffer + to_copy;
        uint32_t name_len = ((uint32_t*)event_base)[3];
        uint32_t event_size = INOTIFY_EVENT_SIZE + name_len;

        // Malformed internal packet: drop buffered data for safety.
        if (event_size < INOTIFY_EVENT_SIZE || to_copy + event_size > instance->buf_pos) {
            instance->buf_pos = 0;
            spinlock_release(&g_inotify_lock);
            return -EIO;
        }

        if (to_copy + event_size > count) {
            break;
        }
        to_copy += event_size;
    }

    if (to_copy == 0) {
        spinlock_release(&g_inotify_lock);
        return -EINVAL;
    }

    memory_copy((const char*)instance->buffer, buf, to_copy);

    // Shift remaining data
    if (instance->buf_pos > to_copy) {
        memmove(instance->buffer, instance->buffer + to_copy, instance->buf_pos - to_copy);
    }
    instance->buf_pos -= to_copy;
    
    spinlock_release(&g_inotify_lock);
    
    return to_copy;
}

bool inotify_is_readable(int fd) {
    spinlock_acquire(&g_inotify_lock);
    inotify_instance_t* instance = find_inotify_by_fd(fd);
    bool readable = (instance && instance->buf_pos > 0);
    spinlock_release(&g_inotify_lock);
    return readable;
}

bool inotify_is_writable(int fd) {
    (void)fd;
    bool writable = false; // inotify descriptors are not writable
    return writable;
}

// Close inotify instance
int inotify_close(int fd) {
    spinlock_acquire(&g_inotify_lock);
    
    inotify_instance_t* instance = find_inotify_by_fd(fd);
    if (!instance) {
        spinlock_release(&g_inotify_lock);
        return -EBADF;
    }
    
    // Free all watches
    inotify_watch_t* watch = instance->watches;
    while (watch) {
        inotify_watch_t* next = watch->next;
        kfree(watch);
        watch = next;
    }
    
    // Mark as unused
    instance->used = false;
    instance->owner_pid = 0;
    instance->watches = NULL;
    instance->watch_count = 0;
    instance->buf_pos = 0;

    spinlock_release(&g_inotify_lock);

    return 0;
}

// inotify_close_all_for_task - Release every inotify instance (and its
// watch list) owned by `pid`. Without this, a process that exits/crashes
// without close()ing its inotify fd permanently occupies a slot out of the
// fixed MAX_INOTIFY_INSTANCES pool, and its kmalloc'd watch nodes leak too.
void inotify_close_all_for_task(uint32_t pid) {
    spinlock_acquire(&g_inotify_lock);

    for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
        inotify_instance_t* instance = &g_inotify_instances[i];
        if (!instance->used || instance->owner_pid != pid) {
            continue;
        }

        inotify_watch_t* watch = instance->watches;
        while (watch) {
            inotify_watch_t* next = watch->next;
            kfree(watch);
            watch = next;
        }

        instance->used = false;
        instance->owner_pid = 0;
        instance->watches = NULL;
        instance->watch_count = 0;
        instance->buf_pos = 0;
    }

    spinlock_release(&g_inotify_lock);
}
