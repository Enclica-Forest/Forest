/**
 * @file mmu.h
 * @brief ARM32 VMSAv7 Memory Management Unit interface
 *
 * Implements the ARM Short-Descriptor translation table format (VMSAv7).
 *
 * Translation table walk for a 4KB page:
 *   VA[31:20] -> L1 table index (4096 entries, 4 bytes each = 16 KB table)
 *   VA[19:12] -> L2 table index ( 256 entries, 4 bytes each =  1 KB table)
 *   VA[11:0]  -> 4 KB page offset
 *
 * Translation table walk for a 1 MB section:
 *   VA[31:20] -> L1 table index
 *   VA[19:0]  -> 1 MB section offset
 *
 * Reference: ARM Architecture Reference Manual ARMv7-A/R (DDI 0406C)
 *   - Chapter B3 "The VMSA"
 *   - Table B3-4  "First-level descriptor formats"
 *   - Table B3-11 "Second-level descriptor formats"
 */

#ifndef ARM32_MMU_H
#define ARM32_MMU_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * L1 descriptor type field (bits [1:0])
 * ========================================================================= */

/** L1 entry is invalid (page fault on any access) */
#define ARM_L1_TYPE_FAULT       0x00U

/**
 * L1 entry points to a 1 KB-aligned L2 page table.
 * Covers exactly 1 MB of VA space.
 */
#define ARM_L1_TYPE_PAGE        0x01U

/**
 * L1 entry is a 1 MB section descriptor.
 * When bit 18 (SUPER) is also set the entry describes a 16 MB supersection
 * and bits [31:24] form the physical base address.
 */
#define ARM_L1_TYPE_SECTION     0x02U

/**
 * Alias: supersection shares the same type field value as SECTION.
 * Distinguish by checking bit 18 of the descriptor.
 */
#define ARM_L1_TYPE_SUPERSECT   0x02U

/* =========================================================================
 * Section descriptor bit definitions (L1 entry, type = 0b10)
 *
 * Bit positions match the ARMv7 Short-Descriptor format exactly.
 * ========================================================================= */

/** [0]  PXN – Privileged Execute Never (ARMv7 extension) */
#define ARM_SECT_PXN            (1U << 0)

/* [1:0] = 0b10 : section type (set implicitly by ARM_L1_TYPE_SECTION) */

/** [2]  B – Bufferable memory attribute */
#define ARM_SECT_B              (1U << 2)

/** [3]  C – Cacheable memory attribute */
#define ARM_SECT_C              (1U << 3)

/** [4]  XN – Execute Never (user *and* privileged) */
#define ARM_SECT_XN             (1U << 4)

/**
 * [8:5] Domain field.
 * @param n  Domain number 0-15
 */
#define ARM_SECT_DOMAIN(n)      (((n) & 0xFU) << 5)

/* [9]  IMP – Implementation defined; should be 0 */

/** [10] AP[0] – Access Permission bit 0 */
#define ARM_SECT_AP0            (1U << 10)

/** [11] AP[1] – Access Permission bit 1 */
#define ARM_SECT_AP1            (1U << 11)

/**
 * [14:12] TEX[2:0] – Type Extension field (cache/memory type).
 * @param n  TEX value 0-7
 */
#define ARM_SECT_TEX(n)         (((n) & 0x7U) << 12)

/** [15] AP[2] – Read-only when 1 (combined with AP[1:0]) */
#define ARM_SECT_AP2            (1U << 15)

/** [16] S – Shareable (coherent across multiple CPUs) */
#define ARM_SECT_S              (1U << 16)

/** [17] nG – Not Global (entry is ASID-specific when set) */
#define ARM_SECT_NG             (1U << 17)

/** [18] 0/1 – Section (0) vs Supersection (1) */
#define ARM_SECT_SUPER          (1U << 18)

/** [19] NS – Non-Secure memory */
#define ARM_SECT_NS             (1U << 19)

/* [31:20] Physical base address of the 1 MB section */
#define ARM_SECT_BASE(pa)       ((pa) & 0xFFF00000U)

