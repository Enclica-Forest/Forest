/*
 * Fern - AArch64 MMU initialisation
 *
 * Implements a 4-level page table walk (PGD → PUD → PMD → PTE) using the
 * ARMv8-A 4KB granule, 48-bit virtual / physical address format.
 *
 * Design notes
 * ------------
 * Physical memory for page-table pages is drawn from a small static pool
 * (PT_POOL_PAGES × 4KB) during early boot, before the kernel heap is live.
 * Every pool page is PAGE_SIZE-aligned (required by hardware) and is zeroed
 * when first handed out so all entries are fault descriptors.
 *
 * After the generic memory manager is available the alloc_table() function
 * can be redirected to the real allocator by replacing the hook – this is
 * left as a future exercise; for now the pool is large enough to map the
 * early kernel, MMIO regions, and a handful of user-space pages.
 *
 * Reference: ARM Architecture Reference Manual for ARMv8-A (DDI 0487)
 *   D8   Translation table walks
 *   D13  System control registers in AArch64 state
 */

#include "mmu.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Early page-table memory pool                                         */
/* 64 tables of 4KB = 256 KB – enough to map the boot kernel image.   */
/* ------------------------------------------------------------------ */
#define PT_POOL_PAGES   64

static uint64_t __attribute__((aligned(PAGE_SIZE)))
    pt_pool[PT_POOL_PAGES][TABLE_ENTRIES];
static uint32_t pt_pool_next = 0;

/*
 * alloc_table - Allocate one 4KB page-table page from the static pool.
 *
 * The page is zeroed (all entries = PTE_TYPE_FAULT) before being returned.
 * Returns NULL when the pool is exhausted.
 */
static uint64_t *alloc_table(void)
{
    if (pt_pool_next >= PT_POOL_PAGES)
        return NULL;    /* pool exhausted – caller must handle */

    uint64_t *tbl = pt_pool[pt_pool_next++];

    /* Zero: every entry must start as PTE_TYPE_FAULT (0x0) */
    for (int i = 0; i < (int)TABLE_ENTRIES; i++)
        tbl[i] = PTE_TYPE_FAULT;

    return tbl;
}

/* ------------------------------------------------------------------ */
/* Static kernel PGD (L0 table wired into TTBR1_EL1)                   */
/* ------------------------------------------------------------------ */
static pgd_t __attribute__((aligned(PAGE_SIZE))) kernel_pgd;

pgd_t *aarch64_get_kernel_pgd(void)
{
    return &kernel_pgd;
}

/* ------------------------------------------------------------------ */
/* aarch64_mmu_setup_mair                                               */
/* ------------------------------------------------------------------ */
void aarch64_mmu_setup_mair(void)
{
    /*
     * MAIR_EL1 packs eight 8-bit attribute fields.
     * We use three slots; the remaining five are left zero (Device nGnRnE,
     * same as slot 1 – safe for unused entries).
     *
     * Slot 0 (MAIR_IDX_NORMAL_CACHEABLE = 0):
     *   0xFF = Normal memory, Inner/Outer Write-Back Non-transient,
     *          Read-Allocate, Write-Allocate.
     *
     * Slot 1 (MAIR_IDX_DEVICE_NGNRNE = 1):
     *   0x00 = Device nGnRnE (non-Gathering, non-Reordering,
     *          non-Early Write Acknowledgement) – the most restrictive
     *          device attribute, mandatory for MMIO registers.
     *
     * Slot 2 (MAIR_IDX_NORMAL_NC = 2):
     *   0x44 = Normal Inner/Outer Non-Cacheable.
     */
    uint64_t mair = 0;
    mair |= (0xFFULL << (MAIR_IDX_NORMAL_CACHEABLE * 8));
    mair |= (0x00ULL << (MAIR_IDX_DEVICE_NGNRNE    * 8));
    mair |= (0x44ULL << (MAIR_IDX_NORMAL_NC         * 8));

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("isb");
}

