/*
 * RISC-V 64-bit SMP Initialization
 *
 * Uses SBI HSM (Hart State Management) extension to start secondary harts,
 * or the spin-table method from the Device Tree. Primary hart runs the boot
 * code; secondary harts park in WFI until released.
 */

#include "arch/smp.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "arch/arch.h"
#include <string.h>

/* SBI extension IDs */
#define SBI_EXT_HSM          0x48534D
#define SBI_EXT_TIME         0x54494D45
#define SBI_EXT_IPI          0x735049

/* SBI HSM function IDs */
#define SBI_HSM_HART_START   0
#define SBI_HSM_HART_STOP    1
#define SBI_HSM_HART_GET_STATUS 2
#define SBI_HSM_HART_SUSPEND 3

/* SBI return codes */
#define SBI_SUCCESS          0
#define SBI_ERR_FAILED       (-1)
#define SBI_ERR_NOT_SUPPORTED (-2)
#define SBI_ERR_INVALID_PARAM (-3)
#define SBI_ERR_DENIED       (-4)
#define SBI_ERR_UNAVAILABLE  (-5)

/* SBI Hart States */
#define HART_STOPPED         0
#define HART_STARTING        1
#define HART_STARTED         2
#define HART_SUSPENDED       3

/* Spin-table release address mask */
#define SPIN_TABLE_RELEASE_SHIFT  2   /* physical address >> 2 */

/* Per-CPU stack sizes */
#define AP_KERNEL_STACK_SIZE    (16 * 1024)   /* 16 KB */
#define AP_INTERRUPT_STACK_SIZE (8 * 1024)    /* 8 KB  */

/* Static per-CPU data array */
static per_cpu_data_t g_per_cpu_data[SMP_ARCH_MAX_CPUS];
static uint32_t g_online_count = 0;

/* SBI HSM available flag */
static bool g_sbi_hsm_available = false;

/* Spin-table release addresses (from DTB) */
static uint64_t g_spin_table_addrs[SMP_ARCH_MAX_CPUS];
static uint32_t g_spin_table_count = 0;

/*
 * sbi_call - Generic SBI ecall.
 *
 * @ext:    Extension ID
 * @func:   Function ID
 * @a0-a5:  Arguments
 *
 * Returns { error, value } pair.
 */
typedef struct { long error; long value; } sbi_ret_t;

static sbi_ret_t sbi_call(long ext, long func,
                           long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long a0 asm("a0") = a0;
    register long a1 asm("a1") = a1;
    register long a2 asm("a2") = a2;
    register long a3 asm("a3") = a3;
    register long a4 asm("a4") = a4;
    register long a5 asm("a5") = a5;
    register long a6 asm("a6") = func;
    register long a7 asm("a7") = ext;

    __asm__ volatile ("ecall"
                      : "+r"(a0), "+r"(a1)
                      : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                      : "memory");

    sbi_ret_t ret;
    ret.error = a0;
    ret.value = a1;
    return ret;
}

/*
 * sbi_hsm_hart_start - Start a hart via SBI HSM.
 */
static int sbi_hsm_hart_start(uint64_t hartid, uint64_t entry, uint64_t priv)
{
    sbi_ret_t ret = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_START,
                              hartid, entry, priv, 0, 0, 0);
    return (int)ret.error;
}

/*
 * sbi_hsm_hart_get_status - Get hart state via SBI HSM.
 */
static int sbi_hsm_hart_get_status(uint64_t hartid)
{
    sbi_ret_t ret = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_GET_STATUS,
                              hartid, 0, 0, 0, 0, 0);
    return (int)ret.value;
}

/*
 * riscv64_secondary_entry - Entry point for secondary harts.
 *
 * Called from the SBI HSM start function or after spin-table release.
 * Each secondary hart sets up its per-CPU data and enters the idle loop.
 */
void riscv64_secondary_entry(void)
{
    uint64_t hartid;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(hartid));

    per_cpu_data_t *per_cpu = smp_get_per_cpu((uint32_t)hartid);
    if (!per_cpu) {
        for (;;) { __asm__ volatile ("wfi"); }
    }

    /* Set up per-CPU stack */
    uint64_t sp = per_cpu->kernel_stack_top;
    __asm__ volatile ("mv sp, %0" :: "r"(sp));

    /* Set supervisor trap vector */
    extern uint64_t stvec_handler[];
    __asm__ volatile ("csrw stvec, %0" :: "r"(stvec_handler));

    per_cpu->online = true;

    debuglog(DEBUG_INFO, "RISC-V SMP: Hart %lu online\n", hartid);

    /* Enable supervisor interrupts */
    uint64_t sie;
    __asm__ volatile ("csrr %0, sie" : "=r"(sie));
    sie |= (1 << 1);   /* Supervisor software interrupt */
    sie |= (1 << 5);   /* Supervisor timer interrupt */
    __asm__ volatile ("csrw sie, %0" :: "r"(sie));

    /* Idle loop */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

/*
 * riscv64_start_hart_sbi - Start a secondary hart via SBI HSM.
 */
static int riscv64_start_hart_sbi(uint64_t hartid, uint64_t entry)
{
    int result = sbi_hsm_hart_start(hartid, entry, 0);

    if (result == SBI_SUCCESS) {
        return 0;
    }

    debuglog(DEBUG_WARN, "RISC-V SMP: SBI HSM hart_start for hart %lu failed: %d\n",
             hartid, result);
    return -1;
}

