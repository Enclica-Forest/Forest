#ifndef ARCH_VMM_H
#define ARCH_VMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../include/memory.h"

/*
 * Cross-architecture Virtual Memory Manager interface.
 *
 * Provides a unified API for page table manipulation that dispatches to
 * the appropriate architecture-specific implementation at compile time.
 *
 * page_directory_t is defined as a void* here so the unified API can use
 * a single pointer type.  Each arch casts to its native root-table type:
 *   x86_64   : pml4_t*  (uint64_t[512])
 *   AArch64  : pgd_t*   (pte_t[512])
 *   ARM32    : arm_l1_table_t* (uint32_t[4096])
 *   RISC-V   : sv39_pgd_t* (sv39_table_t)
 *
 * NOTE: memory.h defines page_directory_t as page_entry_t[1024] for the
 * legacy x86 32-bit code path.  That definition is only active when
 * neither of the new arch defines below match.
 */

/* ---------- page_directory_t per-architecture definition ---------- */

#if defined(__x86_64__)
    #ifndef page_directory_t_DEFINED
    #define page_directory_t_DEFINED
    typedef void page_directory_t;
    #endif
#elif defined(__aarch64__)
    #ifndef page_directory_t_DEFINED
    #define page_directory_t_DEFINED
    typedef void page_directory_t;
    #endif
#elif defined(__arm__)
    #ifndef page_directory_t_DEFINED
    #define page_directory_t_DEFINED
    typedef void page_directory_t;
    #endif
#elif defined(__riscv) && (__riscv_xlen == 64)
    #ifndef page_directory_t_DEFINED
    #define page_directory_t_DEFINED
    typedef void page_directory_t;
    #endif
#endif
/* For x86 32-bit, page_directory_t is already defined by memory.h. */

/* ---------- Unified page flags (arch-independent) ---------- */

#ifndef PAGE_PRESENT
#define PAGE_PRESENT       0x001
#endif
#ifndef PAGE_WRITABLE
#define PAGE_WRITABLE      0x002
#endif
#ifndef PAGE_USER
#define PAGE_USER          0x004
#endif
#define PAGE_EXECUTABLE    0x010

/* ---------- Unified VMM API ---------- */

/**
 * vmm_map_page - Map a single 4KB page.
 *
 * @dir:   Pointer to the root page table (page_directory_t*).
 * @va:    Virtual address to map (will be page-aligned internally).
 * @pa:    Physical address to map (will be page-aligned internally).
 * @flags: Combination of PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_EXECUTABLE.
 *
 * Returns MEMORY_OK on success, or a MEMORY_ERROR_* code.
 */
memory_result_t vmm_map_page(page_directory_t* dir, uint32_t va, uint32_t pa, uint32_t flags);

/**
 * vmm_unmap_page - Remove a page mapping at the given virtual address.
 *
 * @dir: Pointer to the root page table.
 * @va:  Virtual address to unmap.
 *
 * Returns MEMORY_OK on success, or MEMORY_ERROR_NOT_MAPPED.
 */
memory_result_t vmm_unmap_page(page_directory_t* dir, uint32_t va);

/**
 * vmm_get_physical_addr - Translate a virtual address to physical.
 *
 * @dir: Pointer to the root page table.
 * @va:  Virtual address to translate.
 *
 * Returns the physical address, or 0 if the page is not mapped.
 */
uint32_t vmm_get_physical_addr(page_directory_t* dir, uint32_t va);

/**
 * vmm_switch_page_directory - Load a new page directory as the active one.
 *
 * @dir: Pointer to the root page table to activate.
 */
void vmm_switch_page_directory(page_directory_t* dir);

/**
 * vmm_create_address_space - Allocate and initialise a new empty address space.
 *
 * The returned directory has no user mappings; kernel (higher-half) entries
 * are inherited from the kernel address space where applicable.
 *
 * Returns a pointer to the new root page table, or NULL on failure.
 */
page_directory_t* vmm_create_address_space(void);

/**
 * vmm_destroy_address_space - Free all user-space pages and page tables.
 *
 * @dir: Pointer to the root page table to destroy.
 *       Must NOT be the kernel address space.
 */
void vmm_destroy_address_space(page_directory_t* dir);

/* ---------- Architecture-specific function declarations ---------- */

/*
 * These are implemented in the per-arch source files (paging64.c,
 * aarch64/mmu.c, arm32/mmu.c, riscv64/mmu.c) and called by vmm.c.
 */

#if defined(__x86_64__)

/* x86_64 uses pml4_t* (uint64_t[512]) as the root table type. */
#include "../include/paging64.h"

int x64_map_page(pml4_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
int x64_unmap_page(pml4_t* pml4, uint64_t virt);
uint64_t x64_get_phys(pml4_t* pml4, uint64_t virt);
pml4_t* pml4_create(void);
void x64_load_pml4(pml4_t* pml4);

#elif defined(__aarch64__)

#include "aarch64/mmu.h"

int aarch64_map_page(pgd_t* pgd, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t aarch64_get_phys(pgd_t* pgd, uint64_t virt);
pgd_t* aarch64_mmu_create_pgd(void);
void aarch64_mmu_enable(pgd_t* kernel_pgd, pgd_t* user_pgd);

#elif defined(__arm__)

#include "arm32/mmu.h"

void arm_map_page(arm_l1_table_t* ttb, uint32_t virt, uint32_t phys, uint32_t flags);
uint32_t arm_get_physical_addr(arm_l1_table_t* ttb, uint32_t vaddr);

#elif defined(__riscv) && (__riscv_xlen == 64)

#include "riscv64/mmu.h"

int riscv64_map_page(sv39_pgd_t* pgd, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t riscv64_unmap_page(sv39_pgd_t* pgd, uint64_t virt);
uint64_t riscv64_get_phys(sv39_pgd_t* pgd, uint64_t virt);
sv39_pgd_t* riscv64_alloc_page_table(void);
void riscv64_enable_mmu(sv39_pgd_t* pgd);

#endif

#endif /* ARCH_VMM_H */
