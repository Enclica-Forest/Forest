/*
 * sched.h - Execution scheduling
 * 
 * POSIX compatible scheduling definitions for Fern libc.
 */
#ifndef _SCHED_H
#define _SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

/* Scheduling policies */
#define SCHED_OTHER     0   /* Default time-sharing policy */
#define SCHED_FIFO      1   /* First-in first-out policy */
#define SCHED_RR        2   /* Round-robin policy */
#define SCHED_BATCH     3   /* Batch-style execution (Linux) */
#define SCHED_IDLE      5   /* Very low priority background (Linux) */
#define SCHED_DEADLINE  6   /* Deadline scheduling (Linux) */

/* Scheduling parameters */
struct sched_param {
    int sched_priority;
};

/* CPU set type for affinity */
#define CPU_SETSIZE 1024

typedef struct {
    unsigned long __bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

/* CPU set manipulation macros */
#define CPU_SET(cpu, set)   ((set)->__bits[(cpu) / (8 * sizeof(unsigned long))] |= \
                             (1UL << ((cpu) % (8 * sizeof(unsigned long)))))
#define CPU_CLR(cpu, set)   ((set)->__bits[(cpu) / (8 * sizeof(unsigned long))] &= \
                             ~(1UL << ((cpu) % (8 * sizeof(unsigned long)))))
#define CPU_ISSET(cpu, set) ((set)->__bits[(cpu) / (8 * sizeof(unsigned long))] & \
                             (1UL << ((cpu) % (8 * sizeof(unsigned long)))))
#define CPU_ZERO(set)       memset((set), 0, sizeof(cpu_set_t))
#define CPU_COUNT(set)      __sched_cpucount(sizeof(cpu_set_t), set)

/* CPU set operations */
#define CPU_AND(destset, srcset1, srcset2) \
    __CPU_OP_S(sizeof(cpu_set_t), destset, srcset1, srcset2, &)
#define CPU_OR(destset, srcset1, srcset2) \
    __CPU_OP_S(sizeof(cpu_set_t), destset, srcset1, srcset2, |)
#define CPU_XOR(destset, srcset1, srcset2) \
    __CPU_OP_S(sizeof(cpu_set_t), destset, srcset1, srcset2, ^)
#define CPU_EQUAL(set1, set2) \
    (__builtin_memcmp(set1, set2, sizeof(cpu_set_t)) == 0)

/* Dynamically-sized CPU sets */
#define CPU_ALLOC_SIZE(count) \
    ((((count) + (8 * sizeof(unsigned long)) - 1) / (8 * sizeof(unsigned long))) * sizeof(unsigned long))
cpu_set_t *CPU_ALLOC(int num_cpus);
void CPU_FREE(cpu_set_t *set);

/* Scheduling functions */
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_rr_get_interval(pid_t pid, struct timespec *tp);
int sched_yield(void);

/* Affinity functions (Linux) */
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);

/* Clone flags (for internal use) */
#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_PTRACE        0x00002000
#define CLONE_VFORK         0x00004000
#define CLONE_PARENT        0x00008000
#define CLONE_THREAD        0x00010000
#define CLONE_NEWNS         0x00020000
#define CLONE_SYSVSEM       0x00040000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED      0x00400000
#define CLONE_UNTRACED      0x00800000
#define CLONE_CHILD_SETTID  0x01000000
#define CLONE_NEWCGROUP     0x02000000
#define CLONE_NEWUTS        0x04000000
#define CLONE_NEWIPC        0x08000000
#define CLONE_NEWUSER       0x10000000
#define CLONE_NEWPID        0x20000000
#define CLONE_NEWNET        0x40000000
#define CLONE_IO            0x80000000

/* Helper function */
static inline int __sched_cpucount(size_t setsize, const cpu_set_t *set) {
    int count = 0;
    size_t i;
    for (i = 0; i < setsize / sizeof(unsigned long); i++) {
        count += __builtin_popcountl(set->__bits[i]);
    }
    return count;
}

#ifdef __cplusplus
}
#endif

#endif /* _SCHED_H */