/*
 * riscv64_start_hart_spintable - Start a secondary hart via spin-table.
 */
static int riscv64_start_hart_spintable(uint64_t hartid, uint64_t entry)
{
    if (hartid >= g_spin_table_count || g_spin_table_addrs[hartid] == 0) {
        debuglog(DEBUG_WARN, "RISC-V SMP: No spin-table entry for hart %lu\n", hartid);
        return -1;
    }

    volatile uint64_t *release_addr =
        (volatile uint64_t *)g_spin_table_addrs[hartid];

    /* Write the entry point */
    *release_addr = entry;

    /* Data Synchronization Barrier */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    /* Send IPI to wake the hart (SBI IPI call) */
    sbi_call(SBI_EXT_IPI, 0, hartid, 0, 0, 0, 0, 0);

    return 0;
}

/*
 * smp_init_arch - RISC-V 64-bit SMP initialization.
 *
 * Detects SBI HSM support, discovers harts, and starts secondary harts.
 */
uint32_t smp_init_arch(void)
{
    memset(g_per_cpu_data, 0, sizeof(g_per_cpu_data));
    memset(g_spin_table_addrs, 0, sizeof(g_spin_table_addrs));
    g_online_count = 0;

    /* Check if SBI HSM extension is available */
    sbi_ret_t ret = sbi_call(SBI_EXT_HSM, 0, 0, 0, 0, 0, 0, 0);
    g_sbi_hsm_available = (ret.error >= 0);

    debuglog(DEBUG_INFO, "RISC-V SMP: SBI HSM %s\n",
             g_sbi_hsm_available ? "available" : "not available");

    /* Register BSP (hart 0) */
    g_per_cpu_data[0].cpu_id = 0;
    g_per_cpu_data[0].apic_id = 0;
    g_per_cpu_data[0].online = true;
    g_online_count = 1;

    /*
     * Discover secondary harts.
     *
     * In a real implementation, parse the DTB /cpus node to find
     * all enabled harts and their enable-method (sbi / spin-table).
     */
    uint32_t total_harts = 1;  /* Will be set by DTB parsing */

    /* TODO: Parse DTB to discover secondary harts */

    if (total_harts <= 1) {
        debuglog(DEBUG_INFO, "RISC-V SMP: Single hart mode\n");
        return 1;
    }

    debuglog(DEBUG_INFO, "RISC-V SMP: Starting %u harts\n", total_harts);

    uint64_t entry_phys = (uint64_t)(uintptr_t)riscv64_secondary_entry;

    for (uint64_t i = 1; i < total_harts; i++) {
        g_per_cpu_data[i].cpu_id = (uint32_t)i;
        g_per_cpu_data[i].apic_id = (uint32_t)i;
        g_per_cpu_data[i].online = false;

        /* Allocate per-CPU stacks */
        g_per_cpu_data[i].kernel_stack_top =
            (uint64_t)(uintptr_t)kmalloc(AP_KERNEL_STACK_SIZE) + AP_KERNEL_STACK_SIZE;
        g_per_cpu_data[i].interrupt_stack_top =
            (uint64_t)(uintptr_t)kmalloc(AP_INTERRUPT_STACK_SIZE) + AP_INTERRUPT_STACK_SIZE;

        int result;
        if (g_sbi_hsm_available) {
            result = riscv64_start_hart_sbi(i, entry_phys);
        } else {
            result = riscv64_start_hart_spintable(i, entry_phys);
        }

        if (result == 0) {
            /* Wait for the hart to come online */
            uint32_t timeout = 1000000;
            while (!g_per_cpu_data[i].online && timeout--) {
                __asm__ volatile ("nop");
            }

            if (g_per_cpu_data[i].online) {
                g_online_count++;
                debuglog(DEBUG_INFO, "RISC-V SMP: Hart %lu online\n", i);
            } else {
                debuglog(DEBUG_WARN, "RISC-V SMP: Hart %lu timed out\n", i);
            }
        }
    }

    debuglog(DEBUG_INFO, "RISC-V SMP: %u harts online\n", g_online_count);
    return g_online_count;
}

uint32_t smp_get_cpu_count(void)
{
    return g_online_count > 0 ? g_online_count : 1;
}

uint32_t smp_get_cpu_id(void)
{
    uint64_t hartid;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(hartid));
    return (uint32_t)hartid;
}

int smp_send_ipi(uint32_t target_cpu, uint32_t vector)
{
    per_cpu_data_t *target = smp_get_per_cpu(target_cpu);
    if (!target || !target->online) {
        return -1;
    }

    /* Use SBI IPI extension to send software interrupt */
    sbi_ret_t ret = sbi_call(SBI_EXT_IPI, 0, target_cpu, 0, 0, 0, 0, 0);
    return (ret.error == SBI_SUCCESS) ? 0 : -1;
}

void smp_halt_others(void)
{
    uint64_t bsp_id = smp_get_cpu_id();

    for (uint64_t i = 0; i < SMP_ARCH_MAX_CPUS; i++) {
        per_cpu_data_t *cpu = &g_per_cpu_data[i];
        if (!cpu->online || i == bsp_id) continue;

        if (g_sbi_hsm_available) {
            /* Send SBI IPI to interrupt the WFI loop, then the hart should
             * check a shutdown flag */
            sbi_call(SBI_EXT_IPI, 0, i, 0, 0, 0, 0, 0);
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
    return smp_get_cpu_id() == 0;  /* BSP is always hart 0 */
}
