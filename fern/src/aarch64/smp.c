/*
 * AArch64 SMP Initialization
 *
 * Uses PSCI (Power State Coordination Interface) or spin-table method
 * from the Device Tree to start secondary CPUs. Each secondary CPU
 * enters via a release address that the primary CPU writes to.
 */

#include "arch/smp.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "arch/arch.h"
#include <string.h>

/* PSCI method detection */
#define PSCI_METHOD_UNKNOWN     0
#define PSCI_METHOD_HVC         1
#define PSCI_METHOD_SMC         2

/* PSCI function IDs (SMC/HVC call numbers, ARMv8 PSCI v0.2+) */
#define PSCI_VERSION            0x84000000
#define PSCI_CPU_ON_AARCH64    0xC4000003
#define PSCI_CPU_OFF           0x84000002
#define PSCI_SYSTEM_RESET      0x84000009

/* PSCI return codes */
#define PSCI_SUCCESS           0
#define PSCI_E_NOT_SUPPORTED   (-1)
#define PSCI_E_INVALID_PARAMS  (-2)
#define PSCI_E_DENIED          (-3)
#define PSCI_E_ALREADY_ON      (-4)
#define PSCI_E_ON_PENDING      (-5)

/* Spin-table: release address is a 64-bit entry the secondary CPU polls */
#define SPIN_TABLE_RELEASE_SHIFT  2   /* physical address >> 2 for spin-table */

/* Per-CPU stack sizes */
#define AP_KERNEL_STACK_SIZE    (16 * 1024)   /* 16 KB */
#define AP_INTERRUPT_STACK_SIZE (8 * 1024)    /* 8 KB  */

/* Static per-CPU data array */
static per_cpu_data_t g_per_cpu_data[SMP_ARCH_MAX_CPUS];
static uint32_t g_online_count = 0;

/* PSCI method used (HVC or SMC) */
static int g_psci_method = PSCI_METHOD_UNKNOWN;

/* Spin-table release addresses (from DTB) */
static uint64_t g_spin_table_addrs[SMP_ARCH_MAX_CPUS];
static uint32_t g_spin_table_count = 0;

/* Secondary CPU entry point (set by primary, read by secondaries) */
static volatile uint64_t g_secondary_entry_point = 0;

/*
 * aarch64_psci_call - Issue a PSCI SMC/HVC call.
 *
 * @func_id:  PSCI function ID
 * @arg0-arg3: Arguments
 *
 * Returns PSCI return code.
 */
static int64_t aarch64_psci_call(uint32_t func_id, uint64_t arg0,
                                  uint64_t arg1, uint64_t arg2)
{
    register uint64_t x0 asm("x0") = func_id;
    register uint64_t x1 asm("x1") = arg0;
    register uint64_t x2 asm("x2") = arg1;
    register uint64_t x3 asm("x3") = arg2;

    if (g_psci_method == PSCI_METHOD_HVC) {
        __asm__ volatile ("hvc #0"
                          : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                          :: "memory");
    } else {
        __asm__ volatile ("smc #0"
                          : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                          :: "memory");
    }

    return (int64_t)x0;
}

/*
 * aarch64_detect_psci_method - Determine if PSCI uses HVC or SMC.
 *
 * Checks CNTFRQ_EL0 (if non-zero, EL2 is present and we're likely
 * using HVC). For QEMU virt, PSCI is typically via HVC.
 */
static void aarch64_detect_psci_method(void)
{
    /* If EL2 is present (QEMU virt), PSCI is typically HVC */
    uint32_t el = aarch64_current_el();
    if (el <= 2) {
        g_psci_method = PSCI_METHOD_HVC;
    } else {
        g_psci_method = PSCI_METHOD_SMC;
    }

    /* Verify PSCI is available by calling PSCI_VERSION */
    int64_t ver = aarch64_psci_call(PSCI_VERSION, 0, 0, 0);
    if (ver < 0) {
        /* Try the other method */
        g_psci_method = (g_psci_method == PSCI_METHOD_HVC)
                        ? PSCI_METHOD_SMC : PSCI_METHOD_HVC;
        ver = aarch64_psci_call(PSCI_VERSION, 0, 0, 0);
    }

    debuglog(DEBUG_INFO, "AArch64 SMP: PSCI version %d.%d via %s\n",
             (int)(ver >> 16), (int)(ver & 0xFFFF),
             g_psci_method == PSCI_METHOD_HVC ? "HVC" : "SMC");
}

