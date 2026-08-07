/*
 * Fern - AArch64 kernel entry point
 *
 * This file provides the AArch64-specific kernel_main() that is called
 * from boot.S after the BSS is zeroed and the initial stack is set up.
 *
 * Initialisation order:
 *   1. UART (serial console) – needed for all subsequent debug output
 *   2. MMU  – set up page tables and enable address translation
 *   3. GIC  – interrupt controller
 *   4. Timer – periodic tick (100 Hz via CNTP)
 *   5. Syscall dispatch table
 *   6. Idle task + scheduler entry
 */

#include "uart.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "syscall.h"
#include "exception_handlers.h"
#include "task.h"
#include "elf_loader.h"
#include "../fdt.h"
#include "../arch/smp.h"
#include <stdint.h>

/* Defined in linker script */
extern uint64_t _kernel_end;
extern uint64_t _initrd_start;
extern uint64_t _initrd_end;

/* ------------------------------------------------------------------ */
/* AArch64 minimal scheduler                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    AA64_TASK_STATE_RUNNING,
    AA64_TASK_STATE_READY,
    AA64_TASK_STATE_WAITING,
} aarch64_task_state_t;

typedef struct aarch64_task {
    const char *name;
    aarch64_task_state_t state;
    uint64_t ticks_left;
    struct aarch64_task *next;
} aarch64_task_t;

static aarch64_task_t idle_task;
static aarch64_task_t *ready_head = (void *)0;
static aarch64_task_t *current_task_ptr = (void *)0;

static void aarch64_scheduler_add(aarch64_task_t *task)
{
    task->next = ready_head;
    ready_head = task;
}

static aarch64_task_t *aarch64_scheduler_pick(void)
{
    if (!ready_head) {
        return &idle_task;
    }
    aarch64_task_t *t = ready_head;
    ready_head = ready_head->next;
    t->next = (void *)0;
    t->state = AA64_TASK_STATE_RUNNING;
    return t;
}

/* Called from the timer IRQ handler after incrementing tick count */
void aarch64_scheduler_tick(void)
{
    if (current_task_ptr && current_task_ptr != &idle_task) {
        if (current_task_ptr->ticks_left > 0) {
            current_task_ptr->ticks_left--;
        }
        if (current_task_ptr->ticks_left == 0) {
            current_task_ptr->state = AA64_TASK_STATE_READY;
            aarch64_scheduler_add(current_task_ptr);
            current_task_ptr = aarch64_scheduler_pick();
        }
    }
}

/*
 * Override the weak timer_tick() from timer.c so the scheduler
 * is invoked on every timer IRQ.
 */
void timer_tick(void)
{
    aarch64_scheduler_tick();
}

/* ------------------------------------------------------------------ */
/* Idle task                                                          */
/* ------------------------------------------------------------------ */

