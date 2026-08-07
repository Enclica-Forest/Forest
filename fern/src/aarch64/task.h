/*
 * Fern - AArch64 Task Management Header
 */
#ifndef AARCH64_TASK_H
#define AARCH64_TASK_H

#include <stdint.h>
#include "mmu.h"

/* Maximum number of concurrent tasks */
#define MAX_TASKS       8

/* Kernel stack per task (16 KB, must fit the context_switch frame) */
#define KERNEL_STACK_SIZE  (16 * 1024)

/* Default time slice in timer ticks */
#define TASK_TIME_SLICE  10

typedef enum {
    TASK_STATE_DEAD = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_WAITING,
} task_state_t;

typedef struct task {
    int             pid;
    const char     *name;
    task_state_t    state;
    uint64_t        ticks_left;

    /* User-mode state */
    uint64_t        entry;       /* ELF entry point */
    uint64_t        sp_el0;      /* User stack pointer */

    /* Kernel / MMU state */
    uint64_t        ttbr0_el1;   /* Physical address of user PGD */
    uint64_t        kernel_sp;   /* Kernel stack pointer for context_switch */

    /* NEON/FP (SIMD) context — lazily allocated on first FPU use.
     * NULL if the task has not yet used floating-point/NEON instructions.
     * Points to a 544-byte fpu_context_t (see fpu.h).
     * Named vfp_context to match the generic task_t field. */
    void           *vfp_context;

    /* Kernel stack (holds the saved frame for task_switch_asm) */
    uint8_t         kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
} task_t;

/* Current running task (global pointer) */
extern task_t *current_task;

/* Forward declaration for context_switch.S */
extern void task_start_usermode_asm(uint64_t entry, uint64_t sp);

/* Forward declaration for elf_loader.c */
pgd_t *aarch64_elf_create_user_pgd(void);

/* API */
void    task_init(void);
task_t *task_create_elf(const char *name, const uint8_t *elf_data, uint64_t size);
task_t *task_schedule(void);
void    task_exit(task_t *task);
task_t *task_get_by_pid(int pid);

#endif /* AARCH64_TASK_H */
