/*
 * kernel_arm32.c - ARM32 kernel entry point for Forest OS
 *
 * Called from boot.S after BSS is zeroed, .data is copied, and the
 * PL011 UART has been initialised for early serial output.
 *
 * This file provides the kernel_main() symbol for the ARM32 build.
 * It replaces the x86-specific src/kernel.c during ARM32 linking.
 *
 * Initialization sequence:
 *   1. UART – boot.S already did early init; re-init via driver for IRQ support
 *   2. MMU  – identity-map kernel RAM + device MMIO regions
 *   3. GIC  – ARM Generic Interrupt Controller v2 (GIC-400)
 *   4. IRQ  – dispatch table for per-IRQ handler registration
 *   5. Timer – ARM Generic Timer at 100 Hz periodic interrupt
 *   6. Enable IRQs in CPSR
 *   7. Enter idle loop (WFI-based, with periodic heartbeat)
 *
 * The generic Fern kernel (src/kernel.c, src/task.c, src/syscall.c) is
 * x86-specific and is NOT linked for the ARM32 target.  Syscall handling
 * is provided by src/arm32/syscall.c via the SWI exception vector.
 */

#include "arm32.h"
#include "timer.h"
#include "vfp.h"
#include "task.h"
#include "elf_loader.h"
#include "../fdt.h"
#include "../arch/smp.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Linker symbols (defined in link.ld)
 * --------------------------------------------------------------------- */
extern char kernel_start[];
extern char _heap_start[];

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */
extern void early_puts(const char *s);

/* -----------------------------------------------------------------------
 * Simple integer-to-decimal helper (avoids libc dependency)
 * --------------------------------------------------------------------- */
