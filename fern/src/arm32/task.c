#include "task.h"
#include "elf_loader.h"
#include "arm32.h"
#include <string.h>

/* ========================================================================
 * Global state
 * ======================================================================== */

arm32_task_t *arm32_current_task = NULL;

static arm32_task_t *task_list_head = NULL;
static arm32_task_t *task_list_tail = NULL;
static uint32_t next_pid = 1;

/* Static kernel stack for the initial task setup */
static uint8_t init_kernel_stack[ARM32_KERNEL_STACK_SIZE]
    __attribute__((aligned(8)));

/* ========================================================================
 * Forward declarations (assembly, context_switch.S)
 * ======================================================================== */

extern void task_start_usermode_asm(uint32_t entry, uint32_t user_sp);
extern void enter_usermode_asm(uint32_t entry, uint32_t user_sp);
extern void task_switch_asm(uint32_t *old_sp_ptr, uint32_t new_sp);

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

/**
 * allocate_task - Allocate and initialise a bare task_t.
 */
static arm32_task_t *allocate_task(const char *name)
{
    /* Allocate task struct from the page bump allocator.
     * Since we don't have kmalloc on ARM32 yet, use the same page
     * allocator that the ELF loader uses. We need a small helper
     * that we can call here -- but the page allocator is static to
     * elf_loader.c.  For now, carve a task from the linker-provided
     * static initial stack (only 1 task at init time).  A proper
     * heap allocator would replace this. */

    /* Simple: use a static pool for the few tasks we support */
    static arm32_task_t task_pool[8];
    static uint32_t pool_idx = 0;

    if (pool_idx >= 8) {
        return NULL;
    }

    arm32_task_t *t = &task_pool[pool_idx++];
    memset(t, 0, sizeof(arm32_task_t));

    t->pid       = next_pid++;
    t->state     = TASK_STATE_WAITING;
    t->name      = name;
    t->priority  = ARM32_TASK_PRIORITY_NORMAL;
    t->ticks_left = ARM32_TASK_TICKS_DEFAULT;

    /* Link into the task list */
    t->next = NULL;
    if (task_list_tail) {
        task_list_tail->next = t;
    } else {
        task_list_head = t;
    }
    task_list_tail = t;

    return t;
}

/**
 * build_initial_user_stack - Prepare the kernel stack for user-mode entry.
 *
 * task_start_usermode_asm expects:
 *   r0 = entry point
 *   r1 = user stack pointer
 *
 * It is called from SVC mode after task_switch_asm returns (bx lr).
 * The kernel stack layout for a fresh task:
 *
 *   [high addr]
 *   return_addr  = task_start_usermode_asm (lr for task_switch_asm)
 *   saved r11
 *   saved r10
 *   ...
 *   saved r4     <- sp points here
 *   [low addr]
 *
 * task_switch_asm does:
 *   stmfd sp!, {r4-r11, lr}  -- save callee-saved
 *   ... switch sp ...
 *   ldmfd sp!, {r4-r11, lr}  -- restore
 *   bx lr                    -- jump to lr (our return_addr)
 *
 * So we build a frame where lr = task_start_usermode_asm.
 */
static uint32_t build_usermode_kernel_stack(uint32_t entry, uint32_t user_sp)
{
    uint32_t stack_top = (uint32_t)&init_kernel_stack[ARM32_KERNEL_STACK_SIZE];

    /* Align to 8 bytes per AAPCS */
    stack_top &= ~7U;

    /* Build the frame for task_switch_asm:
     *   ldmfd sp!, {r4-r11, lr}
     *   bx lr    -> jumps to task_start_usermode_asm
     *
     * We need lr at the highest address (pushed last = popped first).
     * Push in reverse order of ldmfd (r4, r5, ..., r11, lr). */

    uint32_t *sp = (uint32_t *)stack_top;

    /* r4  (lowest address, pushed first) */
    *(--sp) = 0;       /* r4 */
    *(--sp) = 0;       /* r5 */
    *(--sp) = 0;       /* r6 */
    *(--sp) = 0;       /* r7 */
    *(--sp) = 0;       /* r8 */
    *(--sp) = 0;       /* r9 */
    *(--sp) = 0;       /* r10 */
    *(--sp) = 0;       /* r11 */
    *(--sp) = (uint32_t)(uintptr_t)task_start_usermode_asm;  /* lr */

    return (uint32_t)sp;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void arm32_task_init(void)
{
    arm32_elf_init();

    /* Create the initial "kernel" task (represents this boot thread) */
    arm32_task_t *init = allocate_task("kernel");
    if (init) {
        init->state = TASK_STATE_RUNNING;
        init->page_directory = NULL;  /* uses kernel L1 table */
        arm32_current_task = init;
    }
}

arm32_task_t *arm32_task_create_elf(const char *name,
                                    const uint8_t *elf_data,
                                    uint32_t elf_size)
{
    arm32_task_t *t = allocate_task(name);
    if (!t) {
        return NULL;
    }

    uint32_t entry = 0;
    uint32_t sp    = 0;
    arm_l1_table_t *l1 = NULL;

    int rc = arm32_elf_load(elf_data, elf_size, &entry, &sp, &l1);
    if (rc != 0) {
        return NULL;
    }

    t->entry_point    = entry;
    t->user_sp        = sp;
    t->page_directory = l1;

    /* Build the kernel stack frame so task_switch_asm -> bx lr
     * lands in task_start_usermode_asm(entry, user_sp). */
    t->kernel_sp = build_usermode_kernel_stack(entry, sp);

    t->state = TASK_STATE_READY;

    return t;
}

void arm32_task_start(const uint8_t *elf_data, uint32_t elf_size)
{
    uint32_t entry = 0;
    uint32_t sp    = 0;
    arm_l1_table_t *l1 = NULL;

    /* Load the ELF */
    int rc = arm32_elf_load(elf_data, elf_size, &entry, &sp, &l1);
    if (rc != 0) {
        early_puts("[TASK] ELF load failed, code=");
        /* Print error code as single digit */
        char digit = '0' + (char)(-rc);
        early_puts(&digit);
        early_puts("\r\n");
        for (;;) { arm32_wfi(); }
    }

    early_puts("[TASK] ELF loaded: entry=0x");
    /* Hex print entry */
    for (int i = 28; i >= 0; i -= 4) {
        char nib = "0123456789ABCDEF"[(entry >> i) & 0xF];
        early_puts(&nib);
    }
    early_puts(" sp=0x");
    for (int i = 28; i >= 0; i -= 4) {
        char nib = "0123456789ABCDEF"[(sp >> i) & 0xF];
        early_puts(&nib);
    }
    early_puts("\r\n");

    /* Switch to the new page table.
     * Write the L1 table address to TTBR0. */
    uint32_t ttbr0_val = (uint32_t)(uintptr_t)l1;
    arm_write_ttbr0(ttbr0_val);
    arm_flush_tlb_all();

    early_puts("[TASK] Page table switched, entering user mode...\r\n");

    /* Enter user mode -- does not return */
    enter_usermode_asm(entry, sp);

    /* Never reached */
    for (;;) { arm32_wfi(); }
}

void arm32_task_schedule(void)
{
    /* Placeholder: no preemption yet.
     * In a full kernel, this would pick the next READY task
     * and call task_switch_asm(). */
}

void arm32_task_exit(void)
{
    if (arm32_current_task) {
        arm32_current_task->state = TASK_STATE_TERMINATED;
    }
    /* Halt -- no other task to switch to yet */
    for (;;) { arm32_wfi(); }
}
