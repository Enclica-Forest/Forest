/*
 * Fern - RISC-V Sv39 MMU definitions
 *
 * Sv39 page table format (3-level, 39-bit VA, 4KB granule):
 *   VA[38:30] = L2 index  (root table)  → 1GB per entry
 *   VA[29:21] = L1 index               → 2MB per entry
 *   VA[20:12] = L0 index               → 4KB per entry
 *   VA[11:0]  = page offset
 *
 * Virtual addresses are sign-extended from bit 38, so the upper half
 * of the address space covers 0xFFFFFF80_00000000 – 0xFFFFFFFF_FFFFFFFF.
 *
 * Reference: RISC-V Privileged Architecture Specification v20211203
 *   Section 4.3 – Sv39
 */
#ifndef RISCV64_MMU_H
#define RISCV64_MMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Virtual address space boundaries                                     */
/* ------------------------------------------------------------------ */
#define RISCV64_USER_VA_END     0x0000003FFFFFFFFFULL  /* 256 GB */
#define RISCV64_KERN_VA_START   0xFFFFFF8000000000ULL  /* sign-ext bit 38 */

/* Kernel image placement (must match linker script) */
#define KERNEL_VIRT_BASE        0xFFFFFF8080000000ULL
#define KERNEL_PHYS_BASE        0x80000000ULL

/* QEMU virt machine memory start */
#define RISCV64_MEMORY_BASE     0x80000000ULL

/* ------------------------------------------------------------------ */
/* Page / table geometry                                                */
/* ------------------------------------------------------------------ */
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)     /* 4096 bytes            */
#define PAGE_MASK       (~(PAGE_SIZE - 1UL))

#define TABLE_ENTRIES   512                     /* 9-bit index → 2^9     */
#define TABLE_SIZE      (TABLE_ENTRIES * 8)     /* 4096 bytes            */

/* VA index extraction helpers (Sv39) */
#define L2_INDEX(va)    (((uint64_t)(va) >> 30) & 0x1FFUL)
#define L1_INDEX(va)    (((uint64_t)(va) >> 21) & 0x1FFUL)
#define L0_INDEX(va)    (((uint64_t)(va) >> 12) & 0x1FFUL)

/* ------------------------------------------------------------------ */
/* PTE flag bits (low 10 bits of each entry)                           */
/* RISC-V Privileged Spec v20211203, Section 4.3.2                    */
/* ------------------------------------------------------------------ */
#define PTE_V   (1UL << 0)     /* Valid                              */
#define PTE_R   (1UL << 1)     /* Read                               */
#define PTE_W   (1UL << 2)     /* Write                              */
#define PTE_X   (1UL << 3)     /* Execute                            */
#define PTE_U   (1UL << 4)     /* User-mode accessible               */
#define PTE_G   (1UL << 5)     /* Global (not ASID-tagged)           */
#define PTE_A   (1UL << 6)     /* Accessed – HW sets on access       */
#define PTE_D   (1UL << 7)     /* Dirty – HW sets on write           */

/* ------------------------------------------------------------------ */
/* PTE PPN fields (physical page number)                                */
/* Bits [19:10] = PPN[0], [28:20] = PPN[1], [53:30] = PPN[2]          */
/* ------------------------------------------------------------------ */
#define PTE_PPN0_SHIFT  10
#define PTE_PPN0_MASK   (0x3FFUL << PTE_PPN0_SHIFT)   /* bits 19:10  */
#define PTE_PPN1_SHIFT  20
#define PTE_PPN1_MASK   (0x1FFUL << PTE_PPN1_SHIFT)   /* bits 28:20  */
#define PTE_PPN2_SHIFT  28
#define PTE_PPN2_MASK   (0x3FFFFFFUL << PTE_PPN2_SHIFT) /* bits 53:30 */

/* RSW bits [9:8] – software use, reserved by HW */
#define PTE_RSW_SHIFT   8
#define PTE_RSW_MASK    (0x3UL << PTE_RSW_SHIFT)