/* ------------------------------------------------------------------ */
/* aarch64_mmu_setup_tcr                                                */
/* ------------------------------------------------------------------ */
void aarch64_mmu_setup_tcr(void)
{
    /*
     * TCR_EL1 – Translation Control Register
     *
     * Field       Bits    Value  Meaning
     * -------     ----    -----  -------
     * T0SZ        [5:0]   16     48-bit TTBR0 VA (2^(64-16) = 2^48 range)
     * IRGN0       [9:8]   01     TTBR0 inner WB WBWA cacheable
     * ORGN0       [11:10] 01     TTBR0 outer WB WBWA cacheable
     * SH0         [13:12] 11     TTBR0 inner shareable
     * TG0         [15:14] 00     TTBR0 4KB granule
     * T1SZ        [21:16] 16     48-bit TTBR1 VA
     * IRGN1       [25:24] 01     TTBR1 inner WB WBWA cacheable
     * ORGN1       [27:26] 01     TTBR1 outer WB WBWA cacheable
     * SH1         [29:28] 11     TTBR1 inner shareable
     * TG1         [31:30] 10     TTBR1 4KB granule (encoding 10 = 4KB for TG1)
     * IPS         [34:32] 101    Intermediate PA size = 48 bits
     * AS          [36]    0      8-bit ASID
     */
    uint64_t tcr = 0;
    tcr |= (16ULL <<  0);   /* T0SZ = 16  → 48-bit TTBR0 VA */
    tcr |= (1ULL  <<  8);   /* IRGN0 = 01 inner WB WBWA      */
    tcr |= (1ULL  << 10);   /* ORGN0 = 01 outer WB WBWA      */
    tcr |= (3ULL  << 12);   /* SH0 = 11   inner shareable    */
    tcr |= (0ULL  << 14);   /* TG0 = 00   4KB granule        */
    tcr |= (16ULL << 16);   /* T1SZ = 16  → 48-bit TTBR1 VA */
    tcr |= (1ULL  << 24);   /* IRGN1 = 01 inner WB WBWA      */
    tcr |= (1ULL  << 26);   /* ORGN1 = 01 outer WB WBWA      */
    tcr |= (3ULL  << 28);   /* SH1 = 11   inner shareable    */
    tcr |= (2ULL  << 30);   /* TG1 = 10   4KB granule        */
    tcr |= (5ULL  << 32);   /* IPS = 101  48-bit PA          */
    tcr |= (0ULL  << 36);   /* AS = 0     8-bit ASID         */

    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("isb");
}

/* ------------------------------------------------------------------ */
/* aarch64_mmu_create_pgd                                               */
/* ------------------------------------------------------------------ */
pgd_t *aarch64_mmu_create_pgd(void)
{
    /*
     * A pgd_t is typedef'd as pte_t[TABLE_ENTRIES], which is the same
     * layout as one pool page.  Cast the raw pointer to pgd_t*.
     */
    return (pgd_t *)alloc_table();
}

/* ------------------------------------------------------------------ */
/* Internal: get or create the next-level table                         */
/* ------------------------------------------------------------------ */

/*
 * get_or_create_next_table - Given a pointer to a table descriptor entry,
 * return the child table it points to, allocating one if necessary.
 *
 * @entry: pointer to the L0/L1/L2 descriptor word.
 *
 * Returns a pointer to the child (L1/L2/L3) table, or NULL on ENOMEM.
 */
static uint64_t *get_or_create_next_table(uint64_t *entry)
{
    if ((*entry & 0x3UL) == PTE_TYPE_TABLE) {
        /* Already a valid table descriptor – extract PA[47:12] */
        return (uint64_t *)(uintptr_t)(*entry & PTE_PHYS_MASK);
    }

    uint64_t *child = alloc_table();
    if (!child)
        return NULL;

    /* Install the table descriptor: PA[47:12] | TYPE_TABLE */
    uint64_t child_pa = (uint64_t)(uintptr_t)child;
    *entry = (child_pa & PTE_PHYS_MASK) | PTE_TYPE_TABLE;

    return child;
}