static void put_uint64(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (val == 0) {
        buf[--i] = '0';
    } else {
        while (val > 0 && i > 0) {
            buf[--i] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    early_puts(buf + i);
}

/* -----------------------------------------------------------------------
 * Phase 1: UART re-init via driver
 *
 * boot.S already initialised the PL011 for polled 115200 8N1 output.
 * We call uart_init() to install the full driver state so that
 * uart_printf() and interrupt-driven I/O work from this point on.
 * --------------------------------------------------------------------- */
static void arm32_uart_setup(void)
{
    /* The PL011 is already working from boot.S early init.
     * Re-init through the driver to enable the interrupt-ready path. */
    uart_init(115200);
}

/* -----------------------------------------------------------------------
 * Phase 2: MMU setup
 *
 * Identity-map the kernel's physical RAM range and device MMIO regions
 * so that both physical and virtual addressing work after the MMU is
 * enabled.  All mappings use 1 MB section descriptors for simplicity.
 *
 * QEMU virt memory map (relevant regions):
 *   0x08000000 – 0x0801FFFF  GIC-400 (Distributor + CPU Interface)
 *   0x09000000 – 0x09000FFF  PL011 UART
 *   0x10012000 – 0x10012FFF  SP804 Timer (if used; we use Generic Timer)
 *   0x40000000 – 0x47FFFFFF  RAM (1 GiB)
 * --------------------------------------------------------------------- */
static void arm32_mmu_setup(void)
{
    early_puts("[init] Setting up MMU page tables...\r\n");

    /*
     * Identity-map the kernel image: from kernel_start to _heap_start
     * (covers .text.boot, .text, .rodata, .data, .bss).
     * phys == virt so no address translation issues during early boot.
     */
    uint32_t phys_base = (uint32_t)(uintptr_t)kernel_start;
    uint32_t phys_end  = (uint32_t)(uintptr_t)_heap_start;
    uint32_t kernel_size = phys_end - phys_base;

    arm_mmu_init(phys_base, phys_base, kernel_size);

    /*
     * Add device MMIO mappings to the L1 table.
     *
     * arm_mmu_init() loaded the L1 table address into TTBR0.
     * Read it back and add 1 MB section descriptors for device regions.
     *
     * QEMU virt device map:
     *   0x08000000  GIC-400 Distributor  (GICD)
     *   0x08010000  GIC-400 CPU Interface (GICC)
     *   0x09000000  PL011 UART
     *
     * Device memory: B=1 (bufferable), C=0 (not cacheable), TEX=0.
     * This gives Device-nGnRnE semantics (strongly ordered, uncacheable).
     */
    arm_l1_table_t *l1 = (arm_l1_table_t *)(uintptr_t)arm_read_ttbr0();
    uint32_t dev_flags = ARM_MEM_DEVICE | ARM_AP_KERNEL_RW | ARM_SECT_DOMAIN(0);

    arm_map_section(l1, 0x08000000u, 0x08000000u, dev_flags);  /* GICD */
    arm_map_section(l1, 0x08010000u, 0x08010000u, dev_flags);  /* GICC */
    arm_map_section(l1, 0x09000000u, 0x09000000u, dev_flags);  /* UART */

    /* Flush TLB to pick up the new device mappings before enabling MMU. */
    arm_flush_tlb_all();

    /* Enable MMU with data and instruction caches. */
    arm_enable_mmu(l1);

    early_puts("[init] MMU enabled (identity map + device MMIO)\r\n");
}

/* -----------------------------------------------------------------------
 * Phase 3: GIC setup
 * --------------------------------------------------------------------- */
static void arm32_gic_setup(void)
{
    early_puts("[init] Initialising GIC-400 (GICv2)...\r\n");
    gic_init();
    early_puts("[init] GIC ready\r\n");
}

/* -----------------------------------------------------------------------
 * Phase 4: IRQ dispatch table
 * --------------------------------------------------------------------- */
static void arm32_irq_setup(void)
{
    early_puts("[init] Initialising IRQ dispatch table...\r\n");
    irq_init();
    early_puts("[init] IRQ dispatch ready\r\n");
}

/* -----------------------------------------------------------------------
 * Phase 5: Timer setup
 * --------------------------------------------------------------------- */
static void arm32_timer_setup(void)
{
    early_puts("[init] Starting ARM Generic Timer at 100 Hz...\r\n");
    arm32_timer_init(100);
    early_puts("[init] Timer running\r\n");
}

/* -----------------------------------------------------------------------
 * Phase 6: Enable IRQs
 * --------------------------------------------------------------------- */
static void arm32_enable_irqs(void)
{
    arm32_irq_enable();
    early_puts("[init] IRQs enabled (CPSR.I cleared)\r\n");
}

/* -----------------------------------------------------------------------
 * Phase 7: Initrd and user-mode task launch
 *
 * QEMU loads the initrd (cpio/tar archive) at a physical address
 * specified in the DTB under /chosen/linux,initrd-{start,end}.
 * We scan the initrd for the first ELF32 executable and launch it.
 * --------------------------------------------------------------------- */

/**
 * find_initrd - Query the DTB for initrd location.
 *
 * QEMU -machine virt places the initrd start/end addresses in the
 * /chosen node as "linux,initrd-start" and "linux,initrd-end" (u32).
 *
 * @start_out  Receives the physical start address of the initrd.
 * @end_out    Receives the physical end address of the initrd.
 *
 * Returns true if the initrd was found, false otherwise.
 */
static bool find_initrd(uint32_t *start_out, uint32_t *end_out)
{
    uint32_t start = fdt_get_u32("/chosen", "linux,initrd-start", 0);
    uint32_t end   = fdt_get_u32("/chosen", "linux,initrd-end", 0);

    if (start == 0 || end == 0 || end <= start) {
        return false;
    }

    *start_out = start;
    *end_out   = end;
    return true;
}

/**
 * find_elf_in_initrd - Scan the initrd for the first ELF32 executable.
 *
 * Walks the initrd memory looking for the ELF magic bytes (\x7fELF).
 * This is a simple heuristic that works for flat cpio archives or raw
 * concatenated binaries.
 *
 * @initrd_start  Physical start of the initrd.
 * @initrd_size   Size of the initrd in bytes.
 * @elf_size_out  Receives the size of the found ELF (computed from
 *                program headers if possible, otherwise a conservative max).
 *
 * Returns pointer to the ELF data, or NULL if not found.
 */
static const uint8_t *find_elf_in_initrd(uint32_t initrd_start,
                                          uint32_t initrd_size,
                                          uint32_t *elf_size_out)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)initrd_start;
    uint32_t scan_limit = initrd_size - 4;  /* need 4 bytes for magic */

    /* Scan for ELF magic: 0x7f 'E' 'L' 'F' */
    for (uint32_t off = 0; off < scan_limit; off += 4) {
        if (base[off]     == 0x7F &&
            base[off + 1] == 'E'  &&
            base[off + 2] == 'L'  &&
            base[off + 3] == 'F') {

            /* Verify it's a 32-bit ARM executable */
            const elf32_ehdr_t *eh = (const elf32_ehdr_t *)(base + off);
            if (eh->e_ident[EI_CLASS]   != ELF_CLASS_32)  continue;
            if (eh->e_ident[EI_DATA]    != ELF_DATA_2LSB) continue;
            if (eh->e_machine           != ELF_MACHINE_ARM) continue;
            if (eh->e_type              != ELF_TYPE_EXEC)  continue;

            /* Compute ELF extent from program headers */
            uint32_t max_end = off + sizeof(elf32_ehdr_t);
            if (eh->e_phoff > 0 && eh->e_phnum > 0 && eh->e_phentsize > 0) {
                const elf32_phdr_t *ph =
                    (const elf32_phdr_t *)(base + off + eh->e_phoff);
                for (uint32_t i = 0; i < eh->e_phnum; i++) {
                    uint32_t seg_end = ph[i].p_offset + ph[i].p_memsz;
                    if (seg_end > max_end) {
                        max_end = seg_end;
                    }
                }
            }

            /* Clamp to initrd bounds */
            uint32_t available = initrd_size - off;
            uint32_t elf_size = max_end - off;
            if (elf_size > available) {
                elf_size = available;
            }

            *elf_size_out = elf_size;
            return base + off;
        }
    }

    return NULL;
}