/* =========================================================================
 * Commonly used AP field combinations
 *
 * ARMv7 AP encoding (AP[2:0]):
 *   AP2=0 AP[1:0]=00  No access (with SCTLR.AFE=0)
 *   AP2=0 AP[1:0]=01  PL1 read/write, PL0 no access
 *   AP2=0 AP[1:0]=10  PL1 read/write, PL0 read only
 *   AP2=0 AP[1:0]=11  Full access (PL1 and PL0 read/write)
 *   AP2=1 AP[1:0]=01  PL1 read only, PL0 no access
 *   AP2=1 AP[1:0]=11  Read only (PL1 and PL0)
 * ========================================================================= */

/** Privileged read/write, user no access */
#define ARM_AP_KERNEL_RW        (ARM_SECT_AP0)

/** Privileged read/write, user read-only */
#define ARM_AP_KERNEL_RW_USER_R (ARM_SECT_AP0 | ARM_SECT_AP1)

/** Full access: privileged and user read/write */
#define ARM_AP_FULL_ACCESS      (ARM_SECT_AP0 | ARM_SECT_AP1)

/** Privileged read-only, user no access */
#define ARM_AP_KERNEL_RO        (ARM_SECT_AP2 | ARM_SECT_AP0)

/** Read-only for all */
#define ARM_AP_READ_ONLY        (ARM_SECT_AP2 | ARM_SECT_AP0 | ARM_SECT_AP1)

/* =========================================================================
 * TEX + C + B memory type shortcuts (SCTLR.TRE = 0 / TEX remap disabled)
 *
 * See ARMv7 ARM Table B3-10.
 * ========================================================================= */

/** Strongly Ordered (uncached, unbuffered, non-reorderable) */
#define ARM_MEM_STRONGLY_ORDERED    (0U)

/** Device memory (bufferable, ordered relative to other Device accesses) */
#define ARM_MEM_DEVICE              (ARM_SECT_B)

/** Normal Non-Cacheable */
#define ARM_MEM_NORMAL_NC           (ARM_SECT_TEX(1))

/** Normal Write-Back, Write-Allocate (fastest for regular RAM) */
#define ARM_MEM_NORMAL_WB_WA        (ARM_SECT_TEX(1) | ARM_SECT_C | ARM_SECT_B)

/** Normal Write-Through */
#define ARM_MEM_NORMAL_WT           (ARM_SECT_C)

/* =========================================================================
 * L2 small-page descriptor bit definitions (4 KB pages)
 * ========================================================================= */

/** L2 entry type: invalid / fault */
#define ARM_L2_TYPE_FAULT       0x00U

/** L2 entry type: large page (64 KB) – not used by this driver */
#define ARM_L2_TYPE_LARGE       0x01U

/** L2 entry type: small page (4 KB) */
#define ARM_L2_TYPE_SMALL       0x02U

/** [0]  XN – Execute Never (small page) */
#define ARM_PAGE_XN             (1U << 0)

/* [1]   = 1 for small page (set implicitly) */

/** [2]  B – Bufferable */
#define ARM_PAGE_B              (1U << 2)

/** [3]  C – Cacheable */
#define ARM_PAGE_C              (1U << 3)

/** [4]  AP[0] */
#define ARM_PAGE_AP0            (1U << 4)

/** [5]  AP[1] */
#define ARM_PAGE_AP1            (1U << 5)

/**
 * [8:6]  TEX[2:0]
 * @param n  TEX value 0-7
 */
#define ARM_PAGE_TEX(n)         (((n) & 0x7U) << 6)

/** [9]  AP[2] */
#define ARM_PAGE_AP2            (1U << 9)

/** [10] S – Shareable */
#define ARM_PAGE_S              (1U << 10)

/** [11] nG – Not Global */
#define ARM_PAGE_NG             (1U << 11)

/* [31:12] Physical base address of the 4 KB page */
#define ARM_PAGE_BASE(pa)       ((pa) & 0xFFFFF000U)

/* =========================================================================
 * L2 AP combinations (same encoding as section AP, different bit offsets)
 * ========================================================================= */
