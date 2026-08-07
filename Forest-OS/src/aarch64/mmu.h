/*
 * Fern - AArch64 MMU definitions
 *
 * 4KB granule, 4-level page table walk (48-bit VA space):
 *   VA[47:39] = L0 (PGD) index  → 512GB per entry
 *   VA[38:30] = L1 (PUD) index  → 1GB per entry
 *   VA[29:21] = L2 (PMD) index  → 2MB per entry
 *   VA[20:12] = L3 (PTE) index  → 4KB per entry
 *   VA[11:0]  = page offset
 *
 * Two-range VA split managed by TTBR0_EL1 (user) and TTBR1_EL1 (kernel):
 *   TTBR0: 0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF  (48-bit user)
 *   TTBR1: 0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF  (48-bit kernel)
 *
 * Reference: ARM Architecture Reference Manual for ARMv8-A (DDI 0487)
 *   D8.3  Translation table descriptor formats
 *   D13.2 System control registers in AArch64
 */
#ifndef AARCH64_MMU_H
#define AARCH64_MMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Virtual address space boundaries                                     */
/* ------------------------------------------------------------------ */
#define AARCH64_USER_VA_START   0x0000000000000000UL
#define AARCH64_USER_VA_END     0x0000FFFFFFFFFFFFUL
#define AARCH64_KERN_VA_START   0xFFFF000000000000UL

/* Kernel image placement (must match linker script) */
#define KERNEL_VIRT_BASE        0xFFFF000000080000UL
#define KERNEL_PHYS_BASE        0x0000000000080000UL

/* ------------------------------------------------------------------ */
/* Page / table geometry                                                */
/* ------------------------------------------------------------------ */
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)     /* 4096 bytes            */
#define PAGE_MASK       (~(PAGE_SIZE - 1UL))

#define TABLE_ENTRIES   512                     /* 9-bit index → 2^9     */
#define TABLE_SIZE      (TABLE_ENTRIES * 8)     /* 4096 bytes            */

/* VA index extraction helpers */
#define L0_INDEX(va)    (((uint64_t)(va) >> 39) & 0x1FFUL)
#define L1_INDEX(va)    (((uint64_t)(va) >> 30) & 0x1FFUL)
#define L2_INDEX(va)    (((uint64_t)(va) >> 21) & 0x1FFUL)
#define L3_INDEX(va)    (((uint64_t)(va) >> 12) & 0x1FFUL)

/* ------------------------------------------------------------------ */
/* Page-table entry (descriptor) type bits [1:0]                       */
/* ARMv8-A D8.3 Table D8-1                                             */
/* ------------------------------------------------------------------ */
#define PTE_TYPE_FAULT  0x0UL   /* Invalid entry (all levels)           */
#define PTE_TYPE_BLOCK  0x1UL   /* Block descriptor (L1 = 1GB, L2 = 2MB) */
#define PTE_TYPE_TABLE  0x3UL   /* Table descriptor (L0/L1/L2 → next level) */
#define PTE_TYPE_PAGE   0x3UL   /* Page descriptor (L3 only, 4KB)       */

/* ------------------------------------------------------------------ */
/* Descriptor attribute bits                                            */
/* ------------------------------------------------------------------ */

/* AttrIndx[2:0] – index into MAIR_EL1 (bits [4:2]) */
#define PTE_ATTRINDX(n)         ((uint64_t)((n) & 0x7) << 2)

/* AP[2:1] – Access Permission (bits [7:6]) */
#define PTE_AP_RW_EL1   (0x0UL << 6)   /* R/W at EL1, no EL0 access    */
#define PTE_AP_RW_ALL   (0x1UL << 6)   /* R/W at EL0 + EL1             */
#define PTE_AP_RO_EL1   (0x2UL << 6)   /* R/O at EL1, no EL0 access    */
#define PTE_AP_RO_ALL   (0x3UL << 6)   /* R/O at EL0 + EL1             */

/* Aliases used by older internal code */
#define PTE_AP_KERN_RW  PTE_AP_RW_EL1
#define PTE_AP_RW       PTE_AP_RW_ALL
#define PTE_AP_KERN_RO  PTE_AP_RO_EL1
#define PTE_AP_RO       PTE_AP_RO_ALL

/* Shareability bits SH[1:0] (bits [9:8]) */
#define PTE_SH_NONE     (0x0UL << 8)   /* Non-shareable                */
#define PTE_SH_OUTER    (0x2UL << 8)   /* Outer Shareable              */
#define PTE_SH_INNER    (0x3UL << 8)   /* Inner Shareable              */

/* Other attribute bits */
#define PTE_VALID       (1UL <<  0)     /* Descriptor valid             */
#define PTE_TABLE       (1UL <<  1)     /* Table/page descriptor (vs block) */
#define PTE_AF          (1UL << 10)     /* Access Flag – must be set or   */
                                        /* first access generates fault   */
#define PTE_NG          (1UL << 11)     /* Not Global (ASID tagged)     */
#define PTE_PXN         (1UL << 53)     /* Privileged Execute Never     */
#define PTE_UXN         (1UL << 54)     /* Unprivileged Execute Never   */

