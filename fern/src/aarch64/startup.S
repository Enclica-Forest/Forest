/*
 * Forest OS - AArch64 CPU Startup
 * Target: QEMU -machine virt -cpu cortex-a53
 *
 * QEMU -machine virt boots the primary CPU (core 0) at EL2 by default.
 * This file:
 *   1. Parks all secondary CPUs in a WFE loop.
 *   2. Detects the current exception level.
 *   3. Configures EL2 and drops to EL1 (or configures EL3 and drops to EL2
 *      first if we happen to start at EL3).
 *   4. Installs the exception vector table (VBAR_EL1).
 *   5. Zeroes the BSS section.
 *   6. Sets up the initial kernel stack.
 *   7. Calls kernel_main().
 *
 * The exception vector table symbol "exception_vectors" is defined in
 * vectors.S and is also aliased as "vectors" for compatibility with
 * boot.S which uses the latter name.
 *
 * Register usage during startup (before the stack is valid):
 *   x0-x3   scratch; must not be relied on across labels
 *   All other registers undefined until explicitly initialised.
 *
 * Stack:
 *   _stack_top is defined by the linker script.  The AArch64 ABI requires
 *   SP to be 16-byte aligned at function call boundaries.
 */

    .arch armv8-a
    .cpu  cortex-a53

#include "boot_defs.h"

/* -----------------------------------------------------------------------
 * Kernel stack size.  The linker script allocates this in .bss.
 * --------------------------------------------------------------------- */
#define STACK_SIZE  0x8000          /* 32 KiB                             */

/* -----------------------------------------------------------------------
 * SPSR values for ERET
 *
 * SPSR_EL2 when returning to EL1h:
 *   M[4:0] = 0b00101  = EL1h (EL1 with SP_EL1)
 *   F  = 1 (FIQ masked initially)
 *   I  = 1 (IRQ masked initially)
 *   A  = 1 (SError masked initially)
 *   D  = 1 (Debug masked initially)
 *   => 0x3C5
 *
 * SPSR_EL3 when returning to EL2h:
 *   M[4:0] = 0b01001  = EL2h
 *   DAIF   = 1111
 *   => 0x3C9
 * --------------------------------------------------------------------- */
#define SPSR_EL2_TO_EL1H    0x3C5
#define SPSR_EL3_TO_EL2H    0x3C9

/* -----------------------------------------------------------------------
 * HCR_EL2.RW = bit 31: EL1 executes in AArch64 state
 * --------------------------------------------------------------------- */
#define HCR_EL2_RW          (1 << 31)

/* -----------------------------------------------------------------------
 * SCR_EL3 bits:
 *   RW  = bit 10  : next lower level is AArch64
 *   NS  = bit 0   : non-secure state
 * --------------------------------------------------------------------- */
#define SCR_EL3_RW          (1 << 10)
#define SCR_EL3_NS          (1 << 0)

/* -----------------------------------------------------------------------
 * _start — kernel entry point
 * --------------------------------------------------------------------- */
    .section ".text.boot", "ax"
    .global _start
    .type   _start, %function

_start:
    /* ------------------------------------------------------------------ */
    /* Step 0: Park all CPUs except core 0                                 */
    /*                                                                      */
    /* MPIDR_EL1[7:0] is Aff0 — the CPU number within the innermost       */
    /* cluster.  QEMU virt starts all CPUs at this entry; only core 0      */
    /* should proceed.                                                      */
    /* ------------------------------------------------------------------ */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF           /* Isolate Aff0                       */
    cbz     x0, .Lprimary_cpu
.Lwfe_loop:
    wfe
    b       .Lwfe_loop

.Lprimary_cpu:

    /* ------------------------------------------------------------------ */
    /* Step 1: Determine current exception level                           */
    /*                                                                      */
    /* CurrentEL[3:2] encodes EL:                                          */
    /*   0b01 = EL1   0b10 = EL2   0b11 = EL3                             */
    /* ------------------------------------------------------------------ */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2              /* x0 = current EL (1, 2, or 3)       */
    cmp     x0, #2
    b.eq    .Lsetup_el2             /* QEMU default: start at EL2         */
    cmp     x0, #3
    b.eq    .Lsetup_el3             /* Rare: started at EL3               */
    /* Already at EL1 — skip EL2/EL3 configuration */
    b       .Lsetup_el1

/* -----------------------------------------------------------------------
 * .Lsetup_el3 — configure EL3 and drop to EL2
 *
 * Set SCR_EL3 so that EL2 runs in AArch64 (RW=1) and non-secure (NS=1),
 * then ERET to EL2.
 * --------------------------------------------------------------------- */
.Lsetup_el3:
    mov     x0, #(SCR_EL3_RW | SCR_EL3_NS)
    msr     scr_el3, x0
    isb

    mov     x0, #SPSR_EL3_TO_EL2H
    msr     spsr_el3, x0

    adr     x0, .Lsetup_el2
    msr     elr_el3, x0
    isb
    eret                            /* Jump to EL2                        */