#define ARM_PAGE_AP_KERNEL_RW        (ARM_PAGE_AP0)
#define ARM_PAGE_AP_FULL_ACCESS      (ARM_PAGE_AP0 | ARM_PAGE_AP1)
#define ARM_PAGE_AP_KERNEL_RO        (ARM_PAGE_AP2 | ARM_PAGE_AP0)
#define ARM_PAGE_AP_READ_ONLY        (ARM_PAGE_AP2 | ARM_PAGE_AP0 | ARM_PAGE_AP1)

/* =========================================================================
 * L2 memory type shortcuts (same TEX+C+B logic, different macros for L2)
 * ========================================================================= */
#define ARM_PAGE_MEM_NORMAL_WB_WA    (ARM_PAGE_TEX(1) | ARM_PAGE_C | ARM_PAGE_B)
#define ARM_PAGE_MEM_STRONGLY_ORDERED (0U)
#define ARM_PAGE_MEM_DEVICE          (ARM_PAGE_B)
#define ARM_PAGE_MEM_NORMAL_NC       (ARM_PAGE_TEX(1))

/* =========================================================================
 * Translation table types
 * ========================================================================= */

/**
 * L1 translation table.
 * Must be placed at a 16 KB-aligned physical address (TTBR0 requires bits
 * [13:0] to be zero when TTBCR.N = 0).
 * 4096 entries × 4 bytes = 16 KB.
 */
typedef uint32_t arm_l1_table_t[4096];

/**
 * L2 page table.
 * Must be placed at a 1 KB-aligned physical address.
 * 256 entries × 4 bytes = 1 KB.
 */
typedef uint32_t arm_l2_table_t[256];

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * arm_mmu_init - Initialise the ARM MMU subsystem.
 *
 * Allocates an L1 translation table (statically), zeroes it, identity-maps
 * the kernel's physical range, sets the Domain Access Control Register
 * (DACR) so that domain 0 has Client access (i.e. access permissions are
 * checked by hardware), and loads TTBR0.
 *
 * @param kernel_phys_base  Physical start address of the kernel image.
 * @param kernel_virt_base  Virtual start address of the kernel image.
 *                          If equal to kernel_phys_base the mapping is an
 *                          identity map.  A higher-half kernel sets this to
 *                          something like 0xC0000000.
 * @param kernel_size       Size of the kernel image in bytes (will be rounded
 *                          up to the next 1 MB boundary internally).
 */
void arm_mmu_init(uint32_t kernel_phys_base,
                  uint32_t kernel_virt_base,
                  uint32_t kernel_size);

/**
 * arm_map_section - Insert a 1 MB section descriptor into an L1 table.
 *
 * @param ttb      Pointer to the 16 KB-aligned L1 translation table.
 * @param virt_mb  Virtual address of the 1 MB region (only bits [31:20]
 *                 are significant; lower bits are ignored).
 * @param phys_mb  Physical address of the 1 MB region (only bits [31:20]
 *                 are significant).
 * @param flags    OR-combination of ARM_SECT_* flags defined above.
 *                 Do NOT include the type field; it is set automatically.
 */
void arm_map_section(arm_l1_table_t *ttb,
                     uint32_t        virt_mb,
                     uint32_t        phys_mb,
                     uint32_t        flags);

/**
 * arm_map_page - Insert a 4 KB page mapping.
 *
 * Creates an L2 table for the enclosing 1 MB region if one does not already
 * exist.  The L2 table is allocated from a small static pool managed inside
 * mmu.c.  A kernel that needs more than ARM_L2_POOL_SIZE L2 tables must
 * increase that constant.
 *
 * @param ttb   Pointer to the L1 translation table.
 * @param virt  Virtual address (only bits [31:12] significant).
 * @param phys  Physical address (only bits [31:12] significant).
 * @param flags OR-combination of ARM_PAGE_* flags.
 *              Do NOT include the small-page type bits; they are set
 *              automatically.
 */
void arm_map_page(arm_l1_table_t *ttb,
                  uint32_t        virt,
                  uint32_t        phys,
                  uint32_t        flags);

