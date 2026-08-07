/*
 * Cross-architecture VMM implementation.
 *
 * Translates the unified vmm_* API to the appropriate arch-specific
 * functions at compile time via #ifdef dispatch.
 */

#include "vmm.h"
#include "../include/memory.h"
#include "../include/screen.h"
#include "../include/string.h"

/* =========================================================================
 * Unified → arch-specific flag translation
 * =========================================================================
 * The unified flags (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
 * PAGE_EXECUTABLE) are translated to the native page-table-entry
 * bit format for each architecture.
 * ========================================================================= */

#if defined(__x86_64__)

static inline uint64_t vmm_to_x64_flags(uint32_t flags) {
    uint64_t f = 0;
    f |= PAGE64_PRESENT;
    if (flags & PAGE_WRITABLE)   f |= PAGE64_WRITABLE;
    if (flags & PAGE_USER)       f |= PAGE64_USER;
    /* x86_64 uses NX (inverted): set NX when NOT executable. */
    if (!(flags & PAGE_EXECUTABLE)) f |= PAGE64_NX;
    return f;
}

#elif defined(__aarch64__)

static inline uint64_t vmm_to_aarch64_flags(uint32_t flags) {
    uint64_t f = PTE_TYPE_PAGE | PTE_AF | PTE_SH_INNER
               | PTE_ATTRINDX(MAIR_IDX_NORMAL_CACHEABLE);
    if (flags & PAGE_USER)
        f |= PTE_AP_RW_ALL;
    else
        f |= PTE_AP_RW_EL1;
    if (flags & PAGE_EXECUTABLE)
        f &= ~(uint64_t)PTE_UXN;
    else
        f |= PTE_UXN;
    return f;
}

#elif defined(__arm__)

static inline uint32_t vmm_to_arm32_flags(uint32_t flags) {
    uint32_t f = ARM_PAGE_MEM_NORMAL_WB_WA;
    if (flags & PAGE_WRITABLE)
        f |= ARM_PAGE_AP_FULL_ACCESS;
    else
        f |= ARM_PAGE_AP_KERNEL_RW;
    if (flags & PAGE_USER)
        f |= ARM_PAGE_AP_FULL_ACCESS;
    if (!(flags & PAGE_EXECUTABLE))
        f |= ARM_PAGE_XN;
    return f;
}

#elif defined(__riscv) && (__riscv_xlen == 64)

static inline uint64_t vmm_to_riscv64_flags(uint32_t flags) {
    uint64_t f = PTE_V | PTE_A | PTE_D;
    f |= PTE_R;
    if (flags & PAGE_WRITABLE)  f |= PTE_W;
    if (flags & PAGE_EXECUTABLE) f |= PTE_X;
    if (flags & PAGE_USER)      f |= PTE_U;
    return f;
}

#endif

/* =========================================================================
 * Unified VMM API — arch-dispatched implementations
 * ========================================================================= */

memory_result_t vmm_map_page(page_directory_t* dir, uint32_t va,
                             uint32_t pa, uint32_t flags)
{
    if (!dir) return MEMORY_ERROR_NULL_PTR;

#if defined(__x86_64__)
    uint64_t f = vmm_to_x64_flags(flags);
    int rc = x64_map_page((pml4_t*)dir, (uint64_t)va, (uint64_t)pa, f);
    return (rc == MEMORY_OK) ? MEMORY_OK : (memory_result_t)rc;

#elif defined(__aarch64__)
    uint64_t f = vmm_to_aarch64_flags(flags);
    int rc = aarch64_map_page((pgd_t*)dir, (uint64_t)va, (uint64_t)pa, f);
    return (rc == 0) ? MEMORY_OK : MEMORY_ERROR_OUT_OF_MEMORY;

#elif defined(__arm__)
    uint32_t f = vmm_to_arm32_flags(flags);
    arm_map_page((arm_l1_table_t*)dir, va, pa, f);
    return MEMORY_OK;

#elif defined(__riscv) && (__riscv_xlen == 64)
    uint64_t f = vmm_to_riscv64_flags(flags);
    int rc = riscv64_map_page((sv39_pgd_t*)dir, (uint64_t)va, (uint64_t)pa, f);
    return (rc == 0) ? MEMORY_OK : MEMORY_ERROR_OUT_OF_MEMORY;

#else
    (void)dir; (void)va; (void)pa; (void)flags;
    return MEMORY_ERROR_NOT_INITIALIZED;
#endif
}