/* Physical address mask: bits [47:12] for a 48-bit PA space */
#define PTE_PHYS_MASK   0x0000FFFFFFFFF000ULL

/* ------------------------------------------------------------------ */
/* MAIR_EL1 attribute indices                                           */
/* ------------------------------------------------------------------ */
#define MAIR_IDX_NORMAL_CACHEABLE   0   /* Normal WB, RW-allocate      */
#define MAIR_IDX_DEVICE_NGNRNE      1   /* Device nGnRnE (most strict) */
#define MAIR_IDX_NORMAL_NC          2   /* Normal Non-Cacheable        */

/*
 * MAIR_EL1 encoding:
 *   Attr0 = 0xFF : Normal Inner/Outer Write-Back Non-transient RW-alloc
 *   Attr1 = 0x00 : Device nGnRnE
 *   Attr2 = 0x44 : Normal Inner/Outer Non-Cacheable
 */
#define MAIR_EL1_VALUE  \
    ((0xFFUL        )   |   /* Attr0: Normal WB cacheable  */ \
     (0x00UL <<  8  )   |   /* Attr1: Device nGnRnE        */ \
     (0x44UL << 16  ))      /* Attr2: Normal Non-Cacheable */

/* ------------------------------------------------------------------ */
/* TCR_EL1 encoding (48-bit VA, 4KB granule)                           */
/* ------------------------------------------------------------------ */
/*
 * T0SZ = 16  → TTBR0 VA[47:0] (48-bit, top 16 bits = 0000)
 * T1SZ = 16  → TTBR1 VA[47:0] (48-bit, top 16 bits = FFFF)
 * IRGN0/1 = 01 → Inner WB WBWA cacheable
 * ORGN0/1 = 01 → Outer WB WBWA cacheable
 * SH0/1   = 11 → Inner Shareable
 * TG0 = 00     → 4KB TTBR0 granule
 * TG1 = 10     → 4KB TTBR1 granule (TG1 encoding is different from TG0)
 * IPS = 101    → 48-bit Physical Address Space
 * AS  = 0      → 8-bit ASID
 */
#define TCR_T0SZ        (16UL   <<  0)
#define TCR_IRGN0_WB   (0x1UL  <<  8)
#define TCR_ORGN0_WB   (0x1UL  << 10)
#define TCR_SH0_INNER  (0x3UL  << 12)
#define TCR_TG0_4KB    (0x0UL  << 14)
#define TCR_T1SZ        (16UL  << 16)
#define TCR_IRGN1_WB  (0x1UL  << 24)
#define TCR_ORGN1_WB  (0x1UL  << 26)
#define TCR_SH1_INNER (0x3UL  << 28)
#define TCR_TG1_4KB   (0x2UL  << 30)   /* TG1: 10 = 4KB (inverted encoding) */
#define TCR_IPS_48BIT (0x5UL  << 32)   /* 48-bit IPA / PA                   */
#define TCR_AS_8BIT   (0x0UL  << 36)   /* 8-bit ASID                        */

#define TCR_EL1_VALUE   (TCR_T0SZ  | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER | \
                         TCR_TG0_4KB | TCR_T1SZ | TCR_IRGN1_WB | TCR_ORGN1_WB |    \
                         TCR_SH1_INNER | TCR_TG1_4KB | TCR_IPS_48BIT | TCR_AS_8BIT)

/* ------------------------------------------------------------------ */
/* SCTLR_EL1 control bits (see boot_defs.h for full set)               */
/* ------------------------------------------------------------------ */
#define SCTLR_EL1_M     (1UL <<  0)    /* MMU enable                  */
#define SCTLR_EL1_A     (1UL <<  1)    /* Alignment fault enable      */
#define SCTLR_EL1_C     (1UL <<  2)    /* Data cache enable           */
#define SCTLR_EL1_SA    (1UL <<  3)    /* Stack-pointer alignment check */
#define SCTLR_EL1_SA0   (1UL <<  4)    /* EL0 SP alignment check      */
#define SCTLR_EL1_I     (1UL << 12)    /* I-cache enable              */
#define SCTLR_EL1_WXN   (1UL << 19)    /* Write implies XN            */
/* Required-one (RES1) bits per ARMv8-A spec */
#define SCTLR_EL1_RES1  0x00C50830UL

/* ------------------------------------------------------------------ */
/* Convenience flag combinations for aarch64_map_page()                */
/* ------------------------------------------------------------------ */
#define PAGE_FLAGS_KERNEL   (PTE_TYPE_PAGE | PTE_AF | PTE_SH_INNER | \
                             PTE_AP_RW_EL1 | PTE_ATTRINDX(MAIR_IDX_NORMAL_CACHEABLE) | \
                             PTE_UXN)

#define PAGE_FLAGS_USER     (PTE_TYPE_PAGE | PTE_AF | PTE_SH_INNER | \
                             PTE_AP_RW_ALL | PTE_ATTRINDX(MAIR_IDX_NORMAL_CACHEABLE) | \
                             PTE_UXN)

