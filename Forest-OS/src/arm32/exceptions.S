/*
 * exceptions.S - ARM32 exception handler stubs for Forest OS
 *
 * Target: ARMv7-A (Cortex-A15)
 *
 * Each handler saves the full CPU context onto the mode stack, calls the
 * appropriate C dispatcher, then restores state and returns.
 *
 * ARM exception return:
 *   - IRQ/FIQ:  MOVS pc, lr   (or SUBS pc, lr, #4) restores CPSR from SPSR
 *   - SWI:      MOVS pc, lr   (lr_svc already points past the SWI instruction)
 *   - ABT/UND:  SUBS pc, lr, #4 / #8 (see ARM ARM for exact offsets)
 *
 * Register save layout on stack (grows downward):
 *
 *   SP+0:  r0
 *   SP+4:  r1
 *   ...
 *   SP+48: r12
 *   SP+52: lr_<mode>   (original PC visible to C as saved_lr)
 *   SP+56: spsr        (caller's CPSR)
 *
 * The layout matches struct arm_regs_t in arm32/exceptions.h.
 */

.syntax unified
.arch armv7-a
.cpu  cortex-a15

/* -----------------------------------------------------------------------
 * Macro: SAVE_CONTEXT
 *
 * Must be called immediately on entry to an exception handler, before
 * any register is touched.
 *
 * After this macro:
 *   sp  -> saved context frame (struct arm_regs_t)
 *   r0  = saved SPSR (also accessible at [sp + 14*4])
 *
 * The "^" suffix on STM restores CPSR from SPSR on the final LDM if the
 * PC is in the register list (exception return); here we only use it to
 * access user-mode banked registers.
 * --------------------------------------------------------------------- */
.macro SAVE_CONTEXT
    /* push r0-r12 and the exception LR */
    stmfd  sp!, {r0-r12, lr}
    /* push SPSR so the dispatcher can inspect/modify the interrupted CPSR */
    mrs    r0, spsr
    stmfd  sp!, {r0}
.endm

/* -----------------------------------------------------------------------
 * Macro: RESTORE_CONTEXT_AND_RETURN
 *
 * Pops SPSR and r0-r12, then performs exception return via "ldmfd …, {pc}^"
 * which simultaneously restores PC and CPSR from SPSR.
 * --------------------------------------------------------------------- */
.macro RESTORE_CONTEXT_AND_RETURN
    /* restore SPSR */
    ldmfd  sp!, {r0}
    msr    spsr_cxsf, r0
    /* restore r0-r12 and return (pc = saved lr, CPSR <- SPSR via ^) */
    ldmfd  sp!, {r0-r12, pc}^
.endm

/* -----------------------------------------------------------------------
 * IRQ handler
 *
 * On IRQ entry lr_irq = PC_interrupted + 4, so we subtract 4 before saving
 * to get the address of the interrupted instruction.
 * --------------------------------------------------------------------- */
.section .text

.global irq_handler
.type   irq_handler, %function
irq_handler:
    /* Adjust LR: for IRQ, lr = PC + 4; we want PC of interrupted insn */
    sub    lr, lr, #4

    SAVE_CONTEXT

    /* sp is now struct arm_regs_t * */
    mov    r0, sp               /* arg0 = pointer to saved registers    */
    bl     irq_dispatch          /* C handler in irq_dispatch.c         */

    RESTORE_CONTEXT_AND_RETURN
.size irq_handler, . - irq_handler

/* -----------------------------------------------------------------------
 * FIQ handler
 *
 * FIQ has banked r8-r12 so the handler can avoid saving/restoring them.
 * For simplicity we save the full context anyway.
 * --------------------------------------------------------------------- */
.global fiq_handler
.type   fiq_handler, %function
fiq_handler:
    sub    lr, lr, #4

    SAVE_CONTEXT

    mov    r0, sp
    bl     fiq_dispatch

    RESTORE_CONTEXT_AND_RETURN
.size fiq_handler, . - fiq_handler

