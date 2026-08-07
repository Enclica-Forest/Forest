/*
 * Forest OS - AArch64 Boot Entry
 * Target: QEMU -machine virt -cpu cortex-a53
 *
 * QEMU virt boots secondary CPUs at EL2. Primary CPU (core 0) enters here.
 * We drop to EL1 and set up the initial kernel environment before calling
 * kernel_main().
 */

#include "boot_defs.h"

    .section ".text.boot", "ax"
    .global _start
    .type   _start, %function

_start:
    /* ------------------------------------------------------------------ */
    /* Step 0: Park all CPUs except core 0                                 */
    /* ------------------------------------------------------------------ */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF           /* Aff0: core ID within cluster       */
    cbz     x0, .Lprimary_cpu
.Lwfe_loop:
    wfe
    b       .Lwfe_loop

.Lprimary_cpu:

    /* ------------------------------------------------------------------ */
    /* Step 1: Determine current exception level                           */
    /* ------------------------------------------------------------------ */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    .Lsetup_el2             /* We are at EL2 - drop to EL1        */
    cmp     x0, #1
    b.eq    .Lsetup_el1             /* Already at EL1                     */
    /* EL3: configure SCR_EL3 then ERET to EL2 */
    bl      .Lsetup_el3_to_el2

.Lsetup_el2:
    /* ------------------------------------------------------------------ */
    /* Step 2: EL2 configuration                                           */
    /* ------------------------------------------------------------------ */

    /* HCR_EL2: RW=1 (EL1 is AArch64), no virtualisation                 */
    mov     x0, #(1 << 31)          /* RW bit - EL1 is AArch64            */
    msr     hcr_el2, x0

    /* SCTLR_EL2: disable MMU, caches, alignment faults                   */
    mov     x0, #0x30C50830         /* RES1 bits per spec                 */
    msr     sctlr_el2, x0

    /* CNTHCTL_EL2: allow EL1/EL0 to access physical counter             */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3             /* EL1PCEN | EL1PCTEN                 */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr       /* Virtual offset = 0                 */

    /* VPIDR_EL2 / VMPIDR_EL2: pass through real IDs to EL1              */
    mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0

    /* Set up SPSR_EL2 for ERET to EL1h (use SP_EL1), IRQ/FIQ masked     */
    mov     x0, #0x3C5             /* DAIF=1111, M[4:0]=00101 = EL1h     */
    msr     spsr_el2, x0

    /* ELR_EL2: return address = EL1 setup code                           */
    adr     x0, .Lsetup_el1
    msr     elr_el2, x0

    isb
    eret                            /* Drop to EL1                        */

.Lsetup_el3_to_el2:
    /* Configure SCR_EL3 to route to EL2 AArch64 */
    mov     x0, #(1 << 10)         /* RW: next level is AArch64          */
    orr     x0, x0, #(1 << 0)      /* NS: non-secure                     */
    msr     scr_el3, x0
    mov     x0, #0x3C9             /* DAIF=1111, M=01001 = EL2h          */
    msr     spsr_el3, x0
    adr     x0, .Lsetup_el2
    msr     elr_el3, x0
    isb
    eret

.Lsetup_el1:
    /* ------------------------------------------------------------------ */
    /* Step 3: EL1 system register configuration (MMU initially off)       */
    /* ------------------------------------------------------------------ */

    /* SCTLR_EL1: clear M (MMU), C (D-cache), I (I-cache)               */
    /* Set RES1 bits per ARMv8 spec, clear everything unsafe              */
    ldr     x0, =SCTLR_EL1_RES1
    msr     sctlr_el1, x0
    isb

    /* CPACR_EL1: FPEN=11 - enable FP/SIMD at EL1 and EL0               */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0

    /* Clear FP state */
    msr     fpcr, xzr
    msr     fpsr, xzr

    /* ------------------------------------------------------------------ */
    /* Step 4: Install exception vector table                              */
    /* ------------------------------------------------------------------ */
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* ------------------------------------------------------------------ */
    /* Step 5: Zero BSS                                                    */
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
    /* Step 6: Set up kernel stack                                         */
    /* ------------------------------------------------------------------ */
    adr     x0, _stack_top
    /* 16-byte align required by AArch64 ABI */
    and     x0, x0, #~0xF
    mov     sp, x0
    msr     sp_el1, x0

    /* ------------------------------------------------------------------ */
    /* Step 7: Call kernel_main()                                          */
    /* ------------------------------------------------------------------ */
    mov     x0, #0                  /* argc = 0                           */
    mov     x1, #0                  /* argv = NULL                        */
    bl      kernel_main

    /* Should never return, but park just in case */
.Lhalt:
    wfi
    b       .Lhalt

    .size _start, . - _start

    /* ------------------------------------------------------------------ */
    /* BSS and stack symbols (defined in linker script)                    */
    /* ------------------------------------------------------------------ */
    .extern _bss_start
    .extern _bss_end
    .extern _stack_top
