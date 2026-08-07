/*
 * Fern - Cross-Architecture Power Management
 * power.c
 *
 * Provides a unified power management interface across all architectures:
 *   x86:     ACPI shutdown via FADT PM1a_CNT, keyboard controller reset
 *   ARM32:   PSCI SMC call for shutdown/reboot
 *   AArch64: PSCI HVC/SMC call for shutdown/reboot
 *   RISC-V:  SBI SRST extension for shutdown/reboot
 *
 * Each architecture implements the low-level operations; this file
 * dispatches to the correct backend at compile time.
 */

#include "power.h"
#include "platform.h"
#include "../include/system.h"
#include "../include/debuglog.h"
#include "../include/interrupt.h"

#if UEFI_BOOT
#include "../uefi/uefi_runtime.h"
#endif

/* =========================================================================
 * x86: ACPI shutdown via FADT PM1a_CNT, keyboard controller reset
 * ========================================================================= */
#if ARCH_IS_X86

#include "../include/acpi.h"

static void x86_acpi_shutdown(void) {
    const acpi_fadt_t *fadt = acpi_get_fadt();
    if (fadt && fadt->pm1a_control_block) {
        /* SLP_TYP = 5 (S5 soft-off), SLP_EN = bit 13 */
        outportw(fadt->pm1a_control_block, (5 << 10) | (1 << 13));
    }
}

static void x86_keyboard_reset(void) {
    const int timeout = 100000;
    for (int i = 0; i < timeout; i++) {
        if ((inportb(0x64) & 0x02) == 0) break;
    }
    outportb(0x64, 0xFE);
}

static void x86_triple_fault(void) {
    struct { uint16 limit; uint32 base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ __volatile__("cli");
    __asm__ __volatile__("lidt %0" : : "m"(null_idt));
    __asm__ __volatile__("int $3");
}

#endif /* ARCH_IS_X86 */

/* =========================================================================
 * ARM32: PSCI SMC calls
 * ========================================================================= */
#if ARCH_ARM32

/* PSCI function IDs (ARMv7 PSCI v0.1+) */
#define ARM32_PSCI_SYSTEM_OFF    0x84000008
#define ARM32_PSCI_SYSTEM_RESET  0x84000009

static void arm32_psci_call(uint32_t func_id) {
    register uint32_t r0 __asm__("r0") = func_id;
    __asm__ __volatile__("smc #0" : "+r"(r0) : : "r1", "r2", "r3", "r12");
}

#endif /* ARCH_ARM32 */

/* =========================================================================
 * AArch64: PSCI calls via SMC (broadest compatibility)
 * ========================================================================= */
#if ARCH_ARM64

/* PSCI function IDs (SMC32-compatible for broad firmware support) */
#define AARCH64_PSCI_SYSTEM_OFF   0x84000008
#define AARCH64_PSCI_SYSTEM_RESET 0x84000009

static void aarch64_psci_call(uint64_t func_id) {
    register uint64_t x0 __asm__("x0") = func_id;
    __asm__ __volatile__(
        "smc #0"
        : "+r"(x0)
        :
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
          "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
          "memory"
    );
}

#endif /* ARCH_ARM64 */

/* =========================================================================
 * RISC-V: SBI SRST (System Reset) extension
 * ========================================================================= */
#if ARCH_RISCV64

#define SBI_EXT_SRST           0x53525354
#define SBI_SRST_SYSTEM_RESET  0
#define SBI_SRST_SYSTEM_SHUTDOWN 1

static void riscv64_sbi_srst(int action) {
    register long a0 __asm__("a0") = SBI_EXT_SRST;
    register long a1 __asm__("a1") = action;
    register long a2 __asm__("a2") = 0; /* reason: NO_REASON */
    __asm__ __volatile__(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2)
        : "memory"
    );
}

#endif /* ARCH_RISCV64 */

/* =========================================================================
 * Common: CPU halt loop
 * ========================================================================= */

static void power_halt_loop(void) {
    debuglog_write("[POWER] System halted - safe to power off\n");
    while (1) {
#if ARCH_IS_X86
        __asm__ __volatile__("cli; hlt");
#elif ARCH_IS_ARM
        __asm__ __volatile__("wfi");
#elif ARCH_RISCV64
        __asm__ __volatile__("wfi");
#endif
    }
}

/* =========================================================================
 * Cross-architecture power management interface
 * ========================================================================= */

bool power_shutdown(void) {
    debuglog_write("[POWER] Shutdown requested\n");

#if UEFI_BOOT
    {
        const EFI_RUNTIME_SERVICES *rt = uefi_runtime_get_services();
        if (rt && rt->ResetSystem) {
            rt->ResetSystem(EFI_RESET_SHUTDOWN, 0, 0, NULL);
        }
    }
#endif

#if ARCH_IS_X86
    x86_acpi_shutdown();
    power_halt_loop();
#elif ARCH_ARM32
    arm32_psci_call(ARM32_PSCI_SYSTEM_OFF);
    power_halt_loop();
#elif ARCH_ARM64
    aarch64_psci_call(AARCH64_PSCI_SYSTEM_OFF);
    power_halt_loop();
#elif ARCH_RISCV64
    riscv64_sbi_srst(SBI_SRST_SYSTEM_SHUTDOWN);
    power_halt_loop();
#else
    power_halt_loop();
#endif

    return false;
}

bool power_reboot(void) {
    debuglog_write("[POWER] Reboot requested\n");

#if UEFI_BOOT
    {
        const EFI_RUNTIME_SERVICES *rt = uefi_runtime_get_services();
        if (rt && rt->ResetSystem) {
            rt->ResetSystem(EFI_RESET_WARM, 0, 0, NULL);
        }
    }
#endif

#if ARCH_IS_X86
    x86_keyboard_reset();
    x86_triple_fault();
#elif ARCH_ARM32
    arm32_psci_call(ARM32_PSCI_SYSTEM_RESET);
    power_halt_loop();
#elif ARCH_ARM64
    aarch64_psci_call(AARCH64_PSCI_SYSTEM_RESET);
    power_halt_loop();
#elif ARCH_RISCV64
    riscv64_sbi_srst(SBI_SRST_SYSTEM_RESET);
    power_halt_loop();
#else
    power_halt_loop();
#endif

    return false;
}

bool power_suspend(void) {
    debuglog_write("[POWER] Suspend requested\n");

#if ARCH_IS_X86
    /* ACPI S3: write SLP_TYP=3 to PM1a_CNT */
    {
        const acpi_fadt_t *fadt = acpi_get_fadt();
        if (fadt && fadt->pm1a_control_block) {
            irq_disable_safe();
            outportw(fadt->pm1a_control_block, (3 << 10) | (1 << 13));
        }
    }
#elif ARCH_ARM32
    /* PSCI CPU_SUSPEND (best-effort; not available on all ARMv7 firmware) */
    arm32_psci_call(0x84000001);
#elif ARCH_ARM64
    /* PSCI CPU_SUSPEND (best-effort; requires ARMv8.1+) */
    aarch64_psci_call(0x84000001);
#endif

    debuglog_write("[POWER] Suspend not supported or failed\n");
    return false;
}

bool power_halt(void) {
    debuglog_write("[POWER] CPU halt requested\n");
    irq_disable_safe();
    power_halt_loop();
    return false;
}
