/*
 * Fern - RISC-V 64-bit Minimal Task Management
 *
 * Provides task creation from ELF64, round-robin scheduling,
 * and context switching via task_switch_asm / enter_usermode_asm.
 */
#include "task.h"
#include "elf_loader.h"
#include "mmu.h"
#include "uart.h"
#include "clint.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Task structure                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_EXITED
} riscv64_task_state_t;

typedef struct riscv64_task {
    uint64_t              pid;
    riscv64_task_state_t  state;
    uint64_t              kernel_stack_base;
    uint64_t              sp;         /* kernel sp for context switch */
    sv39_pgd_t           *page_table;
    uint64_t              entry_point;
    uint64_t              user_sp;
    uint64_t              satp;       /* SATP value for this task */
    struct riscv64_task  *next;
} riscv64_task_t;

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

#define MAX_TASKS       16
#define KERNEL_STACK_SZ (8 * 1024)

static riscv64_task_t  task_pool[MAX_TASKS];
static uint64_t        next_pid = 1;
static riscv64_task_t *ready_queue = NULL;
riscv64_task_t        *current_task = NULL;

/* Per-task kernel stacks (static allocation) */
static uint8_t task_kstacks[MAX_TASKS][KERNEL_STACK_SZ]
    __attribute__((aligned(16)));
static int kstack_next = 0;

/* Boot task's kernel stack (from boot.S) */
extern uint64_t _stack_top;
static riscv64_task_t boot_task;

/* ------------------------------------------------------------------ */
/* Assembly helpers (context_switch.S)                                  */
/* ------------------------------------------------------------------ */

extern void task_switch_asm(uint64_t *old_sp, uint64_t new_sp);
extern void enter_usermode_asm(uint64_t entry, uint64_t user_sp);
extern void riscv64_new_task_entry(void);

/* ------------------------------------------------------------------ */
/* Build kernel stack frame for a new user task                        */
/*                                                                     */
/* task_switch_asm saves/restores: ra, s0-s11, sp (14 entries = 112B)  */
/* Frame layout (high → low):                                          */
/*   [sp]  = pointer to bottom of frame                                */
/*   [s11] .. [s2] = 0                                                 */
/*   [s1]  = user stack top                                            */
/*   [s0]  = ELF entry point                                           */
/*   [ra]  = riscv64_new_task_entry                                    */
/* After task_switch_asm returns, ra = new_task_entry which calls       */
/* enter_usermode_asm(entry, user_sp).                                 */
/* ------------------------------------------------------------------ */

static void setup_new_task_stack(riscv64_task_t *task,
                                 uint64_t entry, uint64_t user_sp)
{
    uint64_t *ksp = (uint64_t *)(task->kernel_stack_base + KERNEL_STACK_SZ);
    uint64_t *frame_bot = ksp - 14;

    /* Push sp (at offset 0x68) – points to ra */
    *(--ksp) = (uint64_t)frame_bot;
    /* s11 .. s2  (10 entries, all zero) */
    for (int i = 0; i < 10; i++)
        *(--ksp) = 0;
    /* s1 = user stack top */
    *(--ksp) = user_sp;
    /* s0 = entry point */
    *(--ksp) = entry;
    /* ra = new_task_entry */
    *(--ksp) = (uint64_t)riscv64_new_task_entry;

    task->sp = (uint64_t)ksp;
}

/* ------------------------------------------------------------------ */
/* task_create_elf                                                     */
/* ------------------------------------------------------------------ */

