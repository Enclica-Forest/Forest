/**
 * @file cache.c
 * @brief ARM32 cache maintenance implementation
 *
 * Implements the cache and barrier operations declared in cache.h.
 *
 * All cache maintenance uses CP15 coprocessor instructions (MCR/MRC p15).
 * The set/way iteration in arm_dcache_clean_all() follows the algorithm
 * described in the ARM Cortex-A Series Programmer's Guide (DEN0013D)
 * §11.4 and the ARMv7 Architecture Reference Manual §B7.2.
 *
 * CP15 registers used
 * -------------------
 *  c0, c0, 1   CLIDR  Cache Level ID Register
 *              [31:30] LoUIS  Level of Unification Inner Shareable
 *              [29:27] LoC    Level of Coherency
 *              [26:24] LoU    Level of Unification
 *              [n*3+2 : n*3] Cache type for level n+1
 *                0 = No cache  1 = I only  2 = D only  3 = Separate I+D
 *                4 = Unified
 *
 *  c0, c0, 0   CCSIDR Cache Size ID Register (selected by CSSELR)
 *              [27:13] NumSets-1
 *              [12:3]  Associativity-1
 *              [2:0]   LineSize: log2(words per line) - 2
 *                LineSize=0 → 4 words (16 B)
 *                LineSize=1 → 8 words (32 B)
 *                LineSize=2 → 16 words (64 B)
 *
 *  c0, c0, 2   CSSELR Cache Size Selection Register
 *              [3:1]   Level (0 = L1, 1 = L2, ...)
 *              [0]     InD  (0 = Data/Unified, 1 = Instruction)
 *
 *  c7, c10, 2  DCCISW  Data Cache Clean and Invalidate by Set/Way
 *  c7, c14, 2  DCCISW  (alternate encoding on some implementations)
 *  c7, c6,  2  DCISW   Data Cache Invalidate by Set/Way
 *  c7, c10, 1  DCCMVAC Data Cache Clean by MVA to PoC
 *  c7, c14, 1  DCCIMVAC Data Cache Clean and Invalidate by MVA to PoC
 *  c7, c5,  0  ICIALLU  I-Cache Invalidate All to PoU
 *  c7, c5,  6  BPIALL   Branch Predictor Invalidate All
 */

#include "cache.h"

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * read_clidr - Read the Cache Level ID Register (CLIDR).
 *
 * @return CLIDR value.
 */
static inline uint32_t read_clidr(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(val) : : "memory");
    return val;
}

/**
 * read_ccsidr - Read the Cache Size ID Register (CCSIDR).
 *
 * Must be called after writing the desired cache level/type to CSSELR.
 *
 * @return CCSIDR value.
 */
static inline uint32_t read_ccsidr(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(val) : : "memory");
    return val;
}

/**
 * write_csselr - Write the Cache Size Selection Register (CSSELR).
 *
 * @param val  Bits [3:1] = cache level - 1; bit [0] = 1 for I-cache.
 */
static inline void write_csselr(uint32_t val)
{
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" : : "r"(val) : "memory");
    /* ISB required after CSSELR write before reading CCSIDR. */
    __asm__ volatile("isb" : : : "memory");
}

/**
 * dcache_op_setway - Issue DCCISW (clean+invalidate) or DCISW (invalidate)
 *                    for one cache set/way combination.
 *
 * The encoding of the CP15 register operand for set/way operations:
 *   [31 : 32-log2(assoc)]  Way number
 *   [log2(line)-1 : log2(line) - set_bits]  Set number (shifted up by
 *       the line-size offset so it aligns with the physical index in the
 *       cache RAM)
 *   [3:1]  Cache level - 1
 *
 * @param setway_val  Pre-encoded set/way/level value.
 * @param clean       If non-zero, use DCCISW (clean+invalidate); otherwise
 *                    use DCISW (invalidate only).
 */
static inline void dcache_op_setway(uint32_t setway_val, int clean)
{
    if (clean) {
        /* DCCISW: Data Cache Clean and Invalidate by Set/Way */
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 2"
                         : : "r"(setway_val) : "memory");
    } else {
        /* DCISW: Data Cache Invalidate by Set/Way */
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 2"
                         : : "r"(setway_val) : "memory");
    }
}

/**
 * dcache_iterate_all - Iterate over all levels of the D/unified cache
 *                      hierarchy and call dcache_op_setway() for every
 *                      set/way combination.
 *
 * @param clean  Non-zero → clean+invalidate; zero → invalidate only.
 */
