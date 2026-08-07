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
 *   4. Timer – periodic tick
 *   5. Syscall dispatch table
 *   6. Hand off to generic Fern kernel (kernel_main_generic if available)
 *      or spin in a basic event loop.
 */

#include "uart.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "syscall.h"
#include "exception_handlers.h"
#include <stdint.h>

/* Defined in linker script */
extern uint64_t _kernel_end;

/* Forward declaration: generic Fern kernel (provided by kernel.c).
 * We declare it here rather than including the x86-heavy kernel.h.    */
extern int kernel_main(void);

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

    uint64_t *pgd = aarch64_get_kernel_pgd();
    uint64_t pgd_pa = (uint64_t)(uintptr_t)pgd;

    uart_printf("[init] Kernel PGD PA: 0x%lx\n", pgd_pa);

    /*
     * Enable MMU:
     *   TTBR0 = kernel PGD (identity map for physical addresses used pre-MMU)
     *   TTBR1 = kernel PGD (higher-half kernel virtual addresses)
     *
     * Both use the same PGD for simplicity during early boot.  A full
     * implementation would separate user and kernel PGDs.
     */
    aarch64_enable_mmu(pgd_pa, pgd_pa);
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
    uart_puts("[init] Starting generic timer at 100 Hz...\n");
    /* Enable virtual timer PPI in the GIC */
    gicv3_enable_irq(27);   /* VTIMER_INTID */
    timer_init(100);         /* 100 Hz = 10 ms tick */
    uart_puts("[init] Timer running\n");
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
int kernel_main(void)
{
    /* Phase 1: serial console (no dependencies) */
    aarch64_early_init();

    /* Phase 2: MMU */
    aarch64_mmu_setup();

    /* Phase 3: Interrupt controller */
    aarch64_irq_setup();

    /* Phase 4: Syscall dispatch */
    aarch64_syscall_init();

    /* Phase 5: Timer */
    aarch64_timer_setup();

    /* Phase 6: Enable hardware interrupts */
    aarch64_enable_irqs();

    uart_puts("\n[init] AArch64 kernel initialised.\n");
    uart_puts("[init] Entering event loop (WFI).\n");

    /*
     * Main kernel event loop.
     * A more complete implementation would call the generic Fern
     * scheduler here.  For now we just wait for timer ticks.
     */
    uint64_t last_tick = 0;
    for (;;) {
        __asm__ volatile("wfi");

        uint64_t tick = timer_get_tick_count();
        if (tick != last_tick && (tick % 100) == 0) {
            /* Print a heartbeat once per second */
            uart_printf("[heartbeat] uptime=%lu s  ns=%lu\n",
                        tick / 100, timer_get_ns());
            last_tick = tick;
        }
    }

    /* Never reached */
    return 0;
}
