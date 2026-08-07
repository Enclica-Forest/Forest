/**
 * Forest-OS POSIX Shared Memory Implementation
 * Provides POSIX shared memory (/dev/shm) for inter-process memory sharing
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/util.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/libc/errno.h"
#include "include/libc/sys/types.h"

// Kernel-side fallback for stat shape used by shm_fstat.
#ifndef FOREST_KERNEL_STAT_DEFINED
#define FOREST_KERNEL_STAT_DEFINED 1
struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};
#endif

#ifndef S_IFREG
#define S_IFREG 0100000
#endif

#ifndef O_CREAT
#define O_CREAT 0100
#endif

#ifndef O_EXCL
#define O_EXCL 0200
#endif

// Some builds expose kmalloc via other headers; keep an explicit prototype here.
void* kmalloc(size_t size);

// Maximum number of shared memory objects
#define MAX_SHM_OBJECTS 64
#define MAX_SHM_NAME 256
#define MAX_SHM_SIZE (64 * 1024 * 1024) // 64 MB max

// POSIX shared memory object
typedef struct {
    bool used;
    char name[MAX_SHM_NAME];
    int fd;                // File descriptor
    uint32_t creator_pid; // shm_open() creator's PID, for cleanup on task exit
    uint8_t* addr;         // Mapped address
    size_t size;           // Size of object
    int oflag;             // Open flags
    mode_t mode;           // Permissions
    uint32_t refcount;    // Number of mappings
} posix_shm_t;

// Global shared memory data
static posix_shm_t g_shm_objects[MAX_SHM_OBJECTS];
static spinlock_t g_shm_lock;
static int g_next_fd = 0;

// Initialize POSIX shared memory subsystem
void posix_shm_init(void) {
    spinlock_init(&g_shm_lock, "posix_shm");
    memory_set((uint8*)g_shm_objects, 0, sizeof(g_shm_objects));
    debuglog(DEBUG_INFO, "[POSIX_SHM] POSIX shared memory initialized\n");
}

// Find shared memory object by name
static posix_shm_t* find_shm_by_name(const char* name) {
    for (int i = 0; i < MAX_SHM_OBJECTS; i++) {
        if (g_shm_objects[i].used && strcmp(g_shm_objects[i].name, name) == 0) {
            return &g_shm_objects[i];
        }
    }
    return NULL;
}

// Find shared memory object by fd
static posix_shm_t* find_shm_by_fd(int fd) {
    for (int i = 0; i < MAX_SHM_OBJECTS; i++) {
        if (g_shm_objects[i].used && g_shm_objects[i].fd == fd) {
            return &g_shm_objects[i];
        }
    }
    return NULL;
}

// Find empty slot
static posix_shm_t* find_empty_shm(void) {
    for (int i = 0; i < MAX_SHM_OBJECTS; i++) {
        if (!g_shm_objects[i].used) {
            return &g_shm_objects[i];
        }
    }
    return NULL;
}

// shm_open - Create/open a shared memory object
int shm_open(const char* name, int oflag, mode_t mode) {
    if (!name || name[0] != '/') {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_shm_lock);
    
    // Check if object already exists
    posix_shm_t* shm = find_shm_by_name(name);
    
    if (shm) {
        // Object exists
        if ((oflag & O_EXCL) && (oflag & O_CREAT)) {
            // O_EXCL specified but object exists
            spinlock_release(&g_shm_lock);
            return -EEXIST;
        }
        
        // Return existing object
        int result = shm->fd;
        spinlock_release(&g_shm_lock);
        return result;
    }
    
    // Create new object
    if (!(oflag & O_CREAT)) {
        spinlock_release(&g_shm_lock);
        return -ENOENT;
    }
    
    // Find empty slot
    shm = find_empty_shm();
    if (!shm) {
        spinlock_release(&g_shm_lock);
        return -ENOMEM;
    }
    
    // Initialize shared memory object
    shm->used = true;
    strncpy(shm->name, name, sizeof(shm->name) - 1);
    shm->name[sizeof(shm->name) - 1] = '\0';
    shm->fd = g_next_fd++;
    shm->creator_pid = current_task ? current_task->id : 0;
    shm->addr = NULL;
    shm->size = 0;
    shm->oflag = oflag;
    shm->mode = mode & 0777;
    shm->refcount = 0;
    
    int result = shm->fd;
    spinlock_release(&g_shm_lock);
    
    return result;
}

// shm_unlink - Remove a shared memory object
int shm_unlink(const char* name) {
    if (!name || name[0] != '/') {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_shm_lock);
    
    posix_shm_t* shm = find_shm_by_name(name);
    if (!shm) {
        spinlock_release(&g_shm_lock);
        return -ENOENT;
    }
    
    // Check if object is still mapped
    if (shm->refcount > 0) {
        spinlock_release(&g_shm_lock);
        return -EBUSY;
    }
    
    // Free memory if allocated
    if (shm->addr) {
        kfree(shm->addr);
    }
    
    // Mark as unused
    shm->used = false;
    shm->addr = NULL;
    shm->size = 0;
    
    spinlock_release(&g_shm_lock);
    
    return 0;
}

// ftruncate on shared memory object
int shm_ftruncate(int fd, off_t length) {
    if (length < 0) {
        return -EINVAL;
    }
    
    if (length > MAX_SHM_SIZE) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_shm_lock);
    
    posix_shm_t* shm = find_shm_by_fd(fd);
    if (!shm) {
        spinlock_release(&g_shm_lock);
        return -EBADF;
    }
    
    // Reallocate if needed
    if ((size_t)length > shm->size) {
        // Free old memory
        if (shm->addr) {
            kfree(shm->addr);
        }
        
        // Allocate new memory
        shm->addr = (uint8_t*)kmalloc(length);
        if (!shm->addr) {
            shm->size = 0;
            spinlock_release(&g_shm_lock);
            return -ENOMEM;
        }
        
        // Zero the new memory
        memory_set(shm->addr, 0, length);
    }
    
    shm->size = length;
    
    spinlock_release(&g_shm_lock);
    
    return 0;
}

// fstat on shared memory object
int shm_fstat(int fd, struct stat* statbuf) {
    if (!statbuf) {
        return -EFAULT;
    }
    
    spinlock_acquire(&g_shm_lock);
    
    posix_shm_t* shm = find_shm_by_fd(fd);
    if (!shm) {
        spinlock_release(&g_shm_lock);
        return -EBADF;
    }
    
    // Fill stat structure
    statbuf->st_dev = 0;
    statbuf->st_ino = fd;
    statbuf->st_mode = S_IFREG | shm->mode;
    statbuf->st_nlink = 1;
    statbuf->st_uid = 0;
    statbuf->st_gid = 0;
    statbuf->st_rdev = 0;
    statbuf->st_size = shm->size;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks = (shm->size + 511) / 512;
    statbuf->st_atime = 0;
    statbuf->st_mtime = 0;
    statbuf->st_ctime = 0;
    
    spinlock_release(&g_shm_lock);
    
    return 0;
}

// Get shared memory size (for mmap)
size_t shm_get_size(int fd) {
    spinlock_acquire(&g_shm_lock);
    
    posix_shm_t* shm = find_shm_by_fd(fd);
    if (!shm) {
        spinlock_release(&g_shm_lock);
        return 0;
    }
    
    size_t size = shm->size;
    spinlock_release(&g_shm_lock);
    
    return size;
}

// Get shared memory address (for mmap)
void* shm_get_addr(int fd) {
    spinlock_acquire(&g_shm_lock);
    
    posix_shm_t* shm = find_shm_by_fd(fd);
    if (!shm) {
        spinlock_release(&g_shm_lock);
        return NULL;
    }
    
    // Allocate if not already done
    if (!shm->addr && shm->size > 0) {
        shm->addr = (uint8_t*)kmalloc(shm->size);
        if (shm->addr) {
            memory_set(shm->addr, 0, shm->size);
        }
    }
    
    void* addr = shm->addr;
    spinlock_release(&g_shm_lock);
    
    return addr;
}

// Close shared memory (decrement refcount)
void shm_close(int fd) {
    spinlock_acquire(&g_shm_lock);
    
    posix_shm_t* shm = find_shm_by_fd(fd);
    if (shm && shm->refcount > 0) {
        shm->refcount--;
    }
    
    spinlock_release(&g_shm_lock);
}

// mmap shared memory
void* shm_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)prot;
    (void)flags;
    (void)offset;
    
    // Get shared memory object
    void* shm_addr = shm_get_addr(fd);
    if (!shm_addr) {
        return (void*)-ENOMEM;
    }
    
    // Check size
    size_t shm_size = shm_get_size(fd);
    if (offset + length > shm_size) {
        return (void*)-EINVAL;
    }
    
    // Increment refcount
    spinlock_acquire(&g_shm_lock);
    posix_shm_t* shm = find_shm_by_fd(fd);
    if (shm) {
        shm->refcount++;
    }
    spinlock_release(&g_shm_lock);
    
    // Return address with offset
    return (uint8_t*)shm_addr + offset;
}

// munmap shared memory
int shm_munmap(void* addr, size_t length) {
    (void)length;

    // shm_mmap() hands back `shm->addr + offset`, so the object owning this
    // mapping is whichever one's [addr, addr+size) range contains it. This
    // used to be a complete no-op (refcount never decremented on munmap at
    // all), which combined with shm_close() never being called anywhere
    // meant shm_unlink()'s refcount>0 busy-check could never pass once a
    // segment had been mapped even once, permanently leaking its backing
    // kmalloc'd memory.
    if (!addr) {
        return 0;
    }

    spinlock_acquire(&g_shm_lock);
    for (int i = 0; i < MAX_SHM_OBJECTS; i++) {
        posix_shm_t* shm = &g_shm_objects[i];
        if (shm->used && shm->addr &&
            (uint8_t*)addr >= shm->addr && (uint8_t*)addr < shm->addr + shm->size) {
            if (shm->refcount > 0) {
                shm->refcount--;
            }
            break;
        }
    }
    spinlock_release(&g_shm_lock);

    return 0;
}

// posix_shm_close_all_for_task - Reclaim every POSIX shm object created by
// `pid` that's still around. Real POSIX shm objects are supposed to persist
// independent of their creating process until an explicit shm_unlink(), but
// this kernel has no notion of an owning namespace surviving process death
// to eventually call that -- left as-is, a crashed creator's object would
// hold its slot (and backing memory) forever, one of the fixed
// MAX_SHM_OBJECTS pool. Reclaiming on creator exit trades strict POSIX
// persistence for actually bounding memory use over long uptime.
void posix_shm_close_all_for_task(uint32_t pid) {
    spinlock_acquire(&g_shm_lock);

    for (int i = 0; i < MAX_SHM_OBJECTS; i++) {
        posix_shm_t* shm = &g_shm_objects[i];
        if (!shm->used || shm->creator_pid != pid) {
            continue;
        }

        if (shm->addr) {
            kfree(shm->addr);
        }
        memory_set((uint8*)shm, 0, sizeof(posix_shm_t));
    }

    spinlock_release(&g_shm_lock);
}