static void dcache_iterate_all(int clean)
{
    uint32_t clidr = read_clidr();

    /*
     * LoC (Level of Coherency) is in CLIDR[29:27].  We iterate cache
     * levels 1..LoC (stored as 0-based index in CLIDR).
     */
    uint32_t loc = (clidr >> 24) & 0x7U; /* LoC field */

    for (uint32_t level = 0; level < loc; level++) {

        /*
         * Cache type for this level is in bits [(level*3)+2 : level*3].
         * Values: 0=none, 1=I only, 2=D only, 3=separate I+D, 4=unified.
         * Skip if there is no D/unified cache at this level.
         */
        uint32_t ctype = (clidr >> (level * 3)) & 0x7U;
        if (ctype < 2U) {
            continue; /* No D-cache or unified cache at this level */
        }

        /* Select D-cache (or unified cache) at this level. */
        write_csselr((level << 1) | 0U); /* InD=0 for D/unified */

        uint32_t ccsidr = read_ccsidr();

        /*
         * Extract cache geometry:
         *   LineSize  = 2^(linesize_field + 2) words = 2^(linesize_field+4) bytes
         *   NumSets   = (ccsidr[27:13]) + 1
         *   Assoc     = (ccsidr[12:3]) + 1
         */
        uint32_t linesize_field = ccsidr & 0x7U;
        uint32_t assoc          = ((ccsidr >> 3) & 0x3FFU) + 1U;
        uint32_t num_sets       = ((ccsidr >> 13) & 0x7FFFU) + 1U;

        /*
         * log2(line_bytes): line_bytes = 4 * 2^(linesize_field + 2)
         * So log2(line_bytes) = linesize_field + 4.
         * The set field in the set/way register starts at bit
         * log2(line_bytes) = linesize_field + 4.
         */
        uint32_t set_shift  = linesize_field + 4U;

        /*
         * The way field is in the most significant bits.
         * way_shift = 32 - log2(assoc).
         * We compute log2(assoc) by counting leading zeros.
         */
        uint32_t way_shift;
        {
            /*
             * __builtin_clz(assoc - 1) gives 32 - ceil(log2(assoc))
             * for assoc >= 2.  For assoc = 1 (direct mapped), way is
             * always 0 and way_shift is irrelevant (only 1 iteration).
             */
            if (assoc == 1U) {
                way_shift = 0U;
            } else {
                /* way_shift = 32 - log2(roundup_power2(assoc)) */
                uint32_t tmp = assoc - 1U;
                uint32_t log2_assoc = 32U - (uint32_t)__builtin_clz(tmp);
                way_shift = 32U - log2_assoc;
            }
        }

        /* Level field bits [3:1] in the set/way register */
        uint32_t level_field = level << 1;

        /* Iterate over all sets and ways. */
        for (uint32_t way = 0; way < assoc; way++) {
            for (uint32_t set = 0; set < num_sets; set++) {
                uint32_t sw = (way << way_shift)
                            | (set << set_shift)
                            | level_field;
                dcache_op_setway(sw, clean);
            }
        }
    }

    /* DSB to ensure all set/way operations complete. */
    __asm__ volatile("dsb" : : : "memory");
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * arm_dcache_clean_all - Clean (and invalidate) the entire D-cache hierarchy.
 *
 * Walks all levels from L1 to LoC using set/way operations.  Each line is
 * cleaned (written back) and then invalidated so that subsequent accesses
 * go to main memory or the next cache level.
 */
void arm_dcache_clean_all(void)
{
    dcache_iterate_all(1 /* clean = true */);
}

/**
 * arm_icache_invalidate_all - Invalidate the entire I-cache.
 *
 * ICIALLU invalidates all instruction cache entries up to the Point of
 * Unification (PoU).  On SMP systems this also broadcasts to inner-
 * shareable cluster members (the IS variant ICIALLUIS is used here for
 * correctness on any topology).
 */
void arm_icache_invalidate_all(void)
{
    uint32_t zero = 0U;

    /* ICIALLUIS: Invalidate I-cache All to PoU, Inner Shareable */
    __asm__ volatile("mcr p15, 0, %0, c7, c1, 0"
                     : : "r"(zero) : "memory");

    /* Branch predictor invalidate all (BPIALL) */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6"
                     : : "r"(zero) : "memory");

    __asm__ volatile("dsb\n" "isb\n" : : : "memory");
}

/**
 * arm_dcache_invalidate_range - Invalidate D-cache lines covering [start, end).
 *
 * For lines that only partially overlap the range (the first and last lines
 * if the range is not cache-line aligned) we use DCCIMVAC (clean+invalidate)
 * rather than plain DCIMVAC to avoid discarding dirty data outside the
 * requested range.  Interior lines are invalidated with DCCIMVAC as well
 * for simplicity — the extra clean is harmless for correctness.
 *
 * @param start  Inclusive start address.
 * @param end    Exclusive end address (one byte past the last byte).
 */
void arm_dcache_invalidate_range(uint32_t start, uint32_t end)
{
    if (start >= end) {
        return;
    }

    uint32_t line_size = arm_dcache_line_size();
    uint32_t line_mask = line_size - 1U;

    /* Align start down and end up to line boundaries. */
    uint32_t addr = start & ~line_mask;
    uint32_t end_aligned = (end + line_mask) & ~line_mask;

    /*
     * DCCIMVAC: Data Cache Clean and Invalidate by MVA to PoC.
     * The MVA is written to CP15 c7, c14, 1.
     *
     * Using clean+invalidate for every line is safe (though slightly
     * conservative for interior lines) and avoids the need to special-case
     * the partial first/last lines.
     */
    while (addr < end_aligned) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1"
                         : : "r"(addr) : "memory");
        addr += line_size;
    }

    /* DSB to ensure all cache operations are visible. */
    __asm__ volatile("dsb" : : : "memory");
}

/**
 * arm_dcache_line_size - Return the L1 D-cache line size in bytes.
 *
 * Selects the L1 D-cache via CSSELR, reads CCSIDR, and decodes the
 * LineSize field.
 *
 * LineSize encoding: log2(cache line in 32-bit words) - 2
 *   0 → 4 words = 16 bytes
 *   1 → 8 words = 32 bytes   (common on Cortex-A8, A9)
 *   2 → 16 words = 64 bytes  (common on Cortex-A15, A53)
 */
uint32_t arm_dcache_line_size(void)
{
    /* Select L1 D-cache. */
    write_csselr(0U); /* Level = 0 (L1), InD = 0 (D/unified) */

    uint32_t ccsidr = read_ccsidr();
    uint32_t linesize_field = ccsidr & 0x7U;

    /*
     * line_bytes = 4 words * 2^(linesize_field + 2)
     *            = 2^2 * 2^(linesize_field + 2)
     *            = 2^(linesize_field + 4)
     */
    return 1U << (linesize_field + 4U);
}
