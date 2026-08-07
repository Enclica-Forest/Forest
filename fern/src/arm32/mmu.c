/**
 * @file mmu.c
 * @brief ARM32 VMSAv7 MMU implementation
 *
 * Implements the ARM Short-Descriptor translation table format (VMSAv7).
 *
 * Design notes
 * ------------
 *  - The L1 table (16 KB, 4096 × 4-byte entries) is allocated statically in
 *    the BSS section so it is always available without a heap.
 *  - L2 tables (1 KB each, 256 entries) are drawn from a small static pool
 *    (ARM_L2_POOL_SIZE entries).  Increase that constant if the kernel needs
 *    more fine-grained 4 KB mappings.
 *  - All addresses are assumed to be physical == virtual at MMU-enable time
 *    (the caller must ensure this is the case for the page tables themselves).
 *  - No locks: the implementation is single-CPU and must be called before SMP
 *    is brought up.  Add spinlocks around the pool allocator for SMP.
 *
 * Reference: ARM Architecture Reference Manual ARMv7-A/R (DDI 0406C.d)
 *   §B3.5  Short-descriptor translation table format
 *   §B3.10 CP15 registers for VMSA
 */

#include "mmu.h"
#include "cache.h"

/* =========================================================================
 * Compile-time configuration
 * ========================================================================= */

/**
 * Number of L2 page tables pre-allocated in the static pool.
 * Each table covers 1 MB at 4 KB granularity (256 entries).
 * Adjust upward if arm_map_page() returns with a panic due to pool
 * exhaustion.
 */
#ifndef ARM_L2_POOL_SIZE
#define ARM_L2_POOL_SIZE 64
#endif

/* =========================================================================
 * Static storage
 * ========================================================================= */

/*
 * The L1 translation table MUST be 16 KB-aligned (TTBCR.N = 0 means the
 * full 4 GB VA space is covered by TTBR0, requiring bits [13:0] of TTBR0
 * to be zero).
 *
 * We use __attribute__((aligned(16384))) and place the table in BSS so it
 * is automatically zeroed before kernel_main() is entered (boot.S clears
 * BSS before calling the C entry point).
 */
static arm_l1_table_t arm_kernel_l1
    __attribute__((aligned(16384)));

/*
 * Pool of L2 page tables.  Each entry is 1 KB but we align the whole pool
 * to 1 KB so that the very first element is already aligned.  Individual
 * entries are 256 × 4 = 1024 bytes, which is a power of two, so every
 * subsequent element in the array is also 1 KB aligned.
 */
static arm_l2_table_t arm_l2_pool[ARM_L2_POOL_SIZE]
    __attribute__((aligned(1024)));

/* Index of the next free L2 table in the pool. */
static uint32_t arm_l2_pool_next = 0;

/* Set to true once arm_mmu_init() has completed. */
static bool arm_mmu_ready = false;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * alloc_l2_table - Allocate one L2 page table from the static pool and
 *                  zero-initialise it.
 *
 * @return Pointer to a freshly zeroed 256-entry L2 table, or NULL if the
 *         pool is exhausted (caller must handle this as a fatal error).
 */
static arm_l2_table_t *alloc_l2_table(void)
{
    if (arm_l2_pool_next >= ARM_L2_POOL_SIZE) {
        /* Pool exhausted – the caller is responsible for panicking. */
        return (arm_l2_table_t *)0;
    }

    arm_l2_table_t *tbl = &arm_l2_pool[arm_l2_pool_next++];

    /* Zero all 256 entries (fault descriptors = 0x00). */
    uint32_t *p = (uint32_t *)tbl;
    for (uint32_t i = 0; i < 256; i++) {
        p[i] = 0U;
    }

    return tbl;
}

/**
 * round_up_mb - Round a byte count up to the next 1 MB boundary.
 */
