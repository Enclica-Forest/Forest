/*
 * Forest OS - AArch64 NEON/FP (Floating-Point / Advanced SIMD) Context
 *
 * Provides save/restore of the full NEON/FP register file for context
 * switching.  The 32 × 128-bit Q registers (Q0-Q31) plus FPCR and FPSR
 * are saved/restored using STP/LDP Q-register pair instructions.
 *
 * fpu_context_t layout (544 bytes, 16-byte aligned):
 *
 *   Offset  Size  Content
 *   ------  ----  -------
 *   0x000   512   Q0-Q31 (32 × 128-bit, stored as 64 × uint64_t)
 *   0x200     8   FPCR (Floating-Point Control Register)
 *   0x208     8   FPSR (Floating-Point Status Register)
 *   0x210    16   Padding (alignment to 16 bytes)
 *   Total:  0x220 = 544 bytes
 *
 * Stored in task_t.vfp_context (lazily allocated on first FPU use).
 */
#ifndef AARCH64_FPU_H
#define AARCH64_FPU_H

#include <stdint.h>

/*
 * NEON/FP register context.
 *
 * Q registers are 128-bit.  STP/LDP with Q-register operands store two
 * consecutive Q registers (32 bytes) per instruction.  We store them as
 * an array of uint64_t for easy offset calculation:
 *   Q0  → q[0],  q[1]     (offset 0x000)
 *   Q1  → q[2],  q[3]     (offset 0x010)
 *   ...
 *   Q31 → q[62], q[63]    (offset 0x1F0)
 */
typedef struct fpu_context {
    uint64_t q[64];     /* Q0-Q31: 32 × 128-bit = 512 bytes */
    uint64_t fpcr;      /* Floating-Point Control Register  */
    uint64_t fpsr;      /* Floating-Point Status Register   */
    uint64_t _pad[2];   /* Alignment padding to 544 bytes   */
} fpu_context_t;

/* Offsets for assembly access (must match struct layout) */
#define FPU_CTX_Q_BASE      0x000   /* Start of Q register storage      */
#define FPU_CTX_FPCR        0x200   /* Offset of FPCR                   */
#define FPU_CTX_FPSR        0x208   /* Offset of FPSR                   */
#define FPU_CTX_SIZE        0x220   /* Total size: 544 bytes            */

/**
 * fpu_init - Enable FP/SIMD access at EL1 and EL0.
 *
 * Sets CPACR_EL1 bits [21:20] (FPEN) to 0b11 for full access.
 * Must be called once during boot before any floating-point code runs.
 * Safe to call multiple times (idempotent).
 */
void fpu_init(void);

/**
 * fpu_save - Save NEON/FP state to the given context.
 *
 * Saves all 32 Q registers (Q0-Q31), FPCR, and FPSR using STP/LDP
 * Q-register pair instructions.  The context pointer must be 16-byte
 * aligned and point to at least FPU_CTX_SIZE (544) bytes of storage.
 *
 * @ctx: Pointer to an fpu_context_t (must be 16-byte aligned).
 */
void fpu_save(fpu_context_t *ctx);

/**
 * fpu_restore - Restore NEON/FP state from the given context.
 *
 * Restores all 32 Q registers (Q0-Q31), FPCR, and FPSR using LDP
 * Q-register pair instructions.  FPCR is restored before FPSR to
 * ensure the control register is set before the status register.
 *
 * @ctx: Pointer to a previously saved fpu_context_t.
 */
void fpu_restore(fpu_context_t *ctx);

/**
 * fpu_is_enabled - Check if NEON/FP access is enabled in CPACR_EL1.
 *
 * Returns non-zero if FPEN bits [21:20] are set (full access),
 * zero if FP/SIMD access is trapped or disabled.
 */
int fpu_is_enabled(void);

/**
 * fpu_save_current - Save FPU state of the currently running task.
 *
 * Convenience function for use from exception vectors.  Accesses the
 * global current_task pointer and saves FPU state to current_task->vfp_context.
 * No-op if current_task or current_task->vfp_context is NULL.
 */
void fpu_save_current(void);

/**
 * fpu_restore_current - Restore FPU state of the currently running task.
 *
 * Convenience function for use from exception vectors.  Accesses the
 * global current_task pointer and restores FPU state from current_task->vfp_context.
 * No-op if current_task or current_task->vfp_context is NULL.
 */
void fpu_restore_current(void);

#endif /* AARCH64_FPU_H */