riscv64_task_t *riscv64_task_create_elf(const uint8_t *elf_data,
                                         uint64_t size, const char *name)
{
    /* Find a free slot */
    riscv64_task_t *t = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_EXITED ||
            task_pool[i].pid == 0) {
            t = &task_pool[i];
            break;
        }
    }
    if (!t) {
        riscv64_uart_puts("[TASK] No free slots\n");
        return NULL;
    }

    /* Clear */
    uint64_t *p = (uint64_t *)t;
    for (uint64_t *e = (uint64_t *)(t + 1); p < e; p++)
        *p = 0;

    /* Load ELF */
    riscv64_elf_result_t er;
    int rc = riscv64_elf_load(elf_data, size, &er);
    if (rc != 0 || !er.valid) {
        riscv64_uart_printf("[TASK] ELF load err %d\n", rc);
        return NULL;
    }

    /* Allocate kernel stack */
    if (kstack_next >= MAX_TASKS) {
        riscv64_uart_puts("[TASK] No kernel stacks\n");
        return NULL;
    }
    uint64_t kstack = (uint64_t)&task_kstacks[kstack_next][0];
    kstack_next++;

    t->pid              = next_pid++;
    t->state            = TASK_STATE_READY;
    t->kernel_stack_base = kstack;
    t->page_table       = er.page_table;
    t->entry_point      = er.entry_point;
    t->user_sp          = er.stack_top;

    /* SATP for Sv39 */
    uint64_t ppn = (uint64_t)(uintptr_t)er.page_table >> 12;
    t->satp = SATP_SV39(ppn, 0);

    setup_new_task_stack(t, er.entry_point, er.stack_top);

    /* Append to ready queue */
    t->next = NULL;
    if (!ready_queue) {
        ready_queue = t;
    } else {
        riscv64_task_t *q = ready_queue;
        while (q->next)
            q = q->next;
        q->next = t;
    }

    riscv64_uart_printf("[TASK] Created pid=%lu '%s' entry=0x%lx\n",
                        t->pid, name ? name : "?", t->entry_point);
    return t;
}

/* ------------------------------------------------------------------ */
/* task_schedule – round-robin                                          */
/* ------------------------------------------------------------------ */

void riscv64_task_schedule(void)
{
    if (!ready_queue)
        return;

    /* Find next READY task */
    riscv64_task_t *nxt = ready_queue;
    while (nxt && nxt->state != TASK_STATE_READY)
        nxt = nxt->next;

    if (!nxt)
        return;

    if (current_task && current_task->state == TASK_STATE_RUNNING)
        current_task->state = TASK_STATE_READY;

    nxt->state = TASK_STATE_RUNNING;

    if (current_task != nxt) {
        riscv64_task_t *prev = current_task;
        current_task = nxt;

        /* Switch page tables */
        riscv64_set_satp(nxt->satp);

        /* Context switch */
        task_switch_asm(prev ? &prev->sp : NULL, nxt->sp);
    }
}

/* ------------------------------------------------------------------ */
/* task_exit                                                           */
/* ------------------------------------------------------------------ */

void riscv64_task_exit(int code)
{
    if (!current_task)
        return;

    riscv64_uart_printf("[TASK] pid=%lu exit code=%d\n",
                        current_task->pid, code);

    current_task->state = TASK_STATE_EXITED;

    /* Unlink from ready queue */
    riscv64_task_t **pp = &ready_queue;
    while (*pp) {
        if (*pp == current_task) {
            *pp = current_task->next;
            break;
        }
        pp = &(*pp)->next;
    }

    current_task = NULL;
    riscv64_task_schedule();
}

/* ------------------------------------------------------------------ */
/* task_init – set up boot task                                        */
/* ------------------------------------------------------------------ */

void riscv64_task_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        task_pool[i].pid = 0;
        task_pool[i].state = TASK_STATE_EXITED;
    }

    /* Boot task is the kernel thread that called kernel_main() */
    uint64_t *bp = (uint64_t *)&boot_task;
    for (uint64_t *be = (uint64_t *)(&boot_task + 1); bp < be; bp++)
        *bp = 0;

    boot_task.pid              = next_pid++;
    boot_task.state            = TASK_STATE_RUNNING;
    boot_task.kernel_stack_base = (uint64_t)&_stack_top - KERNEL_STACK_SZ;
    boot_task.page_table       = riscv64_get_kernel_pgd();

    uint64_t ppn = (uint64_t)(uintptr_t)boot_task.page_table >> 12;
    boot_task.satp = SATP_SV39(ppn, 0);

    current_task = &boot_task;

    /* Boot task must be in the ready queue so the scheduler can
     * switch back to it after a user task exits. */
    boot_task.next = NULL;
    ready_queue = &boot_task;

    riscv64_uart_printf("[TASK] Init done. Boot pid=%lu\n", boot_task.pid);
}
