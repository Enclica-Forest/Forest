/*
 * Forest OS - AArch64 NEON/FP (Floating-Point / Advanced SIMD) Context
 *
 * Implements save/restore of the full NEON/FP register file using
 * STP/LDP Q-register pair instructions (ARMv8-A Advanced SIMD).
 *
 * Reference: ARM Architecture Reference Manual for ARMv8-A
 *   D8.4  Floating-point and Advanced SIMD instruction encoding
 *   D12.2  CPACR_EL1, Coprocessor Access Control Register
 */

#include "fpu.h"
#include "task.h"

/* ------------------------------------------------------------------ */
/* CPACR_EL1 bit definitions                                           */
/* ------------------------------------------------------------------ */
#define CPACR_EL1_FPEN_MASK    (3UL << 20)   /* Bits [21:20]: FPEN     */
#define CPACR_EL1_FPEN_FULL    (3UL << 20)   /* 0b11 = full access     */

/* ------------------------------------------------------------------ */
/* fpu_init                                                            */
/* ------------------------------------------------------------------ */

void fpu_init(void)
{
    uint64_t cpacr;

    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= CPACR_EL1_FPEN_FULL;
    asm volatile("msr cpacr_el1, %0" : : "r"(cpacr));
    asm volatile("isb");
}

/* ------------------------------------------------------------------ */
/* fpu_save — Save Q0-Q31, FPCR, FPSR                                 */
/*                                                                     */
/* Uses STP with Q-register pairs (128-bit each).  Each STP stores     */
/* two consecutive Q registers: 32 bytes per instruction.               */
/* 16 STP instructions cover all 32 Q registers (512 bytes).           */
/* ------------------------------------------------------------------ */

void fpu_save(fpu_context_t *ctx)
{
    asm volatile(
        "stp     q0,  q1,  [%0, #0x000]\n"
        "stp     q2,  q3,  [%0, #0x020]\n"
        "stp     q4,  q5,  [%0, #0x040]\n"
        "stp     q6,  q7,  [%0, #0x060]\n"
        "stp     q8,  q9,  [%0, #0x080]\n"
        "stp     q10, q11, [%0, #0x0A0]\n"
        "stp     q12, q13, [%0, #0x0C0]\n"
        "stp     q14, q15, [%0, #0x0E0]\n"
        "stp     q16, q17, [%0, #0x100]\n"
        "stp     q18, q19, [%0, #0x120]\n"
        "stp     q20, q21, [%0, #0x140]\n"
        "stp     q22, q23, [%0, #0x160]\n"
        "stp     q24, q25, [%0, #0x180]\n"
        "stp     q26, q27, [%0, #0x1A0]\n"
        "stp     q28, q29, [%0, #0x1C0]\n"
        "stp     q30, q31, [%0, #0x1E0]\n"
        "mrs     x8, fpcr\n"
        "str     x8, [%0, #0x200]\n"
        "mrs     x8, fpsr\n"
        "str     x8, [%0, #0x208]\n"
        :
        : "r"(ctx)
        : "x8", "memory"
    );
}

/* ------------------------------------------------------------------ */
/* fpu_restore — Restore Q0-Q31, FPCR, FPSR                           */
/*                                                                     */
/* Uses LDP with Q-register pairs.  FPCR is restored before FPSR       */
/* to ensure the control register is set before the status register.    */
/* ------------------------------------------------------------------ */

void fpu_restore(fpu_context_t *ctx)
{
    asm volatile(
        "ldr     x8, [%0, #0x200]\n"
        "msr     fpcr, x8\n"
        "ldr     x8, [%0, #0x208]\n"
        "msr     fpsr, x8\n"
        "ldp     q0,  q1,  [%0, #0x000]\n"
        "ldp     q2,  q3,  [%0, #0x020]\n"
        "ldp     q4,  q5,  [%0, #0x040]\n"
        "ldp     q6,  q7,  [%0, #0x060]\n"
        "ldp     q8,  q9,  [%0, #0x080]\n"
        "ldp     q10, q11, [%0, #0x0A0]\n"
        "ldp     q12, q13, [%0, #0x0C0]\n"
        "ldp     q14, q15, [%0, #0x0E0]\n"
        "ldp     q16, q17, [%0, #0x100]\n"
        "ldp     q18, q19, [%0, #0x120]\n"
        "ldp     q20, q21, [%0, #0x140]\n"
        "ldp     q22, q23, [%0, #0x160]\n"
        "ldp     q24, q25, [%0, #0x180]\n"
        "ldp     q26, q27, [%0, #0x1A0]\n"
        "ldp     q28, q29, [%0, #0x1C0]\n"
        "ldp     q30, q31, [%0, #0x1E0]\n"
        :
        : "r"(ctx)
        : "x8", "memory"
    );
}

/* ------------------------------------------------------------------ */
/* fpu_is_enabled                                                      */
/* ------------------------------------------------------------------ */

int fpu_is_enabled(void)
{
    uint64_t cpacr;
    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    return (int)((cpacr >> 20) & 0x3);
}

/* ------------------------------------------------------------------ */
/* fpu_save_current / fpu_restore_current                              */
/*                                                                     */
/* Convenience wrappers for exception vector code.  Access the global   */
/* current_task pointer and save/restore FPU state to the task's        */
/* vfp_context buffer.                                                 */
/* ------------------------------------------------------------------ */

void fpu_save_current(void)
{
    if (current_task && current_task->vfp_context)
        fpu_save((fpu_context_t *)current_task->vfp_context);
}

void fpu_restore_current(void)
{
    if (current_task && current_task->vfp_context)
        fpu_restore((fpu_context_t *)current_task->vfp_context);
}
