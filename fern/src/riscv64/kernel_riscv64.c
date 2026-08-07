/*
 * Fern - RISC-V 64-bit kernel entry point
 *
 * This file provides the RISC-V-specific kernel_main() that is called
 * from boot.S after the BSS is zeroed and the initial stack is set up.
 *
 * Initialisation order:
 *   1. UART (serial console) – needed for all subsequent debug output
 *   2. MMU  – set up Sv39 page tables and enable address translation
 *   3. PLIC – interrupt controller
 *   4. CLINT timer – periodic tick
 *   5. Enable supervisor interrupts
 *   6. Hand off to main event loop (WFI with heartbeat)
 */

#include "uart.h"
#include "mmu.h"
#include "plic.h"
#include "clint.h"
#include "task.h"
#include "elf_loader.h"
#include "../fdt.h"
#include "../arch/pmm.h"
#include <stdint.h>

/* Defined in linker script */
extern uint64_t _kernel_end;

/* ------------------------------------------------------------------ */
/* riscv64_early_init                                                  */
/* Called before MMU is enabled; UART must work with MMIO identity map */
/* ------------------------------------------------------------------ */
static void riscv64_early_init(void)
{
    riscv64_uart_init();
    riscv64_uart_puts("\n");
    riscv64_uart_puts("====================================================\n");
    riscv64_uart_puts("  Forest OS RISC-V 64-bit\n");
    riscv64_uart_puts("====================================================\n");
    riscv64_uart_printf("  Kernel end PA: 0x%lx\n",
                        (uint64_t)(uintptr_t)&_kernel_end);
    riscv64_uart_puts("\n");
}

/* ------------------------------------------------------------------ */
/* riscv64_mmu_setup                                                   */
/* ------------------------------------------------------------------ */
static void riscv64_mmu_setup(void)
{
    riscv64_uart_puts("[init] Setting up Sv39 page tables...\n");
    riscv64_mmu_init();
    riscv64_uart_puts("[init] MMU enabled\n");
}

/* ------------------------------------------------------------------ */
/* riscv64_irq_setup                                                   */
/* ------------------------------------------------------------------ */
static void riscv64_irq_setup(void)
{
    riscv64_uart_puts("[init] Initialising PLIC...\n");
    plic_init();
    riscv64_uart_puts("[init] PLIC ready\n");
}

/* ------------------------------------------------------------------ */
/* riscv64_timer_setup                                                 */
/* ------------------------------------------------------------------ */
static void riscv64_timer_setup(void)
{
    riscv64_uart_puts("[init] Starting CLINT timer at 100 Hz...\n");
    clint_init(CLINT_DEFAULT_INTERVAL);
    riscv64_uart_puts("[init] Timer running\n");
}

/* ------------------------------------------------------------------ */
/* riscv64_enable_irqs - unmask supervisor interrupts                  */
/* ------------------------------------------------------------------ */
static void riscv64_enable_irqs(void)
{
    /* Enable supervisor external interrupts (sie.SEIE) */
    uint64_t sie;
    __asm__ volatile("csrr %0, sie" : "=r"(sie));
    sie |= (1ULL << 9);   /* SEIE - Supervisor External Interrupt Enable */
    sie |= (1ULL << 5);   /* STIE - Supervisor Timer Interrupt Enable */
    __asm__ volatile("csrw sie, %0" :: "r"(sie));

    /* Set sstatus.SIE to enable supervisor interrupts globally */
    uint64_t sstatus;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= (1ULL << 1);   /* SIE bit */
    __asm__ volatile("csrw sstatus, %0" :: "r"(sstatus));

    riscv64_uart_puts("[init] Supervisor interrupts enabled\n");
}

/* ------------------------------------------------------------------ */
/* kernel_main - RISC-V 64-bit C entry point called from boot.S       */
/* ------------------------------------------------------------------ */

/*
 * Minimal embedded ELF64: loads at VA 0, entry 0.
 * Code: "j 0" (jal x0, 0) – infinite loop.
 * Used as a test user task until a real ELF is available.
 */
