/*
 * Fern - RISC-V Sv39 MMU implementation
 *
 * 3-level page table walk (L2 root → L1 → L0) using the RISC-V Sv39
 * virtual memory scheme with 4KB pages and 39-bit virtual addresses.
 *
 * Design notes
 * ------------
 * Physical memory for page-table pages is drawn from a static pool
 * during early boot, before the kernel heap is live.  Every pool page is
 * PAGE_SIZE-aligned and zeroed on allocation so all PTEs are invalid
 * (V=0).
 *
 * Reference: RISC-V Privileged Architecture Specification v20211203
 *   4.3  Sv39 – Page-Based 39-bit Virtual-Addressing
 *   4.1  Supervisor Address Translation and Protection (satp)
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
 * The page is zeroed (all PTEs = 0, i.e. V=0) before being returned.
 * Returns NULL when the pool is exhausted.
 */
static uint64_t *alloc_table(void)
{
    if (pt_pool_next >= PT_POOL_PAGES)
        return NULL;

    uint64_t *tbl = pt_pool[pt_pool_next++];

    for (int i = 0; i < (int)TABLE_ENTRIES; i++)
        tbl[i] = 0;

    return tbl;
}

/* ------------------------------------------------------------------ */
/* Static kernel PGD (L2 root table)                                    */
/* ------------------------------------------------------------------ */
static sv39_pgd_t __attribute__((aligned(PAGE_SIZE))) kernel_pgd;

sv39_pgd_t *riscv64_get_kernel_pgd(void)
{
    return &kernel_pgd;
}

sv39_pgd_t *riscv64_alloc_page_table(void)
{
    return (sv39_pgd_t *)alloc_table();
}

/* ------------------------------------------------------------------ */
/* Translate MAP_* flags to PTE bits                                    */
/* ------------------------------------------------------------------ */
static uint64_t map_flags_to_pte(uint64_t flags)
{
    uint64_t pte = PTE_V | PTE_A | PTE_D;

    if (flags & MAP_DEVICE) {
        /* Device memory: R/W, no execute, global, no user */
        pte |= PTE_R | PTE_W | PTE_G;
    } else {
        if (flags & MAP_READ)
            pte |= PTE_R;
        if (flags & MAP_WRITE)
            pte |= PTE_W;
        if (flags & MAP_EXEC)
            pte |= PTE_X;
        if (flags & MAP_USER)
            pte |= PTE_U;
        /* Kernel mappings are global */
        if (flags & MAP_KERNEL)
            pte |= PTE_G;
    }

    return pte;
}

/* ------------------------------------------------------------------ */
/* Internal: get or create the next-level table                         */
/* ------------------------------------------------------------------ */

/*
 * get_or_create_next_table - Given a PTE, return its child table or
 * allocate one if the PTE is invalid.
 *
 * @entry: pointer to the L2 or L1 descriptor.
 *
 * Returns child table pointer, or NULL on allocation failure.
 */
static uint64_t *get_or_create_next_table(uint64_t *entry)
{
    if (*entry & PTE_V) {
        /* Valid entry – extract physical page address from PPN fields */
        uint64_t pa = PTE_PA(*entry);
        return (uint64_t *)(uintptr_t)pa;
    }

    /* Allocate a new table */
    uint64_t *child = alloc_table();
    if (!child)
        return NULL;

    uint64_t child_pa = (uint64_t)(uintptr_t)child;
    *entry = MK_PTE(child_pa, PTE_V);

    return child;
}