/* ------------------------------------------------------------------ */
/* aarch64_map_page                                                     */
/* ------------------------------------------------------------------ */
int aarch64_map_page(pgd_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags)
{
    /*
     * Translate the caller's flags into ARMv8-A descriptor attribute bits.
     *
     * The flags argument accepts either:
     *   (a) raw PTE attribute bits (high bits set, e.g. PAGE_FLAGS_KERNEL), OR
     *   (b) AARCH64_MAP_* logical flags (only low bits set).
     *
     * We detect case (a) by checking whether any bit above bit 5 is set.
     * If so we use the flags directly as PTE attributes (caller takes full
     * responsibility).  Otherwise we construct the PTE from the logical flags.
     */
    uint64_t pte_attrs;

    if (flags & ~0x3FUL) {
        /* Raw PTE attribute bits provided – use them directly. */
        pte_attrs = flags;
    } else {
        /* Logical AARCH64_MAP_* flags – translate to PTE attributes. */
        pte_attrs = PTE_AF;  /* Access Flag must be set; otherwise page fault */

        if (flags & AARCH64_MAP_DEVICE) {
            /* Device nGnRnE: non-cacheable, no sharing, no execute */
            pte_attrs |= PTE_ATTRINDX(MAIR_IDX_DEVICE_NGNRNE);
            pte_attrs |= PTE_AP_RW_EL1;
            pte_attrs |= PTE_SH_NONE;
            pte_attrs |= PTE_PXN | PTE_UXN;
        } else if (flags & AARCH64_MAP_NC) {
            /* Normal Non-Cacheable */
            pte_attrs |= PTE_ATTRINDX(MAIR_IDX_NORMAL_NC);
            pte_attrs |= PTE_SH_INNER;
            pte_attrs |= PTE_PXN | PTE_UXN;
            if (flags & AARCH64_MAP_USER)
                pte_attrs |= (flags & AARCH64_MAP_RO) ? PTE_AP_RO_ALL : PTE_AP_RW_ALL;
            else
                pte_attrs |= (flags & AARCH64_MAP_RO) ? PTE_AP_RO_EL1 : PTE_AP_RW_EL1;
        } else {
            /* Normal cacheable (most common case) */
            pte_attrs |= PTE_ATTRINDX(MAIR_IDX_NORMAL_CACHEABLE);
            pte_attrs |= PTE_SH_INNER;

            if (flags & AARCH64_MAP_X) {
                /*
                 * Executable kernel mapping: R/O at EL1, never executable
                 * from EL0 (UXN), no PXN so EL1 code can run here.
                 */
                pte_attrs |= PTE_AP_RO_EL1;
                pte_attrs |= PTE_UXN;
                /* PXN left clear → privileged execute allowed */
            } else if (flags & AARCH64_MAP_USER) {
                pte_attrs |= PTE_UXN | PTE_PXN;
                pte_attrs |= (flags & AARCH64_MAP_RO) ? PTE_AP_RO_ALL : PTE_AP_RW_ALL;
            } else {
                /* Kernel data: not executable from either EL */
                pte_attrs |= PTE_PXN | PTE_UXN;
                pte_attrs |= (flags & AARCH64_MAP_RO) ? PTE_AP_RO_EL1 : PTE_AP_RW_EL1;
            }
        }

        /* L3 page descriptor type bits */
        pte_attrs |= PTE_TYPE_PAGE;
    }

    /* Ensure the type bits are always set for an L3 page descriptor */
    pte_attrs |= PTE_TYPE_PAGE;

    /* ------ 4-level walk: PGD (L0) → PUD (L1) → PMD (L2) → PTE (L3) ------ */

    /* L0 (PGD): VA[47:39] */
    uint64_t *l0 = (uint64_t *)pgd;
    uint64_t *l1 = get_or_create_next_table(&l0[L0_INDEX(virt)]);
    if (!l1)
        return -1;

    /* L1 (PUD): VA[38:30] */
    uint64_t *l2 = get_or_create_next_table(&l1[L1_INDEX(virt)]);
    if (!l2)
        return -1;

    /* L2 (PMD): VA[29:21] */
    uint64_t *l3 = get_or_create_next_table(&l2[L2_INDEX(virt)]);
    if (!l3)
        return -1;

    /* L3 (PTE): VA[20:12] – install the leaf page descriptor */
    l3[L3_INDEX(virt)] = (phys & PTE_PHYS_MASK) | pte_attrs;

    return 0;
}