static void idle_task_func(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ------------------------------------------------------------------ */
/* aarch64_early_init                                                   */
/* Called before MMU is enabled; UART must work with MMIO identity map */
/* ------------------------------------------------------------------ */
static void aarch64_early_init(void)
{
    uart_init();
    uart_puts("\n");
    uart_puts("====================================================\n");
    uart_puts("  Fern - AArch64 (Cortex-A53 / QEMU virt)\n");
    uart_puts("====================================================\n");
    uart_printf("  Kernel end PA: 0x%lx\n",
                (uint64_t)(uintptr_t)&_kernel_end);
    uart_puts("\n");
}

/* ------------------------------------------------------------------ */
/* aarch64_mmu_setup                                                    */
/* ------------------------------------------------------------------ */
static void aarch64_mmu_setup(void)
{
    uart_puts("[init] Setting up MMU page tables...\n");
    aarch64_mmu_init();
    uart_puts("[init] MMU enabled\n");
}

/* ------------------------------------------------------------------ */
/* aarch64_irq_setup                                                    */
/* ------------------------------------------------------------------ */
static void aarch64_irq_setup(void)
{
    uart_puts("[init] Initialising GICv3...\n");
    gicv3_init();
    uart_puts("[init] GICv3 ready\n");
}

/* ------------------------------------------------------------------ */
/* aarch64_timer_setup                                                  */
/* ------------------------------------------------------------------ */
static void aarch64_timer_setup(void)
{
    uart_puts("[init] Starting physical timer at 100 Hz...\n");
    /* Configure CNTP_TVAL_EL1 and CNTP_CTL_EL1 for 100 Hz tick.
     * aarch64_timer_init_phys() programs the countdown, enables the
     * timer, and enables the physical timer PPI (INTID 30) in the GIC. */
    aarch64_timer_init_phys(100);
    uart_puts("[init] Timer running\n");
}

/* ------------------------------------------------------------------ */
/* aarch64_scheduler_setup                                              */
/* ------------------------------------------------------------------ */
static void aarch64_scheduler_setup(void)
{
    uart_puts("[init] Setting up idle task...\n");

    idle_task.name = "idle";
    idle_task.state = AA64_TASK_STATE_READY;
    idle_task.ticks_left = 0;
    idle_task.next = (void *)0;

    aarch64_scheduler_add(&idle_task);

    current_task_ptr = aarch64_scheduler_pick();
    uart_printf("[init] Scheduler ready, current task: %s\n",
                current_task_ptr->name);
}

/* ------------------------------------------------------------------ */
/* aarch64_enable_irqs - unmask IRQs at the PSTATE level               */
/* ------------------------------------------------------------------ */
static void aarch64_enable_irqs(void)
{
    __asm__ volatile("msr daifclr, #2");   /* Clear I bit (IRQ mask) */
    __asm__ volatile("isb");
    uart_puts("[init] IRQs enabled\n");
}

/* ------------------------------------------------------------------ */
/* kernel_main - AArch64 C entry point called from boot.S              */
/* ------------------------------------------------------------------ */
int kernel_main(void *dtb_addr)
{
    (void)dtb_addr; /* TODO: parse DTB for memory map, initrd location */

    /* Phase 1: serial console (no dependencies) */
    aarch64_early_init();

    /* Phase 2: MMU */
    aarch64_mmu_setup();

    /* Phase 3: Interrupt controller */
    aarch64_irq_setup();

    /* Phase 3.5: Parse Device Tree */
    if (dtb_addr) {
        uart_puts("[init] Parsing Device Tree...\n");
        if (fdt_parse(dtb_addr) == 0) {
            uart_puts("[init] Device Tree parsed successfully\n");
        } else {
            uart_puts("[init] WARNING: Device Tree parse failed\n");
        }
    } else {
        uart_puts("[init] No Device Tree address provided\n");
    }

    /* Phase 3.5.1: Initialise PMM from DTB memory nodes */
    if (pmm_init_from_memory_map(NULL, 0) == 0) {
        uart_puts("[init] PMM initialised from Device Tree\n");
    } else {
        uart_puts("[init] WARNING: PMM init from DTB failed\n");
    }

    /* Phase 3.6: SMP — discover and start secondary CPUs via PSCI / spin-table */
    {
        uint32_t online = smp_init_arch();
        if (online > 1) {
            uart_printf("[init] SMP: %u CPUs online\n", online);
        } else {
            uart_puts("[init] SMP: single CPU mode\n");
        }
    }

    /* Phase 4: Syscall dispatch */
    aarch64_syscall_init();

    /* Phase 5: Timer (CNTP at 100 Hz) */
    aarch64_timer_setup();

    /* Phase 6: Idle task + scheduler */
    aarch64_scheduler_setup();

    /* Phase 7: Enable hardware interrupts */
    aarch64_enable_irqs();

    uart_puts("\n[init] AArch64 kernel initialised.\n");

    /* Phase 7.5: Initialize GL software renderer if framebuffer is ready */
#ifdef ENABLE_OPENGL
    {
        extern int framebuffer_is_available(void);
        if (framebuffer_is_available()) {
            extern void gl_init_with_framebuffer(void);
            gl_init_with_framebuffer();
            uart_puts("[init] GL software renderer initialized\n");
        } else {
            uart_puts("[init] GL software renderer skipped (no framebuffer)\n");
        }
    }
#endif

    /* Phase 8: Load initrd ELF and launch first user task */
    uart_puts("[init] Searching for initrd...\n");
    uint64_t initrd_start = (uint64_t)(uintptr_t)&_initrd_start;
    uint64_t initrd_end   = (uint64_t)(uintptr_t)&_initrd_end;

    if (initrd_start != 0 && initrd_end > initrd_start) {
        uint64_t initrd_size = initrd_end - initrd_start;
        uart_printf("[init] Initrd: 0x%lx - 0x%lx (%lu bytes)\n",
                    initrd_start, initrd_end, initrd_size);

        /* Quick check: is this an ELF64? */
        const uint8_t *elf_data = (const uint8_t *)initrd_start;
        if (elf_data[0] == 0x7F && elf_data[1] == 'E' &&
            elf_data[2] == 'L' && elf_data[3] == 'F' &&
            elf_data[4] == 2 /* ELFCLASS64 */) {

            task_init();
            task_t *init_task = task_create_elf("init", elf_data, initrd_size);
            if (init_task) {
                uart_printf("[init] Launching user task '%s' (pid=%d)\n",
                            init_task->name, init_task->pid);

                /* Switch to the user task's page tables and ERET to EL0 */
                uint64_t ttbr0 = init_task->ttbr0_el1;
                uint64_t entry = init_task->entry;
                uint64_t sp    = init_task->sp_el0;

                current_task = init_task;

                /* Load user page tables into TTBR0 */
                __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0));
                __asm__ volatile("dsb ishst");
                __asm__ volatile("tlbi vmalle1is");
                __asm__ volatile("dsb ish");
                __asm__ volatile("isb");

                uart_printf("[init] TTBR0=0x%lx entry=0x%lx sp=0x%lx\n",
                            ttbr0, entry, sp);

                /* Drop to EL0 */
                enter_usermode_asm(entry, sp);
            } else {
                uart_puts("[init] Failed to create init task\n");
            }
        } else {
            uart_puts("[init] Initrd is not an ELF64 binary\n");
        }
    } else {
        uart_puts("[init] No initrd found\n");
    }

    uart_puts("[init] Entering idle loop.\n");

    /*
     * Main kernel loop: run tasks from the scheduler.
     * Each iteration picks the next ready task; the idle task
     * calls WFI when nothing else is runnable.
     */
    for (;;) {
        aarch64_task_t *task = current_task_ptr;
        if (task == &idle_task) {
            __asm__ volatile("wfi");
        }
    }

    /* Never reached */
    return 0;
}