/* ------------------------------------------------------------------ */
/* riscv64_map_page                                                     */
/* ------------------------------------------------------------------ */
int riscv64_map_page(sv39_pgd_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t pte_attrs;

    /*
     * If the caller already passed raw PTE bits (high bits set), use
     * them directly.  Otherwise translate from MAP_* logical flags.
     */
    if (flags & ~0x3FFULL) {
        pte_attrs = flags;
    } else {
        pte_attrs = map_flags_to_pte(flags);
    }

    /* Ensure leaf PTE has A+D set to avoid page faults on first access */
    pte_attrs |= PTE_A | PTE_D;

    /* --- L2 (root): VA[38:30] --- */
    uint64_t *l2 = (uint64_t *)pgd;
    uint64_t *l1 = get_or_create_next_table(&l2[L2_INDEX(virt)]);
    if (!l1)
        return -1;

    /* --- L1: VA[29:21] --- */
    uint64_t *l0 = get_or_create_next_table(&l1[L1_INDEX(virt)]);
    if (!l0)
        return -1;

    /* --- L0 (leaf): VA[20:12] --- */
    l0[L0_INDEX(virt)] = MK_PTE(phys, pte_attrs);

    return 0;
}

/* ------------------------------------------------------------------ */
/* riscv64_unmap_page                                                   */
/* ------------------------------------------------------------------ */
uint64_t riscv64_unmap_page(sv39_pgd_t *pgd, uint64_t virt)
{
    uint64_t *l2 = (uint64_t *)pgd;

    /* L2 → L1 */
    if (!(l2[L2_INDEX(virt)] & PTE_V))
        return 0;
    uint64_t *l1 = (uint64_t *)(uintptr_t)PTE_PA(l2[L2_INDEX(virt)]);

    /* L1 → L0 */
    if (!(l1[L1_INDEX(virt)] & PTE_V))
        return 0;
    uint64_t *l0 = (uint64_t *)(uintptr_t)PTE_PA(l1[L1_INDEX(virt)]);

    /* L0: clear the leaf PTE */
    uint64_t old = l0[L0_INDEX(virt)];
    l0[L0_INDEX(virt)] = 0;

    return old;
}

/* ------------------------------------------------------------------ */
/* riscv64_get_phys                                                     */
/* ------------------------------------------------------------------ */
uint64_t riscv64_get_phys(sv39_pgd_t *pgd, uint64_t virt)
{
    uint64_t *l2 = (uint64_t *)pgd;

    /* L2 → L1 */
    uint64_t l2e = l2[L2_INDEX(virt)];
    if (!(l2e & PTE_V))
        return (uint64_t)-1;
    uint64_t *l1 = (uint64_t *)(uintptr_t)PTE_PA(l2e);

    /* L1 → L0 */
    uint64_t l1e = l1[L1_INDEX(virt)];
    if (!(l1e & PTE_V))
        return (uint64_t)-1;
    uint64_t *l0 = (uint64_t *)(uintptr_t)PTE_PA(l1e);

    /* L0: leaf PTE */
    uint64_t l0e = l0[L0_INDEX(virt)];
    if (!(l0e & PTE_V))
        return (uint64_t)-1;

    /* Reconstruct PA from PPN fields + page offset */
    return PTE_PA(l0e) | (virt & (PAGE_SIZE - 1UL));
}

/* ------------------------------------------------------------------ */
/* TLB maintenance                                                      */
/* ------------------------------------------------------------------ */

void riscv64_flush_tlb(void)
{
    /*
     * sfence.vma with no operands invalidates all TLB entries.
     * The trailing fence ensures subsequent instruction fetches see
     * the updated translations.
     */
    __asm__ volatile("sfence.vma \n\t fence.i \n\t" ::: "memory");
}

void riscv64_flush_tlb_asid(uint16_t asid)
{
    __asm__ volatile(
        "sfence.vma x0, %0 \n\t"
        "fence.i \n\t"
        :: "r"((uint64_t)asid << SATP_ASID_SHIFT) : "memory"
    );
}

void riscv64_flush_tlb_page(uint64_t vaddr)
{
    __asm__ volatile(
        "sfence.vma %0, x0 \n\t"
        "fence.i \n\t"
        :: "r"(vaddr) : "memory"
    );
}

