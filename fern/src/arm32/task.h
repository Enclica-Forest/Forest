#ifndef ARM32_TASK_H
#define ARM32_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "mmu.h"

/* Task states */
typedef enum {
    TASK_STATE_RUNNING,
    TASK_STATE_READY,
    TASK_STATE_WAITING,
    TASK_STATE_TERMINATED,
    TASK_STATE_ZOMBIE,
} arm32_task_state_t;

/* Minimal task structure for ARM32 */
typedef struct arm32_task {
    uint32_t            pid;
    arm32_task_state_t  state;
    const char         *name;

    /* User-mode entry point and stack */
    uint32_t            entry_point;
    uint32_t            user_sp;

    /* Kernel-mode saved context (for task_switch_asm) */
    uint32_t            kernel_sp;

    /* Per-task page table (TTBR0 value) */
    arm_l1_table_t     *page_directory;

    /* Scheduling */
    struct arm32_task  *next;
    uint32_t            ticks_left;
    uint32_t            priority;
} arm32_task_t;

/* Default user priority (lower = higher priority, 0 = highest) */
#define ARM32_TASK_PRIORITY_NORMAL 5
#define ARM32_TASK_TICKS_DEFAULT   4

/* Size of the per-task kernel stack (for exception frames) */
#define ARM32_KERNEL_STACK_SIZE 4096

/**
 * arm32_task_init - Initialise the task subsystem.
 */
void arm32_task_init(void);

/**
 * arm32_task_create_elf - Create a task from an ELF binary.
 *
 * @name       Human-readable task name.
 * @elf_data   Pointer to the ELF file in memory.
 * @elf_size   Size of the ELF file in bytes.
 *
 * Returns a pointer to the new task, or NULL on failure.
 */
arm32_task_t *arm32_task_create_elf(const char *name,
                                    const uint8_t *elf_data,
                                    uint32_t elf_size);

/**
 * arm32_task_start - Launch the first user-mode task.
 *
 * Loads the ELF, switches to its page table, and enters user mode.
 * Does not return on success.  On failure, prints an error and spins.
 */
void arm32_task_start(const uint8_t *elf_data, uint32_t elf_size);

/**
 * arm32_task_schedule - Round-robin scheduler.
 *
 * Picks the next READY task and switches to it.
 * Currently a no-op (single-task placeholder).
 */
void arm32_task_schedule(void);

/**
 * arm32_task_exit - Mark the current task as terminated.
 */
void arm32_task_exit(void);

/* Pointer to the currently running task */
extern arm32_task_t *arm32_current_task;

#endif /* ARM32_TASK_H */