static inline uint32_t round_up_mb(uint32_t bytes)
{
    return (bytes + 0x000FFFFFU) & 0xFFF00000U;
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

/**
 * arm_map_section - Insert a 1 MB section descriptor into an L1 table.
 *
 * The descriptor format (ARMv7 ARM Table B3-4, bits for a section):
 *   [31:20] Section base address (PA[31:20])
 *   [19]    NS
 *   [18]    0 (section, not supersection)
 *   [17]    nG
 *   [16]    S
 *   [15]    AP[2]
 *   [14:12] TEX[2:0]
 *   [11:10] AP[1:0]
 *   [9]     IMP (implementation defined, 0)
 *   [8:5]   Domain[3:0]
 *   [4]     XN
 *   [3]     C
 *   [2]     B
 *   [1:0]   10 (section type)
 */
void arm_map_section(arm_l1_table_t *ttb,
                     uint32_t        virt_mb,
                     uint32_t        phys_mb,
                     uint32_t        flags)
{
    /* L1 index = VA[31:20] */
    uint32_t idx = virt_mb >> 20;

    /* Physical base goes in bits [31:20] of the descriptor. */
    uint32_t desc = ARM_SECT_BASE(phys_mb)
                  | (flags & ~0x3U)         /* user flags, type bits cleared */
                  | ARM_L1_TYPE_SECTION;    /* set type = 0b10 */

    (*ttb)[idx] = desc;
}

/**
 * arm_map_page - Insert a 4 KB small-page mapping.
 *
 * If the L1 entry for the containing 1 MB region is currently a fault entry,
 * a new L2 table is allocated from the static pool and the L1 entry is
 * updated to point to it.  If the L1 entry is already a PAGE entry, the
 * existing L2 table is reused.
 *
 * The L2 small-page descriptor format (ARMv7 ARM Table B3-11):
 *   [31:12] Small page base address (PA[31:12])
 *   [11]    nG
 *   [10]    S
 *   [9]     AP[2]
 *   [8:6]   TEX[2:0]
 *   [5]     AP[1]
 *   [4]     AP[0]
 *   [3]     C
 *   [2]     B
 *   [1]     1  (small page type bit)
 *   [0]     XN
 */
void arm_map_page(arm_l1_table_t *ttb,
                  uint32_t        virt,
                  uint32_t        phys,
                  uint32_t        flags)
{
    uint32_t l1_idx  = virt >> 20;            /* VA[31:20] */
    uint32_t l2_idx  = (virt >> 12) & 0xFFU; /* VA[19:12] */

    uint32_t l1_desc = (*ttb)[l1_idx];
    arm_l2_table_t *l2;

    if ((l1_desc & 0x3U) == ARM_L1_TYPE_FAULT) {
        /* No L2 table yet – allocate one from the pool. */
        l2 = alloc_l2_table();
        if (!l2) {
            /*
             * Pool exhausted.  We cannot call a kernel panic function here
             * because we don't want to pull in unrelated subsystems.  Simply
             * return; the caller will observe a fault when accessing the
             * mapping.  In a production kernel replace with kernel_panic().
             */
            return;
        }

        /*
         * L1 PAGE descriptor (ARMv7 ARM Table B3-4):
         *   [31:10] L2 table base address (PA[31:10])
         *   [8:5]   Domain
         *   [4]     IMP (0)
         *   [3]     NS (Non-Secure, 0 = Secure)
         *   [2]     PXN (Privileged Execute Never for the whole L2 table)
         *   [1:0]   01 (page table type)
         *
         * We use domain 0 (same as sections) and keep NS=0, PXN=0 here;
         * per-page XN is set in the L2 descriptor.
         */
        uint32_t l2_phys = (uint32_t)(uintptr_t)l2;
        (*ttb)[l1_idx] = (l2_phys & 0xFFFFFC00U)
                       | ARM_SECT_DOMAIN(0)
                       | ARM_L1_TYPE_PAGE;

    } else if ((l1_desc & 0x3U) == ARM_L1_TYPE_PAGE) {
        /* L2 table already exists; recover its virtual address. */
        uint32_t l2_phys = l1_desc & 0xFFFFFC00U;
        l2 = (arm_l2_table_t *)(uintptr_t)l2_phys;

    } else {
        /*
         * The 1 MB region is already covered by a section descriptor.
         * Mixing section and page mappings within the same 1 MB is
         * legal only after removing the section entry first.  Return
         * without modifying anything to prevent descriptor corruption.
         */
        return;
    }

    /*
     * Write the L2 small-page descriptor.
     * Bit 1 = 1 marks a small page; bit 0 carries XN.
     * OR the user-supplied flags (which may include ARM_PAGE_XN,
     * ARM_PAGE_AP*, ARM_PAGE_TEX*, etc.) and force the small-page
     * type bit.
     */
    uint32_t l2_desc = ARM_PAGE_BASE(phys)
                     | (flags & ~0x3U)      /* preserve all flag bits */
                     | ARM_L2_TYPE_SMALL;   /* bit 1 = 1 */

    (*l2)[l2_idx] = l2_desc;
}

/**
 * arm_flush_tlb_all - Invalidate the unified TLB.
 *
 * MCR p15, 0, Rd, c8, c7, 0  =>  TLBIALL (invalidate all TLB entries)
 * Followed by DSB to ensure the TLB invalidation is complete before any
 * further memory accesses, and ISB so that any subsequent instruction
 * fetches see the new mappings.
 */
void arm_flush_tlb_all(void)
{
    uint32_t zero = 0U;
    __asm__ volatile(
        "mcr p15, 0, %0, c8, c7, 0\n"  /* TLBIALL */
        "dsb\n"
        "isb\n"
        : : "r"(zero) : "memory"
    );
}

/**
 * arm_flush_tlb_page - Invalidate a single TLB entry by virtual address.
 *
 * MCR p15, 0, Rd, c8, c7, 1  =>  TLBIMVA (invalidate TLB entry by MVA)
 * Bits [11:0] of the address are ignored by the hardware; we mask them
 * anyway for clarity.  Bits [7:0] of the register carry the ASID (0 here
 * because we use ASID 0 / global mappings).
 */
void arm_flush_tlb_page(uint32_t vaddr)
{
    uint32_t mva = vaddr & 0xFFFFF000U; /* Page-align; ASID 0 in [7:0] */
    __asm__ volatile(
        "mcr p15, 0, %0, c8, c7, 1\n"  /* TLBIMVA */
        "dsb\n"
        "isb\n"
        : : "r"(mva) : "memory"
    );
}

/**
 * arm_get_physical_addr - Software page-table walk.
 *
 * Handles section (1 MB) and small-page (4 KB) mappings.  Returns 0 for
 * fault entries or unrecognised descriptor types.
 */
uint32_t arm_get_physical_addr(arm_l1_table_t *ttb, uint32_t vaddr)
{
    uint32_t l1_idx  = vaddr >> 20;
    uint32_t l1_desc = (*ttb)[l1_idx];

    switch (l1_desc & 0x3U) {

    case ARM_L1_TYPE_FAULT:
        return 0U;

    case ARM_L1_TYPE_SECTION: {
        /* Supersection check (bit 18): not currently mapped by this driver,
         * but handle gracefully by treating as a regular section. */
        uint32_t offset = vaddr & 0x000FFFFFU; /* VA[19:0] */
        return (l1_desc & 0xFFF00000U) | offset;
    }

    case ARM_L1_TYPE_PAGE: {
        /* Walk the L2 table. */
        uint32_t l2_phys = l1_desc & 0xFFFFFC00U;
        arm_l2_table_t *l2 = (arm_l2_table_t *)(uintptr_t)l2_phys;
        uint32_t l2_idx  = (vaddr >> 12) & 0xFFU;
        uint32_t l2_desc = (*l2)[l2_idx];

        switch (l2_desc & 0x3U) {
        case ARM_L2_TYPE_FAULT:
            return 0U;
        case ARM_L2_TYPE_SMALL:
            return (l2_desc & 0xFFFFF000U) | (vaddr & 0xFFFU);
        case ARM_L2_TYPE_LARGE:
            /* 64 KB large page: base in [31:16], offset VA[15:0] */
            return (l2_desc & 0xFFFF0000U) | (vaddr & 0xFFFFU);
        default:
            return 0U;
        }
    }

    default:
        return 0U;
    }
}

/**
 * arm_mmu_init - Set up the MMU translation tables and configure CP15.
 *
 * Sequence:
 *  1. Zero the static L1 table (insurance; BSS should already be zero).
 *  2. Identity-map the kernel physical range using 1 MB sections (Normal
 *     Write-Back Write-Allocate, shareable, kernel R/W only).
 *  3. If the kernel has a separate virtual base (higher-half), also install
 *     the higher-half mapping so the kernel can run after the jump.
 *  4. Set TTBCR to 0 (use TTBR0 for the full 4 GB VA space, N = 0).
 *  5. Set DACR: domain 0 = Client (AP bits in descriptors are checked).
 *  6. Write the L1 table physical address to TTBR0.
 *  7. Issue a DSB + ISB to ensure all writes are visible before the caller
 *     calls arm_enable_mmu().
 */
void arm_mmu_init(uint32_t kernel_phys_base,
                  uint32_t kernel_virt_base,
                  uint32_t kernel_size)
{
    /* 1. Zero the L1 table (redundant if BSS was cleared, but defensive). */
    uint32_t *p = (uint32_t *)arm_kernel_l1;
    for (uint32_t i = 0; i < 4096; i++) {
        p[i] = 0U;
    }

    /* Round kernel size up to the next 1 MB boundary. */
    uint32_t mapped_size = round_up_mb(kernel_size);

    /*
     * Section flags for normal kernel code/data:
     *   - Normal WB/WA memory (TEX=001, C=1, B=1)
     *   - Shareable
     *   - Kernel read/write (AP[2:0] = 001)
     *   - Domain 0
     *   - Not global (kernel only)
     *   - Execute allowed (XN = 0) – restrict further per page if needed
     */
    uint32_t sect_flags = ARM_MEM_NORMAL_WB_WA
                        | ARM_SECT_S
                        | ARM_AP_KERNEL_RW
                        | ARM_SECT_DOMAIN(0);

    /* 2. Identity-map the physical kernel range. */
    for (uint32_t off = 0; off < mapped_size; off += 0x100000U) {
        arm_map_section(&arm_kernel_l1,
                        kernel_phys_base + off,
                        kernel_phys_base + off,
                        sect_flags);
    }

    /* 3. Map the virtual range if different from the physical range. */
    if (kernel_virt_base != kernel_phys_base) {
        for (uint32_t off = 0; off < mapped_size; off += 0x100000U) {
            arm_map_section(&arm_kernel_l1,
                            kernel_virt_base + off,
                            kernel_phys_base + off,
                            sect_flags);
        }
    }

    /* 4. TTBCR: N=0, use TTBR0 for entire 4 GB VA space. */
    arm_write_ttbcr(0U);

    /* 5. DACR: domain 0 = Client (check AP bits). */
    arm_write_dacr(ARM_DACR_D(0, ARM_DACR_CLIENT));

    /* 6. Load the L1 table physical address into TTBR0.
     *
     * TTBR0 format (TTBCR.N = 0):
     *   [31:14] Translation table base (must be 16 KB aligned)
     *   [6]     IRGN[1] – inner cache attribute bit 1
     *   [4:3]   RGN     – outer cache attribute
     *   [1]     S       – shareable
     *   [0]     C / IRGN[0] – inner cache attribute bit 0
     *
     * For simplicity we use the Non-cacheable, Non-shareable setting
     * (all attribute bits = 0).  Enable TTBR0 caching attributes after the
     * cache hierarchy is fully initialised.
     */
    uint32_t ttbr0 = (uint32_t)(uintptr_t)(&arm_kernel_l1);
    arm_write_ttbr0(ttbr0);

    /* 7. Ensure all CP15 writes are committed before returning. */
    __asm__ volatile("dsb\n" "isb\n" : : : "memory");

    arm_mmu_ready = true;
}

/**
 * arm_enable_mmu - Enable the MMU by setting the M bit in SCTLR.
 *
 * The caller MUST ensure:
 *  - TTBR0 is loaded with a valid, fully-populated L1 table.
 *  - The code path from this function through the ISB and back to the
 *    caller is identity-mapped (VA == PA) so that the program counter
 *    remains valid after the M bit is set.
 *
 * After enabling the MMU the function sets C (data cache) and I
 * (instruction cache) bits as well, as it is safe to do so once the
 * translation tables are correct.
 */
void arm_enable_mmu(arm_l1_table_t *ttb)
{
    /*
     * Re-write TTBR0 with the supplied table pointer.  This is a
     * convenience in case the caller prepared a different table from the
     * one arm_mmu_init() used.
     */
    uint32_t ttbr0 = (uint32_t)(uintptr_t)ttb;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0"
                     : : "r"(ttbr0) : "memory");

    /* Flush branch predictor (BPIALL). */
    {
        uint32_t zero = 0U;
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 6\n"
                         : : "r"(zero) : "memory");
    }

    /* Invalidate TLB (TLBIALL). */
    arm_flush_tlb_all();

    /* Invalidate I-cache (ICIALLU). */
    arm_icache_invalidate_all();

    /* DSB: ensure all cache/TLB ops are complete. */
    arm_barrier_dsb();

    /* Read-modify-write SCTLR: set M, C, I bits. */
    uint32_t sctlr = arm_read_sctlr();
    sctlr |= ARM_SCTLR_M | ARM_SCTLR_C | ARM_SCTLR_I;
    arm_write_sctlr(sctlr);

    /*
     * ISB: flush the instruction pipeline so that the instruction after
     * this barrier executes with the new SCTLR value (MMU enabled).
     */
    arm_barrier_isb();
}