/* ------------------------------------------------------------------ */
/* aarch64_get_phys                                                     */
/* ------------------------------------------------------------------ */
uint64_t aarch64_get_phys(pgd_t *pgd, uint64_t virt)
{
    uint64_t *l0 = (uint64_t *)pgd;

    /* L0 → L1 */
    uint64_t l0e = l0[L0_INDEX(virt)];
    if ((l0e & 0x3UL) != PTE_TYPE_TABLE)
        return (uint64_t)-1;
    uint64_t *l1 = (uint64_t *)(uintptr_t)(l0e & PTE_PHYS_MASK);

    /* L1 → L2 */
    uint64_t l1e = l1[L1_INDEX(virt)];
    if ((l1e & 0x3UL) == PTE_TYPE_FAULT)
        return (uint64_t)-1;
    /* Handle L1 block descriptor (1GB) */
    if ((l1e & 0x3UL) == PTE_TYPE_BLOCK) {
        /* PA[47:30] from l1e, VA[29:0] as offset */
        return (l1e & 0x0000FFFFC0000000ULL) | (virt & 0x3FFFFFFFUL);
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(l1e & PTE_PHYS_MASK);

    /* L2 → L3 */
    uint64_t l2e = l2[L2_INDEX(virt)];
    if ((l2e & 0x3UL) == PTE_TYPE_FAULT)
        return (uint64_t)-1;
    /* Handle L2 block descriptor (2MB) */
    if ((l2e & 0x3UL) == PTE_TYPE_BLOCK) {
        /* PA[47:21] from l2e, VA[20:0] as offset */
        return (l2e & 0x0000FFFFFFE00000ULL) | (virt & 0x1FFFFFUL);
    }
    uint64_t *l3 = (uint64_t *)(uintptr_t)(l2e & PTE_PHYS_MASK);

    /* L3: leaf PTE */
    uint64_t l3e = l3[L3_INDEX(virt)];
    if ((l3e & 0x3UL) != PTE_TYPE_PAGE)
        return (uint64_t)-1;

    /* PA[47:12] from PTE + VA[11:0] page offset */
    return (l3e & PTE_PHYS_MASK) | (virt & (PAGE_SIZE - 1UL));
}

/* ------------------------------------------------------------------ */
/* TLB maintenance                                                      */
/* ------------------------------------------------------------------ */

void aarch64_tlb_flush_all(void)
{
    /*
     * TLBI VMALLE1IS – Invalidate all TLB entries at EL1, inner-shareable.
     * The IS (Inner Shareable) variant ensures all CPUs in the same
     * coherency domain see the invalidation.
     *
     * DSB ISH ensures the invalidation completes before any subsequent
     * table walks.  ISB ensures the pipeline is flushed before we continue.
     */
    __asm__ volatile(
        "tlbi vmalle1is \n\t"
        "dsb  ish       \n\t"
        "isb            \n\t"
        ::: "memory"
    );
}

void aarch64_tlb_flush_page(uint64_t vaddr)
{
    /*
     * TLBI VAE1IS – Invalidate TLB entry by VA at EL1, inner-shareable.
     *
     * The operand is VA[55:12] (the address right-shifted by PAGE_SHIFT)
     * per ARMv8-A D13.8.  Bit 63:44 and ASID are ignored when AS=0
     * (8-bit ASID mode) but we pass only bits [47:12] to be safe.
     */
    uint64_t page_addr = vaddr >> PAGE_SHIFT;

    __asm__ volatile(
        "tlbi vae1is, %0 \n\t"
        "dsb  ish        \n\t"
        "isb             \n\t"
        :: "r"(page_addr) : "memory"
    );
}

/* ------------------------------------------------------------------ */
/* aarch64_mmu_enable                                                   */
/* ------------------------------------------------------------------ */
void aarch64_mmu_enable(pgd_t *kernel_pgd, pgd_t *user_pgd)
{
    uint64_t ttbr1 = (uint64_t)(uintptr_t)kernel_pgd;
    uint64_t ttbr0 = user_pgd ? (uint64_t)(uintptr_t)user_pgd : 0ULL;

    /*
     * Step 1: Load TTBR0_EL1 (user-space page tables, VA[47:0] = 0000…)
     *         and TTBR1_EL1 (kernel-space page tables, VA[47:0] = FFFF…).
     *
     * ISB after both writes ensures the PE has picked up the new base
     * addresses before we touch SCTLR_EL1.
     */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0) : "memory");
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"(ttbr1) : "memory");
    __asm__ volatile("isb");

    /*
     * Step 2: Invalidate all stale TLB entries before the MMU starts
     * walking the new tables.  Without this, the PE might cache a stale
     * walk from an earlier TTBR value.
     */
    __asm__ volatile(
        "tlbi vmalle1is \n\t"
        "dsb  ish       \n\t"
        "isb            \n\t"
        ::: "memory"
    );

    /*
     * Step 3: Enable the MMU (SCTLR_EL1.M = 1) together with data-cache
     * (C) and instruction-cache (I) enables, plus stack-pointer alignment
     * checking (SA).
     *
     * We read-modify-write SCTLR_EL1 to preserve all RES1 bits and any
     * configuration already present (e.g. WXN set by a previous stage).
     *
     * The ISB after the MSR serialises the pipeline so that all subsequent
     * instruction fetches and data accesses use the newly enabled MMU.
     */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_EL1_M;   /* MMU enable                   */
    sctlr |= SCTLR_EL1_C;   /* Data cache enable            */
    sctlr |= SCTLR_EL1_I;   /* Instruction cache enable     */
    sctlr |= SCTLR_EL1_SA;  /* SP alignment check at EL1    */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    __asm__ volatile("isb");
}