/* -----------------------------------------------------------------------
 * .Lsetup_el2 — configure EL2 and drop to EL1
 *
 * Minimum configuration required before ERET to EL1:
 *   HCR_EL2.RW = 1   — EL1 uses AArch64 (not AArch32)
 *   SCTLR_EL2        — reset to safe RES1 values (MMU/cache off)
 *   CNTHCTL_EL2      — allow EL1/EL0 to access the physical timer
 *   VPIDR/VMPIDR     — pass through real CPU ID to EL1
 *   SPSR_EL2         — encode return to EL1h with all exceptions masked
 *   ELR_EL2          — set return address to .Lsetup_el1
 * --------------------------------------------------------------------- */
.Lsetup_el2:
    /* HCR_EL2: EL1 is AArch64, no virtualisation */
    mov     x0, #HCR_EL2_RW
    msr     hcr_el2, x0
    isb

    /*
     * SCTLR_EL2: write the architecturally mandated RES1 bits.
     * This disables the MMU, data cache, and instruction cache at EL2.
     * Value 0x30C50830 sets all RES1 bits for ARMv8-A and clears M/C/I.
     */
    mov     x0, #0x0830
    movk    x0, #0x30C5, lsl #16
    msr     sctlr_el2, x0
    isb

    /* CNTHCTL_EL2: allow EL1 and EL0 to access the physical counter */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3             /* EL1PCEN | EL1PCTEN                 */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr       /* Zero the virtual counter offset    */

    /* Pass through real MIDR / MPIDR so EL1 sees the real CPU ID */
    mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0

    /* SPSR_EL2: DAIF=1111, M=EL1h (0b00101) => 0x3C5 */
    mov     x0, #SPSR_EL2_TO_EL1H
    msr     spsr_el2, x0

    /* ELR_EL2: return to EL1 setup code */
    adr     x0, .Lsetup_el1
    msr     elr_el2, x0
    isb
    eret                            /* Drop to EL1                        */

/* -----------------------------------------------------------------------
 * .Lsetup_el1 — EL1 system register configuration
 *
 * At this point we are running at EL1.  The MMU is off, caches are off.
 * We configure the minimum set of system registers needed before calling
 * C code:
 *   SCTLR_EL1  — RES1 bits, MMU/cache off
 *   CPACR_EL1  — enable FP/SIMD so C code can use 'd' registers
 *   VBAR_EL1   — install the exception vector table
 * --------------------------------------------------------------------- */
.Lsetup_el1:
    /*
     * SCTLR_EL1: write RES1 bits, clear M (MMU), C (D-cache), I (I-cache).
     * SCTLR_EL1_RES1 = 0x00C50830 is defined in boot_defs.h.
     */
    ldr     x0, =SCTLR_EL1_RES1
    msr     sctlr_el1, x0
    isb

    /* CPACR_EL1: FPEN[21:20] = 0b11 — allow FP/SIMD at EL1 and EL0 */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* Clear FP status registers to a defined state */
    msr     fpcr, xzr
    msr     fpsr, xzr

    /* ------------------------------------------------------------------ */
    /* Step 2: Install exception vector table                              */
    /*                                                                      */
    /* exception_vectors is defined in vectors.S (.text.vectors section).  */
    /* VBAR_EL1 must point to a 2KB-aligned address.                       */
    /* ------------------------------------------------------------------ */
    adr     x0, exception_vectors
    msr     vbar_el1, x0
    isb

    /* ------------------------------------------------------------------ */
    /* Step 3: Zero the BSS section                                        */
    /*                                                                      */
    /* _bss_start and _bss_end are provided by the linker script.          */
    /* We write 8 bytes at a time; the linker script must align both       */
    /* symbols to at least 8 bytes.                                        */
    /* ------------------------------------------------------------------ */
    adr     x0, _bss_start
    adr     x1, _bss_end
    cmp     x0, x1
    b.ge    .Lbss_done
.Lbss_loop:
    str     xzr, [x0], #8
    cmp     x0, x1
    b.lt    .Lbss_loop
.Lbss_done:

    /* ------------------------------------------------------------------ */
    /* Step 4: Initialise the kernel stack                                 */
    /*                                                                      */
    /* _stack_top is defined by the linker script as the high end of the   */
    /* kernel stack region.  We align it to 16 bytes as required by AAPCS. */
    /* Both SP_EL1 (used in EL1 kernel context) and SP (alias for SP_EL1  */
    /* while at EL1) are set.                                              */
    /* ------------------------------------------------------------------ */
    adr     x0, _stack_top
    and     x0, x0, #~0xF          /* Enforce 16-byte alignment          */
    mov     sp, x0
    msr     sp_el1, x0

    /* ------------------------------------------------------------------ */
    /* Step 5: Call kernel_main()                                          */
    /*                                                                      */
    /* kernel_main(int argc, char **argv)                                  */
    /* We pass argc=0, argv=NULL for bare-metal entry.                     */
    /* ------------------------------------------------------------------ */
    mov     x0, xzr                 /* argc = 0                           */
    mov     x1, xzr                 /* argv = NULL                        */
    bl      kernel_main

    /* ------------------------------------------------------------------ */
    /* Step 6: Halt — kernel_main should never return                      */
    /* ------------------------------------------------------------------ */
.Lhalt:
    wfi
    b       .Lhalt

    .size   _start, . - _start

/* -----------------------------------------------------------------------
 * External symbols provided by the linker script
 * --------------------------------------------------------------------- */
    .extern exception_vectors
    .extern _bss_start
    .extern _bss_end
    .extern _stack_top
    .extern kernel_main
