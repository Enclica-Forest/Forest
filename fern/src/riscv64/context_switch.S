/*
 * Forest OS - RISC-V 64-bit Context Switch and User-Mode Entry
 * Target: QEMU -machine virt -bios opensbi
 *
 * Provides:
 *   task_switch_asm()       - Save/restore callee-saved registers
 *   task_start_usermode_asm() - Enter user mode from a new task
 *   enter_usermode_asm()    - Direct entry to user mode
 *
 * RISC-V calling convention:
 *   - Arguments: a0-a7
 *   - Callee-saved: s0-s11, ra, sp
 *   - Return value: a0
 *
 * Task kernel stack layout when suspended by task_switch_asm:
 *   (high address)
 *   [saved s11]
 *   [saved s10]
 *   ...
 *   [saved ra]
 *   [saved sp]        ← old_sp_ptr points here
 *   (low address, sp points here)
 */

/* -----------------------------------------------------------------------
 * void task_switch_asm(uintptr_t *old_sp_ptr, uintptr_t new_sp)
 *
 * a0 = old_sp_ptr  – pointer to where to save current SP (may be NULL)
 * a1 = new_sp      – SP to load for the next task
 *
 * Saves callee-saved registers (s0-s11, ra, sp) to *old_sp_ptr,
 * loads them from new_sp. Returns to the caller of the previous
 * save point.
 * --------------------------------------------------------------------- */
    .section ".text", "ax"
    .global task_switch_asm
    .type   task_switch_asm, %function
task_switch_asm:
    /* Save callee-saved registers to kernel stack */
    addi    sp, sp, -112        /* 8 regs × 8 bytes each, 16-byte aligned */
    sd      ra,  0x00(sp)
    sd      s0,  0x08(sp)
    sd      s1,  0x10(sp)
    sd      s2,  0x18(sp)
    sd      s3,  0x20(sp)
    sd      s4,  0x28(sp)
    sd      s5,  0x30(sp)
    sd      s6,  0x38(sp)
    sd      s7,  0x40(sp)
    sd      s8,  0x48(sp)
    sd      s9,  0x50(sp)
    sd      s10, 0x58(sp)
    sd      s11, 0x60(sp)
    sd      sp,  0x68(sp)       /* save SP at the bottom of frame */

    /* Save outgoing task's kernel stack pointer */
    beqz    a0, .Lskip_save
    sd      sp, 0(a0)           /* *old_sp_ptr = sp */
.Lskip_save:

    /* Switch to incoming task's kernel stack */
    mv      sp, a1

    /* Restore callee-saved registers from new task's kernel stack */
    ld      ra,  0x00(sp)
    ld      s0,  0x08(sp)
    ld      s1,  0x10(sp)
    ld      s2,  0x18(sp)
    ld      s3,  0x20(sp)
    ld      s4,  0x28(sp)
    ld      s5,  0x30(sp)
    ld      s6,  0x38(sp)
    ld      s7,  0x40(sp)
    ld      s8,  0x48(sp)
    ld      s9,  0x50(sp)
    ld      s10, 0x58(sp)
    ld      s11, 0x60(sp)
    ld      sp,  0x68(sp)       /* restore SP from frame */

    ret
    .size   task_switch_asm, . - task_switch_asm


/* -----------------------------------------------------------------------
 * task_start_usermode_asm()
 *
 * Entry point for brand-new user tasks. task_switch_asm "returns" here
 * because setup_initial_task() placed this address as the return address
 * on the task's kernel stack.
 *
 * At this point:
 *   a0 = entry  – user-space instruction pointer
 *   a1 = sp     – user-space stack pointer
 *
 * Builds an sret frame on the kernel stack and returns to user mode
 * via sret.
 * --------------------------------------------------------------------- */
    .section ".text", "ax"
    .global task_start_usermode_asm
    .type   task_start_usermode_asm, %function
task_start_usermode_asm:
    /* a0 = entry, a1 = user sp */

    /* Set up sstatus for user mode:
     *   SPP = 0 (return to U-mode)
     *   SPIE = 1 (enable interrupts after sret)
     *   FS/VS = 0 (no FP/VM context yet)
     */
    li      t0, (1 << 5)           /* SPIE = 1 */
    csrw    sstatus, t0

    /* Set sepc to user entry point */
    csrw    sepc, a0

    /* Set user stack pointer in sscratch (scratch register for trap handler) */
    csrw    sscratch, a1

    /* Set a0 = 0 (argc), a1 = 0 (argv) for main() in user mode */
    li      a0, 0
    li      a1, 0

    sret
    .size   task_start_usermode_asm, . - task_start_usermode_asm


/* -----------------------------------------------------------------------
 * enter_usermode_asm(uint64_t entry, uint64_t user_sp)
 *
 * a0 = entry     – user-space instruction pointer
 * a1 = user_sp   – user-space stack pointer
 *
 * Direct entry to user mode. This is the non-scheduler path.
 * Builds an sret frame and drops to U-mode.
 * --------------------------------------------------------------------- */
    .section ".text", "ax"
    .global enter_usermode_asm
    .type   enter_usermode_asm, %function
enter_usermode_asm:
    /* Set sstatus for user mode */
    li      t0, (1 << 5)           /* SPIE = 1 */
    csrw    sstatus, t0

    /* Set sepc to user entry */
    csrw    sepc, a0

    /* Save user sp in sscratch for trap handler */
    csrw    sscratch, a1

    /* Return argc=0, argv=0 to user main() */
    li      a0, 0
    li      a1, 0

    sret
    .size   enter_usermode_asm, . - enter_usermode_asm


/* -----------------------------------------------------------------------
 * riscv64_new_task_entry()
 *
 * First code a brand-new user task executes after task_switch_asm
 * restores its registers and does "ret".
 *
 * setup_new_task_stack() placed:
 *   s0 = ELF entry point
 *   s1 = user stack top
 *   ra = address of this function
 *
 * We forward those to enter_usermode_asm(entry, user_sp).
 * --------------------------------------------------------------------- */
    .section ".text", "ax"
    .global riscv64_new_task_entry
    .type   riscv64_new_task_entry, %function
riscv64_new_task_entry:
    mv      a0, s0
    mv      a1, s1
    j       enter_usermode_asm
    .size   riscv64_new_task_entry, . - riscv64_new_task_entry