/* Reserved bits that must be zero in a PTE */
#define PTE_RESERVED_MASK   0x3FC0000000000000ULL  /* bits 63:54 */

/* ------------------------------------------------------------------ */
/* PTE format helpers                                                   */
/* Extract PA from a leaf PTE: reconstruct from PPN[2]:PPN[1]:PPN[0]   */
/* ------------------------------------------------------------------ */
#define PTE_PA(pte) \
    ((((pte) >> PTE_PPN2_SHIFT) << (PAGE_SHIFT + 2*9)) | \
     (((pte) >> PTE_PPN1_SHIFT) << (PAGE_SHIFT + 1*9)) | \
     (((pte) >> PTE_PPN0_SHIFT) << PAGE_SHIFT))

#define PTE_FLAGS(pte)  ((pte) & 0x3FFUL)

/* Build a leaf PTE from a physical address + flags */
#define MK_PTE(pa, flags) \
    ( (((pa) >> (PAGE_SHIFT + 2*9)) << PTE_PPN2_SHIFT) | \
      (((pa) >> (PAGE_SHIFT + 1*9)) << PTE_PPN1_SHIFT) | \
      (((pa) >> PAGE_SHIFT) << PTE_PPN0_SHIFT) | \
      ((flags) & 0x3FFUL) )

/* ------------------------------------------------------------------ */
/* SATP register (Supervisor Address Translation and Protection)         */
/* RISC-V Privileged Spec, Section 4.1.11                               */
/* ------------------------------------------------------------------ */
/* MODE field (bits 63:60) */
#define SATP_MODE_BARE  0x0ULL
#define SATP_MODE_SV39  (8ULL << 60)

/* ASID field (bits 59:44) – 16 bits for Sv39 */
#define SATP_ASID_SHIFT 44
#define SATP_ASID_MASK  (0xFFFFUL << SATP_ASID_SHIFT)

/* PPN field (bits 43:0) – root page table physical page number */
#define SATP_PPN_SHIFT  0
#define SATP_PPN_MASK   (0xFFFFFFFFFFFUL)

/* Build a SATP value for Sv39 */
#define SATP_SV39(ppn, asid) \
    (SATP_MODE_SV39 | \
     (((uint64_t)(asid) << SATP_ASID_SHIFT) & SATP_ASID_MASK) | \
     ((ppn) & SATP_PPN_MASK))

/* ------------------------------------------------------------------ */
/* Map flags for riscv64_map_page()                                     */
/* These mirror the aarch64 convention for API compatibility.           */
/* ------------------------------------------------------------------ */
#define MAP_READ        0x01u
#define MAP_WRITE       0x02u
#define MAP_EXEC        0x04u
#define MAP_USER        0x08u
#define MAP_KERNEL      0x10u
#define MAP_DEVICE      0x20u

/* ------------------------------------------------------------------ */
/* Convenience PTE flag combinations                                     */
/* ------------------------------------------------------------------ */
#define PTE_FLAGS_KERN_R \
    (PTE_V | PTE_R)
#define PTE_FLAGS_KERN_RW \
    (PTE_V | PTE_R | PTE_W)
#define PTE_FLAGS_KERN_RX \
    (PTE_V | PTE_R | PTE_X)
#define PTE_FLAGS_KERN_RWX \
    (PTE_V | PTE_R | PTE_W | PTE_X)

#define PTE_FLAGS_USER_R \
    (PTE_V | PTE_R | PTE_U)
#define PTE_FLAGS_USER_RW \
    (PTE_V | PTE_R | PTE_W | PTE_U)
#define PTE_FLAGS_USER_RX \
    (PTE_V | PTE_R | PTE_X | PTE_U)
#define PTE_FLAGS_USER_RWX \
    (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U)

/* Device mapping: R/W, no execute, no user, global */
#define PTE_FLAGS_DEVICE \
    (PTE_V | PTE_R | PTE_W | PTE_G)

