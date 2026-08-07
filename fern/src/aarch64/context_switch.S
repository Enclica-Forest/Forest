/*
 * Forest OS - AArch64 Context Switch
 * Target: QEMU -machine virt -cpu cortex-a53
 *
 * Low-level context switching for AArch64 EL1.
 *
 * AArch64 calling convention:
 *   Arguments: x0-x7
 *   Callee-saved: x19-x30, sp_el0
 *   Caller-saved: x0-x18
 *
 * Functions:
 *   task_switch_asm(old_sp_ptr, new_sp, old_fpu, new_fpu) — voluntary context switch
 *   task_start_usermode_asm(entry, sp)                    — launch first user task (eret)
 *   enter_usermode_asm(entry, sp)                         — direct user-mode entry (eret)
 *
 * Stack frame layout (144 bytes, 16-byte aligned):
 *
 *   Offset   Register       Notes
 *   ------   --------       -----
 *    +0x00    x19            callee-saved
 *    +0x08    x20            callee-saved
 *    +0x10    x21            callee-saved
 *    +0x18    x22            callee-saved
 *    +0x20    x23            callee-saved
 *    +0x28    x24            callee-saved
 *    +0x30    x25            callee-saved
 *    +0x38    x26            callee-saved
 *    +0x40    x27            callee-saved
 *    +0x48    x28            callee-saved
 *    +0x50    x29 (fp)       frame pointer
 *    +0x58    x30 (lr)       link register
 *    +0x60    sp_el0         user stack pointer (from EL0 or saved kernel value)
 *    +0x68    sctlr_el1      system control register
 *    +0x70    ttbr0_el1      translation table base 0 (user page tables)
 *    +0x78    tcr_el1        translation control register
 *    +0x80    mair_el1       memory attribute indirection register
 *    +0x88    pad            alignment to 16 bytes
 *
 * Total frame size: 0x90 = 144 bytes.
 */

    .arch armv8-a
    .cpu  cortex-a53

#define FRAME_SIZE  0x90    /* 144 bytes, 16-byte aligned */

/* SPSR for dropping to EL0:
 *   M[4:0] = 0b00000  EL0t (EL0, SP_EL0, AArch64)
 *   D = bit 9   = 1   Debug masked
 *   A = bit 8   = 1   SError masked
 *   I = bit 7   = 1   IRQ masked
 *   F = bit 6   = 1   FIQ masked
 *   => 0x3C0
 */
#define SPSR_EL0T_DAIF  0x3C0

    .section ".text", "ax"

/* =======================================================================
 * task_switch_asm — Voluntary context switch
 *
 * void task_switch_asm(uint64_t *old_sp_ptr, uint64_t new_sp,
 *                      void *old_fpu, void *new_fpu);
 *
 * x0 = old_sp_ptr   — where to save current SP (may be NULL)
 * x1 = new_sp        — SP to load for the next task
 * x2 = old_fpu       — FPU context to save to (may be NULL)
 * x3 = new_fpu       — FPU context to restore from (may be NULL)
 *
 * Saves callee-saved registers and system page-table registers to the
 * current kernel stack, saves/restores NEON/FP state, switches SP,
 * restores from the new stack, and returns.  For brand-new tasks the
 * return address on the stack will be task_start_usermode_asm (placed
 * there by the C task setup code).
 * ======================================================================= */
    .global task_switch_asm
    .type   task_switch_asm, %function
