/*
 * cai_aarch64.h - AArch64 (ARMv8-A 64-bit) instruction interpreter
 *
 * Part of the crossarcinterpret subsystem for Fern.
 * Allows AArch64 ELF binaries to run on any Fern host (x86 or ARM).
 *
 * Register file:
 *   x0-x30   general-purpose (x29 = frame pointer, x30 = link register)
 *   sp       stack pointer (EL0)
 *   pc       program counter
 *   nzcv     condition flags (N bit31, Z bit30, C bit29, V bit28)
 *   fpcr     floating-point control register (tracked but FP not executed)
 *   fpsr     floating-point status register  (tracked but FP not executed)
 *
 * XZR (the zero register) is synthesised: reads return 0, writes are discarded.
 * Index 31 in the x[] array is therefore never accessed directly.
 */

#ifndef CAI_AARCH64_H
#define CAI_AARCH64_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================
 * AArch64 register file
 * ========================================================================= */

typedef struct aarch64_regs {
    uint64_t x[31];    /* x0-x30  (index 31 = XZR, not stored here)         */
    uint64_t sp;       /* Stack pointer (SP_EL0)                              */
    uint64_t pc;       /* Program counter                                     */
    uint64_t nzcv;     /* Condition flags – only bits 31-28 are significant   */
    uint64_t fpcr;     /* Floating-point Control Register (stored, not used)  */
    uint64_t fpsr;     /* Floating-point Status Register  (stored, not used)  */
} aarch64_regs_t;

/* Condition-flag masks within nzcv */
#define NZCV_N  (1ULL << 31)   /* Negative                                   */
#define NZCV_Z  (1ULL << 30)   /* Zero                                       */
#define NZCV_C  (1ULL << 29)   /* Carry                                      */
#define NZCV_V  (1ULL << 28)   /* Overflow                                   */

/* =========================================================================
 * Stand-alone interpreter context
 *
 * This "thin" context is used when the AArch64 interpreter is invoked
 * independently (e.g. unit tests, future stand-alone mode).  For normal
 * Fern use the interpreter is driven through the cai_context_t /
 * cai_aarch64_step() path defined in crossarcinterpret.h.
 * ========================================================================= */

typedef struct cai_aarch64_ctx {
    aarch64_regs_t  regs;
    uint8_t        *mem;        /* Host pointer to the flat guest memory pool  */
    size_t          mem_size;   /* Pool size in bytes                          */
    uint64_t        mem_base;   /* Guest virtual address that maps to mem[0]   */
    bool            running;    /* Set to false by SVC exit / BRK              */
    int             exit_code;  /* Guest exit code                             */
} cai_aarch64_ctx_t;

/* =========================================================================
 * Stand-alone context lifecycle
 * ========================================================================= */

/*
 * cai_aarch64_create - allocate a stand-alone interpreter context.
 *
 * @mem_size : size in bytes of the flat guest memory pool to allocate.
 *
 * Returns a pointer to the new context, or NULL on allocation failure.
 * The caller must call cai_aarch64_destroy() when done.
 */
cai_aarch64_ctx_t *cai_aarch64_create(size_t mem_size);

/*
 * cai_aarch64_destroy - free all resources owned by @ctx.
 */
void cai_aarch64_destroy(cai_aarch64_ctx_t *ctx);

/*
 * cai_aarch64_step_sa - execute one instruction from the stand-alone context.
 * (The canonical cai_aarch64_step(cai_context_t*) is in crossarcinterpret.h.)
 *
 * Returns 0 on success, CAI_EXITED (2) when the guest calls exit,
 * or a negative CAI_E* error code.
 */
int cai_aarch64_step_sa(cai_aarch64_ctx_t *ctx);

/*
 * cai_aarch64_run - execute up to @max_steps instructions.
 *
 * Returns CAI_OK, CAI_EXITED, or a negative error code.
 * Pass max_steps <= 0 to run until exit or error.
 */
int cai_aarch64_run(cai_aarch64_ctx_t *ctx, int max_steps);

/*
 * cai_aarch64_load_elf - parse and load an AArch64 ELF binary into @ctx.
 *
 * @elf  : pointer to the raw ELF image bytes
 * @size : byte length of the image
 *
 * Returns 0 on success, negative on error.
 */
int cai_aarch64_load_elf(cai_aarch64_ctx_t *ctx, const uint8_t *elf,
                         size_t size);

/* =========================================================================
 * Stand-alone memory access helpers
 *
 * These wrap the flat mem[] pool with bounds checking.
 * They return 0/default on out-of-bounds; callers should check ctx->running.
 * ========================================================================= */

uint8_t  cai_aa64_read8 (cai_aarch64_ctx_t *ctx, uint64_t addr);
uint16_t cai_aa64_read16(cai_aarch64_ctx_t *ctx, uint64_t addr);
uint32_t cai_aa64_read32(cai_aarch64_ctx_t *ctx, uint64_t addr);
uint64_t cai_aa64_read64(cai_aarch64_ctx_t *ctx, uint64_t addr);

void cai_aa64_write8 (cai_aarch64_ctx_t *ctx, uint64_t addr, uint8_t  v);
void cai_aa64_write16(cai_aarch64_ctx_t *ctx, uint64_t addr, uint16_t v);
void cai_aa64_write32(cai_aarch64_ctx_t *ctx, uint64_t addr, uint32_t v);
void cai_aa64_write64(cai_aarch64_ctx_t *ctx, uint64_t addr, uint64_t v);

#endif /* CAI_AARCH64_H */
