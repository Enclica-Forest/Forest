/**
 * @file cache.h
 * @brief ARM32 cache and memory barrier interface
 *
 * Provides portable wrappers around the CP15 cache-management operations and
 * the ARM memory-barrier instructions required on ARMv7.
 *
 * Key ARM cache architecture facts (ARMv7-A)
 * ------------------------------------------
 *  - I-cache and D-cache are separate on Cortex-A series.
 *  - The I-cache is always VIPT (Virtually Indexed, Physically Tagged) on
 *    Cortex-A; cleaning the I-cache is therefore done by invalidation.
 *  - The D-cache is cleaned by line (MVA) or by set/way; the latter is
 *    required for maintenance that must be guaranteed to reach the PoC
 *    (Point of Coherency) independently of virtual address mapping.
 *  - "Clean" flushes dirty data to the next level; "Invalidate" discards
 *    data (dirty or clean); "Clean and Invalidate" does both.
 *
 * Barrier instructions
 * --------------------
 *  - DSB (Data Synchronisation Barrier): all memory accesses before the
 *    barrier complete before any instruction after it begins.  Required
 *    before enabling/disabling the MMU or cache.
 *  - ISB (Instruction Synchronisation Barrier): flushes the CPU pipeline
 *    and any speculative state.  Required after writing to SCTLR.
 *  - DMB (Data Memory Barrier): ordering only — no completion guarantee.
 *    Required between a store and any dependent load on a shared resource.
 *
 * Reference: ARM Architecture Reference Manual ARMv7-A/R (DDI 0406C.d)
 *   §B2.3  Memory barriers
 *   §B7.2  Cache maintenance operations
 */

#ifndef ARM32_CACHE_H
#define ARM32_CACHE_H

#include <stdint.h>

/* =========================================================================
 * Memory barrier wrappers
 *
 * These expand to single ARM barrier instructions.  The "memory" clobber
 * tells GCC not to reorder memory accesses across the barrier at the
 * compiler level.
 * ========================================================================= */

/**
 * arm_barrier_dsb - Data Synchronisation Barrier.
 *
 * Ensures all memory accesses and cache/TLB maintenance operations issued
 * before the barrier are complete before the CPU executes any instruction
 * after it.
 */
static inline void arm_barrier_dsb(void)
{
    __asm__ volatile("dsb" : : : "memory");
}

/**
 * arm_barrier_isb - Instruction Synchronisation Barrier.
 *
 * Flushes the CPU instruction pipeline.  Use after modifying SCTLR,
 * TTBR0/1, or any CP15 register that affects instruction execution.
 */
static inline void arm_barrier_isb(void)
{
    __asm__ volatile("isb" : : : "memory");
}

/**
 * arm_barrier_dmb - Data Memory Barrier.
 *
 * Ensures ordering of memory accesses.  Weaker than DSB (no completion
 * guarantee); used between producer and consumer in shared-memory protocols.
 */
static inline void arm_barrier_dmb(void)
{
    __asm__ volatile("dmb" : : : "memory");
}

/* =========================================================================
 * D-cache operations
 * ========================================================================= */

/**
 * arm_dcache_clean_all - Clean the entire D-cache to the Point of Coherency.
 *
 * Iterates over all cache sets and ways using the DCCISW (Data Cache Clean
 * and Invalidate by Set/Way) operation.  This form is necessary when the
 * cache must be cleaned independently of the virtual address mapping
 * (e.g. before disabling the MMU or shutting down a CPU).
 *
 * The function automatically determines the cache geometry (number of sets,
 * ways, and line size) from the CCSIDR register and handles both L1 and
 * (if present) L2 unified/data caches via CSSELR.
 */
void arm_dcache_clean_all(void);

/**
 * arm_dcache_invalidate_range - Invalidate D-cache lines covering a
 *                               virtual address range.
 *
 * Lines that overlap the range but extend beyond it are cleaned first
 * (DCIMVAC → clean+invalidate) to avoid discarding valid data outside the
 * requested range.
 *
 * @param start  Start virtual address (inclusive; rounded down to line boundary).
 * @param end    End virtual address (exclusive; rounded up to line boundary).
 *
 * Note: "end" is the address one byte past the last byte of the range,
 * following the convention used in the Linux kernel.
 */
void arm_dcache_invalidate_range(uint32_t start, uint32_t end);

/* =========================================================================
 * I-cache operations
 * ========================================================================= */

/**
 * arm_icache_invalidate_all - Invalidate the entire I-cache.
 *
 * Uses ICIALLU (I-Cache Invalidate All to PoU, Inner Shareable) which is
 * the ARMv7 broadcast form.  On a uniprocessor system ICIALLUIS and ICIALLU
 * are equivalent; the IS form is safer for SMP.
 *
 * After this call, all subsequent instruction fetches will miss the I-cache
 * and be re-fetched from memory (or the unified L2).
 */
void arm_icache_invalidate_all(void);

/* =========================================================================
 * Cache line size query
 * ========================================================================= */

/**
 * arm_dcache_line_size - Return the D-cache line size in bytes.
 *
 * Reads the CCSIDR register after selecting the L1 D-cache via CSSELR.
 * The result is a power of two (typically 32 or 64 bytes on Cortex-A).
 *
 * @return Cache line size in bytes.
 */
uint32_t arm_dcache_line_size(void);

#endif /* ARM32_CACHE_H */
