/*
 * Forest OS - AArch64 Exception Vector Table and Handlers
 * Target: QEMU -machine virt -cpu cortex-a53
 *
 * AArch64 exception model:
 *   - VBAR_EL1 points to a 2KB-aligned table of 16 entries.
 *   - Each entry is 128 bytes (32 instructions) and is entered directly
 *     by the CPU — there is no "LDR PC, =" indirection needed.
 *   - The four groups of four vectors are:
 *       [0x000] Current EL, SP0   (sync / IRQ / FIQ / SError)
 *       [0x200] Current EL, SPx   (sync / IRQ / FIQ / SError)
 *       [0x400] Lower EL, AArch64 (sync / IRQ / FIQ / SError)
 *       [0x600] Lower EL, AArch32 (sync / IRQ / FIQ / SError)
 *
 * Register save frame layout (grows down, SP points to lowest address):
 *
 *   Offset   Register   Notes
 *   ------   --------   -----
 *    +0x00    ELR_EL1   exception return address        (saved first via pair)
 *    +0x08    SPSR_EL1  saved processor state
 *    +0x10    x0
 *    +0x18    x1
 *    +0x20    x2
 *    +0x28    x3
 *    +0x30    x4
 *    +0x38    x5
 *    +0x40    x6
 *    +0x48    x7
 *    +0x50    x8
 *    +0x58    x9
 *    +0x60    x10
 *    +0x68    x11
 *    +0x70    x12
 *    +0x78    x13
 *    +0x80    x14
 *    +0x88    x15
 *    +0x90    x16
 *    +0x98    x17
 *    +0xa0    x18
 *    +0xa8    x19
 *    +0xb0    x20
 *    +0xb8    x21
 *    +0xc0    x22
 *    +0xc8    x23
 *    +0xd0    x24
 *    +0xd8    x25
 *    +0xe0    x26
 *    +0xe8    x27
 *    +0xf0    x28
 *    +0xf8    x29 (fp)
 *   +0x100    x30 (lr)   + 8-byte pad for 16-byte alignment
 *   +0x108    pad
 *
 * Total frame size: 0x110 = 272 bytes.
 *
 * AArch64 ABI for syscalls (Linux convention, used by EL0):
 *   x8  = syscall number
 *   x0  = arg0 / return value
 *   x1  = arg1
 *   x2  = arg2
 *   x3  = arg3
 *   x4  = arg4
 *   x5  = arg5
 *   x6  = arg6  (7th argument, rarely used)
 *
 * ESR_EL1 layout:
 *   [31:26] EC  - Exception Class
 *   [25]    IL  - Instruction Length
 *   [24:0]  ISS - Instruction Specific Syndrome
 *
 * EC values relevant here:
 *   0x15  SVC instruction from AArch64 EL0
 *   0x20  Instruction abort from lower EL
 *   0x24  Data abort from lower EL
 *   0x22  PC alignment fault
 *   0x26  Data abort from current EL
 */

    .arch armv8-a
    .cpu  cortex-a53

/* -----------------------------------------------------------------------
 * Frame size constant
 * --------------------------------------------------------------------- */
#define FRAME_SIZE  0x110   /* 272 bytes, 16-byte aligned */

/* -----------------------------------------------------------------------
 * Macro: save_regs
 *
 * Saves the full general-purpose register set plus ELR_EL1 / SPSR_EL1
 * onto the current SP_EL1 stack.
 *
 * On entry SP must already be at EL1 (it always is at EL1).
 * After this macro SP points to the base of the frame (ELR slot).
 * --------------------------------------------------------------------- */
