/*
 * ARM32 SMP Initialization (Minimal)
 *
 * Uses PSCI or spin-table method for secondary CPU startup.
 * ARM32 SMP is limited compared to 64-bit architectures due to
 * the lack of a standard firmware interface (no EL3 SMC on most
 * ARMv7 implementations).
 */

#include "arch/smp.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "arch/arch.h"
#include <string.h>

/* PSCI function IDs (ARMv7 PSCI v0.1+) */
#define ARM32_PSCI_VERSION         0x84000000
#define ARM32_PSCI_CPU_ON          0x84000003
#define ARM32_PSCI_CPU_OFF         0x84000002
#define ARM32_PSCI_SYSTEM_RESET    0x84000009

/* PSCI return codes */
#define PSCI_SUCCESS               0
#define PSCI_E_NOT_SUPPORTED       (-1)
#define PSCI_E_INVALID_PARAMS      (-2)
#define PSCI_E_DENIED              (-3)
#define PSCI_E_ALREADY_ON          (-4)

/* Processor modes */
#define ARM32_MODE_SVC    0x13
#define ARM32_MODE_SYS    0x1F

/* Per-CPU stack sizes */
#define AP_KERNEL_STACK_SIZE    (8 * 1024)    /* 8 KB  (smaller for 32-bit) */
#define AP_INTERRUPT_STACK_SIZE (4 * 1024)    /* 4 KB  */

/* Static per-CPU data array */
static per_cpu_data_t g_per_cpu_data[SMP_ARCH_MAX_CPUS];
static uint32_t g_online_count = 0;

/* PSCI method: 0 = none, 1 = HVC, 2 = SMC */
static int g_psci_method = 0;

/* Spin-table release addresses */
static volatile uint32_t *g_spin_table_addrs[SMP_ARCH_MAX_CPUS];
static uint32_t g_spin_table_count = 0;

/*
 * arm32_psci_call - Issue a PSCI SMC/HVC call.
 */
static int32_t arm32_psci_call(uint32_t func_id, uint32_t arg0)
{
    register uint32_t r0 asm("r0") = func_id;
    register uint32_t r1 asm("r1") = arg0;

    if (g_psci_method == 1) {
        /* HVC */
        __asm__ volatile ("hvc #0"
                          : "+r"(r0), "+r"(r1)
                          :: "memory", "r2", "r3", "r12");
    } else {
        /* SMC */
        __asm__ volatile ("smc #0"
                          : "+r"(r0), "+r"(r1)
                          :: "memory", "r2", "r3", "r12");
    }

    return (int32_t)r0;
}

/*
 * arm32_secondary_entry - Entry point for secondary CPUs.
 *
 * Called after spin-table release or PSCI CPU_ON. Sets up the
 * SVC stack and enters the idle loop.
 */