/* ------------------------------------------------------------------ */
/* aarch64_mmu_init - High-level boot-time initialisation              */
/* ------------------------------------------------------------------ */
void aarch64_mmu_init(void)
{
    /* ---- 1. Initialise the kernel PGD -------------------------------- */
    for (int i = 0; i < (int)TABLE_ENTRIES; i++)
        kernel_pgd[i] = PTE_TYPE_FAULT;

    /* ---- 2. Program MAIR_EL1 and TCR_EL1 ---------------------------- */
    aarch64_mmu_setup_mair();
    aarch64_mmu_setup_tcr();

    /* ---- 3. Identity-map first 64 MB (PA == VA) ----------------------
     *
     * This keeps the CPU from faulting on the instructions immediately
     * after MMU enable (which are still fetched using physical addresses
     * that equal the pre-higher-half virtual addresses).
     *
     * Map as kernel R/W, non-executable.
     */
    for (uint64_t pa = 0; pa < 0x04000000UL; pa += PAGE_SIZE)
        aarch64_map_page(&kernel_pgd, pa, pa, AARCH64_MAP_RW);

    /* ---- 4. Map the kernel image at its higher-half virtual address ---
     *
     * KERNEL_VIRT_BASE = 0xFFFF000000080000
     * KERNEL_PHYS_BASE = 0x0000000000080000
     *
     * Reserve 8 MB for the entire kernel image (text + rodata + data + BSS
     * + initial stack).  The virtual address for the higher-half range uses
     * the AARCH64_KERN_VA_START sentinel ORed with the physical offset so
     * that TTBR1 resolves it correctly.
     */
    for (uint64_t off = 0; off < 0x800000UL; off += PAGE_SIZE) {
        uint64_t va = AARCH64_KERN_VA_START | (KERNEL_PHYS_BASE + off);
        uint64_t pa = KERNEL_PHYS_BASE + off;
        aarch64_map_page(&kernel_pgd, va, pa, AARCH64_MAP_RW);
    }

    /* ---- 5. Map MMIO regions (QEMU virt machine) ----------------------
     *
     * All MMIO must use device-nGnRnE attributes to prevent speculative
     * access, reordering, and write-combining.  Identity-mapped (PA == VA)
     * so drivers can use the physical address directly before any higher-
     * half remapping is performed.
     *
     *  PL011 UART  : 0x09000000  4KB
     *  GICv3 GICD  : 0x08000000  64KB
     *  GICv3 GICR  : 0x080A0000  128KB (2 redistributors at 64KB each)
     */
    static const struct {
        uint64_t base;
        uint64_t size;
    } mmio_regions[] = {
        { 0x09000000UL, 0x001000UL },   /* PL011 UART0 (4KB)            */
        { 0x08000000UL, 0x010000UL },   /* GICv3 Distributor (64KB)     */
        { 0x080A0000UL, 0x020000UL },   /* GICv3 Redistributor (128KB)  */
        { 0x00000000UL, 0x000000UL },   /* sentinel – size 0 = end      */
    };

    for (int r = 0; mmio_regions[r].size != 0; r++) {
        for (uint64_t off = 0; off < mmio_regions[r].size; off += PAGE_SIZE) {
            uint64_t pa = mmio_regions[r].base + off;
            aarch64_map_page(&kernel_pgd, pa, pa, AARCH64_MAP_DEVICE);
        }
    }

    /* ---- 6. Enable the MMU ------------------------------------------- */
    /*
     * Pass NULL for the user PGD – no user-space mappings are needed at
     * this stage.  TTBR0 will be loaded with a real user PGD when the
     * first process is created.
     */
    aarch64_mmu_enable(&kernel_pgd, NULL);
}