/* ------------------------------------------------------------------ */
/* riscv64_set_satp                                                     */
/* ------------------------------------------------------------------ */
void riscv64_set_satp(uint64_t satp_value)
{
    /*
     * Write the SATP CSR, then flush the TLB to ensure old translations
     * are discarded before the new page table takes effect.
     */
    __asm__ volatile("csrw satp, %0" :: "r"(satp_value) : "memory");
    __asm__ volatile("sfence.vma \n\t fence.i \n\t" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* riscv64_enable_mmu                                                   */
/* ------------------------------------------------------------------ */
void riscv64_enable_mmu(sv39_pgd_t *pgd)
{
    /*
     * Build SATP for Sv39:
     *   MODE = 8 (Sv39)
     *   ASID = 0 (kernel context)
     *   PPN  = physical page number of the root table
     */
    uint64_t root_pa = (uint64_t)(uintptr_t)pgd;
    uint64_t ppn = root_pa >> PAGE_SHIFT;
    uint64_t satp = SATP_SV39(ppn, 0);

    riscv64_set_satp(satp);
}

/* ------------------------------------------------------------------ */
/* riscv64_mmu_init - High-level boot-time initialisation              */
/* ------------------------------------------------------------------ */
void riscv64_mmu_init(void)
{
    /* ---- 1. Zero the kernel PGD ------------------------------------ */
    for (int i = 0; i < (int)TABLE_ENTRIES; i++)
        kernel_pgd[i] = 0;

    /* ---- 2. Identity-map the first 1GB (0x80000000 – 0x84000000) ----
     *
     * On RISC-V QEMU virt, DRAM starts at 0x80000000.  We identity-map
     * this first gigabyte so that the instruction stream continues to
     * work after the MMU is enabled (the PC still holds physical
     * addresses at that point).
     *
     * For a 1GB identity region we can place a single L2 superpage
     * entry, but since our riscv64_map_page() always creates 4KB
     * leaf entries, we map page-by-page.  This is simple and correct;
     * the superpage optimization can come later.
     *
     * Map as kernel R/W (not executable) – code runs from the high-half
     * after the trampoline.
     */
    for (uint64_t pa = RISCV64_MEMORY_BASE; pa < 0x84000000ULL; pa += PAGE_SIZE)
        riscv64_map_page(&kernel_pgd, pa, pa, PTE_FLAGS_KERN_RW);

    /* ---- 3. Map the kernel image at its higher-half virtual address ----
     *
     * KERNEL_VIRT_BASE = 0xFFFFFF8080000000
     * KERNEL_PHYS_BASE = 0x80000000
     *
     * Reserve 16 MB for the kernel image (text + rodata + data + BSS
     * + initial stack).  Kernel text pages are mapped R/X; data is R/W.
     * For simplicity at boot, map everything RWX initially.
     */
    for (uint64_t off = 0; off < 0x1000000ULL; off += PAGE_SIZE) {
        uint64_t va = KERNEL_VIRT_BASE + off;
        uint64_t pa = KERNEL_PHYS_BASE + off;
        riscv64_map_page(&kernel_pgd, va, pa, PTE_FLAGS_KERN_RWX);
    }

    /* ---- 4. Map MMIO regions (QEMU virt machine) --------------------
     *
     * Device memory must not be cached or reordered.  Since Sv39 does
     * not have MAIR-style memory attributes, these are mapped as plain
     * R/W; the physical memory attributes are handled by the platform
     * (PMA registers or fixed by firmware/FDT).
     */
    static const struct {
        uint64_t base;
        uint64_t size;
    } mmio_regions[] = {
        { 0x10000000ULL, 0x00001000ULL },  /* CLINT (timer/IPI)       */
        { 0x0C000000ULL, 0x40000000ULL },  /* PCIe ECAM               */
        { 0x10000000ULL, 0x00010000ULL },  /* UART0 (NS16550A)        */
        { 0x00000000ULL, 0x00000000ULL },  /* sentinel                */
    };

    for (int r = 0; mmio_regions[r].size != 0; r++) {
        for (uint64_t off = 0; off < mmio_regions[r].size; off += PAGE_SIZE) {
            uint64_t pa = mmio_regions[r].base + off;
            riscv64_map_page(&kernel_pgd, pa, pa, PTE_FLAGS_DEVICE);
        }
    }

    /* ---- 5. Enable Sv39 -------------------------------------------- */
    riscv64_enable_mmu(&kernel_pgd);
}