/*
 * aarch64_secondary_entry - Entry point for secondary CPUs.
 *
 * Reached via PSCI CPU_ON or spin-table release. Each secondary
 * CPU has its own stack and per-CPU data.
 *
 * This function is called with the MMU already off (secondary CPUs
 * start at the physical address written by the primary). It must
 * set up minimal EL1 state and jump to the C ap_main() function.
 */
void aarch64_secondary_entry(void)
{
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t cpu_id = (uint32_t)(mpidr & 0xFF);

    per_cpu_data_t *per_cpu = smp_get_per_cpu(cpu_id);
    if (!per_cpu) {
        for (;;) { __asm__ volatile ("wfi"); }
    }

    /* Set up vector table */
    extern char vectors[];
    __asm__ volatile ("msr vbar_el1, %0" :: "r"((uint64_t)vectors));

    /* Set up per-CPU stack */
    uint64_t sp = per_cpu->kernel_stack_top;
    __asm__ volatile ("mov sp, %0" :: "r"(sp));

    per_cpu->online = true;

    debuglog(DEBUG_INFO, "AArch64 SMP: CPU %u online\n", cpu_id);

    /* Enable IRQ */
    __asm__ volatile ("msr daifclr, #2");

    /* Idle loop */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

/*
 * aarch64_start_secondary_psci - Start a secondary CPU via PSCI.
 */
static int aarch64_start_secondary_psci(uint32_t cpu_id, uint64_t entry_addr)
{
    int64_t result = aarch64_psci_call(PSCI_CPU_ON_AARCH64,
                                        (uint64_t)cpu_id << 8,  /* MPIDR Aff0 shifted */
                                        entry_addr,
                                        0 /* context ID */);

    if (result == PSCI_SUCCESS) {
        return 0;
    } else if (result == PSCI_E_ALREADY_ON) {
        debuglog(DEBUG_DEBUG, "AArch64 SMP: CPU %u already on\n", cpu_id);
        return 0;  /* Already started, not an error */
    }

    debuglog(DEBUG_WARN, "AArch64 SMP: PSCI CPU_ON for CPU %u failed: %ld\n",
             cpu_id, (long)result);
    return -1;
}

/*
 * aarch64_start_secondary_spintable - Start a secondary CPU via spin-table.
 *
 * Writes the entry point to the spin-table release address for the
 * target CPU and issues SEV to wake it.
 */
static int aarch64_start_secondary_spintable(uint32_t cpu_id, uint64_t entry_addr)
{
    if (cpu_id >= g_spin_table_count || g_spin_table_addrs[cpu_id] == 0) {
        debuglog(DEBUG_WARN, "AArch64 SMP: No spin-table entry for CPU %u\n", cpu_id);
        return -1;
    }

    volatile uint64_t *release_addr =
        (volatile uint64_t *)g_spin_table_addrs[cpu_id];

    /* Write the entry point (physical address) */
    *release_addr = entry_addr;

    /* Data Synchronization Barrier to ensure write is visible */
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Send SEV to wake all WFE-spinning CPUs */
    __asm__ volatile ("sev");

    return 0;
}

/*
 * smp_init_arch - AArch64 SMP initialization.
 *
 * Detects PSCI method, discovers secondary CPUs from the CPU DTB node,
 * and starts each secondary CPU.
 */
uint32_t smp_init_arch(void)
{
    memset(g_per_cpu_data, 0, sizeof(g_per_cpu_data));
    memset(g_spin_table_addrs, 0, sizeof(g_spin_table_addrs));
    g_online_count = 0;

    /* Detect PSCI method */
    aarch64_detect_psci_method();

    /* Register BSP */
    uint32_t bsp_id = smp_get_cpu_id();
    g_per_cpu_data[0].cpu_id = 0;
    g_per_cpu_data[0].apic_id = bsp_id;
    g_per_cpu_data[0].online = true;
    g_online_count = 1;

    /*
     * Discover secondary CPUs.
     *
     * In a real implementation, we would parse the DTB "cpus" node
     * to find all CPU entries and their enable-method (psci / spin-table).
     * For now, we use a fixed secondary CPU count from the DTB
     * (read from the /cpus node's #address-cells and child count).
     */
    uint32_t total_cpus = 1;  /* Will be set by DTB parsing */

    /* TODO: Parse DTB to discover secondary CPUs */
    /* For QEMU virt with -smp N, the DTB lists N CPUs */

    if (total_cpus <= 1) {
        debuglog(DEBUG_INFO, "AArch64 SMP: Single CPU mode\n");
        return 1;
    }

    debuglog(DEBUG_INFO, "AArch64 SMP: Starting %u CPUs\n", total_cpus);

    /* Get the physical address of the secondary entry point */
    uint64_t entry_phys = (uint64_t)(uintptr_t)aarch64_secondary_entry;

    /* Start each secondary CPU */
    for (uint32_t i = 1; i < total_cpus; i++) {
        g_per_cpu_data[i].cpu_id = i;
        g_per_cpu_data[i].apic_id = i;  /* Aff0 = core ID */
        g_per_cpu_data[i].online = false;

        /* Allocate per-CPU stacks */
        g_per_cpu_data[i].kernel_stack_top =
            (uint64_t)(uintptr_t)kmalloc(AP_KERNEL_STACK_SIZE) + AP_KERNEL_STACK_SIZE;
        g_per_cpu_data[i].interrupt_stack_top =
            (uint64_t)(uintptr_t)kmalloc(AP_INTERRUPT_STACK_SIZE) + AP_INTERRUPT_STACK_SIZE;

        int result;
        if (g_psci_method != PSCI_METHOD_UNKNOWN) {
            result = aarch64_start_secondary_psci(i, entry_phys);
        } else {
            result = aarch64_start_secondary_spintable(i, entry_phys);
        }

        if (result == 0) {
            /* Wait for the secondary CPU to come online */
            uint32_t timeout = 1000000;
            while (!g_per_cpu_data[i].online && timeout--) {
                __asm__ volatile ("yield");
            }

            if (g_per_cpu_data[i].online) {
                g_online_count++;
                debuglog(DEBUG_INFO, "AArch64 SMP: CPU %u online\n", i);
            } else {
                debuglog(DEBUG_WARN, "AArch64 SMP: CPU %u timed out\n", i);
            }
        }
    }

    debuglog(DEBUG_INFO, "AArch64 SMP: %u CPUs online\n", g_online_count);
    return g_online_count;
}

uint32_t smp_get_cpu_count(void)
{
    return g_online_count > 0 ? g_online_count : 1;
}

uint32_t smp_get_cpu_id(void)
{
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);  /* Aff0 */
}

int smp_send_ipi(uint32_t target_cpu, uint32_t vector)
{
    per_cpu_data_t *target = smp_get_per_cpu(target_cpu);
    if (!target || !target->online) {
        return -1;
    }

    /* Use GIC SGI: INTID = vector (0-15), target = Aff0 bitmask */
    uint16_t target_list = 1U << target_cpu;
    aarch64_gic_sgi((uint8_t)vector, 0, 0, 0, target_list);

    return 0;
}

void smp_halt_others(void)
{
    uint32_t bsp_id = smp_get_cpu_id();

    for (uint32_t i = 0; i < SMP_ARCH_MAX_CPUS; i++) {
        per_cpu_data_t *cpu = &g_per_cpu_data[i];
        if (!cpu->online || cpu->cpu_id == bsp_id) continue;

        /* Try PSCI CPU_OFF */
        aarch64_psci_call(PSCI_CPU_OFF, 0, 0, 0);
    }

    /* Wait for secondaries to halt */
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
    return smp_get_cpu_id() == 0;  /* BSP is always Aff0 = 0 */
}