/* ------------------------------------------------------------------ */
/* Memory type constants (for future PMA/PGMA use)                      */
/* Sv39 itself does not have MAIR-style attributes; memory types are    */
/* configured via physical memory attributes (PMA) in page tables when  */
/* the N extension or Svpbmt is available. These are placeholder defs.  */
/* ------------------------------------------------------------------ */
#define MEM_TYPE_NORMAL_WB     0   /* Normal Write-Back cacheable      */
#define MEM_TYPE_DEVICE_NGNRNE 1   /* Device nGnRnE (I/O)              */

/* ------------------------------------------------------------------ */
/* Type definitions                                                     */
/* ------------------------------------------------------------------ */
typedef uint64_t pte_t;

/*
 * sv39_table_t – one Sv39 page table page (512 entries × 8 bytes = 4KB).
 */
typedef pte_t sv39_table_t[TABLE_ENTRIES];

/* Alias for root page table type (SATP points to this) */
typedef sv39_table_t sv39_pgd_t;

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * riscv64_mmu_init - High-level MMU initialisation entry point.
 *
 * Allocates root page table, builds identity map + high-half kernel map,
 * configures SATP and enables Sv39 translation.
 */
void riscv64_mmu_init(void);

/**
 * riscv64_map_page - Install a single 4KB mapping in the page table.
 *
 * @pgd:   Pointer to the root (L2) table (physical address).
 * @virt:  Virtual address (4KB aligned).
 * @phys:  Physical address (4KB aligned).
 * @flags: PTE flag bits (PTE_V | PTE_R | PTE_W | ...) or MAP_* flags.
 *
 * Intermediate tables are allocated as needed.
 * Returns 0 on success, -1 on allocation failure.
 */
int riscv64_map_page(sv39_pgd_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * riscv64_unmap_page - Remove a mapping at the given virtual address.
 *
 * @pgd:  Pointer to the root (L2) table.
 * @virt: Virtual address to unmap.
 *
 * Clears the leaf PTE. Does NOT free intermediate tables.
 * Returns the old PTE value, or 0 if not mapped.
 */
uint64_t riscv64_unmap_page(sv39_pgd_t *pgd, uint64_t virt);

/**
 * riscv64_get_phys - Walk page tables and return physical address.
 *
 * @pgd:  Pointer to the root (L2) table.
 * @virt: Virtual address to translate.
 *
 * Returns the physical address, or (uint64_t)-1 if not mapped.
 */
uint64_t riscv64_get_phys(sv39_pgd_t *pgd, uint64_t virt);

/**
 * riscv64_flush_tlb - Invalidate all TLB entries (sfence.vma, no ASID).
 */
void riscv64_flush_tlb(void);

/**
 * riscv64_flush_tlb_asid - Invalidate TLB entries matching an ASID.
 *
 * @asid: Address Space ID to flush.
 */
void riscv64_flush_tlb_asid(uint16_t asid);

/**
 * riscv64_flush_tlb_page - Invalidate TLB entry for a single page.
 *
 * @vaddr: Virtual address within the page to invalidate.
 */
void riscv64_flush_tlb_page(uint64_t vaddr);

/**
 * riscv64_set_satp - Write the SATP supervisor register.
 *
 * @satp_value: The full 64-bit SATP value to install.
 */
void riscv64_set_satp(uint64_t satp_value);

/**
 * riscv64_enable_mmu - Switch on Sv39 address translation.
 *
 * @pgd: Physical address of the root page table.
 *
 * Builds SATP, loads it, and flushes the TLB.
 */
void riscv64_enable_mmu(sv39_pgd_t *pgd);

/**
 * riscv64_get_kernel_pgd - Return a pointer to the static kernel PGD.
 */
sv39_pgd_t *riscv64_get_kernel_pgd(void);

/**
 * riscv64_alloc_page_table - Allocate one page-table page from the pool.
 *
 * Returns a pointer to a zeroed 4KB page suitable for use as an Sv39
 * page table, or NULL when the pool is exhausted.
 */
sv39_pgd_t *riscv64_alloc_page_table(void);

#endif /* RISCV64_MMU_H */