.macro save_regs
    /* Allocate stack frame */
    sub     sp, sp, #FRAME_SIZE

    /* Save x0-x29 as pairs */
    stp     x0,  x1,  [sp, #0x10]
    stp     x2,  x3,  [sp, #0x20]
    stp     x4,  x5,  [sp, #0x30]
    stp     x6,  x7,  [sp, #0x40]
    stp     x8,  x9,  [sp, #0x50]
    stp     x10, x11, [sp, #0x60]
    stp     x12, x13, [sp, #0x70]
    stp     x14, x15, [sp, #0x80]
    stp     x16, x17, [sp, #0x90]
    stp     x18, x19, [sp, #0xa0]
    stp     x20, x21, [sp, #0xb0]
    stp     x22, x23, [sp, #0xc0]
    stp     x24, x25, [sp, #0xd0]
    stp     x26, x27, [sp, #0xe0]
    stp     x28, x29, [sp, #0xf0]

    /* Save x30 (lr) with padding */
    str     x30,      [sp, #0x100]

    /* Save ELR_EL1 and SPSR_EL1 at the top of the frame */
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x21, x22, [sp, #0x00]
.endm

/* -----------------------------------------------------------------------
 * Macro: restore_regs
 *
 * Restores the full context and returns with eret.
 * Must match save_regs exactly.
 * --------------------------------------------------------------------- */
.macro restore_regs
    /* Restore ELR_EL1 and SPSR_EL1 */
    ldp     x21, x22, [sp, #0x00]
    msr     elr_el1,  x21
    msr     spsr_el1, x22

    /* Restore x30 */
    ldr     x30,      [sp, #0x100]

    /* Restore x0-x29 */
    ldp     x28, x29, [sp, #0xf0]
    ldp     x26, x27, [sp, #0xe0]
    ldp     x24, x25, [sp, #0xd0]
    ldp     x22, x23, [sp, #0xc0]
    ldp     x20, x21, [sp, #0xb0]
    ldp     x18, x19, [sp, #0xa0]
    ldp     x16, x17, [sp, #0x90]
    ldp     x14, x15, [sp, #0x80]
    ldp     x12, x13, [sp, #0x70]
    ldp     x10, x11, [sp, #0x60]
    ldp     x8,  x9,  [sp, #0x50]
    ldp     x6,  x7,  [sp, #0x40]
    ldp     x4,  x5,  [sp, #0x30]
    ldp     x2,  x3,  [sp, #0x20]
    ldp     x0,  x1,  [sp, #0x10]

    /* Free stack frame and return from exception */
    add     sp, sp, #FRAME_SIZE
    eret
.endm

/* -----------------------------------------------------------------------
 * Macro: ventry label
 *
 * Each vector slot is exactly 128 bytes (.align 7 = 2^7).
 * A single branch instruction suffices because the handler can be
 * anywhere in the 4 GB ±128 MB branch range within the kernel.
 * --------------------------------------------------------------------- */
.macro ventry label
    .align 7
    b       \label
.endm

/* -----------------------------------------------------------------------
 * Exception vector table
 *
 * MUST be 2KB aligned (.align 11 = 2^11 = 2048).
 * Placed in .text so the linker puts it in the kernel image.
 * The symbol "vectors" is referenced from boot.S (msr vbar_el1, x0).
 * --------------------------------------------------------------------- */
    .section ".text.vectors", "ax"
    .align  11
    .global vectors
    .type   vectors, %function
vectors:
    /* ---- Group 0: Current EL with SP_EL0 ---- */
    ventry  el1_sp0_sync        /* 0x000 Synchronous                    */
    ventry  el1_sp0_irq         /* 0x080 IRQ / vIRQ                     */
    ventry  el1_sp0_fiq         /* 0x100 FIQ / vFIQ                     */
    ventry  el1_sp0_serror      /* 0x180 SError / vSError               */

    /* ---- Group 1: Current EL with SP_ELx ---- */
    ventry  el1_spx_sync        /* 0x200 Synchronous                    */
    ventry  el1_spx_irq         /* 0x280 IRQ / vIRQ                     */
    ventry  el1_spx_fiq         /* 0x300 FIQ / vFIQ                     */
    ventry  el1_spx_serror      /* 0x380 SError / vSError               */

    /* ---- Group 2: Lower EL using AArch64 (EL0 64-bit) ---- */
    ventry  el0_aarch64_sync    /* 0x400 Synchronous — SVC arrives here */
    ventry  el0_aarch64_irq     /* 0x480 IRQ / vIRQ                     */
    ventry  el0_aarch64_fiq     /* 0x500 FIQ / vFIQ                     */
    ventry  el0_aarch64_serror  /* 0x580 SError / vSError               */

    /* ---- Group 3: Lower EL using AArch32 (EL0 32-bit) ---- */
    ventry  el0_aarch32_sync    /* 0x600 Synchronous                    */
    ventry  el0_aarch32_irq     /* 0x680 IRQ / vIRQ                     */
    ventry  el0_aarch32_fiq     /* 0x700 FIQ / vFIQ                     */
    ventry  el0_aarch32_serror  /* 0x780 SError / vSError               */

/* -----------------------------------------------------------------------
 * Handler implementations
 * --------------------------------------------------------------------- */
    .section ".text", "ax"

/* -------- EL1 / SP0 handlers (kernel code running on SP_EL0) --------- */

    .global el1_sp0_sync
    .type   el1_sp0_sync, %function
el1_sp0_sync:
    save_regs
    mrs     x0, esr_el1
    mrs     x1, far_el1
    bl      aarch64_sync_handler
    restore_regs
    .size   el1_sp0_sync, . - el1_sp0_sync

    .global el1_sp0_irq
    .type   el1_sp0_irq, %function
el1_sp0_irq:
    save_regs
    mov     x0, sp
    bl      aarch64_irq_handler
    restore_regs
    .size   el1_sp0_irq, . - el1_sp0_irq

    .global el1_sp0_fiq
    .type   el1_sp0_fiq, %function
el1_sp0_fiq:
    save_regs
    mov     x0, sp
    bl      aarch64_fiq_handler
    restore_regs
    .size   el1_sp0_fiq, . - el1_sp0_fiq

    .global el1_sp0_serror
    .type   el1_sp0_serror, %function
el1_sp0_serror:
    save_regs
    mov     x0, sp
    mrs     x1, esr_el1
    bl      aarch64_serror_handler
    /* SError is typically fatal; the C handler loops. Satisfy assembler: */
    restore_regs
    .size   el1_sp0_serror, . - el1_sp0_serror

/* -------- EL1 / SPx handlers (normal kernel execution) --------------- */

    .global el1_spx_sync
    .type   el1_spx_sync, %function
el1_spx_sync:
    save_regs
    mrs     x0, esr_el1
    mrs     x1, far_el1
    bl      aarch64_sync_handler
    restore_regs
    .size   el1_spx_sync, . - el1_spx_sync

    .global el1_spx_irq
    .type   el1_spx_irq, %function
el1_spx_irq:
    save_regs
    mov     x0, sp
    bl      aarch64_irq_handler
    restore_regs
    .size   el1_spx_irq, . - el1_spx_irq

    .global el1_spx_fiq
    .type   el1_spx_fiq, %function
el1_spx_fiq:
    save_regs
    mov     x0, sp
    bl      aarch64_fiq_handler
    restore_regs
    .size   el1_spx_fiq, . - el1_spx_fiq

    .global el1_spx_serror
    .type   el1_spx_serror, %function
el1_spx_serror:
    save_regs
    mov     x0, sp
    mrs     x1, esr_el1
    bl      aarch64_serror_handler
    restore_regs
    .size   el1_spx_serror, . - el1_spx_serror

/* -------- EL0 AArch64 handlers (64-bit userspace) -------------------- */
/*
 * el0_aarch64_sync is the hot path for SVC (system calls).
 *
 * ESR_EL1[31:26] = EC field.  For SVC from AArch64 EL0, EC = 0x15.
 * We decode EC here in assembly to avoid C overhead on the critical path,
 * then branch to the dedicated SVC dispatcher el0_svc or fall through to
 * the generic C handler for all other synchronous exceptions from EL0.
 */
    .global el0_aarch64_sync
    .type   el0_aarch64_sync, %function
el0_aarch64_sync:
    save_regs

    /* Save FPU/NEON state of the interrupted EL0 task */
    bl      fpu_save_current

    /* Read ESR and extract EC field [31:26] */
    mrs     x25, esr_el1
    lsr     x24, x25, #26          /* x24 = EC                           */

    /* Check for SVC from AArch64 EL0 (EC == 0x15) */
    cmp     x24, #0x15
    b.eq    el0_svc

    /* All other EL0 synchronous exceptions go to the generic C handler */
    mov     x0, sp                  /* arg0: pointer to saved frame        */
    mov     x1, x25                 /* arg1: ESR_EL1 value                 */
    bl      aarch64_el0_sync_handler

    /* Restore FPU/NEON state before returning to EL0 */
    bl      fpu_restore_current
    restore_regs
    .size   el0_aarch64_sync, . - el0_aarch64_sync

/*
 * el0_svc — SVC (system call) fast path
 *
 * On entry:
 *   save_regs has already been executed by el0_aarch64_sync.
 *   The saved frame sits at SP, with x0-x8 values at their frame offsets.
 *
 * AArch64 Linux syscall register convention:
 *   x8  = syscall number
 *   x0  = arg0 (also the return value on exit)
 *   x1  = arg1
 *   x2  = arg2
 *   x3  = arg3
 *   x4  = arg4
 *   x5  = arg5
 *   x6  = arg6
 *
 * We reload x0-x8 from the saved frame so that any modification done
 * by save_regs (there are none, but this is defensive) is irrelevant.
 * The return value from aarch64_syscall_handle() is stored back into
 * the x0 slot of the frame so that restore_regs returns it to EL0.
 */
    .global el0_svc
    .type   el0_svc, %function
el0_svc:
    /*
     * Load syscall arguments from the saved frame.
     * Frame layout (see FRAME_SIZE / save_regs above):
     *   x0 at [sp, #0x10], x1 at [sp, #0x18], ..., x8 at [sp, #0x50]
     */
    ldr     x8,  [sp, #0x50]   /* syscall number                          */
    ldr     x0,  [sp, #0x10]   /* arg0                                    */
    ldr     x1,  [sp, #0x18]   /* arg1                                    */
    ldr     x2,  [sp, #0x20]   /* arg2                                    */
    ldr     x3,  [sp, #0x28]   /* arg3                                    */
    ldr     x4,  [sp, #0x30]   /* arg4                                    */
    ldr     x5,  [sp, #0x38]   /* arg5                                    */
    ldr     x6,  [sp, #0x40]   /* arg6                                    */

    /*
     * Call C dispatcher:
     *   int64_t aarch64_syscall_handle(uint64_t nr,
     *                                  uint64_t a0, uint64_t a1,
     *                                  uint64_t a2, uint64_t a3,
     *                                  uint64_t a4, uint64_t a5,
     *                                  uint64_t a6);
     *
     * AArch64 C ABI passes arguments in x0-x7.  We need x8 (nr) in x0,
     * then shift the six args up by one register.
     */
    mov     x7,  x6             /* arg6 → x7                              */
    mov     x6,  x5             /* arg5 → x6                              */
    mov     x5,  x4             /* arg4 → x5                              */
    mov     x4,  x3             /* arg3 → x4                              */
    mov     x3,  x2             /* arg2 → x3                              */
    mov     x2,  x1             /* arg1 → x2                              */
    mov     x1,  x0             /* arg0 → x1                              */
    mov     x0,  x8             /* nr   → x0                              */

    bl      aarch64_syscall_handle

    /* Store return value (x0) back into the saved x0 slot in the frame   */
    str     x0,  [sp, #0x10]

    /* Restore FPU/NEON state before returning to EL0 */
    bl      fpu_restore_current

    restore_regs
    .size   el0_svc, . - el0_svc

    .global el0_aarch64_irq
    .type   el0_aarch64_irq, %function
el0_aarch64_irq:
    save_regs
    bl      fpu_save_current
    mov     x0, sp
    bl      aarch64_irq_handler
    bl      fpu_restore_current
    restore_regs
    .size   el0_aarch64_irq, . - el0_aarch64_irq

    .global el0_aarch64_fiq
    .type   el0_aarch64_fiq, %function
el0_aarch64_fiq:
    save_regs
    bl      fpu_save_current
    mov     x0, sp
    bl      aarch64_fiq_handler
    bl      fpu_restore_current
    restore_regs
    .size   el0_aarch64_fiq, . - el0_aarch64_fiq

    .global el0_aarch64_serror
    .type   el0_aarch64_serror, %function
el0_aarch64_serror:
    save_regs
    bl      fpu_save_current
    mov     x0, sp
    mrs     x1, esr_el1
    bl      aarch64_serror_handler
    bl      fpu_restore_current
    restore_regs
    .size   el0_aarch64_serror, . - el0_aarch64_serror

/* -------- EL0 AArch32 handlers (32-bit userspace — not supported) ---- */
/*
 * Forest OS does not run 32-bit ARM EL0 userspace.  These stubs capture
 * the context and call a C handler that will terminate the task.
 */
    .global el0_aarch32_sync
    .type   el0_aarch32_sync, %function
el0_aarch32_sync:
    save_regs
    mov     x0, sp
    mrs     x1, esr_el1
    bl      aarch64_el0_aarch32_handler
    restore_regs
    .size   el0_aarch32_sync, . - el0_aarch32_sync

    .global el0_aarch32_irq
    .type   el0_aarch32_irq, %function
el0_aarch32_irq:
    save_regs
    mov     x0, sp
    bl      aarch64_irq_handler
    restore_regs
    .size   el0_aarch32_irq, . - el0_aarch32_irq

    .global el0_aarch32_fiq
    .type   el0_aarch32_fiq, %function
el0_aarch32_fiq:
    save_regs
    mov     x0, sp
    bl      aarch64_fiq_handler
    restore_regs
    .size   el0_aarch32_fiq, . - el0_aarch32_fiq

    .global el0_aarch32_serror
    .type   el0_aarch32_serror, %function
el0_aarch32_serror:
    save_regs
    mov     x0, sp
    mrs     x1, esr_el1
    bl      aarch64_serror_handler
    restore_regs
    .size   el0_aarch32_serror, . - el0_aarch32_serror

/* -----------------------------------------------------------------------
 * Weak stubs for C-side handlers.
 *
 * These allow the kernel to link even when the full C handlers are not
 * yet present.  Each stub spins forever so a missing handler is obvious
 * in a debugger rather than causing a silent bad return.
 *
 * aarch64_sync_handler is provided by fault.c (no weak stub needed).
 * --------------------------------------------------------------------- */
    .weak   aarch64_irq_handler
    .weak   aarch64_fiq_handler
    .weak   aarch64_serror_handler
    .weak   aarch64_el0_aarch32_handler

    .type   aarch64_irq_handler, %function
aarch64_irq_handler:
    .type   aarch64_fiq_handler, %function
aarch64_fiq_handler:
    .type   aarch64_serror_handler, %function
aarch64_serror_handler:
    .type   aarch64_el0_aarch32_handler, %function
aarch64_el0_aarch32_handler:
.Lweak_spin:
    wfi
    b       .Lweak_spin
