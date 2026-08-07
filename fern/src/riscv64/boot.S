/*
 * Forest OS - RISC-V 64-bit Boot Entry
 * Target: QEMU -machine virt -bios opensbi
 *
 * OpenSBI loads the kernel at 0x80200000 in M-mode.
 * This stub transitions to S-mode via ecall, then sets up the initial
 * kernel environment before calling kernel_main().
 *
 * Boot flow:
 *   1. _start (M-mode, loaded by OpenSBI)
 *   2. Configure mstatus for S-mode
 *   3. Set mepc to S-mode entry, disable paging in satp
 *   4. Set up M-mode trap vector (mtvec)
 *   5. ecall → OpenSBI transitions to S-mode at mepc
 *   6. S-mode: zero BSS, set up stack, call kernel_main
 */

#include "boot_defs.h"

    .section ".text.boot", "ax"
    .global _start
    .type   _start, %function

_start:
    /* ------------------------------------------------------------------ */
    /* Step 1: Park all harts except hart 0                               */
    /* ------------------------------------------------------------------ */
    /* a0 = hart ID, a1 = DTB address (OpenSBI passes these) */
    mv      s0, a1              /* s0 = DTB address (preserved across calls) */
    csrr    a0, mhartid
    bnez    a0, .Lpark

    /* ------------------------------------------------------------------ */
    /* Step 2: M-mode configuration before ecall to S-mode                */
    /* ------------------------------------------------------------------ */

    /* Disable paging: satp = 0 */
    csrw    satp, zero

    /* Set up M-mode trap vector (just spin on traps in M-mode) */
    la      t0, mmode_trap_vector
    csrw    mtvec, t0

    /* Configure mstatus:
     *   MPP[1:0] = 11 (Machine) → next privilege mode after mret is S-mode
     *   MPIE     = 1  (enable interrupts in S-mode after mret)
     */
    li      t0, (3 << 11)          /* MPP = 11 (supervisor) */
    li      t1, (1 << 7)           /* MPIE = 1 */
    or      t0, t0, t1
    csrw    mstatus, t0

    /* Set mepc to the S-mode entry point (will be set per-hart below) */
    la      t0, smode_entry
    csrw    mepc, t0

    /* Set mstatus.MPP = 11 so mret drops to S-mode */
    li      t0, (3 << 11)
    csrw    mstatus, t0

    /* ------------------------------------------------------------------ */
    /* Step 3: Request OpenSBI to switch to S-mode                        */
    /* ------------------------------------------------------------------ */
    /* ecall in M-mode invokes OpenSBI runtime which:
     *   - Reads mstatus.MPP → sets privilege to S-mode
     *   - Returns at mepc (smode_entry)                   */
    ecall
    /* We should never return here */
    j       .Lpark

/* ---------------------------------------------------------------------- */
/* M-mode trap vector (simple spin loop for M-mode traps)                 */
/* ---------------------------------------------------------------------- */
    .align 2
mmode_trap_vector:
    csrr    t0, mcause
    csrr    t1, mepc
    csrr    t2, mtval
    j       mmode_trap_vector

/* ---------------------------------------------------------------------- */
/* S-mode entry point (reached after ecall → OpenSBI → S-mode)           */
/* ---------------------------------------------------------------------- */
    .section ".text.boot", "ax"
    .align 2
    .global smode_entry
    .type   smode_entry, %function
smode_entry:
    /* ------------------------------------------------------------------ */
    /* Step 4: S-mode initial configuration                               */
    /* ------------------------------------------------------------------ */

    /* Disable paging in supervisor mode too (defensive) */
    csrw    satp, zero

    /* Set supervisor trap vector to our S-mode handler */
    la      t0, stvec_handler
    csrw    stvec, t0

    /* Clear interrupt pending bits (we're not ready for interrupts yet) */
    csrw    sip, zero

    /* ------------------------------------------------------------------ */
    /* Step 5: Zero BSS                                                   */
    /* ------------------------------------------------------------------ */
    la      t0, _bss_start
    la      t1, _bss_end
    bgeu    t0, t1, .Lbss_done
.Lbss_loop:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    bltu    t0, t1, .Lbss_loop
.Lbss_done:

    /* ------------------------------------------------------------------ */
    /* Step 6: Set up kernel stack (16KB, 16-byte aligned)                */
    /* ------------------------------------------------------------------ */
    la      sp, _stack_top
    andi    sp, sp, ~0xF           /* 16-byte align per RISC-V ABI */

    /* ------------------------------------------------------------------ */
    /* Step 7: Call kernel_main(dtb_addr)                                  */
    /* ------------------------------------------------------------------ */
    /* a0 = argc (unused), a1 = DTB address */
    li      a0, 0
    mv      a1, s0              /* a1 = DTB address */
    call    kernel_main

    /* Should never return, but park just in case */
.Lpark:
    wfi
    j       .Lpark

    .size _start, . - _start

/* ---------------------------------------------------------------------- */
/* S-mode trap vector stub                                                 */
/* ---------------------------------------------------------------------- */
    .align 2
    .global stvec_handler
    .type   stvec_handler, %function
stvec_handler:
    /* Minimal trap handler - just spin for now */
    csrr    t0, scause
    j       stvec_handler
    .size   stvec_handler, . - stvec_handler

/* ---------------------------------------------------------------------- */
/* BSS and stack symbols (defined in linker script)                        */
/* ---------------------------------------------------------------------- */
    .extern _bss_start
    .extern _bss_end
    .extern _stack_top