/**
 * arm_enable_mmu - Enable the MMU.
 *
 * Performs the following sequence required by the ARM architecture:
 *   1. Flush the branch predictor.
 *   2. Invalidate the TLB (all entries, all ASIDs).
 *   3. Invalidate the I-cache.
 *   4. Issue a DSB + ISB to drain the pipeline.
 *   5. Set the M (MMU Enable) bit in SCTLR.
 *   6. Issue another ISB so that subsequent instructions run with the new
 *      SCTLR value.
 *
 * The L1 table must already have been loaded into TTBR0 (by arm_mmu_init or
 * directly via arm_write_ttbr0) before calling this function.
 *
 * @param ttb  Pointer to the L1 translation table.  The function writes
 *             its physical address (assumed == virtual address at this point,
 *             because the MMU is not yet on) to TTBR0 just before enabling.
 */
void arm_enable_mmu(arm_l1_table_t *ttb);

/**
 * arm_flush_tlb_all - Invalidate the entire TLB (all ASIDs, both IS and
 *                     local domains).
 *
 * Issues TLBIALL (c8, c7, 0), followed by a DSB and ISB.
 */
void arm_flush_tlb_all(void);

/**
 * arm_flush_tlb_page - Invalidate the TLB entry for a single virtual page.
 *
 * @param vaddr  Virtual address whose TLB entry should be invalidated.
 *               Only bits [31:12] are used; the page-offset bits are masked.
 */
void arm_flush_tlb_page(uint32_t vaddr);

/**
 * arm_get_physical_addr - Walk the translation tables and return the
 *                         physical address mapped to a virtual address.
 *
 * @param ttb   Pointer to the L1 translation table.
 * @param vaddr Virtual address to translate.
 * @return      Physical address if mapped, or 0 if the mapping is absent
 *              or the entry type is not recognised.
 */
uint32_t arm_get_physical_addr(arm_l1_table_t *ttb, uint32_t vaddr);

/* =========================================================================
 * Low-level CP15 helpers (exposed for testing / cache driver use)
 * ========================================================================= */

/** Write a value to TTBR0 (Translation Table Base Register 0). */
static inline void arm_write_ttbr0(uint32_t val)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0"
                     : : "r"(val) : "memory");
}

/** Read TTBR0. */
static inline uint32_t arm_read_ttbr0(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0"
                     : "=r"(val) : : "memory");
    return val;
}

/** Write TTBCR (Translation Table Base Control Register). */
static inline void arm_write_ttbcr(uint32_t val)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2"
                     : : "r"(val) : "memory");
}

/** Write DACR (Domain Access Control Register). */
static inline void arm_write_dacr(uint32_t val)
{
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0"
                     : : "r"(val) : "memory");
}

/** Read SCTLR (System Control Register). */
static inline uint32_t arm_read_sctlr(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0"
                     : "=r"(val) : : "memory");
    return val;
}

/** Write SCTLR. */
static inline void arm_write_sctlr(uint32_t val)
{
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0"
                     : : "r"(val) : "memory");
}

/* SCTLR bit masks */
#define ARM_SCTLR_M     (1U << 0)   /**< MMU enable */
#define ARM_SCTLR_A     (1U << 1)   /**< Strict alignment fault enable */
#define ARM_SCTLR_C     (1U << 2)   /**< Data / unified cache enable */
#define ARM_SCTLR_Z     (1U << 11)  /**< Branch prediction enable */
#define ARM_SCTLR_I     (1U << 12)  /**< Instruction cache enable */
#define ARM_SCTLR_V     (1U << 13)  /**< High exception vectors (0xFFFF0000) */
#define ARM_SCTLR_AFE   (1U << 29)  /**< Access Flag Enable */
#define ARM_SCTLR_TE    (1U << 30)  /**< Thumb Exception enable */

/* DACR domain encoding (2 bits per domain) */
#define ARM_DACR_NO_ACCESS  0x0U    /**< Any access generates a fault */
#define ARM_DACR_CLIENT     0x1U    /**< AP bits in TT descriptors checked */
#define ARM_DACR_MANAGER    0x3U    /**< AP bits ignored; all accesses allowed */

/** Build a DACR value with a single domain setting. */
#define ARM_DACR_D(n, perm) (((perm) & 0x3U) << ((n) * 2))

#endif /* ARM32_MMU_H */
