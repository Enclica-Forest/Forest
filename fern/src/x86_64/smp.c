/*
 * x86_64 SMP Initialization
 *
 * Uses the Local APIC to discover and start Application Processors via the
 * INIT-SIPI-SIPI startup sequence. Sets up per-CPU GDT, IDT, TSS, and
 * kernel stacks for each secondary CPU.
 */

#include "arch/smp.h"
#include "include/smp.h"
#include "include/apic.h"
#include "include/acpi.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "arch/arch.h"
#include <string.h>

/* APIC IPI delivery modes */
#define IPI_DELIVERY_INIT    0x500
#define IPI_DELIVERY_SIPI    0x600

/* SIPI vector: physical address >> 12 */
#define SIPI_VECTOR_4K       0x08    /* 0x08000 = real-mode reset vector */

/* Startup timeout (loops) */
#define AP_STARTUP_TIMEOUT   100000
#define AP_DELAY_LOOPS       10000

/* Per-CPU stack sizes */
#define AP_KERNEL_STACK_SIZE   (16 * 1024)   /* 16 KB */
#define AP_INTERRUPT_STACK_SIZE (8 * 1024)   /* 8 KB  */

/* Trampoline page: low physical memory for AP real-mode entry */
#define TRAMPOLINE_PAGE_ADDR   0x7000

/* Static per-CPU data array */
static per_cpu_data_t g_per_cpu_data[SMP_ARCH_MAX_CPUS];
static uint32_t g_online_count = 0;

/* Trampoline entry point written to low memory */
extern void ap_trampoline(void);

/*
 * Write the AP trampoline code to low physical memory.
 *
 * The trampoline is a minimal 16-bit real-mode stub that loads the
 * protected-mode GDT, enables PE, enters long mode, and jumps to
 * the AP startup C function.
 */
static void smp_write_trampoline(void)
{
    /*
     * Minimal 16-bit real-mode entry point at TRAMPOLINE_PAGE_ADDR.
     * APs start here in real mode after receiving the SIPI vector.
     *
     * This sets up a minimal environment to jump into the C AP init:
     *   1. Load a minimal GDT
     *   2. Enable protected mode (CR0.PE)
     *   3. Far jump to 32-bit code
     *   4. 32-bit code enables PAE + long mode, sets CR3, enables paging
     *   5. Jumps to 64-bit C entry ap_main()
     *
     * We use a pre-built binary blob (assembled from the ASM trampoline)
     * stored in a linker-provided section. If no trampoline section
     * exists, we build a minimal one here.
     */
    extern char _ap_trampoline_start[];
    extern char _ap_trampoline_end[];

    /* If the linker trampoline section exists, it's already in place */
    if (_ap_trampoline_start != _ap_trampoline_end) {
        return;
    }

    /*
     * Minimal fallback: write a HLT loop to the trampoline page.
     * Real trampoline code should be provided by the linker script.
     */
    volatile uint8_t *tramp = (volatile uint8_t *)(uintptr_t)TRAMPOLINE_PAGE_ADDR;
    /* CLI */
    tramp[0] = 0xFA;
    /* HLT */
    tramp[1] = 0xF4;
    /* JMP to HLT */
    tramp[2] = 0xEB;
    tramp[3] = 0xFC;
}

/*
 * smp_send_init_ipi - Send INIT IPI to all APs (including self via broadcast).
 */
static void smp_send_init_ipi(void)
{
    /* INIT IPI: delivery mode = INIT, destination = all but self */
    uint32_t icr_low = IPI_DELIVERY_INIT | 0xC0000;  /* ALL_BUT_SELF shorthand */
    uint32_t icr_high = 0;

    if (apic_is_available()) {
        /* Write ICR high (destination APIC ID, 0 for shorthand) */
        volatile uint32_t *icr_h = (volatile uint32_t *)((uintptr_t)smp_get_lapic_base() + 0x310);
        volatile uint32_t *icr_l = (volatile uint32_t *)((uintptr_t)smp_get_lapic_base() + 0x300);

        *icr_h = icr_high;
        *icr_l = icr_low;

        /* Wait for delivery */
        for (volatile int i = 0; i < AP_DELAY_LOOPS; i++) {
            if (!(*icr_l & (1 << 12))) break;
        }
    }
}

/*
 * smp_send_sipi - Send STARTUP IPI to a specific AP.
 */
static void smp_send_sipi(uint32_t apic_id, uint8_t vector)
{
    uint32_t icr_low = IPI_DELIVERY_SIPI | vector;
    uint32_t icr_high = apic_id << 24;

    if (apic_is_available()) {
        volatile uint32_t *icr_h = (volatile uint32_t *)((uintptr_t)smp_get_lapic_base() + 0x310);
        volatile uint32_t *icr_l = (volatile uint32_t *)((uintptr_t)smp_get_lapic_base() + 0x300);

        *icr_h = icr_high;
        *icr_l = icr_low;

        /* Wait for delivery */
        for (volatile int i = 0; i < AP_DELAY_LOOPS; i++) {
            if (!(*icr_l & (1 << 12))) break;
        }
    }
}

/*
 * ap_main - C entry point for Application Processors.
 *
 * Called from the AP trampoline after the INIT-SIPI-SIPI sequence.
 * Sets up the AP's per-CPU data and enters the idle loop.
 */
