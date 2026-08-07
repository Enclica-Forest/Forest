/*
 * cai_x86_32.h - x86-32 (IA-32 / i386) instruction interpreter for
 *                Fern crossarcinterpret (CAI).
 *
 * This header exposes the public interface for the x86-32 single-step
 * emulator.  The emulator interprets 32-bit x86 machine code one instruction
 * at a time, allowing an x86-64 Fern kernel to run 32-bit ELF
 * executables without any hardware virtualisation.
 *
 * The interpreter is integrated into the common CAI framework through the
 *   cai_x86_32_step(cai_context_t *ctx)
 * function declared in crossarcinterpret.h.  The additional symbols below
 * are convenience wrappers and direct-memory helpers used by tests and by
 * the ELF loader.
 */

#ifndef CAI_X86_32_H
#define CAI_X86_32_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "crossarcinterpret.h"

/* =========================================================================
 * Standalone register file (mirrors cai_x86_32_regs_t in crossarcinterpret.h)
 *
 * Order follows the Intel manual register encoding:
 *   EAX=0  ECX=1  EDX=2  EBX=3  ESP=4  EBP=5  ESI=6  EDI=7
 * ========================================================================= */

typedef struct x86_32_regs {
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t eip;
    uint32_t eflags;
    uint16_t cs, ds, es, fs, gs, ss;
} x86_32_regs_t;

/* =========================================================================
 * EFLAGS bit masks
 * ========================================================================= */

#define X86_CF  (1u <<  0)   /* Carry flag                                   */
#define X86_PF  (1u <<  2)   /* Parity flag                                  */
#define X86_AF  (1u <<  4)   /* Auxiliary carry flag                         */
#define X86_ZF  (1u <<  6)   /* Zero flag                                    */
#define X86_SF  (1u <<  7)   /* Sign flag                                    */
#define X86_TF  (1u <<  8)   /* Trap flag                                    */
#define X86_IF  (1u <<  9)   /* Interrupt enable flag                        */
#define X86_DF  (1u << 10)   /* Direction flag                               */
#define X86_OF  (1u << 11)   /* Overflow flag                                */

/* =========================================================================
 * Standalone interpreter context
 *
 * For callers that do not need the full CAI framework (e.g., unit tests).
 * Production kernel code should use cai_context_t from crossarcinterpret.h
 * instead and call cai_x86_32_step(ctx).
 * ========================================================================= */

typedef struct cai_x86_32_ctx {
    x86_32_regs_t regs;
    uint8_t      *mem;       /* Flat guest memory backing store              */
    size_t        mem_size;  /* Size of the backing store in bytes           */
    uint32_t      mem_base;  /* Guest virtual address that maps to mem[0]   */
    bool          running;   /* False when the guest has exited              */
    int           exit_code; /* Guest exit code (valid when !running)        */
} cai_x86_32_ctx_t;

/* =========================================================================
 * Standalone context lifecycle
 * ========================================================================= */

/*
 * cai_x86_32_create - Allocate a standalone interpreter context.
 *
 * Allocates @mem_size bytes of flat guest memory (base address 0).
 * Returns NULL on allocation failure.
 */
cai_x86_32_ctx_t *cai_x86_32_create(size_t mem_size);

/*
 * cai_x86_32_destroy - Free all resources owned by @ctx.
 */
void cai_x86_32_destroy(cai_x86_32_ctx_t *ctx);

/* =========================================================================
 * Standalone execution
 * ========================================================================= */

/*
 * cai_x86_32_step_sa - Execute exactly one instruction in the standalone ctx.
 *
 * (The canonical cai_x86_32_step(cai_context_t*) is declared in
 *  crossarcinterpret.h; this wrapper adapts the standalone context.)
 *
 * Returns CAI_OK, CAI_EXITED, or a negative CAI_E* error code.
 */
int cai_x86_32_step_sa(cai_x86_32_ctx_t *ctx);

/*
 * cai_x86_32_run_sa - Execute up to @max_steps instructions (standalone ctx).
 *
 * Returns CAI_OK, CAI_EAGAIN (limit reached), CAI_EXITED, or negative error.
 * Pass max_steps <= 0 to run without a step limit (until exit or error).
 */
int cai_x86_32_run(cai_x86_32_ctx_t *ctx, int max_steps);

/*
 * cai_x86_32_load_elf - Parse a 32-bit ELF binary and map its PT_LOAD
 * segments into the standalone context's flat memory.
 *
 * Sets regs.eip to the ELF entry point.
 * Returns CAI_OK on success, negative CAI_E* on failure.
 */
int cai_x86_32_load_elf(cai_x86_32_ctx_t *ctx,
                         const uint8_t *elf, size_t size);

/* =========================================================================
 * Standalone direct memory accessors
 *
 * All translate @addr relative to ctx->mem_base.
 * Out-of-range reads return 0; out-of-range writes are silently ignored.
 * (These are for convenience/testing; the main interpreter uses the
 *  cai_mem_* helpers from cai_memory.c for bounds-checked access.)
 * ========================================================================= */

uint8_t  cai_x86_32_read8 (cai_x86_32_ctx_t *ctx, uint32_t addr);
uint16_t cai_x86_32_read16(cai_x86_32_ctx_t *ctx, uint32_t addr);
uint32_t cai_x86_32_read32(cai_x86_32_ctx_t *ctx, uint32_t addr);

void cai_x86_32_write8 (cai_x86_32_ctx_t *ctx, uint32_t addr, uint8_t  val);
void cai_x86_32_write16(cai_x86_32_ctx_t *ctx, uint32_t addr, uint16_t val);
void cai_x86_32_write32(cai_x86_32_ctx_t *ctx, uint32_t addr, uint32_t val);

#endif /* CAI_X86_32_H */
