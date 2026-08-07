/*
 * Fern - RISC-V 64-bit Task Management Interface
 */
#ifndef RISCV64_TASK_H
#define RISCV64_TASK_H

#include <stdint.h>

/* Forward declaration – defined in task.c */
typedef struct riscv64_task riscv64_task_t;

/**
 * riscv64_task_init - Initialise the task subsystem and boot task.
 */
void riscv64_task_init(void);

/**
 * riscv64_task_create_elf - Create a task from an ELF64 image.
 *
 * @elf_data: Pointer to the ELF file in memory.
 * @size:     Size of the ELF image.
 * @name:     Human-readable task name (may be NULL).
 *
 * Returns a pointer to the new task, or NULL on failure.
 */
riscv64_task_t *riscv64_task_create_elf(const uint8_t *elf_data,
                                         uint64_t size, const char *name);

/**
 * riscv64_task_schedule - Switch to the next ready task (round-robin).
 */
void riscv64_task_schedule(void);

/**
 * riscv64_task_exit - Terminate the current task and schedule the next.
 *
 * @code: Exit status code.
 */
void riscv64_task_exit(int code);

#endif /* RISCV64_TASK_H */