/**
 * arm32_launch_user_task - Find initrd, locate ELF, and launch it.
 */
static void arm32_launch_user_task(void)
{
    early_puts("[init] Phase 7: Searching for user task...\r\n");

    /* Initialise the task/ELF subsystem */
    arm32_task_init();

    /* Find initrd from Device Tree */
    uint32_t initrd_start = 0, initrd_end = 0;
    if (!find_initrd(&initrd_start, &initrd_end)) {
        early_puts("[init] No initrd found in DTB, skipping user task\r\n");
        return;
    }

    uint32_t initrd_size = initrd_end - initrd_start;
    early_puts("[init] Initrd at 0x");
    {
        char hex[9];
        uint32_t v = initrd_start;
        for (int i = 7; i >= 0; i--) {
            hex[i] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        }
        hex[8] = '\0';
        early_puts(hex);
    }
    early_puts(", size=");
    /* Decimal print initrd_size */
    {
        char buf[12];
        int i = 11;
        buf[i] = '\0';
        uint32_t v = initrd_size;
        if (v == 0) { buf[--i] = '0'; }
        else { while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; } }
        early_puts(buf + i);
    }
    early_puts(" bytes\r\n");

    /* Scan for an ELF in the initrd */
    uint32_t elf_size = 0;
    const uint8_t *elf_data = find_elf_in_initrd(initrd_start, initrd_size,
                                                  &elf_size);
    if (!elf_data) {
        early_puts("[init] No ARM ELF found in initrd, skipping user task\r\n");
        return;
    }

    early_puts("[init] Found ELF in initrd, launching...\r\n");

    /* Load ELF and enter user mode (does not return) */
    arm32_task_start(elf_data, elf_size);

    /* Never reached */
}

/* -----------------------------------------------------------------------
 * kernel_main - ARM32 C entry point (called from boot.S)
 *
 * IRQ and FIQ are masked on entry (boot.S leaves them disabled).
 * Each phase prints a banner and initialises one subsystem.
 * --------------------------------------------------------------------- */