void arm32_secondary_entry(void)
{
    uint32_t mpidr;
    __asm__ volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    uint32_t cpu_id = mpidr & 0x03;

    per_cpu_data_t *per_cpu = smp_get_per_cpu(cpu_id);
    if (!per_cpu) {
        for (;;) { __asm__ volatile ("wfi"); }
    }

    /* Set up VBAR to the kernel vector table */
    extern uint32_t _vector_table[];
    __asm__ volatile ("mcr p15, 0, %0, c12, c0, 0"
                      :: "r"((uint32_t)_vector_table));

    /* Set SVC stack */
    uint32_t sp = (uint32_t)per_cpu->kernel_stack_top;
    __asm__ volatile ("cps #%0" :: "i"(ARM32_MODE_SVC));
    __asm__ volatile ("mov sp, %0" :: "r"(sp));

    per_cpu->online = true;

    debuglog(DEBUG_INFO, "ARM32 SMP: CPU %u online\n", cpu_id);

    /* Enable IRQ (clear I bit in CPSR) */
    uint32_t cpsr;
    __asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
    cpsr &= ~ARM32_CPSR_I;
    __asm__ volatile ("msr cpsr_c, %0" :: "r"(cpsr));

    /* Idle loop */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

/*
 * arm32_start_cpu_psci - Start a secondary CPU via PSCI.
 */
static int arm32_start_cpu_psci(uint32_t cpu_id, uint32_t entry_addr)
{
    int32_t result = arm32_psci_call(ARM32_PSCI_CPU_ON, entry_addr);

    if (result == PSCI_SUCCESS || result == PSCI_E_ALREADY_ON) {
        return 0;
    }

    debuglog(DEBUG_WARN, "ARM32 SMP: PSCI CPU_ON for CPU %u failed: %d\n",
             cpu_id, result);
    return -1;
}

/*
 * arm32_start_cpu_spintable - Start a secondary CPU via spin-table.
 */
static int arm32_start_cpu_spintable(uint32_t cpu_id, uint32_t entry_addr)
{
    if (cpu_id >= g_spin_table_count || !g_spin_table_addrs[cpu_id]) {
        debuglog(DEBUG_WARN, "ARM32 SMP: No spin-table entry for CPU %u\n", cpu_id);
        return -1;
    }

    volatile uint32_t *release_addr = g_spin_table_addrs[cpu_id];

    /* Write the entry point */
    *release_addr = entry_addr;

    /* Data Memory Barrier */
    __asm__ volatile ("dmb sy" ::: "memory");

    /* Send event to wake WFE-spinning CPUs */
    __asm__ volatile ("sev");

    return 0;
}

/*
 * smp_init_arch - ARM32 SMP initialization.
 */
uint32_t smp_init_arch(void)
{
    memset(g_per_cpu_data, 0, sizeof(g_per_cpu_data));
    g_online_count = 0;

    /* Register BSP (CPU 0) */
    uint32_t bsp_id = smp_get_cpu_id();
    g_per_cpu_data[0].cpu_id = 0;
    g_per_cpu_data[0].apic_id = bsp_id;
    g_per_cpu_data[0].online = true;
    g_online_count = 1;

    /* Detect PSCI */
    int32_t ver = arm32_psci_call(ARM32_PSCI_VERSION, 0);
    if (ver >= 0) {
        g_psci_method = 2;  /* SMC for ARMv7 */
        debuglog(DEBUG_INFO, "ARM32 SMP: PSCI version %d.%d via SMC\n",
                 (ver >> 16) & 0xFFFF, ver & 0xFFFF);
    }

    /*
     * Discover secondary CPUs.
     *
     * In a real implementation, parse the DTB /cpus node.
     * For QEMU virt, the CPU count is provided by -smp N.
     */
    uint32_t total_cpus = 1;  /* Set by DTB parsing */

    /* TODO: Parse DTB to discover secondary CPUs */

    if (total_cpus <= 1) {
        debuglog(DEBUG_INFO, "ARM32 SMP: Single CPU mode\n");
        return 1;
    }

    debuglog(DEBUG_INFO, "ARM32 SMP: Starting %u CPUs\n", total_cpus);

    uint32_t entry_phys = (uint32_t)(uintptr_t)arm32_secondary_entry;

    for (uint32_t i = 1; i < total_cpus; i++) {
        g_per_cpu_data[i].cpu_id = i;
        g_per_cpu_data[i].apic_id = i;
        g_per_cpu_data[i].online = false;

        /* Allocate per-CPU stacks */
        g_per_cpu_data[i].kernel_stack_top =
            (uint32_t)(uintptr_t)kmalloc(AP_KERNEL_STACK_SIZE) + AP_KERNEL_STACK_SIZE;
        g_per_cpu_data[i].interrupt_stack_top =
            (uint32_t)(uintptr_t)kmalloc(AP_INTERRUPT_STACK_SIZE) + AP_INTERRUPT_STACK_SIZE;

        int result;
        if (g_psci_method != 0) {
            result = arm32_start_cpu_psci(i, entry_phys);
        } else {
            result = arm32_start_cpu_spintable(i, entry_phys);
        }

        if (result == 0) {
            /* Wait for the CPU to come online */
            uint32_t timeout = 1000000;
            while (!g_per_cpu_data[i].online && timeout--) {
                __asm__ volatile ("yield");
            }

            if (g_per_cpu_data[i].online) {
                g_online_count++;
                debuglog(DEBUG_INFO, "ARM32 SMP: CPU %u online\n", i);
            } else {
                debuglog(DEBUG_WARN, "ARM32 SMP: CPU %u timed out\n", i);
            }
        }
    }

    debuglog(DEBUG_INFO, "ARM32 SMP: %u CPUs online\n", g_online_count);
    return g_online_count;
}

uint32_t smp_get_cpu_count(void)
{
    return g_online_count > 0 ? g_online_count : 1;
}

uint32_t smp_get_cpu_id(void)
{
    uint32_t mpidr;
    __asm__ volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 0x03;  /* Aff0 */
}

int smp_send_ipi(uint32_t target_cpu, uint32_t vector)
{
    per_cpu_data_t *target = smp_get_per_cpu(target_cpu);
    if (!target || !target->online) {
        return -1;
    }

    /*
     * ARM32 IPI delivery depends on the interrupt controller:
     * - GIC: use SGI (INTID 0-15) viaistributor
     * - No GIC: use spin-table flag
     *
     * For QEMU virt with GIC, we would write to GICD_SGIR.
     * This is a placeholder; the GIC driver should provide this.
     */
    debuglog(DEBUG_DEBUG, "ARM32 SMP: IPI to CPU %u vector %u (stub)\n",
             target_cpu, vector);

    return 0;
}

void smp_halt_others(void)
{
    uint32_t bsp_id = smp_get_cpu_id();

    for (uint32_t i = 0; i < SMP_ARCH_MAX_CPUS; i++) {
        per_cpu_data_t *cpu = &g_per_cpu_data[i];
        if (!cpu->online || cpu->cpu_id == bsp_id) continue;

        if (g_psci_method != 0) {
            arm32_psci_call(ARM32_PSCI_CPU_OFF, 0);
        }
    }

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
    return smp_get_cpu_id() == 0;  /* BSP is always CPU 0 */
}