task_switch_asm:
    /* Allocate stack frame */
    sub     sp, sp, #FRAME_SIZE

    /* Save callee-saved registers */
    stp     x19, x20, [sp, #0x00]
    stp     x21, x22, [sp, #0x10]
    stp     x23, x24, [sp, #0x20]
    stp     x25, x26, [sp, #0x30]
    stp     x27, x28, [sp, #0x40]
    stp     x29, x30, [sp, #0x50]

    /* Save sp_el0 (user stack pointer when last at EL0, or kernel-saved value) */
    mrs     x19, sp_el0
    str     x19, [sp, #0x60]

    /* Save page-table system registers for per-process address space */
    mrs     x19, sctlr_el1
    mrs     x20, ttbr0_el1
    mrs     x21, tcr_el1
    mrs     x22, mair_el1
    stp     x19, x20, [sp, #0x68]
    stp     x21, x22, [sp, #0x78]

    /* Save new_sp to callee-saved register before C calls clobber x1 */
    mov     x19, x1

    /* Save outgoing SP to *old_sp_ptr (if non-NULL) */
    cbz     x0, .Lskip_save
    str     sp, [x0]
.Lskip_save:

    /* Save NEON/FP state of outgoing task (x2 = old_fpu context, may be NULL) */
    cbz     x2, .Lno_fpu_save
    mov     x0, x2
    bl      fpu_save
.Lno_fpu_save:

    /* Switch to incoming task's kernel stack */
    mov     sp, x19

    /* Restore NEON/FP state of incoming task (x3 = new_fpu context, may be NULL) */
    cbz     x3, .Lno_fpu_restore
    mov     x0, x3
    bl      fpu_restore
.Lno_fpu_restore:

    /* Restore page-table system registers */
    ldp     x19, x20, [sp, #0x68]
    ldp     x21, x22, [sp, #0x78]
    msr     sctlr_el1, x19
    msr     tcr_el1, x21
    msr     mair_el1, x22

    /* Only flush TLB and switch TTBR0 when the page table actually changes.
     * Writing the same TTBR0 value flushes the TLB unnecessarily but is safe;
     * a caller that wants to skip the switch entirely can pre-load TTBR0. */
    mrs     x23, ttbr0_el1
    cmp     x23, x20
    b.eq    .Lskip_ttbr0
    msr     ttbr0_el1, x20
    /* Full inner-shareable TLB invalidate + barrier */
    dsb     ishst
    tlbi    vmalle1is
    dsb     ish
    isb
.Lskip_ttbr0:

    /* Restore sp_el0 */
    ldr     x19, [sp, #0x60]
    msr     sp_el0, x19

    /* Restore callee-saved registers */
    ldp     x29, x30, [sp, #0x50]
    ldp     x27, x28, [sp, #0x40]
    ldp     x25, x26, [sp, #0x30]
    ldp     x23, x24, [sp, #0x20]
    ldp     x21, x22, [sp, #0x10]
    ldp     x19, x20, [sp, #0x00]

    /* Free stack frame and return */
    add     sp, sp, #FRAME_SIZE
    ret
    .size   task_switch_asm, . - task_switch_asm


/* =======================================================================
 * task_start_usermode_asm — Launch first user-mode task
 *
 * void task_start_usermode_asm(uint64_t entry, uint64_t sp);
 *
 * x0 = entry   — user-space entry point (virtual address)
 * x1 = sp      — top of user-mode stack
 *
 * Called when task_switch_asm "returns" here for a brand-new task
 * (the C task setup code placed this address as the return address
 * on the task's kernel stack).
 *
 * Sets sp_el0 to the user stack, configures SPSR_EL1 for EL0t with
 * all exceptions masked, loads the entry point into ELR_EL1, and
 * executes ERET to drop to EL0.  This is the AArch64 equivalent of
 * x86's IRET.
 * ======================================================================= */
    .global task_start_usermode_asm
    .type   task_start_usermode_asm, %function
task_start_usermode_asm:
    /* Set user stack pointer */
    msr     sp_el0, x1

    /* Configure return state: EL0t, all exceptions masked */
    mov     x2, #SPSR_EL0T_DAIF
    msr     spsr_el1, x2

    /* Set exception return address to user entry point */
    msr     elr_el1, x0

    /* Drop to EL0 */
    eret
    .size   task_start_usermode_asm, . - task_start_usermode_asm


/* =======================================================================
 * enter_usermode_asm — Direct user-mode entry
 *
 * void enter_usermode_asm(uint64_t entry, uint64_t sp);
 *
 * x0 = entry   — user-space instruction pointer
 * x1 = sp      — user-space stack pointer
 *
 * Direct (non-scheduler) path for dropping to EL0.  Same ERET pattern
 * as task_start_usermode_asm but intended for one-off kernel→user
 * transitions outside the scheduler.
 * ======================================================================= */
    .global enter_usermode_asm
    .type   enter_usermode_asm, %function
enter_usermode_asm:
    /* Set user stack pointer */
    msr     sp_el0, x1

    /* Configure return state: EL0t, all exceptions masked */
    mov     x2, #SPSR_EL0T_DAIF
    msr     spsr_el1, x2

    /* Set exception return address to user entry point */
    msr     elr_el1, x0

    /* Drop to EL0 */
    eret
    .size   enter_usermode_asm, . - enter_usermode_asm
