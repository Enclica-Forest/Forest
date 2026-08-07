#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"
#include "atomic.h"

typedef struct {
    atomic8_t locked;
    uint32 owner_cpu;
    const char* name;
    uint32 acquisition_count;
    unsigned long saved_flags;
} spinlock_t;

#define SPINLOCK_INIT(lock_name) { \
    .locked = ATOMIC8_INIT(0), \
    .owner_cpu = 0, \
    .name = lock_name, \
    .acquisition_count = 0, \
    .saved_flags = 0 \
}

#define SPINLOCK_UNLOCKED SPINLOCK_INIT(NULL)
#define SPIN_LOCK_UNLOCKED SPINLOCK_UNLOCKED

#define DEFINE_SPINLOCK(name, lock_name) \
    static spinlock_t name = SPINLOCK_INIT(lock_name)

static inline void spinlock_init(spinlock_t* lock, const char* name) {
    atomic_store8(&lock->locked, 0);
    lock->owner_cpu = 0;
    lock->name = name;
    lock->acquisition_count = 0;
    lock->saved_flags = 0;
}

static inline unsigned long spinlock_irq_save(void) {
    unsigned long flags;
#if defined(__x86_64__)
    __asm__ volatile (
        "pushfq\n\t"
        "cli\n\t"
        "popq %0"
        : "=rm" (flags)
        :
        : "memory"
    );
#elif defined(__i386__)
    __asm__ volatile (
        "pushfl\n\t"
        "cli\n\t"
        "popl %0"
        : "=rm" (flags)
        :
        : "memory"
    );
#elif defined(__aarch64__)
    __asm__ volatile (
        "mrs %0, daif\n\t"
        "msr daifset, #2"
        : "=r" (flags)
        :
        : "memory"
    );
#elif defined(__arm__)
    __asm__ volatile (
        "mrs %0, cpsr\n\t"
        "cpsid i"
        : "=r" (flags)
        :
        : "memory"
    );
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ volatile (
        "csrrci %0, sstatus, 0x2"
        : "=r" (flags)
        :
        : "memory"
    );
#else
    flags = 0;
#endif
    return flags;
}

static inline void spinlock_irq_restore(unsigned long flags) {
#if defined(__x86_64__)
    __asm__ volatile (
        "pushq %0\n\t"
        "popfq"
        :
        : "rm" (flags)
        : "memory", "cc"
    );
#elif defined(__i386__)
    __asm__ volatile (
        "pushl %0\n\t"
        "popfl"
        :
        : "rm" (flags)
        : "memory", "cc"
    );
#elif defined(__aarch64__)
    __asm__ volatile (
        "msr daif, %0"
        :
        : "r" (flags)
        : "memory"
    );
#elif defined(__arm__)
    __asm__ volatile (
        "msr cpsr_c, %0"
        :
        : "r" (flags)
        : "memory"
    );
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ volatile (
        "csrs sstatus, %0"
        :
        : "r" (flags & 0x2)
        : "memory"
    );
    (void)flags;
#endif
}

static inline void spinlock_acquire(spinlock_t* lock) {
    unsigned long flags = spinlock_irq_save();
    uint32 spin_count = 0;
    const uint32 SPIN_YIELD_THRESHOLD = 100;
    
    while (atomic_test_and_set8(&lock->locked)) {
        spin_count++;
        
        if (spin_count >= SPIN_YIELD_THRESHOLD) {
            cpu_pause();
            spin_count = 0;
        }
    }
    
    memory_barrier();
    lock->owner_cpu = 0;
    lock->acquisition_count++;
    lock->saved_flags = flags;
}

static inline bool spinlock_try_acquire(spinlock_t* lock) {
    unsigned long flags = spinlock_irq_save();
    
    if (!atomic_test_and_set8(&lock->locked)) {
        memory_barrier();
        lock->owner_cpu = 0;
        lock->acquisition_count++;
        lock->saved_flags = flags;
        return true;
    }
    
    spinlock_irq_restore(flags);
    return false;
}

static inline void spinlock_release(spinlock_t* lock) {
    uint32 flags = lock->saved_flags;
    memory_barrier();
    lock->owner_cpu = 0;
    atomic_clear8(&lock->locked);
    spinlock_irq_restore(flags);
}

/*
 * Release a spinlock without restoring interrupt flags.
 * Caller is responsible for restoring IF state later.
 */
static inline void spinlock_release_noirq(spinlock_t* lock) {
    memory_barrier();
    lock->owner_cpu = 0;
    atomic_clear8(&lock->locked);
}

static inline bool spinlock_is_locked(const spinlock_t* lock) {
    return atomic_load8(&lock->locked) != 0;
}

/* Legacy compatibility macros */
#define spin_lock_irqsave(lock, flags) \
    do { \
        flags = spinlock_irq_save(); \
        spinlock_acquire(lock); \
    } while(0)

#define spin_unlock_irqrestore(lock, flags) \
    do { \
        spinlock_release(lock); \
        spinlock_irq_restore(flags); \
    } while(0)

/* Linux-style spin lock API compatibility */
#define spin_lock_init(lock) spinlock_init(lock, NULL)
#define spin_lock(lock) spinlock_acquire(lock)
#define spin_unlock(lock) spinlock_release(lock)

#endif // SPINLOCK_H