/* -----------------------------------------------------------------------
 * SWI (Supervisor Call) handler
 *
 * lr_svc = address of instruction after the SWI, so no adjustment needed.
 *
 * The SWI number is encoded in bits [23:0] of the SWI instruction word.
 * We read it from memory: *(lr - 4) & 0x00FFFFFF.
 * --------------------------------------------------------------------- */
.global swi_handler
.type   swi_handler, %function
swi_handler:
    /* lr already points past the SWI; no adjustment */
    SAVE_CONTEXT

    /*
     * Extract the SWI number from the instruction word.
     *   r1 = address of the SWI instruction  (lr_svc - 4, saved as lr)
     * We read it from [sp + 13*4] which is the saved lr slot.
     */
    ldr    r1, [sp, #(13*4)]   /* saved lr (= addr after SWI)         */
    sub    r1, r1, #4           /* point at the SWI instruction        */
    ldr    r1, [r1]             /* load instruction word               */
    bic    r1, r1, #0xFF000000  /* mask to 24-bit immediate            */

    /*
     * Call: void syscall_handle(struct arm_regs_t *regs, uint32_t swi_num)
     *   r0 = regs, r1 = swi number (already set above)
     */
    mov    r0, sp
    bl     syscall_handle

    RESTORE_CONTEXT_AND_RETURN
.size swi_handler, . - swi_handler

/* -----------------------------------------------------------------------
 * Prefetch abort handler
 *
 * lr_abt = address of aborted instruction + 4
 * --------------------------------------------------------------------- */
.global prefetch_handler
.type   prefetch_handler, %function
prefetch_handler:
    sub    lr, lr, #4

    SAVE_CONTEXT

    /* r0 = regs, r1 = IFAR (faulting address from CP15) */
    mov    r0, sp
    mrc    p15, 0, r1, c6, c0, 2   /* IFAR - Instruction Fault Address  */
    mrc    p15, 0, r2, c5, c0, 1   /* IFSR - Instruction Fault Status   */
    bl     prefetch_abort_handler

    RESTORE_CONTEXT_AND_RETURN
.size prefetch_handler, . - prefetch_handler

/* -----------------------------------------------------------------------
 * Data abort handler
 *
 * lr_abt = address of aborted instruction + 8
 * --------------------------------------------------------------------- */
.global data_handler
.type   data_handler, %function
data_handler:
    sub    lr, lr, #8

    SAVE_CONTEXT

    /* r0 = regs, r1 = DFAR, r2 = DFSR */
    mov    r0, sp
    mrc    p15, 0, r1, c6, c0, 0   /* DFAR - Data Fault Address        */
    mrc    p15, 0, r2, c5, c0, 0   /* DFSR - Data Fault Status         */
    bl     data_abort_handler

    RESTORE_CONTEXT_AND_RETURN
.size data_handler, . - data_handler

/* -----------------------------------------------------------------------
 * Undefined instruction handler
 *
 * lr_und = address of undefined instruction + 4
 * --------------------------------------------------------------------- */
.global undef_handler
.type   undef_handler, %function
undef_handler:
    sub    lr, lr, #4

    SAVE_CONTEXT

    mov    r0, sp               /* regs pointer                        */
    ldr    r1, [sp, #(13*4)]   /* saved lr = addr after bad insn      */
    sub    r1, r1, #4           /* addr of the undefined instruction   */
    bl     undef_instr_handler

    RESTORE_CONTEXT_AND_RETURN
.size undef_handler, . - undef_handler

/* -----------------------------------------------------------------------
 * C-side stubs for abort/undef handlers.
 *
 * These are weak symbols; the real implementations live in
 * arm32/exception_handlers.c.  If that file is not linked, these stubs
 * spin forever with a UART message.
 * --------------------------------------------------------------------- */
.weak  irq_dispatch
.weak  fiq_dispatch
.weak  syscall_handle
.weak  prefetch_abort_handler
.weak  data_abort_handler
.weak  undef_instr_handler