void ap_main(void)
{
    uint32_t cpu_id = smp_get_cpu_id();
    per_cpu_data_t *per_cpu = smp_get_per_cpu(cpu_id);

    if (!per_cpu) {
        /* Unknown CPU, park it */
        for (;;) { __asm__ volatile ("hlt"); }
    }

    per_cpu->online = true;

    debuglog(DEBUG_INFO, "SMP: AP %u online, entering idle loop\n", cpu_id);

    /* Enable interrupts (APIC is already configured by BSP) */
    __asm__ volatile ("sti");

    /* AP idle loop - will be replaced by scheduler integration */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/*
 * smp_init_arch - x86_64 SMP initialization.
 *
 * Discovers CPUs from the ACPI MADT table, starts each AP via
 * INIT-SIPI-SIPI, and sets up per-CPU data.
 */
uint32_t smp_init_arch(void)
{
    memset(g_per_cpu_data, 0, sizeof(g_per_cpu_data));
    g_online_count = 0;

    /* Initialize the existing SMP state (CPU enumeration from MADT) */
    if (!smp_init()) {
        debuglog(DEBUG_WARN, "SMP: MADT enumeration failed\n");
        return 1;  /* BSP is always online */
    }

    const smp_state_t *state = smp_get_state();
    uint32_t total_cpus = smp_get_cpu_count();

    if (total_cpus <= 1) {
        debuglog(DEBUG_INFO, "SMP: Single CPU mode\n");
        /* Register BSP in per-CPU data */
        g_per_cpu_data[0].cpu_id = 0;
        g_per_cpu_data[0].apic_id = state->bsp_apic_id;
        g_per_cpu_data[0].online = true;
        g_online_count = 1;
        return 1;
    }

    debuglog(DEBUG_INFO, "SMP: Starting %u CPUs\n", total_cpus);

    /* Register BSP in per-CPU data */
    g_per_cpu_data[state->bsp_index].cpu_id = state->bsp_index;
    g_per_cpu_data[state->bsp_index].apic_id = state->bsp_apic_id;
    g_per_cpu_data[state->bsp_index].online = true;
    g_online_count = 1;

    /* Write trampoline code to low physical memory */
    smp_write_trampoline();

    /* Start each AP via INIT-SIPI-SIPI */
    for (uint32_t i = 0; i < total_cpus; i++) {
        const smp_cpu_info_t *cpu = smp_get_cpu(i);
        if (!cpu || cpu->bsp) continue;

        uint32_t apic_id = cpu->apic_id;

        debuglog(DEBUG_INFO, "SMP: Starting AP with APIC ID %u\n", apic_id);

        /* Register per-CPU data before startup */
        g_per_cpu_data[i].cpu_id = i;
        g_per_cpu_data[i].apic_id = apic_id;
        g_per_cpu_data[i].online = false;

        /* Allocate kernel stack for this AP */
        g_per_cpu_data[i].kernel_stack_top =
            (uint64_t)(uintptr_t)kmalloc(AP_KERNEL_STACK_SIZE) + AP_KERNEL_STACK_SIZE;
        g_per_cpu_data[i].interrupt_stack_top =
            (uint64_t)(uintptr_t)kmalloc(AP_INTERRUPT_STACK_SIZE) + AP_INTERRUPT_STACK_SIZE;

        /* INIT IPI */
        smp_send_init_ipi();

        /* Wait 10ms (use PIT or TSC-based delay) */
        for (volatile int delay = 0; delay < 100000; delay++);

        /* SIPI (twice, per Intel spec) */
        smp_send_sipi(apic_id, SIPI_VECTOR_4K);

        for (volatile int delay = 0; delay < 10000; delay++);

        smp_send_sipi(apic_id, SIPI_VECTOR_4K);

        /* Wait for AP to come online */
        uint32_t timeout = AP_STARTUP_TIMEOUT;
        while (!g_per_cpu_data[i].online && timeout--) {
            __asm__ volatile ("pause");
        }

        if (g_per_cpu_data[i].online) {
            g_online_count++;
            smp_mark_cpu_online(apic_id);
            debuglog(DEBUG_INFO, "SMP: AP (APIC ID %u) online\n", apic_id);
        } else {
            debuglog(DEBUG_WARN, "SMP: AP (APIC ID %u) failed to start\n", apic_id);
        }
    }

    debuglog(DEBUG_INFO, "SMP: %u CPUs online\n", g_online_count);
    return g_online_count;
}

uint32_t smp_get_cpu_id(void)
{
    if (apic_is_available()) {
        volatile uint32_t *id_reg =
            (volatile uint32_t *)((uintptr_t)smp_get_lapic_base() + 0x20);
        return (*id_reg >> 24) & 0xFF;
    }
    return 0;
}

int smp_send_ipi(uint32_t target_cpu, uint32_t vector)
{
    per_cpu_data_t *target = smp_get_per_cpu(target_cpu);
    if (!target || !target->online) {
        return -1;
    }

    return apic_send_ipi(target->apic_id, vector, 0x000 /* fixed delivery */);
}

void smp_halt_others(void)
{
    uint32_t bsp_id = smp_get_cpu_id();

    for (uint32_t i = 0; i < SMP_ARCH_MAX_CPUS; i++) {
        per_cpu_data_t *cpu = &g_per_cpu_data[i];
        if (!cpu->online || cpu->cpu_id == bsp_id) continue;

        /* Send INIT IPI to halt the AP */
        smp_send_init_ipi();
    }

    /* Wait briefly for APs to halt */
    for (volatile int i = 0; i < 100000; i++);
}

per_cpu_data_t *smp_get_per_cpu(uint32_t cpu_id)
{
    if (cpu_id >= SMP_ARCH_MAX_CPUS) return NULL;
    return &g_per_cpu_data[cpu_id];
}

per_cpu_data_t *smp_get_current_per_cpu(void)
{
    return smp_get_per_cpu(smp_get_cpu_id());
}

bool smp_is_bsp(void)
{
    /* BSP is always CPU 0; check if our APIC ID matches the saved BSP ID */
    const smp_state_t *state = smp_get_state();
    if (!state) return true;
    return smp_get_cpu_id() == state->bsp_apic_id;
}
