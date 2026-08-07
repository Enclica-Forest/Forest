/*
 * Forest OS - RISC-V 64-bit Exception/Interrupt Vector Table and Handlers
 * Target: QEMU -machine virt -bios opensbi
 *
 * RISC-V privileged architecture:
 *   - stvec register holds the base address of the trap vector table.
 *   - On trap: PC → stvec, scause = exception/interrupt code,
 *     sepc = trap PC, stval = faulting address (if applicable).
 *   - sret returns from S-mode trap, restoring sepc.
 *
 * scause layout:
 *   Bit 63:    Interrupt (1) / Exception (0)
 *   Bits 62:0: Exception/interrupt code
 *
 * Relevant exception codes:
 *   0  - Instruction address misaligned
 *   1  - Instruction access fault
 *   2  - Illegal instruction
 *   3  - Breakpoint
 *   4  - Load address misaligned
 *   5  - Load access fault
 *   6  - Store/AMO address misaligned
 *   7  - Store/AMO access fault
 *   8  - Environment call from U-mode (ecall)
 *   9  - Environment call from S-mode
 *   12 - Instruction page fault
 *   13 - Load page fault
 *   15 - Store/AMO page fault
 *
 * Interrupt codes (bit 63 set):
 *   1  - Supervisor software interrupt
 *   3  - Supervisor timer interrupt
 *   9  - Supervisor external interrupt
 *
 * Stack frame layout (grows down, SP points to lowest address):
 *   Offset   Register   Notes
 *   ------   --------   -----
 *    +0x00    ra         return address (caller-saved)
 *    +0x08    gp         global pointer
 *    +0x10    tp         thread pointer
 *    +0x18    t0         temporary
 *    +0x20    t1         temporary
 *    +0x28    t2         temporary
 *    +0x30    s0/fp      saved/frame pointer
 *    +0x38    s1         saved register
 *    +0x40    a0         argument/return
 *    +0x48    a1         argument
 *    +0x50    a2         argument
 *    +0x58    a3         argument
 *    +0x60    a4         argument
 *    +0x68    a5         argument
 *    +0x70    a6         argument
 *    +0x78    a7         argument/syscall number
 *    +0x80    s2         saved register
 *    +0x88    s3         saved register
 *    +0x90    s4         saved register
 *    +0x98    s5         saved register
 *    +0xa0    s6         saved register
 *    +0xa8    s7         saved register
 *    +0xb0    s8         saved register
 *    +0xb8    s9         saved register
 *    +0xc0    s10        saved register
 *    +0xc8    s11        saved register
 *    +0xd0    t3         temporary
 *    +0xd8    t4         temporary
 *    +0xe0    t5         temporary
 *    +0xe8    t6         temporary
 *    +0xf0    sstatus    supervisor status register
 *    +0xf8    sepc       supervisor exception PC
 *    +0x100   scause     supervisor cause register
 *    +0x108   stval      supervisor trap value
 *
 * Total frame size: 0x110 = 272 bytes.
 *
 * RISC-V syscall convention (Linux-style):
 *   a7  = syscall number
 *   a0  = arg0 / return value
 *   a1  = arg1
 *   a2  = arg2
 *   a3  = arg3
 *   a4  = arg4
 *   a5  = arg5
 */

/* -----------------------------------------------------------------------
 * Frame size constant
 * --------------------------------------------------------------------- */
#define FRAME_SIZE  0x110   /* 272 bytes, 16-byte aligned */

/* -----------------------------------------------------------------------
 * Macro: save_regs
 *
 * Saves the full general-purpose register set plus sstatus/sepc/scause/stval
 * onto the current kernel stack.
 *
 * After this macro, sp points to the base of the frame.
 * --------------------------------------------------------------------- */
.macro save_regs
    addi    sp, sp, -FRAME_SIZE

    /* Save GPRs: x1-x31 (x0 is always zero) */
    sd      ra,  0x00(sp)
    sd      gp,  0x08(sp)
    sd      tp,  0x10(sp)
    sd      t0,  0x18(sp)
    sd      t1,  0x20(sp)
    sd      t2,  0x28(sp)
    sd      s0,  0x30(sp)
    sd      s1,  0x38(sp)
    sd      a0,  0x40(sp)
    sd      a1,  0x48(sp)
    sd      a2,  0x50(sp)
    sd      a3,  0x58(sp)
    sd      a4,  0x60(sp)
    sd      a5,  0x68(sp)
    sd      a6,  0x70(sp)
    sd      a7,  0x78(sp)
    sd      s2,  0x80(sp)
    sd      s3,  0x88(sp)
    sd      s4,  0x90(sp)
    sd      s5,  0x98(sp)
    sd      s6,  0xa0(sp)
    sd      s7,  0xa8(sp)
    sd      s8,  0xb0(sp)
    sd      s9,  0xb8(sp)
    sd      s10, 0xc0(sp)
    sd      s11, 0xc8(sp)
    sd      t3,  0xd0(sp)
    sd      t4,  0xd8(sp)
    sd      t5,  0xe0(sp)
    sd      t6,  0xe8(sp)

    /* Save CSR registers */
    csrr    t0, sstatus
    csrr    t1, sepc
    csrr    t2, scause
    csrr    t3, stval
    sd      t0, 0xf0(sp)
    sd      t1, 0xf8(sp)
    sd      t2, 0x100(sp)
    sd      t3, 0x108(sp)