int kernel_main(void *dtb_addr)
{
    early_puts("\r\n");
    early_puts("========================================\r\n");
    early_puts("  Forest OS ARM32 (Cortex-A15 / QEMU)\r\n");
    early_puts("========================================\r\n");
    early_puts("\r\n");

    /* Phase 0: Parse Device Tree */
    if (dtb_addr) {
        early_puts("[init] Parsing Device Tree at 0x");
        /* Print address as hex */
        {
            uint32_t addr = (uint32_t)(uintptr_t)dtb_addr;
            char hex[] = "00000000";
            for (int i = 7; i >= 0; i--) {
                hex[i] = "0123456789abcdef"[addr & 0xf];
                addr >>= 4;
            }
            early_puts(hex);
            early_puts("\r\n");
        }
        if (fdt_parse(dtb_addr) == 0) {
            early_puts("[init] Device Tree parsed successfully\r\n");
        } else {
            early_puts("[init] WARNING: Device Tree parse failed\r\n");
        }
    } else {
        early_puts("[init] No Device Tree address provided\r\n");
    }

    /* Phase 0.5: Initialise PMM from DTB memory nodes */
    if (pmm_init_from_memory_map(NULL, 0) == 0) {
        early_puts("[init] PMM initialised from Device Tree\r\n");
    } else {
        early_puts("[init] WARNING: PMM init from DTB failed\r\n");
    }

    /* Phase 3.5: SMP — discover and start secondary CPUs via PSCI / spin-table */
    {
        uint32_t online = smp_init_arch();
        if (online > 1) {
            early_puts("[init] SMP: ");
            {
                char buf[12];
                int i = 11;
                buf[i] = '\0';
                uint32_t v = online;
                if (v == 0) { buf[--i] = '0'; }
                else { while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; } }
                early_puts(buf + i);
            }
            early_puts(" CPUs online\r\n");
        } else {
            early_puts("[init] SMP: single CPU mode\r\n");
        }
    }

    /* Phase 1: UART driver */
    arm32_uart_setup();

    /* Phase 2: MMU */
    arm32_mmu_setup();

    /* Phase 2.5: VFP/NEON */
    early_puts("[init] Enabling VFP/NEON...\r\n");
    vfp_init();
    early_puts("[init] VFP/NEON enabled\r\n");

    /* Phase 3: GIC */
    arm32_gic_setup();

    /* Phase 4: IRQ dispatch */
    arm32_irq_setup();

    /* Phase 5: Timer (100 Hz) */
    arm32_timer_setup();

    /* Phase 6: Unmask IRQs */
    arm32_enable_irqs();

    early_puts("\r\n[init] ARM32 kernel initialised.\r\n");

    /* Phase 6.5: Initialize GL software renderer if framebuffer is ready */
#ifdef ENABLE_OPENGL
    {
        extern int framebuffer_is_available(void);
        if (framebuffer_is_available()) {
            extern void gl_init_with_framebuffer(void);
            gl_init_with_framebuffer();
            early_puts("[init] GL software renderer initialized\r\n");
        } else {
            early_puts("[init] GL software renderer skipped (no framebuffer)\r\n");
        }
    }
#endif

    /* Phase 7: Find and launch user task from initrd.
     * If an ELF is found, this switches to user mode and never returns.
     * If no ELF is found, falls through to the idle loop. */
    arm32_launch_user_task();

    early_puts("[init] No user task launched, entering idle loop (WFI).\r\n");

    /*
     * Idle loop.
     *
     * In a full kernel this would call the scheduler to pick the next
     * runnable task.  For now we spin with WFI and print a heartbeat
     * once per second (every 100 ticks at 100 Hz).
     */
    uint64_t last_heartbeat = 0;

    for (;;) {
        arm32_wfi();

        uint64_t ticks = arm32_timer_get_ticks();
        if (ticks - last_heartbeat >= 100) {
            last_heartbeat = ticks;
            early_puts("[heartbeat] tick=");
            put_uint64(ticks);
            early_puts("\r\n");
        }
    }

    /* Never reached */
    return 0;
}
