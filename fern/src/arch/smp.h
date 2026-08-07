/*
 * Cross-Architecture SMP API
 *
 * Provides a uniform interface for SMP initialization and CPU management
 * across all supported architectures (x86_64, aarch64, riscv64, arm32).
 */

#ifndef FOREST_SMP_ARCH_H
#define FOREST_SMP_ARCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SMP_ARCH_MAX_CPUS 32

/*
 * Per-CPU data area.
 * Allocated once at SMP init, indexed by CPU ID.
 * Each architecture fills platform-specific fields during secondary CPU startup.
 */
typedef struct per_cpu_data {
    uint32_t cpu_id;
    uint32_t apic_id;           /* x86: APIC ID; ARM: MPIDR Aff0; RISC-V: hart ID */
    bool online;

    /* Architecture-specific stack pointers (set during secondary CPU bringup) */
    uint64_t kernel_stack_top;
    uint64_t interrupt_stack_top;

    /* Pointer to current thread/task (architecture fills at context switch) */
    void *current_task;

    /* GDT/IDT/TSS base for x86 (NULL on other architectures) */
    void *arch_gdt;
    void *arch_idt;
    void *arch_tss;

    /* Exception vector base for ARM (VBAR_EL1 / VBAR) */
    uint64_t vbar;

    /* Architecture-specific storage */
    uint8_t arch_data[256];
} per_cpu_data_t;

/*
 * smp_init_arch - Architecture-specific SMP initialization.
 *
 * Called once from the BSP during kernel startup. Responsible for:
 *   - Discovering secondary CPUs (MADT on x86, DTB on ARM/RISC-V)
 *   - Starting secondary CPUs (INIT-SIPI-SIPI, PSCI, spin-table, SBI HSM)
 *   - Setting up per-CPU data for each online CPU
 *
 * Returns the number of CPUs successfully brought online (including BSP).
 */
uint32_t smp_init_arch(void);

/*
 * smp_get_cpu_count - Return total number of online CPUs.
 */
uint32_t smp_get_cpu_count(void);

/*
 * smp_get_cpu_id - Return the ID of the currently executing CPU.
 *
 * On x86: reads the LAPIC ID.
 * On AArch64: reads MPIDR_EL1 Aff0.
 * On RISC-V: reads mhartid / CSR.
 * On ARM32: reads MPIDR Aff0.
 */
uint32_t smp_get_cpu_id(void);

/*
 * smp_send_ipi - Send an Inter-Processor Interrupt to a target CPU.
 *
 * @target_cpu:  CPU ID to send the IPI to.
 * @vector:      Architecture-specific IPI vector / identifier.
 *
 * On x86: writes the APIC ICR.
 * On AArch64: uses GIC SGI.
 * On RISC-V: uses SBI IPI call or software interrupt.
 * On ARM32: uses SGIC or spin-table signaling.
 *
 * Returns 0 on success, -1 on error.
 */
int smp_send_ipi(uint32_t target_cpu, uint32_t vector);

/*
 * smp_halt_others - Halt all CPUs except the caller.
 *
 * Used during panic or shutdown to stop secondary CPUs.
 */
void smp_halt_others(void);

/*
 * smp_get_per_cpu - Get the per-CPU data area for a given CPU.
 *
 * Returns NULL if cpu_id is invalid.
 */
per_cpu_data_t *smp_get_per_cpu(uint32_t cpu_id);

/*
 * smp_get_current_per_cpu - Get the per-CPU data for the current CPU.
 */
per_cpu_data_t *smp_get_current_per_cpu(void);

/*
 * smp_is_bsp - Return true if the current CPU is the Bootstrap Processor.
 */
bool smp_is_bsp(void);

#endif /* FOREST_SMP_ARCH_H */
