/*
 * pthread.h - POSIX threads
 * 
 * POSIX compatible threading definitions for Fern libc.
 * Note: Full threading is not implemented in Fern kernel.
 * These are compatibility stubs for single-threaded programs.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <time.h>
#include <sched.h>

/* Thread types */
typedef int pthread_t;

typedef struct {
    int initialized;
    void *attr;
} pthread_attr_t;

/* Mutex types */
typedef int pthread_mutex_t;

typedef struct {
    int initialized;
    void *attr;
} pthread_mutexattr_t;

/* Condition variable types */
typedef int pthread_cond_t;

typedef struct {
    int initialized;
    void *attr;
} pthread_condattr_t;

/* Read-write lock types */
typedef int pthread_rwlock_t;

typedef struct {
    int initialized;
    void *attr;
} pthread_rwlockattr_t;

/* Thread-specific data key type */
typedef struct {
    int initialized;
    void (*destructor)(void*);
} pthread_key_t;

/* Once control */
typedef int pthread_once_t;

/* Barrier types */
typedef int pthread_barrier_t;

typedef struct {
    int initialized;
    void *attr;
} pthread_barrierattr_t;

/* Spinlock type */
typedef int pthread_spinlock_t;

/* Static initializers */
#define PTHREAD_MUTEX_INITIALIZER       0
#define PTHREAD_COND_INITIALIZER        0
#define PTHREAD_RWLOCK_INITIALIZER      0
#define PTHREAD_ONCE_INIT               0

/* Detach state */
#define PTHREAD_CREATE_JOINABLE         0
#define PTHREAD_CREATE_DETACHED         1

/* Mutex types */
#define PTHREAD_MUTEX_NORMAL            0
#define PTHREAD_MUTEX_RECURSIVE         1
#define PTHREAD_MUTEX_ERRORCHECK        2
#define PTHREAD_MUTEX_DEFAULT           PTHREAD_MUTEX_NORMAL

/* Process sharing */
#define PTHREAD_PROCESS_PRIVATE         0
#define PTHREAD_PROCESS_SHARED          1

/* Cancel state and type */
#define PTHREAD_CANCEL_ENABLE           1
#define PTHREAD_CANCEL_DISABLE          0
#define PTHREAD_CANCEL_DEFERRED         0
#define PTHREAD_CANCEL_ASYNCHRONOUS     1

/* Canceled thread exit status */
#define PTHREAD_CANCELED                ((void *)-1)

/* Barrier serial thread */
#define PTHREAD_BARRIER_SERIAL_THREAD   (-1)

/* Scope */
#define PTHREAD_SCOPE_SYSTEM            0
#define PTHREAD_SCOPE_PROCESS           1

/* Inherit scheduling */
#define PTHREAD_INHERIT_SCHED           0
#define PTHREAD_EXPLICIT_SCHED          1

/* Thread creation and control */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);
int pthread_cancel(pthread_t thread);
void pthread_testcancel(void);
int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);

/* Thread attributes */
int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize);
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize);
int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, size_t *stacksize);
int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize);
int pthread_attr_getscope(const pthread_attr_t *attr, int *scope);
int pthread_attr_setscope(pthread_attr_t *attr, int scope);
int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy);
int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy);
int pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param);
int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param);
int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inheritsched);
int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched);

/* Mutex operations */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* Mutex attributes */
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr, int *robustness);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness);

/* Condition variables */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

/* Condition variable attributes */
int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared);
int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared);
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock_id);
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);

/* Read-write locks */
int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock, const struct timespec *abstime);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock, const struct timespec *abstime);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

/* Read-write lock attributes */
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *pshared);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared);

/* Thread-specific data */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

/* Once initialization */
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

/* Barriers */
int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                         unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

/* Barrier attributes */
int pthread_barrierattr_init(pthread_barrierattr_t *attr);
int pthread_barrierattr_destroy(pthread_barrierattr_t *attr);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr, int *pshared);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared);

/* Spinlocks */
int pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

/* Thread scheduling */
int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param);
int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param);
int pthread_setschedprio(pthread_t thread, int prio);

/* Cleanup handlers */
void pthread_cleanup_push(void (*routine)(void *), void *arg);
void pthread_cleanup_pop(int execute);

/* Thread affinity (Linux extension) */
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const void *cpuset);
int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, void *cpuset);

/* Thread naming (Linux extension) */
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char *name, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */
