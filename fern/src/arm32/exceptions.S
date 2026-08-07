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
    stmfd  sp!, {r0-r12, lr}
    mrs    r0, spsr
    stmfd  sp!, {r0}
.endm

.macro SAVE_VFP_CONTEXT
    ldr    r1, =current_task
    ldr    r1, [r1]
    cmp    r1, #0
    beq    .Lskip_vfp_save_\@
    ldr    r1, [r1, #248]
    cmp    r1, #0
    beq    .Lskip_vfp_save_\@
    mov    r0, r1
    vmrs   r2, fpexc
    tst    r2, #0x40000000
    beq    .Lskip_vfp_save_\@
    stmfd  sp!, {r2}
    vmrs   r2, fpscr
    stmfd  sp!, {r2}
    stmfd  sp!, {s0, s1, s2, s3, s4, s5, s6, s7}
    stmfd  sp!, {s8, s9, s10, s11, s12, s13, s14, s15}
    stmfd  sp!, {s16, s17, s18, s19, s20, s21, s22, s23}
    stmfd  sp!, {s24, s25, s26, s27, s28, s29, s30, s31}
    mov    r2, #0
    vmsr   fpexc, r2
.Lskip_vfp_save_\@:
.endm

.macro RESTORE_VFP_CONTEXT
    ldr    r1, =current_task
    ldr    r1, [r1]
    cmp    r1, #0
    beq    .Lskip_vfp_restore_\@
    ldr    r1, [r1, #248]
    cmp    r1, #0
    beq    .Lskip_vfp_restore_\@
    mov    r0, r1
    vldm   sp!, {s0, s1, s2, s3, s4, s5, s6, s7}
    vldm   sp!, {s8, s9, s10, s11, s12, s13, s14, s15}
    vldm   sp!, {s16, s17, s18, s19, s20, s21, s22, s23}
    vldm   sp!, {s24, s25, s26, s27, s28, s29, s30, s31}
    ldmfd  sp!, {r2}
    vmsr   fpscr, r2
    ldmfd  sp!, {r2}
    vmsr   fpexc, r2
.Lskip_vfp_restore_\@:
.endm

/* -----------------------------------------------------------------------
 * Macro: RESTORE_CONTEXT_AND_RETURN
 *
 * Pops SPSR and r0-r12, then performs exception return via "ldmfd …, {pc}^"
 * which simultaneously restores PC and CPSR from SPSR.
 * --------------------------------------------------------------------- */
.macro RESTORE_CONTEXT_AND_RETURN
    RESTORE_VFP_CONTEXT
    ldmfd  sp!, {r0}
    msr    spsr_cxsf, r0
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
    sub    lr, lr, #4
    SAVE_CONTEXT
    SAVE_VFP_CONTEXT
    mov    r0, sp
    bl     irq_dispatch
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
    SAVE_VFP_CONTEXT
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
    SAVE_CONTEXT
    SAVE_VFP_CONTEXT
    ldr    r1, [sp, #(13*4)]
    sub    r1, r1, #4
    ldr    r1, [r1]
    bic    r1, r1, #0xFF000000
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
    SAVE_VFP_CONTEXT
    mov    r0, sp
    mrc    p15, 0, r1, c6, c0, 2
    mrc    p15, 0, r2, c5, c0, 1
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
    SAVE_VFP_CONTEXT
    mov    r0, sp
    mrc    p15, 0, r1, c6, c0, 0
    mrc    p15, 0, r2, c5, c0, 0
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
    SAVE_VFP_CONTEXT
    mov    r0, sp
    ldr    r1, [sp, #(13*4)]
    sub    r1, r1, #4
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
