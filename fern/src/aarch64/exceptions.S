/*
 * Forest OS - AArch64 Exception Vector Table
 *
 * The AArch64 exception vector table must be:
 *   - Aligned to a 2KB (0x800) boundary
 *   - Each of the 16 entries is 128 bytes (32 instructions) apart
 *
 * Layout (offsets from VBAR_EL1):
 *   0x000  Current EL, SP0,  Sync
 *   0x080  Current EL, SP0,  IRQ
 *   0x100  Current EL, SP0,  FIQ
 *   0x180  Current EL, SP0,  SError
 *   0x200  Current EL, SPx,  Sync
 *   0x280  Current EL, SPx,  IRQ
 *   0x300  Current EL, SPx,  FIQ
 *   0x380  Current EL, SPx,  SError
 *   0x400  Lower EL (AArch64), Sync
 *   0x480  Lower EL (AArch64), IRQ
 *   0x500  Lower EL (AArch64), FIQ
 *   0x580  Lower EL (AArch64), SError
 *   0x600  Lower EL (AArch32), Sync
 *   0x680  Lower EL (AArch32), IRQ
 *   0x700  Lower EL (AArch32), FIQ
 *   0x780  Lower EL (AArch32), SError
 */

    .section ".text.vectors", "ax"

/* ------------------------------------------------------------------ */
/* Macro: save full register context to the kernel stack               */
/* Saves x0-x30 (31 GP regs), sp_el0, elr_el1, spsr_el1             */
/* Total frame: 34 * 8 = 272 bytes                                    */
/* ------------------------------------------------------------------ */
    .macro SAVE_REGS
    sub     sp, sp, #272
    /* x0-x29 in pairs */
    stp     x0,  x1,  [sp, #0  ]
    stp     x2,  x3,  [sp, #16 ]
    stp     x4,  x5,  [sp, #32 ]
    stp     x6,  x7,  [sp, #48 ]
    stp     x8,  x9,  [sp, #64 ]
    stp     x10, x11, [sp, #80 ]
    stp     x12, x13, [sp, #96 ]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    /* x30 (lr), sp_el0 */
    mrs     x0,  sp_el0
    stp     x30, x0,  [sp, #240]
    /* elr_el1, spsr_el1 */
    mrs     x0,  elr_el1
    mrs     x1,  spsr_el1
    stp     x0,  x1,  [sp, #256]
    .endm

/* ------------------------------------------------------------------ */
/* Macro: restore full register context from the kernel stack          */
/* ------------------------------------------------------------------ */
    .macro RESTORE_REGS
    ldp     x0,  x1,  [sp, #256]   /* elr_el1, spsr_el1 */
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldp     x30, x0,  [sp, #240]
    msr     sp_el0, x0
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96 ]
    ldp     x10, x11, [sp, #80 ]
    ldp     x8,  x9,  [sp, #64 ]
    ldp     x6,  x7,  [sp, #48 ]
    ldp     x4,  x5,  [sp, #32 ]
    ldp     x2,  x3,  [sp, #16 ]
    ldp     x0,  x1,  [sp, #0  ]
    add     sp, sp, #272
    .endm

/* ------------------------------------------------------------------ */
/* Macro: vector entry – each slot is exactly 128 bytes (32 instrs)   */
/* ------------------------------------------------------------------ */
    .macro VECTOR_ENTRY label
    .balign 128
\label:
    .endm

/* ------------------------------------------------------------------ */
/* Actual vector table – must be .balign 2048                          */
/* ------------------------------------------------------------------ */
    .balign 2048
    .global vectors
vectors:

/* ---- Current EL with SP0 ----------------------------------------- */
VECTOR_ENTRY vec_sp0_sync
    SAVE_REGS
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mov     x2, sp
    bl      handle_sync_exception
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_sp0_irq
    SAVE_REGS
    mov     x0, sp
    bl      handle_irq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_sp0_fiq
    SAVE_REGS
    mov     x0, sp
    bl      handle_fiq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_sp0_serror
    SAVE_REGS
    mrs     x0, esr_el1
    mov     x1, sp
    bl      handle_serror
    RESTORE_REGS
    eret

/* ---- Current EL with SPx ----------------------------------------- */
VECTOR_ENTRY vec_spx_sync
    SAVE_REGS
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mov     x2, sp
    bl      handle_sync_exception
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_spx_irq
    SAVE_REGS
    mov     x0, sp
    bl      handle_irq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_spx_fiq
    SAVE_REGS
    mov     x0, sp
    bl      handle_fiq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_spx_serror
    SAVE_REGS
    mrs     x0, esr_el1
    mov     x1, sp
    bl      handle_serror
    RESTORE_REGS
    eret

/* ---- Lower EL (AArch64) ------------------------------------------ */
VECTOR_ENTRY vec_lower64_sync
    SAVE_REGS
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mov     x2, sp
    /* Check for SVC (ESR_EL1.EC = 0b010101 = 0x15) */
    lsr     x3, x0, #26
    cmp     x3, #0x15
    b.eq    .Ldo_syscall
    bl      handle_sync_exception
    b       .Llower64_sync_done
.Ldo_syscall:
    /* AArch64 Linux syscall convention: x8=nr, x0-x7=args, x0=ret */
    ldp     x0, x1, [sp, #0 ]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldr     x8, [sp, #64]
    bl      aarch64_syscall_dispatch
    /* Store return value back into saved x0 */
    str     x0, [sp, #0]
.Llower64_sync_done:
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_lower64_irq
    SAVE_REGS
    mov     x0, sp
    bl      handle_irq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_lower64_fiq
    SAVE_REGS
    mov     x0, sp
    bl      handle_fiq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_lower64_serror
    SAVE_REGS
    mrs     x0, esr_el1
    mov     x1, sp
    bl      handle_serror
    RESTORE_REGS
    eret

/* ---- Lower EL (AArch32) ------------------------------------------ */
VECTOR_ENTRY vec_lower32_sync
    SAVE_REGS
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mov     x2, sp
    bl      handle_sync_exception
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_lower32_irq
    SAVE_REGS
    mov     x0, sp
    bl      handle_irq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_lower32_fiq
    SAVE_REGS
    mov     x0, sp
    bl      handle_fiq
    RESTORE_REGS
    eret

VECTOR_ENTRY vec_lower32_serror
    SAVE_REGS
    mrs     x0, esr_el1
    mov     x1, sp
    bl      handle_serror
    RESTORE_REGS
    eret

    /* Pad to ensure the table is exactly 0x800 bytes */
    .balign 2048
    .size vectors, . - vectors
