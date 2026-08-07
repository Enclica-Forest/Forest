/*
 * context_switch.S - ARM32 context switch for Forest OS
 *
 * Target: ARMv7-A (Cortex-A15)
 * Calling convention: ARM EABI (r0-r3 caller-saved, r4-r11 callee-saved)
 *
 * Provides:
 *   task_switch_asm()          - Voluntary context switch
 *   task_start_usermode_asm()  - Launch first user-mode task
 *   enter_usermode_asm()       - Direct user-mode entry (non-scheduler)
 *
 * Task kernel stack layout when suspended by task_switch_asm:
 *   (high address)
 *   [saved lr]      = return address (caller of task_switch_asm, or
 *                      task_start_usermode_asm for a fresh task)
 *   [saved r11]
 *   [saved r10]
 *   ...
 *   [saved r4]      <- sp points here after stmfd
 *   (low address)
 *
 * When task_switch_asm restores and returns (bx lr), it either returns
 * to the C caller (normal switch) or to task_start_usermode_asm (first
 * launch), which transitions to USR mode.
 *
 * ARM exception return (matching exceptions.S):
 *   ldmfd sp!, {pc}^   - pops PC and restores CPSR from SPSR
 *   movs pc, lr        - PC = lr, CPSR = SPSR (used for SVC return)
 */

.syntax unified
.arch armv7-a
.cpu  cortex-a15

.section .text

/* -----------------------------------------------------------------------
 * void task_switch_asm(uint32_t *old_sp_ptr, uint32_t new_sp)
 *
 * r0 = old_sp_ptr  – pointer to save current SP (may be NULL)
 * r1 = new_sp      – SP to load for the next task
 *
 * Saves callee-saved registers to the outgoing task's kernel stack,
 * switches to the incoming task's stack, restores registers, and returns.
 * ----------------------------------------------------------------------- */
.global task_switch_asm
.type   task_switch_asm, %function
task_switch_asm:
    stmfd  sp!, {r4-r11, lr}

    ldr    r2, =current_task
    ldr    r2, [r2]
    cmp    r2, #0
    beq    .Lskip_cs_vfp_save
    ldr    r3, [r2, #248]
    cmp    r3, #0
    beq    .Lskip_cs_vfp_save
    vmrs   r4, fpexc
    tst    r4, #0x40000000
    beq    .Lskip_cs_vfp_save
    mov    r2, r3
    stmfd  sp!, {s16, s17, s18, s19, s20, s21, s22, s23}
    stmfd  sp!, {s24, s25, s26, s27, s28, s29, s30, s31}
    vmrs   r4, fpscr
    stmfd  sp!, {r4}
    add    r4, r2, #64
    vldm   r4, {s16, s17, s18, s19, s20, s21, s22, s23}
    add    r4, r2, #96
    vldm   r4, {s24, s25, s26, s27, s28, s29, s30, s31}
    ldr    r4, [r2, #128]
    vmsr   fpscr, r4
    mov    r4, #0
    vmsr   fpexc, r4
.Lskip_cs_vfp_save:

    cmp    r0, #0
    beq    .Lskip_save
    str    sp, [r0]
.Lskip_save:

    mov    sp, r1

    ldr    r2, =current_task
    ldr    r2, [r2]
    cmp    r2, #0
    beq    .Lskip_cs_vfp_restore
    ldr    r3, [r2, #248]
    cmp    r3, #0
    beq    .Lskip_cs_vfp_restore
    mov    r2, r3
    ldr    r4, [r2, #128]
    vmsr   fpscr, r4
    add    r4, r2, #64
    vldm   r4, {s16, s17, s18, s19, s20, s21, s22, s23}
    add    r4, r2, #96
    vldm   r4, {s24, s25, s26, s27, s28, s29, s30, s31}
    ldmfd  sp!, {r4}
    vmsr   fpscr, r4
    vldm   sp!, {s24, s25, s26, s27, s28, s29, s30, s31}
    vldm   sp!, {s16, s17, s18, s19, s20, s21, s22, s23}
    mov    r4, #0x40000000
    vmsr   fpexc, r4
.Lskip_cs_vfp_restore:

    ldmfd  sp!, {r4-r11, lr}
    bx     lr
.size task_switch_asm, . - task_switch_asm


/* -----------------------------------------------------------------------
 * void task_start_usermode_asm(uint32_t entry, uint32_t user_sp)
 *
 * r0 = entry     – user-space instruction pointer
 * r1 = user_sp   – user-space stack pointer
 *
 * Entry point for brand-new user tasks.  task_switch_asm "returns" here
 * (bx lr) because the initial kernel stack frame placed this address
 * as lr in the saved context.
 *
 * Transitions from SVC mode to USR mode by:
 *   1. Temporarily entering USR mode to set the user stack pointer
 *      (USR SP is banked and cannot be written from SVC mode)
 *   2. Setting SPSR = 0xD0 (USR mode, IRQ+FIQ disabled)
 *   3. Building an SVC exception return frame on the kernel stack
 *   4. Executing ldmfd sp!, {pc}^ which atomically sets PC = entry
 *      and CPSR = SPSR, completing the transition to user mode
 *
 * This is the ARM equivalent of x86's IRET.
 * ----------------------------------------------------------------------- */
.global task_start_usermode_asm
.type   task_start_usermode_asm, %function
task_start_usermode_asm:
    cpsid  if

    mrs    r2, cpsr
    bic    r2, r2, #0x1F
    orr    r2, r2, #0x10
    msr    cpsr_c, r2
    mov    sp, r1
    mrs    r2, cpsr
    bic    r2, r2, #0x1F
    orr    r2, r2, #0x13
    msr    cpsr_c, r2

    mov    r2, #0xD0
    msr    spsr_cxsf, r2

    stmfd  sp!, {r0}
    ldmfd  sp!, {pc}^
.size task_start_usermode_asm, . - task_start_usermode_asm


/* -----------------------------------------------------------------------
 * void enter_usermode_asm(uint32_t entry, uint32_t user_sp)
 *
 * r0 = entry     – user-space instruction pointer
 * r1 = user_sp   – user-space stack pointer
 *
 * Direct entry to user mode.  Same as task_start_usermode_asm but for
 * non-scheduler paths (e.g., execve, first process launch).
 * Called directly from C with r0 = entry, r1 = user_sp.
 * ----------------------------------------------------------------------- */
.global enter_usermode_asm
.type   enter_usermode_asm, %function
enter_usermode_asm:
    cpsid  if

    mrs    r2, cpsr
    bic    r2, r2, #0x1F
    orr    r2, r2, #0x10
    msr    cpsr_c, r2
    mov    sp, r1
    mrs    r2, cpsr
    bic    r2, r2, #0x1F
    orr    r2, r2, #0x13
    msr    cpsr_c, r2

    mov    r2, #0xD0
    msr    spsr_cxsf, r2

    stmfd  sp!, {r0}
    ldmfd  sp!, {pc}^
.size enter_usermode_asm, . - enter_usermode_asm