memory_result_t vmm_unmap_page(page_directory_t* dir, uint32_t va)
{
    if (!dir) return MEMORY_ERROR_NULL_PTR;

#if defined(__x86_64__)
    int rc = x64_unmap_page((pml4_t*)dir, (uint64_t)va);
    return (rc == MEMORY_OK) ? MEMORY_OK : (memory_result_t)rc;

#elif defined(__aarch64__)
    /* AArch64 MMU layer does not expose an unmap; write a zero entry. */
    pgd_t* pgd = (pgd_t*)dir;
    uint64_t v = (uint64_t)va;
    uint64_t idx0 = L0_INDEX(v);
    uint64_t idx1 = L1_INDEX(v);
    uint64_t idx2 = L2_INDEX(v);
    uint64_t idx3 = L3_INDEX(v);
    pte_t* tbl = (pte_t*)pgd;
    if (!(tbl[idx0] & PTE_VALID)) return MEMORY_ERROR_NOT_MAPPED;
    tbl = (pte_t*)(tbl[idx0] & PTE_PHYS_MASK);
    if (!(tbl[idx1] & PTE_VALID)) return MEMORY_ERROR_NOT_MAPPED;
    if ((tbl[idx1] & 0x3) == PTE_TYPE_BLOCK) { tbl[idx1] = 0; return MEMORY_OK; }
    tbl = (pte_t*)(tbl[idx1] & PTE_PHYS_MASK);
    if (!(tbl[idx2] & PTE_VALID)) return MEMORY_ERROR_NOT_MAPPED;
    if ((tbl[idx2] & 0x3) == PTE_TYPE_BLOCK) { tbl[idx2] = 0; return MEMORY_OK; }
    tbl = (pte_t*)(tbl[idx2] & PTE_PHYS_MASK);
    if (!(tbl[idx3] & PTE_VALID)) return MEMORY_ERROR_NOT_MAPPED;
    tbl[idx3] = 0;
    aarch64_tlb_flush_page(v);
    return MEMORY_OK;

#elif defined(__arm__)
    /* ARM32: zero the L2 small-page entry for 4KB pages. */
    arm_l1_table_t* l1 = (arm_l1_table_t*)dir;
    uint32_t l1_idx = (va >> 20) & 0xFFF;
    uint32_t l1_entry = (*l1)[l1_idx];
    if ((l1_entry & 0x3) != ARM_L1_TYPE_PAGE)
        return MEMORY_ERROR_NOT_MAPPED;
    arm_l2_table_t* l2 = (arm_l2_table_t*)(l1_entry & 0xFFFFFC00);
    uint32_t l2_idx = (va >> 12) & 0xFF;
    if (!((*l2)[l2_idx] & 0x2))
        return MEMORY_ERROR_NOT_MAPPED;
    (*l2)[l2_idx] = 0;
    arm_flush_tlb_page(va);
    return MEMORY_OK;

#elif defined(__riscv) && (__riscv_xlen == 64)
    uint64_t old = riscv64_unmap_page((sv39_pgd_t*)dir, (uint64_t)va);
    return (old != 0) ? MEMORY_OK : MEMORY_ERROR_NOT_MAPPED;

#else
    (void)dir; (void)va;
    return MEMORY_ERROR_NOT_INITIALIZED;
#endif
}

uint32_t vmm_get_physical_addr(page_directory_t* dir, uint32_t va)
{
    if (!dir) return 0;

#if defined(__x86_64__)
    return (uint32_t)x64_get_phys((pml4_t*)dir, (uint64_t)va);

#elif defined(__aarch64__)
    uint64_t pa = aarch64_get_phys((pgd_t*)dir, (uint64_t)va);
    return (pa == (uint64_t)-1) ? 0 : (uint32_t)pa;

#elif defined(__arm__)
    return arm_get_physical_addr((arm_l1_table_t*)dir, va);

#elif defined(__riscv) && (__riscv_xlen == 64)
    uint64_t pa = riscv64_get_phys((sv39_pgd_t*)dir, (uint64_t)va);
    return (pa == (uint64_t)-1) ? 0 : (uint32_t)pa;

#else
    (void)dir; (void)va;
    return 0;
#endif
}

void vmm_switch_page_directory(page_directory_t* dir)
{
    if (!dir) return;

#if defined(__x86_64__)
    x64_load_pml4((pml4_t*)dir);

#elif defined(__aarch64__)
    /* AArch64 uses TTBR1 for kernel; load as kernel PGD. */
    aarch64_mmu_enable((pgd_t*)dir, NULL);

#elif defined(__arm__)
    arm_write_ttbr0((uint32_t)(uintptr_t)dir);
    arm_flush_tlb_all();

#elif defined(__riscv) && (__riscv_xlen == 64)
    riscv64_enable_mmu((sv39_pgd_t*)dir);

#endif
}

page_directory_t* vmm_create_address_space(void)
{
#if defined(__x86_64__)
    return (page_directory_t*)pml4_create();

#elif defined(__aarch64__)
    return (page_directory_t*)aarch64_mmu_create_pgd();

#elif defined(__arm__)
    /* ARM32 L1 table: statically allocated from pool (see mmu.c). */
    /* For a dynamic address space, allocate a 16KB-aligned frame. */
    phys_addr_t frame = pmm_alloc_frames(4); /* 16KB = 4 pages */
    if (!frame) return NULL;
    memset((void*)(uintptr_t)frame, 0, 16384);
    return (page_directory_t*)(uintptr_t)frame;

#elif defined(__riscv) && (__riscv_xlen == 64)
    return (page_directory_t*)riscv64_alloc_page_table();

#else
    return NULL;
#endif
}

void vmm_destroy_address_space(page_directory_t* dir)
{
    if (!dir) return;

    /* Free user-space page tables and frames.  Kernel mappings are shared
     * and must NOT be freed.  A full recursive walk is architecture-specific;
     * for now we free the root frame only. */
#if defined(__x86_64__)
    /* TODO: recursively free user page tables (entries 0–255). */
    pmm_free_frame((phys_addr_t)(uintptr_t)dir);

#elif defined(__aarch64__)
    /* TODO: recursively free user page tables (TTBR0 range). */
    pmm_free_frame((phys_addr_t)(uintptr_t)dir);

#elif defined(__arm__)
    /* TODO: recursively free L2 tables for user-space L1 entries. */
    pmm_free_frames((phys_addr_t)(uintptr_t)dir, 4);

#elif defined(__riscv) && (__riscv_xlen == 64)
    /* TODO: recursively free user page tables (lower half). */
    pmm_free_frame((phys_addr_t)(uintptr_t)dir);

#endif
}
