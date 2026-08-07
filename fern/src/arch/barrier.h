/*
 * Fern - Cross-Architecture Memory Barriers
 * barrier.h
 *
 * Unified memory barrier interface for all supported architectures.
 * These thin wrappers compile to a single instruction (or tight sequence)
 * on each arch and enforce compiler-level ordering via the "memory" clobber.
 *
 * Supported architectures:
 *   x86_32/x86_64  - mfence, lfence, sfence
 *   ARM32 (v7)     - dmb sy, dmb ishld, dmb ishst
 *   AArch64 (v8)   - dmb sy, dmb ishld, dmb ishst
 *   RISC-V 64      - fence rw,rw, fence r,r, fence w,w
 *
 * Usage:
 *   #include "arch/barrier.h"
 *   arch_mb();           // full memory barrier
 *   arch_rmb();          // read barrier
 *   arch_wmb();          // write barrier
 *   arch_compiler_barrier();  // compiler-only barrier (no HW fence)
 */

#ifndef FOREST_ARCH_BARRIER_H
#define FOREST_ARCH_BARRIER_H

/* Note: arch.h includes this header, so ARCH_IS_X86 etc. are always available. */

/* =========================================================================
 * Full memory barrier
 *
 * Orders all loads and stores before the barrier with respect to all
 * loads and stores after the barrier.
 * ========================================================================= */

static inline void arch_mb(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("mfence" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("dmb sy" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dmb sy" ::: "memory");
#elif ARCH_RISCV64
    __asm__ volatile ("fence rw, rw" ::: "memory");
#endif
}

/* =========================================================================
 * Read memory barrier
 *
 * Orders all loads before the barrier with respect to all loads
 * after the barrier.  Does not order stores.
 * ========================================================================= */

static inline void arch_rmb(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("lfence" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("dmb ishld" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dmb ishld" ::: "memory");
#elif ARCH_RISCV64
    __asm__ volatile ("fence r, r" ::: "memory");
#endif
}

/* =========================================================================
 * Write memory barrier
 *
 * Orders all stores before the barrier with respect to all stores
 * after the barrier.  Does not order loads.
 * ========================================================================= */

static inline void arch_wmb(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("sfence" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("dmb ishst" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dmb ishst" ::: "memory");
#elif ARCH_RISCV64
    __asm__ volatile ("fence w, w" ::: "memory");
#endif
}

/* =========================================================================
 * Compiler barrier only
 *
 * Prevents the compiler from reordering memory accesses across this
 * point, but emits no hardware fence instruction.  Useful when the
 * architecture provides implicit ordering (e.g. x86 TSO) or when
 * you only need to stop the compiler from tearing volatile accesses.
 * ========================================================================= */

static inline void arch_compiler_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

/* =========================================================================
 * Data Synchronisation Barrier (ARM-specific, no-op elsewhere)
 *
 * Ensures all memory accesses and cache/TLB maintenance operations
 * issued before the barrier complete before any instruction after it.
 * Only meaningful on ARM; on other architectures this is a full barrier.
 * ========================================================================= */

#if ARCH_IS_ARM || ARCH_RISCV64
static inline void arch_dsb(void)
{
#if ARCH_ARM32
    __asm__ volatile ("dsb sy" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dsb sy" ::: "memory");
#elif ARCH_RISCV64
    __asm__ volatile ("fence rw, rw" ::: "memory");
#endif
}

static inline void arch_isb(void)
{
#if ARCH_ARM32
    __asm__ volatile ("isb" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("isb" ::: "memory");
#elif ARCH_RISCV64
    __asm__ volatile ("fence rw, rw" ::: "memory");
#endif
}
#endif /* ARCH_IS_ARM || ARCH_RISCV64 */

#endif /* FOREST_ARCH_BARRIER_H */