.endm

/* -----------------------------------------------------------------------
 * Macro: restore_regs
 *
 * Restores the full context and returns with sret.
 * Must match save_regs exactly.
 * --------------------------------------------------------------------- */
.macro restore_regs
    /* Restore CSR registers */
    ld      t0, 0xf0(sp)
    ld      t1, 0xf8(sp)
    ld      t2, 0x100(sp)
    ld      t3, 0x108(sp)
    csrw    sstatus, t0
    csrw    sepc, t1
    /* scause and stval are read-only, no restore needed */

    /* Restore GPRs */
    ld      ra,  0x00(sp)
    ld      gp,  0x08(sp)
    ld      tp,  0x10(sp)
    ld      t0,  0x18(sp)
    ld      t1,  0x20(sp)
    ld      t2,  0x28(sp)
    ld      s0,  0x30(sp)
    ld      s1,  0x38(sp)
    ld      a0,  0x40(sp)
    ld      a1,  0x48(sp)
    ld      a2,  0x50(sp)
    ld      a3,  0x58(sp)
    ld      a4,  0x60(sp)
    ld      a5,  0x68(sp)
    ld      a6,  0x70(sp)
    ld      a7,  0x78(sp)
    ld      s2,  0x80(sp)
    ld      s3,  0x88(sp)
    ld      s4,  0x90(sp)
    ld      s5,  0x98(sp)
    ld      s6,  0xa0(sp)
    ld      s7,  0xa8(sp)
    ld      s8,  0xb0(sp)
    ld      s9,  0xb8(sp)
    ld      s10, 0xc0(sp)
    ld      s11, 0xc8(sp)
    ld      t3,  0xd0(sp)
    ld      t4,  0xd8(sp)
    ld      t5,  0xe0(sp)
    ld      t6,  0xe8(sp)

    addi    sp, sp, FRAME_SIZE
    sret
.endm

/* -----------------------------------------------------------------------
 * Exception vector table
 *
 * Uses Direct mode: stvec[MODE=0] means all traps jump to the base address.
 * The handler decodes scause to determine the cause.
 *
 * Placed in .text so it's in the kernel image.
 * --------------------------------------------------------------------- */
    .section ".text.trap", "ax"
    .align 2
    .global trap_vector
    .type   trap_vector, %function
trap_vector:
    save_regs

    /* Pass frame pointer to C handler */
    mv      a0, sp

    /* Decode scause: check bit 63 for interrupt */
    ld      t0, 0x100(sp)           /* t0 = scause */
    bltz    t0, .Linterrupt         /* if bit 63 set, it's an interrupt */

    /* Exception: decode code */
    li      t1, 8
    beq     t0, t1, .Lsyscall       /* ecall from U-mode (code 8) */

    /* Other exception → generic C handler */
    mv      a1, t0                  /* a1 = scause */
    ld      a2, 0xf8(sp)            /* a2 = sepc */
    ld      a3, 0x108(sp)           /* a3 = stval */
    call    riscv64_exception_handler
    j       .Ltrap_done

.Lsyscall:
    /* Ecall from U-mode: syscall handler */
    /* a0 already points to trap frame (which contains a0-a7) */
    call    riscv64_syscall_handle
    j       .Ltrap_done

.Linterrupt:
    /* Interrupt: strip bit 63 and dispatch */
    slli    t0, t0, 1
    srli    t0, t0, 1               /* t0 = interrupt code (clear bit 63) */

    mv      a0, sp                  /* a0 = trap frame */
    mv      a1, t0                  /* a1 = interrupt code */
    call    riscv64_irq_handler

.Ltrap_done:
    restore_regs
    .size   trap_vector, . - trap_vector


/* -----------------------------------------------------------------------
 * Weak stubs for C-side handlers.
 *
 * These allow the kernel to link even when the full C handlers are not
 * yet present. Each stub spins forever so a missing handler is obvious
 * in a debugger rather than causing a silent bad return.
 * --------------------------------------------------------------------- */
    .weak   riscv64_exception_handler
    .weak   riscv64_irq_handler

    .type   riscv64_exception_handler, %function
riscv64_exception_handler:
    .type   riscv64_irq_handler, %function
riscv64_irq_handler:
.Lweak_spin:
    wfi
    j       .Lweak_spin
