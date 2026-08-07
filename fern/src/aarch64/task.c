/*
 * Fern - AArch64 Minimal Task Management
 *
 * Provides task creation from ELF binaries, round-robin scheduling,
 * and task lifecycle management.  Designed for early boot without
 * a heap allocator; tasks are stored in a static array.
 */

#include "task.h"
#include "elf_loader.h"
#include "mmu.h"
#include "uart.h"
#include "fpu.h"
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Static task pool                                                    */
/* ------------------------------------------------------------------ */

static task_t task_pool[MAX_TASKS];
static int task_count = 0;

task_t *current_task = NULL;

/* ------------------------------------------------------------------ */
/* task_init                                                           */
/* ------------------------------------------------------------------ */

void task_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        task_pool[i].pid = 0;
        task_pool[i].state = TASK_STATE_DEAD;
    }
    current_task = NULL;
    task_count = 0;
    uart_puts("[task] task subsystem initialised\n");
}

/* ------------------------------------------------------------------ */
/* task_create_elf                                                     */
/*                                                                     */
/* Load an ELF64 binary into a new user address space and create a     */
/* task that can be scheduled to run it.                               */
/* ------------------------------------------------------------------ */

task_t *task_create_elf(const char *name, const uint8_t *elf_data, uint64_t size)
{
    if (!name || !elf_data || size == 0)
        return NULL;

    /* Find a free slot */
    task_t *t = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_DEAD) {
            t = &task_pool[i];
            break;
        }
    }
    if (!t) {
        uart_puts("[task] no free task slots\n");
        return NULL;
    }

    /* Allocate a user page table */
    pgd_t *user_pgd = aarch64_elf_create_user_pgd();
    if (!user_pgd) {
        uart_puts("[task] failed to allocate user pgd\n");
        return NULL;
    }

    /* Load the ELF into the new address space */
    uint64_t entry, sp;
    int rc = aarch64_elf_load(elf_data, size, user_pgd, &entry, &sp);
    if (rc != 0) {
        uart_printf("[task] ELF load failed: %d\n", rc);
        return NULL;
    }

    /* Get the physical address of the user PGD for TTBR0 */
    uint64_t pgd_pa = (uint64_t)(uintptr_t)user_pgd;

    /* Set up the task */
    t->pid       = ++task_count;
    t->state     = TASK_STATE_READY;
    t->name      = name;
    t->entry     = entry;
    t->sp_el0    = sp;
    t->ttbr0_el1 = pgd_pa;
    t->ticks_left = TASK_TIME_SLICE;
    t->vfp_context = NULL;  /* Lazily allocated on first FPU use */

    /*
     * Set up the kernel stack for this task so that when task_switch_asm()
     * restores it, the return address is task_start_usermode_asm(entry, sp).
     *
     * Stack frame layout (from context_switch.S):
     *   [sp + 0x00..0x58]  callee-saved regs (x19-x30, fp, lr, sp_el0, etc.)
     *   The LR slot at offset 0x58 (x30) is the return address.
     *   task_switch_asm restores registers then returns via LR.
     *
     * For a brand-new task, we build a fake kernel stack frame with:
     *   - x30 (LR) = address of task_start_usermode_asm
     *   - x0 = entry point (first arg to task_start_usermode_asm)
     *   - x1 = user stack pointer (second arg)
     */
    memset(t->kernel_stack, 0, sizeof(t->kernel_stack));

    /* Frame base: top of kernel stack, aligned down to 16 bytes */
    uint64_t frame_top = (uint64_t)&t->kernel_stack[KERNEL_STACK_SIZE];
    frame_top &= ~0xFUL;

    /* Offsets within the saved frame (from context_switch.S) */
    #define FR_X30   0x58   /* link register */
    #define FR_SP_EL0 0x60  /* sp_el0 slot */
    #define FR_SIZE   0x90  /* total frame size */

    /* We place a minimal frame at the very top of the kernel stack.
     * task_switch_asm will load SP from the saved frame pointer,
     * restore callee-saved registers, and return via LR. */

    /* Build the frame: callee-saved regs zeroed, LR = task_start_usermode_asm */
    uint64_t *frame = (uint64_t *)(frame_top - FR_SIZE);
    for (int i = 0; i < (FR_SIZE / 8); i++)
        frame[i] = 0;

    /* LR (x30) at offset 0x58 — return address for task_switch_asm */
    frame[FR_X30 / 8] = (uint64_t)(uintptr_t)task_start_usermode_asm;

    /* sp_el0 at offset 0x60 — user stack pointer for the first ERET */
    frame[FR_SP_EL0 / 8] = sp;

    /* Save the stack pointer so task_switch_asm can restore from it.
     * The new_sp argument to task_switch_asm is the value we compute here. */
    t->kernel_sp = (uint64_t)(uintptr_t)&frame[0];

    uart_printf("[task] created '%s' pid=%d entry=0x%lx sp=0x%lx pgd_pa=0x%lx\n",
                name, t->pid, entry, sp, pgd_pa);
    return t;
}

/* ------------------------------------------------------------------ */
/* task_schedule                                                       */
/*                                                                     */
/* Simple round-robin: advance to the next READY task.                 */
/* Returns the next task to run (may be the same task).                */
/* ------------------------------------------------------------------ */

task_t *task_schedule(void)
{
    if (!current_task)
        return NULL;

    /* Find the next ready task after current */
    int start = (int)(current_task - task_pool);
    for (int i = 1; i < MAX_TASKS; i++) {
        int idx = (start + i) % MAX_TASKS;
        if (task_pool[idx].state == TASK_STATE_READY) {
            current_task = &task_pool[idx];
            current_task->state = TASK_STATE_RUNNING;
            current_task->ticks_left = TASK_TIME_SLICE;
            return current_task;
        }
    }

    /* No other task ready — keep running current */
    if (current_task->state == TASK_STATE_READY ||
        current_task->state == TASK_STATE_RUNNING) {
        current_task->state = TASK_STATE_RUNNING;
        return current_task;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* task_exit                                                           */
/* ------------------------------------------------------------------ */

void task_exit(task_t *task)
{
    if (!task)
        return;

    uart_printf("[task] exiting pid=%d '%s'\n", task->pid, task->name);

    /* Release user page table pages — mark the task slot dead.
     * Page table pages from the static pool are not reclaimed here;
     * that is a future enhancement. */
    task->state = TASK_STATE_DEAD;
    task->pid   = 0;

    if (current_task == task)
        current_task = NULL;
}

/* ------------------------------------------------------------------ */
/* task_get_by_pid                                                     */
/* ------------------------------------------------------------------ */

task_t *task_get_by_pid(int pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].pid == pid && task_pool[i].state != TASK_STATE_DEAD)
            return &task_pool[i];
    }
    return NULL;
}