static const uint8_t test_elf[] = {
    /* ELF header (64 bytes) */
    0x7f,'E','L','F',        /* magic                         */
    2,                        /* ELFCLASS64                    */
    1,                        /* little-endian                 */
    1,                        /* EV_CURRENT                    */
    0, 0,0,0, 0,0,0,0,      /* ELFOSABI + padding            */
    2, 0,                     /* ET_EXEC                       */
    0xf3,0,                   /* EM_RISCV (243)                */
    1,0,0,0,                  /* EV_CURRENT                    */
    0,0,0,0, 0,0,0,0,        /* e_entry = 0                   */
    0x40,0,0,0, 0,0,0,0,     /* e_phoff = 64                  */
    0,0,0,0, 0,0,0,0,        /* e_shoff = 0                   */
    0,0,0,0,                  /* e_flags = 0                   */
    0x40,0,                   /* e_ehsize = 64                 */
    0x38,0,                   /* e_phentsize = 56              */
    1,0,                      /* e_phnum = 1                   */
    0,0,  0,0,  0,0,         /* shentsize/shnum/shstrndx      */
    /* Program header (56 bytes) */
    1,0,0,0,                  /* p_type  = PT_LOAD             */
    7,0,0,0,                  /* p_flags = R|W|X               */
    120,0,0,0, 0,0,0,0,      /* p_offset = 120 (0x78)         */
    0,0,0,0, 0,0,0,0,        /* p_vaddr  = 0                  */
    0,0,0,0, 0,0,0,0,        /* p_paddr  = 0                  */
    4,0,0,0, 0,0,0,0,        /* p_filesz = 4                  */
    4,0,0,0, 0,0,0,0,        /* p_memsz  = 4                  */
    0,0x10,0,0, 0,0,0,0,     /* p_align  = 0x1000             */
    /* Code: j 0 (jal x0, 0) */
    0x6f, 0x00, 0x00, 0x00,
};

void kernel_main(void *dtb_addr)
{
    /* Phase 1: serial console (no dependencies) */
    riscv64_early_init();

    /* Phase 2: MMU */
    riscv64_mmu_setup();

    /* Phase 3: Interrupt controller */
    riscv64_irq_setup();

    /* Phase 3.5: Parse Device Tree */
    if (dtb_addr) {
        riscv64_uart_puts("[init] Parsing Device Tree...\n");
        if (fdt_parse(dtb_addr) == 0) {
            riscv64_uart_puts("[init] Device Tree parsed successfully\n");
        } else {
            riscv64_uart_puts("[init] WARNING: Device Tree parse failed\n");
        }
    } else {
        riscv64_uart_puts("[init] No Device Tree address provided\n");
    }

    /* Phase 3.5.1: Initialise PMM from DTB memory nodes */
    if (pmm_init_from_memory_map(NULL, 0) == 0) {
        riscv64_uart_puts("[init] PMM initialised from Device Tree\n");
    } else {
        riscv64_uart_puts("[init] WARNING: PMM init from DTB failed\n");
    }

    /* Phase 3.6: SMP — discover and start secondary harts via SBI HSM / spin-table */
    {
        uint32_t online = smp_init_arch();
        if (online > 1) {
            riscv64_uart_printf("[init] SMP: %u harts online\n", online);
        } else {
            riscv64_uart_puts("[init] SMP: single hart mode\n");
        }
    }

    /* Phase 4: Timer */
    riscv64_timer_setup();

    /* Phase 5: Enable hardware interrupts */
    riscv64_enable_irqs();

    /* Phase 6: Task subsystem */
    riscv64_uart_puts("[init] Initialising task manager...\n");
    riscv64_task_init();

    /* Phase 7: Load and launch the first user task */
    riscv64_uart_puts("[init] Loading test ELF...\n");
    riscv64_task_t *user = riscv64_task_create_elf(
        test_elf, sizeof(test_elf), "init");
    if (user) {
        riscv64_uart_puts("[init] Launching user task...\n");
        riscv64_task_schedule();
    } else {
        riscv64_uart_puts("[init] No user task to launch\n");
    }

    riscv64_uart_puts("\n[init] RISC-V 64-bit kernel initialised.\n");

    /* Phase 6.5: Initialize GL software renderer if framebuffer is ready */
#ifdef ENABLE_OPENGL
    {
        extern int framebuffer_is_available(void);
        if (framebuffer_is_available()) {
            extern void gl_init_with_framebuffer(void);
            gl_init_with_framebuffer();
            riscv64_uart_puts("[init] GL software renderer initialized\n");
        } else {
            riscv64_uart_puts("[init] GL software renderer skipped (no framebuffer)\n");
        }
    }
#endif

    riscv64_uart_puts("[init] Entering event loop (WFI).\n");

    /*
     * Main kernel event loop.
     * When a user task exits, task_exit() switches back here.
     */
    uint64_t last_tick = 0;
    for (;;) {
        __asm__ volatile("wfi");

        uint64_t tick = clint_get_time();
        uint64_t tick_s = tick / CLINT_TICKS_PER_SECOND;
        if (tick_s != last_tick) {
            riscv64_uart_printf("[heartbeat] uptime=%lu s\n", tick_s);
            last_tick = tick_s;
        }
    }

    /* Never reached */
}
