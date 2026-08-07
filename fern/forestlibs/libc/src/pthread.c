/*
 * pthread.c - POSIX threads implementation for Fern libc
 * 
 * Threading is not fully implemented in Fern kernel.
 * These are stub functions that return appropriate error codes
 * to allow single-threaded programs that link against pthread to work.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Basic pthread types (stubs) */
typedef int pthread_t;
typedef struct { int initialized; void *attr; } pthread_attr_t;
typedef int pthread_mutex_t;
typedef struct { int initialized; void *attr; } pthread_mutexattr_t;
typedef int pthread_cond_t;
typedef struct { int initialized; void *attr; } pthread_condattr_t;
typedef int pthread_rwlock_t;
typedef struct { int initialized; void *attr; } pthread_rwlockattr_t;
typedef struct { int initialized; void (*destructor)(void*); } pthread_key_t;
typedef int pthread_once_t;
typedef int pthread_barrier_t;
typedef struct { int initialized; void *attr; } pthread_barrierattr_t;
typedef int pthread_spinlock_t;

/* Constants */
#define PTHREAD_CREATE_JOINABLE     0
#define PTHREAD_CREATE_DETACHED     1
#define PTHREAD_MUTEX_NORMAL        0
#define PTHREAD_MUTEX_RECURSIVE     1
#define PTHREAD_MUTEX_ERRORCHECK    2
#define PTHREAD_MUTEX_DEFAULT       PTHREAD_MUTEX_NORMAL
#define PTHREAD_CANCEL_ENABLE       1
#define PTHREAD_CANCEL_DISABLE      0
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_ONCE_INIT           0
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

/* Thread counter for fake thread IDs */
static int __thread_counter = 1;

/* External getpid declaration */
extern int getpid(void);

/* ============================================================================
 * THREAD CREATION AND MANAGEMENT
 * ============================================================================ */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                  void *(*start_routine)(void *), void *arg) {
    (void)attr;
    (void)start_routine;
    (void)arg;
    
    if (!thread) {
        return EINVAL;
    }
    
    /* Can't actually create threads - return error */
    *thread = __thread_counter++;
    return EAGAIN;  /* Resource temporarily unavailable */
}

int pthread_join(pthread_t thread, void **retval) {
    (void)thread;
    (void)retval;
    return ESRCH;  /* No such process */
}

int pthread_detach(pthread_t thread) {
    (void)thread;
    return ESRCH;
}

void pthread_exit(void *retval) {
    (void)retval;
    /* In a real implementation, this would terminate the thread */
    /* For single-threaded, just exit the process */
    _exit(0);
}

pthread_t pthread_self(void) {
    /* Return process ID as thread ID */
    return (pthread_t)getpid();
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

int pthread_cancel(pthread_t thread) {
    (void)thread;
    return ESRCH;
}

void pthread_testcancel(void) {
    /* No-op - no cancellation support */
}

int pthread_setcancelstate(int state, int *oldstate) {
    if (oldstate) {
        *oldstate = PTHREAD_CANCEL_DISABLE;
    }
    if (state == PTHREAD_CANCEL_ENABLE) {
        return ENOSYS;
    }
    return 0;
}

int pthread_setcanceltype(int type, int *oldtype) {
    if (oldtype) {
        *oldtype = PTHREAD_CANCEL_DEFERRED;
    }
    (void)type;
    return 0;
}

/* ============================================================================
 * THREAD ATTRIBUTES
 * ============================================================================ */

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) {
        return EINVAL;
    }
    attr->initialized = 1;
    attr->attr = NULL;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    if (!attr) {
        return EINVAL;
    }
    attr->initialized = 0;
    if (attr->attr) {
        free(attr->attr);
        attr->attr = NULL;
    }
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
    if (!attr || !detachstate) {
        return EINVAL;
    }
    *detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (!attr) {
        return EINVAL;
    }
    if (detachstate != PTHREAD_CREATE_JOINABLE && detachstate != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
    if (!attr || !stacksize) {
        return EINVAL;
    }
    *stacksize = 8192;  /* Default stack size */
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    (void)attr;
    (void)stacksize;
    return 0;  /* Accept but ignore */
}

