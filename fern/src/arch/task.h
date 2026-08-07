#ifndef FOREST_ARCH_TASK_H
#define FOREST_ARCH_TASK_H

#include "arch.h"

/* =========================================================================
 * 1. Task States
 * ========================================================================= */

typedef enum {
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_WAITING,
    TASK_STATE_ZOMBIE,
    TASK_STATE_TERMINATED
} task_state_t;

/* =========================================================================
 * 2. Task Structure (arch-independent)
 *
 * All pointer-width-dependent fields use arch_word_t so the struct layout
 * is correct on both 32-bit and 64-bit targets.  Arch-specific data (FPU
 * state, TLS base, etc.) lives in the per-arch headers and is referenced
 * from here only as opaque pointers.
 * ========================================================================= */

#define TASK_NAME_MAX       32
#define TASK_PRIORITY_MIN   0
#define TASK_PRIORITY_MAX   7
#define TASK_PRIORITY_DEFAULT 3

typedef struct task {
    /* ---- Identity ---- */
    char                name[TASK_NAME_MAX];
    uint32_t            pid;
    uint32_t            ppid;
    task_state_t        state;

    /* ---- Scheduling ---- */
    uint32_t            priority;
    uint32_t            time_slice;
    uint32_t            ticks_used;

    /* ---- Memory ---- */
    void*               page_directory;     /* arch-specific (x86: page_directory_t*, aarch64/riscv: uint64_t*) */
    arch_word_t         kernel_sp;          /* kernel stack pointer (saved on switch-out) */
    arch_word_t         user_sp;            /* user stack pointer */
    arch_word_t         kernel_stack_base;  /* base of kernel stack allocation */
    arch_word_t         kernel_stack_size;

    /* ---- Entry / Exit ---- */
    arch_vaddr_t        entry_point;
    int32_t             exit_code;

    /* ---- Linked list (ready / zombie queue) ---- */
    struct task*        next;
    struct task*        prev;

    /* ---- Session / Process group membership ---- */
    uint32_t            session_id;         /* session this task belongs to (0 = none) */
    uint32_t            pgrp_id;            /* process group this task belongs to (0 = none) */
    bool                session_leader;     /* true if this task is the session leader */

    /* ---- Timer accounting ---- */
    uint32_t            created_tick;
    uint32_t            last_scheduled_tick;

    /* ---- Arch-specific extension (FPU/SIMD context, TLS, etc.) */
    void*               arch_priv;          /* owned by arch layer, freed on task destroy */
} task_t;

/* =========================================================================
 * 3. Global state
 * ========================================================================= */

extern task_t* current_task;
extern task_t* ready_queue_head;

/* =========================================================================
 * 4. Scheduler API
 * ========================================================================= */

/**
 * task_init - Initialise the cross-arch scheduler.
 *
 * Creates the idle task, sets up the ready queue, and wires in the
 * current_task pointer.  Must be called once during boot before any
 * other task_* function.
 */
void task_init(void);

/**
 * task_create - Create a kernel-mode task.
 *
 * @name:       Human-readable name (copied, max 31 chars).
 * @entry:      Kernel entry point.
 * @flags:      Reserved, pass 0.
 *
 * Returns a pointer to the new task, or NULL on allocation failure.
 * The task is placed in TASK_STATE_READY but is NOT scheduled until
 * the next task_schedule() call.
 */
task_t* task_create(const char* name, void* entry, uint32_t flags);

/**
 * task_create_elf - Load an ELF binary and create a user-mode task.
 *
 * @elf_data:   Pointer to the raw ELF file in memory.
 * @elf_size:   Size in bytes.
 *
 * Returns a pointer to the new task, or NULL on failure.
 * The arch layer must provide the ELF loader (elf_load_executable)
 * and map the resulting segments into a new page directory.
 */
task_t* task_create_elf(const uint8_t* elf_data, uint32_t elf_size);

/**
 * task_schedule - Pick the next runnable task and switch to it.
 *
 * Implements simple round-robin.  Called from the timer IRQ handler
 * and from task_yield().
 */
void task_schedule(void);

/**
 * task_switch_to - Low-level context switch from current_task to @next.
 *
 * Saves callee-saved registers of current_task, loads those of @next,
 * and switches the page directory.  Implemented in arch-specific
 * assembly (task_switch_asm).
 */
void task_switch_to(task_t* next);

/**
 * task_exit - Terminate the calling task.
 *
 * @code:    Exit status (convention: 0 = success, 128+sig = killed).
 *
 * Marks the task as TASK_STATE_ZOMBIE, reparents children, and
 * reschedules.
 */
void task_exit(int32_t code);

/**
 * task_yield - Voluntarily give up the CPU.
 *
 * Equivalent to calling task_schedule() but has clearer intent.
 */
void task_yield(void);

/**
 * task_find_by_pid - Look up a task by its PID.
 *
 * Returns the task pointer, or NULL if no task has that PID.
 */
task_t* task_find_by_pid(uint32_t pid);

/**
 * task_destroy - Free all resources of a terminated task.
 *
 * Removes the task from the ready/zombie queue, frees its kernel
 * stack and the task struct itself.  Must only be called on tasks
 * in TASK_STATE_ZOMBIE or TASK_STATE_TERMINATED state.
 */
void task_destroy(task_t* task);

#endif /* FOREST_ARCH_TASK_H */
