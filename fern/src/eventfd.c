/**
 * Forest-OS eventfd, signalfd, and timerfd Implementation
 * Provides Linux-compatible file descriptor based event handling
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/util.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/timer.h"

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
#ifndef EAGAIN
#define EAGAIN 11
#endif

// Clock IDs
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// sigset_t and itimerspec stubs for kernel use
typedef unsigned long sigset_t;
struct timespec { long tv_sec; long tv_nsec; };

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

struct signalfd_siginfo {
    int ssi_signo;
    int ssi_errno;
    int ssi_code;
    // ... other fields not implemented
};

// Maximum number of eventfds
#define MAX_EVENTFDS 64
#define EFD_SEMAPHORE 1
#define EVENTFD_BASE 12000
#define SIGNALFD_BASE 13000
#define TIMERFD_BASE 14000

// eventfd structure
typedef struct {
    bool used;
    int fd;
    uint32_t owner_pid;     // Creating task's PID, for cleanup on task exit
    uint64_t counter;      // Event counter
    uint32_t flags;        // Flags (EFD_CLOEXEC, EFD_NONBLOCK, EFD_SEMAPHORE)
    spinlock_t lock;
} eventfd_t;

// Global eventfd data
static eventfd_t g_eventfds[MAX_EVENTFDS];
static spinlock_t g_eventfd_lock;
static int g_next_eventfd = EVENTFD_BASE;

// Initialize eventfd subsystem
void eventfd_init(void) {
    spinlock_init(&g_eventfd_lock, "eventfd");
    memory_set((uint8*)g_eventfds, 0, sizeof(g_eventfds));
    debuglog(DEBUG_INFO, "[EVENTFD] eventfd subsystem initialized\n");
}

// Find eventfd by fd
static eventfd_t* find_eventfd_by_fd(int fd) {
    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (g_eventfds[i].used && g_eventfds[i].fd == fd) {
            return &g_eventfds[i];
        }
    }
    return NULL;
}

// eventfd_is_readable - Query readability without consuming the counter
bool eventfd_is_readable(int fd) {
    bool readable = false;

    spinlock_acquire(&g_eventfd_lock);

    eventfd_t* ev = find_eventfd_by_fd(fd);
    if (ev && ev->counter > 0) {
        readable = true;
    }

    spinlock_release(&g_eventfd_lock);

    return readable;
}

// eventfd_is_writable - Query writability conservatively
bool eventfd_is_writable(int fd) {
    bool writable = false;

    spinlock_acquire(&g_eventfd_lock);

    eventfd_t* ev = find_eventfd_by_fd(fd);
    if (ev && ev->counter != UINT64_MAX) {
        // Conservative check: guarantee at least a +1 write would not overflow.
        writable = true;
    }

    spinlock_release(&g_eventfd_lock);

    return writable;
}

// eventfd_create - Create an eventfd
int eventfd_create(unsigned int initval, int flags) {
    spinlock_acquire(&g_eventfd_lock);
    
    // Find empty slot
    int idx = -1;
    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (!g_eventfds[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spinlock_release(&g_eventfd_lock);
        return -ENOMEM;
    }
    
    // Initialize eventfd
    g_eventfds[idx].used = true;
    g_eventfds[idx].fd = g_next_eventfd++;
    g_eventfds[idx].owner_pid = current_task ? current_task->id : 0;
    g_eventfds[idx].counter = initval;
    g_eventfds[idx].flags = flags;
    spinlock_init(&g_eventfds[idx].lock, "eventfd_inst");
    
    int result = g_eventfds[idx].fd;
    spinlock_release(&g_eventfd_lock);
    
    return result;
}

// eventfd_read - Read from eventfd
int eventfd_read(int fd, uint64_t* value) {
    if (!value) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_eventfd_lock);
    
    eventfd_t* ev = find_eventfd_by_fd(fd);
    if (!ev) {
        spinlock_release(&g_eventfd_lock);
        return -EBADF;
    }
    
    if (ev->counter == 0) {
        spinlock_release(&g_eventfd_lock);
        return -EAGAIN;
    }
    
    // Read value
    if (ev->flags & EFD_SEMAPHORE) {
        // Semaphore mode: decrement by 1
        *value = 1;
        ev->counter--;
    } else {
        // Normal mode: return entire counter and reset to 0
        *value = ev->counter;
        ev->counter = 0;
    }
    
    spinlock_release(&g_eventfd_lock);
    
    return 0;
}

// eventfd_write - Write to eventfd
int eventfd_write(int fd, uint64_t value) {
    spinlock_acquire(&g_eventfd_lock);
    
    eventfd_t* ev = find_eventfd_by_fd(fd);
    if (!ev) {
        spinlock_release(&g_eventfd_lock);
        return -EBADF;
    }
    
    // Check for overflow (counter is 64-bit, so unlikely but check anyway)
    uint64_t sum = ev->counter + value;
    if (sum < ev->counter) {
        spinlock_release(&g_eventfd_lock);
        return -EINVAL;
    }
    
    ev->counter = sum;
    
    spinlock_release(&g_eventfd_lock);
    
    return 0;
}

// eventfd_close - Close eventfd
int eventfd_close(int fd) {
    spinlock_acquire(&g_eventfd_lock);

    eventfd_t* ev = find_eventfd_by_fd(fd);
    if (!ev) {
        spinlock_release(&g_eventfd_lock);
        return -EBADF;
    }

    ev->used = false;
    ev->owner_pid = 0;
    ev->counter = 0;

    spinlock_release(&g_eventfd_lock);

    return 0;
}

// eventfd_close_all_for_task - Release every eventfd owned by `pid`. Without
// this, a process that exits/crashes without close()ing its eventfds
// permanently occupies a slot out of the fixed MAX_EVENTFDS pool.
void eventfd_close_all_for_task(uint32_t pid) {
    spinlock_acquire(&g_eventfd_lock);

    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (g_eventfds[i].used && g_eventfds[i].owner_pid == pid) {
            g_eventfds[i].used = false;
            g_eventfds[i].owner_pid = 0;
            g_eventfds[i].counter = 0;
        }
    }

    spinlock_release(&g_eventfd_lock);
}

// ============================================
// signalfd implementation
// ============================================

// Maximum number of signal fds
#define MAX_SIGNALFDS 64

// signalfd structure
typedef struct {
    bool used;
    int fd;
    sigset_t mask;         // Signal mask
    spinlock_t lock;
} signalfd_t;

// Global signalfd data
static signalfd_t g_signal_fds[MAX_SIGNALFDS];
static spinlock_t g_signalfd_lock;
static int g_next_signalfd = SIGNALFD_BASE;

// Initialize signalfd subsystem
void signalfd_init(void) {
    spinlock_init(&g_signalfd_lock, "signalfd");
    memory_set((uint8*)g_signal_fds, 0, sizeof(g_signal_fds));
    debuglog(DEBUG_INFO, "[SIGNALFD] signalfd subsystem initialized\n");
}

// Find signalfd by fd
static signalfd_t* find_signalfd_by_fd(int fd) {
    for (int i = 0; i < MAX_SIGNALFDS; i++) {
        if (g_signal_fds[i].used && g_signal_fds[i].fd == fd) {
            return &g_signal_fds[i];
        }
    }
    return NULL;
}

// signalfd_is_readable - Query readability without consuming signal info
bool signalfd_is_readable(int fd) {
    bool readable = false;

    spinlock_acquire(&g_signalfd_lock);

    signalfd_t* sf = find_signalfd_by_fd(fd);
    if (sf) {
        // Conservative behavior: current implementation does not expose pending signals.
        readable = false;
    }

    spinlock_release(&g_signalfd_lock);

    return readable;
}

// signalfd_is_writable - signalfd is not writable
bool signalfd_is_writable(int fd) {
    bool writable = false;

    spinlock_acquire(&g_signalfd_lock);

    signalfd_t* sf = find_signalfd_by_fd(fd);
    if (sf) {
        writable = false;
    }

    spinlock_release(&g_signalfd_lock);

    return writable;
}

// signalfd_create - Create a signalfd
int signalfd_create(int fd, const sigset_t* mask, int flags) {
    (void)fd;
    (void)flags;
    
    spinlock_acquire(&g_signalfd_lock);
    
    // Find empty slot
    int idx = -1;
    for (int i = 0; i < MAX_SIGNALFDS; i++) {
        if (!g_signal_fds[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spinlock_release(&g_signalfd_lock);
        return -ENOMEM;
    }
    
    // Initialize signalfd
    g_signal_fds[idx].used = true;
    g_signal_fds[idx].fd = g_next_signalfd++;
    if (mask) {
        g_signal_fds[idx].mask = *mask;
    } else {
        memory_set((uint8*)&g_signal_fds[idx].mask, 0, sizeof(sigset_t));
    }
    spinlock_init(&g_signal_fds[idx].lock, "signalfd_inst");
    
    int result = g_signal_fds[idx].fd;
    spinlock_release(&g_signalfd_lock);
    
    return result;
}

// signalfd_read - Read from signalfd (not implemented - would read signals)
int signalfd_read(int fd, struct signalfd_siginfo* info) {
    (void)fd;
    (void)info;
    
    // In a full implementation, this would return pending signals
    // For now, return no data
    return -EAGAIN;
}

// signalfd_close - Close signalfd
int signalfd_close(int fd) {
    spinlock_acquire(&g_signalfd_lock);
    
    signalfd_t* sf = find_signalfd_by_fd(fd);
    if (!sf) {
        spinlock_release(&g_signalfd_lock);
        return -EBADF;
    }
    
    sf->used = false;
    
    spinlock_release(&g_signalfd_lock);
    
    return 0;
}

// ============================================
// timerfd implementation
// ============================================

// Maximum number of timer fds
#define MAX_TIMERFDS 64

// timerfd structure
typedef struct {
    bool used;
    int fd;
    uint64_t expiration;    // Next expiration time
    uint64_t interval;      // Repeat interval
    int clockid;           // Clock type (CLOCK_REALTIME, CLOCK_MONOTONIC)
    bool armed;            // Timer is armed
    spinlock_t lock;
} timerfd_t;

// Global timerfd data
static timerfd_t g_timer_fds[MAX_TIMERFDS];
static spinlock_t g_timerfd_lock;
static int g_next_timerfd = TIMERFD_BASE;

// Initialize timerfd subsystem
void timerfd_init(void) {
    spinlock_init(&g_timerfd_lock, "timerfd");
    memory_set((uint8*)g_timer_fds, 0, sizeof(g_timer_fds));
    debuglog(DEBUG_INFO, "[TIMERFD] timerfd subsystem initialized\n");
}

// Find timerfd by fd
static timerfd_t* find_timerfd_by_fd(int fd) {
    for (int i = 0; i < MAX_TIMERFDS; i++) {
        if (g_timer_fds[i].used && g_timer_fds[i].fd == fd) {
            return &g_timer_fds[i];
        }
    }
    return NULL;
}

// timerfd_is_readable - Query readability without consuming expiration count
bool timerfd_is_readable(int fd) {
    bool readable = false;

    spinlock_acquire(&g_timerfd_lock);

    timerfd_t* tf = find_timerfd_by_fd(fd);
    if (tf) {
        uint64_t current_time = timer_get_ticks() * 1000000;
        if (tf->armed && tf->expiration <= current_time) {
            readable = true;
        }
    }

    spinlock_release(&g_timerfd_lock);

    return readable;
}

// timerfd_is_writable - timerfd is not writable
bool timerfd_is_writable(int fd) {
    bool writable = false;

    spinlock_acquire(&g_timerfd_lock);

    timerfd_t* tf = find_timerfd_by_fd(fd);
    if (tf) {
        writable = false;
    }

    spinlock_release(&g_timerfd_lock);

    return writable;
}

// timerfd_create - Create a timerfd
int timerfd_create(int clockid, int flags) {
    (void)flags;
    
    spinlock_acquire(&g_timerfd_lock);
    
    // Find empty slot
    int idx = -1;
    for (int i = 0; i < MAX_TIMERFDS; i++) {
        if (!g_timer_fds[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spinlock_release(&g_timerfd_lock);
        return -ENOMEM;
    }
    
    // Initialize timerfd
    g_timer_fds[idx].used = true;
    g_timer_fds[idx].fd = g_next_timerfd++;
    g_timer_fds[idx].expiration = 0;
    g_timer_fds[idx].interval = 0;
    g_timer_fds[idx].clockid = clockid;
    g_timer_fds[idx].armed = false;
    spinlock_init(&g_timer_fds[idx].lock, "timerfd_inst");
    
    int result = g_timer_fds[idx].fd;
    spinlock_release(&g_timerfd_lock);
    
    return result;
}

// timerfd_settime - Set timerfd time
int timerfd_settime(int fd, int flags, const struct itimerspec* new_value, struct itimerspec* old_value) {
    (void)flags;
    spinlock_acquire(&g_timerfd_lock);
    
    timerfd_t* tf = find_timerfd_by_fd(fd);
    if (!tf) {
        spinlock_release(&g_timerfd_lock);
        return -EBADF;
    }
    
    // Return old value if requested
    if (old_value) {
        old_value->it_interval.tv_sec = tf->interval / 1000000000;
        old_value->it_interval.tv_nsec = tf->interval % 1000000000;
        old_value->it_value.tv_sec = tf->expiration / 1000000000;
        old_value->it_value.tv_nsec = tf->expiration % 1000000000;
    }
    
    // Set new timer values
    if (new_value) {
        tf->interval = (uint64_t)new_value->it_interval.tv_sec * 1000000000ULL + new_value->it_interval.tv_nsec;
        
        // Calculate absolute expiration time
        uint64_t current_time = 0;
        if (tf->clockid == CLOCK_MONOTONIC) {
            // Get monotonic time
            current_time = timer_get_ticks() * 1000000; // Approximate
        } else {
            // Use real time
            current_time = timer_get_ticks() * 1000000;
        }
        
        tf->expiration = current_time + (uint64_t)new_value->it_value.tv_sec * 1000000000ULL + new_value->it_value.tv_nsec;
        
        // Check if timer should be armed
        tf->armed = (new_value->it_value.tv_sec > 0 || new_value->it_value.tv_nsec > 0);
    }
    
    spinlock_release(&g_timerfd_lock);
    
    return 0;
}

// timerfd_gettime - Get timerfd time
int timerfd_gettime(int fd, struct itimerspec* curr_value) {
    spinlock_acquire(&g_timerfd_lock);
    
    timerfd_t* tf = find_timerfd_by_fd(fd);
    if (!tf) {
        spinlock_release(&g_timerfd_lock);
        return -EBADF;
    }
    
    // Calculate time until next expiration
    uint64_t current_time = 0;
    if (tf->clockid == CLOCK_MONOTONIC) {
        current_time = timer_get_ticks() * 1000000;
    } else {
        current_time = timer_get_ticks() * 1000000;
    }
    
    uint64_t remaining = 0;
    if (tf->armed && tf->expiration > current_time) {
        remaining = tf->expiration - current_time;
    }
    
    curr_value->it_interval.tv_sec = tf->interval / 1000000000;
    curr_value->it_interval.tv_nsec = tf->interval % 1000000000;
    curr_value->it_value.tv_sec = remaining / 1000000000;
    curr_value->it_value.tv_nsec = remaining % 1000000000;
    
    spinlock_release(&g_timerfd_lock);
    
    return 0;
}

// timerfd_read - Read from timerfd
int timerfd_read(int fd, uint64_t* expirations) {
    if (!expirations) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_timerfd_lock);
    
    timerfd_t* tf = find_timerfd_by_fd(fd);
    if (!tf) {
        spinlock_release(&g_timerfd_lock);
        return -EBADF;
    }
    
    // Check if timer has expired
    uint64_t current_time = 0;
    if (tf->clockid == CLOCK_MONOTONIC) {
        current_time = timer_get_ticks() * 1000000;
    } else {
        current_time = timer_get_ticks() * 1000000;
    }
    
    if (!tf->armed || tf->expiration > current_time) {
        spinlock_release(&g_timerfd_lock);
        return -EAGAIN;
    }
    
    // Calculate number of expirations
    uint64_t count = 1;
    if (tf->interval > 0) {
        // Calculate how many intervals have passed
        uint64_t elapsed = current_time - tf->expiration + tf->interval;
        count = elapsed / tf->interval;
        
        // Update next expiration
        tf->expiration += count * tf->interval;
    } else {
        // One-shot timer
        tf->armed = false;
        tf->expiration = 0;
    }
    
    *expirations = count;
    
    spinlock_release(&g_timerfd_lock);
    
    return 0;
}

// timerfd_close - Close timerfd
int timerfd_close(int fd) {
    spinlock_acquire(&g_timerfd_lock);
    
    timerfd_t* tf = find_timerfd_by_fd(fd);
    if (!tf) {
        spinlock_release(&g_timerfd_lock);
        return -EBADF;
    }
    
    tf->used = false;
    tf->armed = false;
    
    spinlock_release(&g_timerfd_lock);
    
    return 0;
}