#define PAGE_FLAGS_DEVICE   (PTE_TYPE_PAGE | PTE_AF | \
                             PTE_AP_RW_EL1 | PTE_ATTRINDX(MAIR_IDX_DEVICE_NGNRNE) | \
                             PTE_PXN | PTE_UXN)

/* Named flag bits for aarch64_map_page() flags argument */
#define AARCH64_MAP_RW      0x01u   /* Read/write (default)            */
#define AARCH64_MAP_RO      0x02u   /* Read-only                       */
#define AARCH64_MAP_X       0x04u   /* Executable (kernel only)        */
#define AARCH64_MAP_DEVICE  0x08u   /* Device memory (nGnRnE)          */
#define AARCH64_MAP_NC      0x10u   /* Normal Non-Cacheable            */
#define AARCH64_MAP_USER    0x20u   /* Accessible from EL0             */

/* ------------------------------------------------------------------ */
/* Type definitions                                                     */
/* ------------------------------------------------------------------ */
typedef uint64_t pte_t;

/*
 * pgd_t – an L0 page-global-directory (512 entries × 8 bytes = 4KB).
 * The array is PAGE_SIZE bytes so it can be placed in any 4KB-aligned slot.
 */
typedef pte_t pgd_t[TABLE_ENTRIES];

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * aarch64_mmu_setup_mair - Program MAIR_EL1 with the three standard memory
 * attribute slots used by Fern.
 *
 * Slot 0 (MAIR_IDX_NORMAL_CACHEABLE): Normal, Inner/Outer Write-Back
 *   Non-transient, Read-Allocate, Write-Allocate.
 * Slot 1 (MAIR_IDX_DEVICE_NGNRNE):   Device nGnRnE (most restrictive).
 * Slot 2 (MAIR_IDX_NORMAL_NC):        Normal Inner/Outer Non-Cacheable.
 */
void aarch64_mmu_setup_mair(void);

/**
 * aarch64_mmu_setup_tcr - Program TCR_EL1 for 48-bit VA, 4KB granule,
 * inner/outer WB-WBWA caches, inner-shareable, and 48-bit IPA.
 *
 * Must be called before enabling the MMU.  After this call:
 *   TTBR0_EL1 governs VA[47:0]  (top 16 bits all-zero  → user range)
 *   TTBR1_EL1 governs VA[47:0]  (top 16 bits all-one   → kernel range)
 */
void aarch64_mmu_setup_tcr(void);

/**
 * aarch64_mmu_create_pgd - Allocate and zero a fresh L0 page table.
 *
 * During early boot the table is drawn from the static pool (no heap needed).
 * Returns NULL if the pool is exhausted.
 */
pgd_t *aarch64_mmu_create_pgd(void);

/**
 * aarch64_map_page - Install a single 4KB mapping in @pgd.
 *
 * @pgd:   Pointer to the L0 table (physical == virtual before MMU on).
 * @virt:  Virtual address (4KB aligned).
 * @phys:  Physical address (4KB aligned).
 * @flags: Combination of AARCH64_MAP_* flags.
 *
 * Intermediate tables are allocated from the early pool as needed.
 * Returns 0 on success, -1 if a table allocation failed.
 */
int aarch64_map_page(pgd_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * aarch64_get_phys - Walk @pgd and return the physical address mapped at @virt.
 *
 * Returns the physical address on success, or (uint64_t)-1 if the mapping
 * does not exist (fault entry encountered at any level).
 */
uint64_t aarch64_get_phys(pgd_t *pgd, uint64_t virt);

/**
 * aarch64_mmu_enable - Load TTBRs and enable the MMU.
 *
 * @kernel_pgd: Physical address of the kernel (TTBR1) L0 table.
 * @user_pgd:   Physical address of the user   (TTBR0) L0 table.
 *              Pass NULL / 0 if no user mappings are needed yet.
 *
 * Sequence: write TTBR0/TTBR1, flush TLBs, set SCTLR_EL1.M + C + I, ISB.
 */
void aarch64_mmu_enable(pgd_t *kernel_pgd, pgd_t *user_pgd);

/**
 * aarch64_tlb_flush_all - Invalidate all TLB entries (TLBI VMALLE1IS).
 * Followed by DSB and ISB to ensure completion before the next instruction.
 */
void aarch64_tlb_flush_all(void);

/**
 * aarch64_tlb_flush_page - Invalidate the TLB entry for a single virtual page.
 *
 * @vaddr: Any virtual address within the 4KB page to invalidate.
 *         The address is shifted right by 12 bits before being written to
 *         TLBI VAE1IS as required by the architecture.
 */
void aarch64_tlb_flush_page(uint64_t vaddr);

/**
 * aarch64_mmu_init - High-level MMU initialisation entry point.
 *
 * Calls aarch64_mmu_setup_mair(), aarch64_mmu_setup_tcr(), builds the
 * early kernel page tables (identity map + higher-half kernel + MMIO),
 * then calls aarch64_mmu_enable() to switch the MMU on.
 */
void aarch64_mmu_init(void);

/**
 * aarch64_get_kernel_pgd - Return a pointer to the static kernel L0 table.
 */
pgd_t *aarch64_get_kernel_pgd(void);

#endif /* AARCH64_MMU_H */
