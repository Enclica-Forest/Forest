/**
 * Forest-OS epoll Implementation
 * Provides Linux-compatible epoll interface for high-performance I/O multiplexing
 * 
 * epoll is essential for modern servers (nginx, node.js style applications)
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/util.h"
#include "include/memory_safe.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"

// Error codes
#ifndef EBADF
#define EBADF  9
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef ENOENT
#define ENOENT 2
#endif

// sigset_t and timespec stubs for kernel use
typedef unsigned long sigset_t;
struct timespec {
    long tv_sec;
    long tv_nsec;
};
struct epoll_event;

// Maximum number of epoll instances
#define MAX_EPOLL_INSTANCES 64
#define MAX_EPOLL_FDS 1024
#define EPOLL_FD_BASE 10000
#define INOTIFY_FD_BASE 11000
#define EVENTFD_BASE 12000
#define TIMERFD_BASE 14000
#define INOTIFY_MAX_FDS 64
#define EVENTFD_MAX_FDS 64
#define TIMERFD_MAX_FDS 64

// epoll event types
#define EPOLLIN      0x001
#define EPOLLPRI     0x002
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDNORM  0x040
#define EPOLLWRNORM  0x080
#define EPOLLRDBAND  0x100
#define EPOLLWRBAND  0x200
#define EPOLLMSG     0x400
#define EPOLLET      (1 << 31)
#define EPOLLONESHOT (1 << 30)
#define EPOLLEXCLUSIVE (1 << 28)

// epoll op codes
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

// epoll flags
#define EPOLL_CLOEXEC 0x200000

typedef union epoll_data_internal {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_internal_t;

typedef struct epoll_event_internal {
    uint32_t events;
    epoll_data_internal_t data;
} epoll_event_internal_t;

typedef struct epoll_entry {
    bool used;
    int fd;                // Monitored file descriptor
    uint32_t events;       // Events to watch
    uint64_t data_u64;     // User data
} epoll_entry_t;

// epoll file descriptor structure
typedef struct epoll_fd {
    bool used;
    int epfd;
    uint32_t owner_pid;    // Creating task's PID, for cleanup on task exit
    epoll_entry_t entries[MAX_EPOLL_FDS];
    uint32_t monitored_count;
    spinlock_t lock;
} epoll_fd_t;

// Global epoll data
static epoll_fd_t g_epoll_instances[MAX_EPOLL_INSTANCES];
static spinlock_t g_epoll_lock;
static int g_next_epfd = EPOLL_FD_BASE;

// Initialize epoll subsystem
void epoll_init(void) {
    spinlock_init(&g_epoll_lock, "epoll");
    memory_set((uint8*)g_epoll_instances, 0, sizeof(g_epoll_instances));
    debuglog(DEBUG_INFO, "[EPOLL] epoll subsystem initialized\n");
}

// Find epoll instance by fd
static epoll_fd_t* find_epoll_by_fd(int epfd) {
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (g_epoll_instances[i].used && g_epoll_instances[i].epfd == epfd) {
            return &g_epoll_instances[i];
        }
    }
    return NULL;
}

static epoll_entry_t* find_entry_by_fd(epoll_fd_t* epoll, int fd) {
    for (int i = 0; i < MAX_EPOLL_FDS; i++) {
        if (epoll->entries[i].used && epoll->entries[i].fd == fd) {
            return &epoll->entries[i];
        }
    }
    return NULL;
}

static epoll_entry_t* find_free_entry(epoll_fd_t* epoll) {
    for (int i = 0; i < MAX_EPOLL_FDS; i++) {
        if (!epoll->entries[i].used) {
            return &epoll->entries[i];
        }
    }
    return NULL;
}

static bool is_inotify_fd(int fd) {
    return (fd >= INOTIFY_FD_BASE) && (fd < INOTIFY_FD_BASE + INOTIFY_MAX_FDS);
}

static bool is_eventfd_fd(int fd) {
    return (fd >= EVENTFD_BASE) && (fd < EVENTFD_BASE + EVENTFD_MAX_FDS);
}

static bool is_timerfd_fd(int fd) {
    return (fd >= TIMERFD_BASE) && (fd < TIMERFD_BASE + TIMERFD_MAX_FDS);
}

static uint32_t get_fd_ready_mask(int fd) {
    if (fd == 0) {
        return EPOLLIN | EPOLLRDNORM;
    }
    if (fd == 1 || fd == 2) {
        return EPOLLOUT | EPOLLWRNORM;
    }
    if (is_inotify_fd(fd) || is_eventfd_fd(fd) || is_timerfd_fd(fd)) {
        return EPOLLIN | EPOLLRDNORM;
    }
    return 0;
}

static int collect_ready_events(epoll_fd_t* epoll, epoll_event_internal_t* out_events, int maxevents) {
    int out_count = 0;

    for (int i = 0; i < MAX_EPOLL_FDS && out_count < maxevents; i++) {
        epoll_entry_t* entry = &epoll->entries[i];
        if (!entry->used) {
            continue;
        }

        uint32_t wanted = entry->events & ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE);
        uint32_t ready = wanted & get_fd_ready_mask(entry->fd);
        if (!ready) {
            continue;
        }

        out_events[out_count].events = ready;
        out_events[out_count].data.u64 = entry->data_u64;
        out_count++;

        if (entry->events & EPOLLONESHOT) {
            entry->events &= (EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE);
        }
    }

    return out_count;
}

// epoll_create / epoll_create1 - Create an epoll instance
int epoll_create(int size) {
    if (size <= 0) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_epoll_lock);
    
    // Find empty slot
    int idx = -1;
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (!g_epoll_instances[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spinlock_release(&g_epoll_lock);
        return -ENOMEM;
    }
    
    // Initialize epoll instance
    g_epoll_instances[idx].used = true;
    g_epoll_instances[idx].epfd = g_next_epfd++;
    g_epoll_instances[idx].owner_pid = current_task ? current_task->id : 0;
    memory_set((uint8*)g_epoll_instances[idx].entries, 0, sizeof(g_epoll_instances[idx].entries));
    g_epoll_instances[idx].monitored_count = 0;
    spinlock_init(&g_epoll_instances[idx].lock, "epoll_inst");
    
    int result = g_epoll_instances[idx].epfd;
    spinlock_release(&g_epoll_lock);
    
    return result;
}

// epoll_ctl - Control interface for an epoll file descriptor
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    epoll_event_internal_t* user_event = (epoll_event_internal_t*)event;

    if (fd < 0) {
        return -EBADF;
    }
    if (fd == epfd) {
        return -EINVAL;
    }

    // Validate epfd
    spinlock_acquire(&g_epoll_lock);
    epoll_fd_t* epoll = find_epoll_by_fd(epfd);
    if (!epoll) {
        spinlock_release(&g_epoll_lock);
        return -EBADF;
    }
    
    int result = 0;
    
    switch (op) {
        case EPOLL_CTL_ADD: {
            // Add file descriptor to epoll
            if (!user_event) {
                result = -EINVAL;
                break;
            }

            epoll_entry_t* existing = find_entry_by_fd(epoll, fd);
            if (existing) {
                result = -EEXIST;
                break;
            }

            epoll_entry_t* free_entry = find_free_entry(epoll);
            if (!free_entry) {
                result = -ENOMEM;
                break;
            }

            free_entry->used = true;
            free_entry->fd = fd;
            free_entry->events = user_event->events;
            free_entry->data_u64 = user_event->data.u64;
            epoll->monitored_count++;
            result = 0;
            break;
        }
        
        case EPOLL_CTL_MOD: {
            // Modify file descriptor in epoll
            if (!user_event) {
                result = -EINVAL;
                break;
            }

            epoll_entry_t* existing = find_entry_by_fd(epoll, fd);
            if (!existing) {
                result = -ENOENT;
                break;
            }

            existing->events = user_event->events;
            existing->data_u64 = user_event->data.u64;
            result = 0;
            break;
        }
        
        case EPOLL_CTL_DEL: {
            // Remove file descriptor from epoll
            epoll_entry_t* existing = find_entry_by_fd(epoll, fd);
            if (!existing) {
                result = -ENOENT;
                break;
            }

            memory_set((uint8*)existing, 0, sizeof(*existing));
            if (epoll->monitored_count > 0) {
                epoll->monitored_count--;
            }
            result = 0;
            break;
        }
        
        default:
            result = -EINVAL;
    }
    
    spinlock_release(&g_epoll_lock);
    return result;
}

// epoll_wait - Wait for I/O events on an epoll file descriptor
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    epoll_event_internal_t* user_events = (epoll_event_internal_t*)events;

    // Validate parameters
    if (!events || maxevents <= 0) {
        return -EINVAL;
    }

    if (timeout < -1) {
        return -EINVAL;
    }

    int64_t max_spins = 0;
    if (timeout > 0) {
        max_spins = (int64_t)timeout * 1000;
    }

    int64_t spins = 0;
    while (true) {
        spinlock_acquire(&g_epoll_lock);

        epoll_fd_t* epoll = find_epoll_by_fd(epfd);
        if (!epoll) {
            spinlock_release(&g_epoll_lock);
            return -EBADF;
        }

        int num_ready = collect_ready_events(epoll, user_events, maxevents);
        spinlock_release(&g_epoll_lock);

        if (num_ready > 0) {
            return num_ready;
        }

        if (timeout == 0) {
            return 0;
        }

        if (timeout > 0 && spins >= max_spins) {
            return 0;
        }

        for (volatile int i = 0; i < 1000; i++) {
        }

        spins++;
    }
}

// epoll_pwait - Wait for I/O events with signal mask
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout, const sigset_t* sigmask) {
    (void)sigmask; // Signal mask not implemented yet
    return epoll_wait(epfd, events, maxevents, timeout);
}

// epoll_pwait2 - Wait for I/O events with timespec timeout
int epoll_pwait2(int epfd, struct epoll_event* events, int maxevents, const struct timespec* timeout, const sigset_t* sigmask) {
    int timeout_ms = 0;
    
    if (timeout) {
        timeout_ms = (timeout->tv_sec * 1000) + (timeout->tv_nsec / 1000000);
    }
    
    return epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask);
}

// Close epoll instance
int epoll_close(int epfd) {
    spinlock_acquire(&g_epoll_lock);

    epoll_fd_t* epoll = find_epoll_by_fd(epfd);
    if (!epoll) {
        spinlock_release(&g_epoll_lock);
        return -EBADF;
    }

    memory_set((uint8*)epoll->entries, 0, sizeof(epoll->entries));
    epoll->used = false;
    epoll->epfd = 0;
    epoll->owner_pid = 0;
    epoll->monitored_count = 0;

    spinlock_release(&g_epoll_lock);

    return 0;
}

// epoll_close_all_for_task - Release every epoll instance owned by `pid`.
// Without this, a process that exits/crashes without explicitly close()ing
// its epoll fds permanently occupies a slot out of the fixed
// MAX_EPOLL_INSTANCES pool -- over days of many short-lived processes,
// every slot eventually leaks and epoll_create() starts failing for good.
void epoll_close_all_for_task(uint32_t pid) {
    spinlock_acquire(&g_epoll_lock);

    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (g_epoll_instances[i].used && g_epoll_instances[i].owner_pid == pid) {
            memory_set((uint8*)g_epoll_instances[i].entries, 0, sizeof(g_epoll_instances[i].entries));
            g_epoll_instances[i].used = false;
            g_epoll_instances[i].epfd = 0;
            g_epoll_instances[i].owner_pid = 0;
            g_epoll_instances[i].monitored_count = 0;
        }
    }

    spinlock_release(&g_epoll_lock);
}