/* ============================================================================
 * MUTEX OPERATIONS
 * ============================================================================ */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) {
        return EINVAL;
    }
    *mutex = 0;  /* Unlocked */
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) {
        return EINVAL;
    }
    *mutex = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) {
        return EINVAL;
    }
    /* Single-threaded: always succeeds */
    *mutex = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) {
        return EINVAL;
    }
    if (*mutex) {
        return EBUSY;
    }
    *mutex = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) {
        return EINVAL;
    }
    *mutex = 0;
    return 0;
}

/* ============================================================================
 * MUTEX ATTRIBUTES
 * ============================================================================ */

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) {
        return EINVAL;
    }
    attr->initialized = 1;
    attr->attr = NULL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    if (!attr) {
        return EINVAL;
    }
    attr->initialized = 0;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    if (!attr || !type) {
        return EINVAL;
    }
    *type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) {
        return EINVAL;
    }
    (void)type;
    return 0;
}

/* ============================================================================
 * CONDITION VARIABLES
 * ============================================================================ */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) {
        return EINVAL;
    }
    *cond = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond) {
        return EINVAL;
    }
    *cond = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    (void)cond;
    (void)mutex;
    /* In single-threaded, this would deadlock - return error */
    return EINVAL;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                          const struct timespec *abstime) {
    (void)cond;
    (void)mutex;
    (void)abstime;
    return ETIMEDOUT;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    (void)cond;
    return 0;  /* No-op in single-threaded */
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    (void)cond;
    return 0;  /* No-op in single-threaded */
}

/* ============================================================================
 * READ-WRITE LOCKS
 * ============================================================================ */

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
    (void)attr;
    if (!rwlock) {
        return EINVAL;
    }
    *rwlock = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
    if (!rwlock) {
        return EINVAL;
    }
    *rwlock = 0;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) {
        return EINVAL;
    }
    (*rwlock)++;
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
    return pthread_rwlock_rdlock(rwlock);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) {
        return EINVAL;
    }
    *rwlock = -1;  /* Indicate write lock */
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
    return pthread_rwlock_wrlock(rwlock);
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) {
        return EINVAL;
    }
    if (*rwlock < 0) {
        *rwlock = 0;
    } else if (*rwlock > 0) {
        (*rwlock)--;
    }
    return 0;
}

/* ============================================================================
 * THREAD-SPECIFIC DATA
 * ============================================================================ */

static void *__tsd_values[128] = {0};
static int __tsd_next_key = 0;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key || __tsd_next_key >= 128) {
        return EAGAIN;
    }
    key->initialized = __tsd_next_key++;
    key->destructor = destructor;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    (void)key;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key.initialized < 0 || key.initialized >= 128) {
        return EINVAL;
    }
    __tsd_values[key.initialized] = (void *)value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key.initialized < 0 || key.initialized >= 128) {
        return NULL;
    }
    return __tsd_values[key.initialized];
}

/* ============================================================================
 * ONCE INITIALIZATION
 * ============================================================================ */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) {
        return EINVAL;
    }
    
    if (*once_control == 0) {
        *once_control = 1;
        init_routine();
    }
    return 0;
}

/* ============================================================================
 * BARRIERS
 * ============================================================================ */

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                        unsigned int count) {
    (void)attr;
    if (!barrier || count == 0) {
        return EINVAL;
    }
    *barrier = count;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    if (!barrier) {
        return EINVAL;
    }
    *barrier = 0;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    (void)barrier;
    /* Single thread - always returns serial */
    return PTHREAD_BARRIER_SERIAL_THREAD;
}

/* ============================================================================
 * SPINLOCKS
 * ============================================================================ */

int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
    (void)pshared;
    if (!lock) {
        return EINVAL;
    }
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
    if (!lock) {
        return EINVAL;
    }
    *lock = 0;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
    if (!lock) {
        return EINVAL;
    }
    *lock = 1;
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
    if (!lock) {
        return EINVAL;
    }
    if (*lock) {
        return EBUSY;
    }
    *lock = 1;
    return 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
    if (!lock) {
        return EINVAL;
    }
    *lock = 0;
    return 0;
}
