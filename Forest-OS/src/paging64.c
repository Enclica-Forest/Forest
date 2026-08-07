/**
 * @file paging64.c
 * @brief 64-bit (Long Mode) Paging Implementation
 * 
 * Implements 4-level paging for x86_64:
 * - PML4 (Page Map Level 4) - 512 entries, each covers 512GB
 * - PDPT (Page Directory Pointer Table) - 512 entries, each covers 1GB
 * - PD (Page Directory) - 512 entries, each covers 2MB
 * - PT (Page Table) - 512 entries, each covers 4KB
 * 
 * Supports:
 * - 4KB, 2MB, and 1GB page sizes
 * - Recursive page table mapping
 * - NX (No Execute) bit
 * - Global pages
 * 
 * Virtual Address Format (48-bit):
 * | 63-48 (sign extend) | 47-39 (PML4) | 38-30 (PDPT) | 29-21 (PD) | 20-12 (PT) | 11-0 (offset) |
 */

#include "include/paging64.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/tlb.h"
#include "include/panic.h"

#ifdef __x86_64__

// ============================================================================
// CONSTANTS
// ============================================================================

#define PAGE_SIZE_4K        0x1000
#define PAGE_SIZE_2M        0x200000
#define PAGE_SIZE_1G        0x40000000

#define PML4_SHIFT          39
#define PDPT_SHIFT          30
#define PD_SHIFT            21
#define PT_SHIFT            12

#define ENTRIES_PER_TABLE   512
#define PAGE_MASK_4K        0xFFF
#define PAGE_MASK_2M        0x1FFFFF
#define PAGE_MASK_1G        0x3FFFFFFF

// Recursive mapping index (last entry of PML4)
#define RECURSIVE_INDEX     510

// Page table entry flags
#define PTE_PRESENT         (1ULL << 0)
#define PTE_WRITABLE        (1ULL << 1)
#define PTE_USER            (1ULL << 2)
#define PTE_WRITE_THROUGH   (1ULL << 3)
#define PTE_CACHE_DISABLE   (1ULL << 4)
#define PTE_ACCESSED        (1ULL << 5)
#define PTE_DIRTY           (1ULL << 6)
#define PTE_HUGE            (1ULL << 7)  // PS bit for 2MB/1GB pages
#define PTE_GLOBAL          (1ULL << 8)
#define PTE_NO_EXECUTE      (1ULL << 63)

// Address mask for different levels
#define ADDR_MASK_4K        0x000FFFFFFFFFF000ULL
#define ADDR_MASK_2M        0x000FFFFFFFE00000ULL
#define ADDR_MASK_1G        0x000FFFFFC0000000ULL

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief 64-bit page table entry
 */
typedef uint64_t pte64_t;

/**
 * @brief Page table (any level)
 */
typedef pte64_t page_table64_t[ENTRIES_PER_TABLE];

/**
 * @brief Paging64 state
 */
static struct {
    bool initialized;
    uint64_t* pml4;                 // Physical address of PML4 (or PML5 for LA57)
    uint64_t* kernel_pml4;          // Kernel PML4
    bool nx_supported;              // NX bit supported
    bool gigabyte_pages;            // 1GB page support
    bool la57_supported;            // 5-level paging support
    bool la57_active;               // Currently using 5-level paging
    bool pcid_supported;            // Process Context ID support
    uint32_t phys_addr_bits;        // Physical address bits
    uint32_t virt_addr_bits;        // Virtual address bits
} paging64_state = { .initialized = false };

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Extract PML4 index from virtual address
 */
static inline uint64_t pml4_index(uint64_t vaddr) {
    return (vaddr >> PML4_SHIFT) & 0x1FF;
}

/**
 * @brief Extract PDPT index from virtual address
 */
static inline uint64_t pdpt_index(uint64_t vaddr) {
    return (vaddr >> PDPT_SHIFT) & 0x1FF;
}

/**
 * @brief Extract PD index from virtual address
 */
static inline uint64_t pd_index(uint64_t vaddr) {
    return (vaddr >> PD_SHIFT) & 0x1FF;
}

/**
 * @brief Extract PT index from virtual address
 */
static inline uint64_t pt_index(uint64_t vaddr) {
    return (vaddr >> PT_SHIFT) & 0x1FF;
}

/**
 * @brief Get physical address from PTE
 */
static inline uint64_t pte_to_phys(pte64_t pte) {
    return pte & ADDR_MASK_4K;
}

/**
 * @brief Create PTE from physical address and flags
 */
static inline pte64_t make_pte(uint64_t phys, uint64_t flags) {
    return (phys & ADDR_MASK_4K) | flags;
}

/**
 * @brief Check if PTE is present
 */
static inline bool pte_present(pte64_t pte) {
    return (pte & PTE_PRESENT) != 0;
}

/**
 * @brief Check if PTE is a huge page
 */
static inline bool pte_huge(pte64_t pte) {
    return (pte & PTE_HUGE) != 0;
}

// ============================================================================
// RECURSIVE MAPPING
// ============================================================================

/**
 * @brief Calculate recursive mapping address for PML4
 * 
 * With recursive mapping at index 510:
 * - PML4 is at: 0xFFFFFF7FBFDFE000
 * - PDPT[i] is at: 0xFFFFFF7FBFC00000 + i * 0x1000
 * - PD[i][j] is at: 0xFFFFFF7F80000000 + i * 0x200000 + j * 0x1000
 * - PT[i][j][k] is at: 0xFFFFFF0000000000 + i * 0x40000000 + j * 0x200000 + k * 0x1000
 */

// Base addresses for recursive mapping (sign-extended)
#define RECURSIVE_PML4_BASE     0xFFFFFF7FBFDFE000ULL
#define RECURSIVE_PDPT_BASE     0xFFFFFF7FBFC00000ULL
#define RECURSIVE_PD_BASE       0xFFFFFF7F80000000ULL
#define RECURSIVE_PT_BASE       0xFFFFFF0000000000ULL

/**
 * @brief Get pointer to PML4 via recursive mapping
 */
static inline pte64_t* get_pml4_ptr(void) {
    return (pte64_t*)RECURSIVE_PML4_BASE;
}

/**
 * @brief Get pointer to PDPT entry via recursive mapping
 */
static inline pte64_t* get_pdpt_ptr(uint64_t pml4_idx) {
    return (pte64_t*)(RECURSIVE_PDPT_BASE + pml4_idx * PAGE_SIZE_4K);
}

/**
 * @brief Get pointer to PD entry via recursive mapping
 */
static inline pte64_t* get_pd_ptr(uint64_t pml4_idx, uint64_t pdpt_idx) {
    return (pte64_t*)(RECURSIVE_PD_BASE + 
                      pml4_idx * PAGE_SIZE_1G / ENTRIES_PER_TABLE +
                      pdpt_idx * PAGE_SIZE_4K);
}

/**
 * @brief Get pointer to PT entry via recursive mapping
 */
static inline pte64_t* get_pt_ptr(uint64_t pml4_idx, uint64_t pdpt_idx, uint64_t pd_idx) {
    return (pte64_t*)(RECURSIVE_PT_BASE +
                      pml4_idx * PAGE_SIZE_1G +
                      pdpt_idx * PAGE_SIZE_2M +
                      pd_idx * PAGE_SIZE_4K);
}

// ============================================================================
// PAGE TABLE MANAGEMENT
// ============================================================================

/**
 * @brief Allocate and zero a page table
 */
static uint64_t alloc_page_table(void) {
    uint64_t frame = pmm_alloc_frame();
    if (frame == 0) {
        return 0;
    }
    
    // Zero the table (before paging setup, direct access)
    memset((void*)frame, 0, PAGE_SIZE_4K);
    
    return frame;
}

/**
 * @brief Ensure page table exists at given level
 */
static pte64_t* ensure_table_exists(pte64_t* parent, uint64_t index, uint64_t flags) {
    pte64_t* entry = &parent[index];
    
    if (!pte_present(*entry)) {
        uint64_t table = alloc_page_table();
        if (table == 0) {
            return NULL;
        }
        *entry = make_pte(table, flags | PTE_PRESENT);
    }
    
    return (pte64_t*)pte_to_phys(*entry);
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Check for CPU features
 */
static void check_cpu_features(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_ext;
    
    // Check for NX support (CPUID.80000001H:EDX[20])
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) 
                     : "a"(0x80000001));
    paging64_state.nx_supported = (edx & (1 << 20)) != 0;
    
    // Check for 1GB page support (CPUID.80000001H:EDX[26])
    paging64_state.gigabyte_pages = (edx & (1 << 26)) != 0;
    
    // Check for LA57 (5-level paging) support (CPUID.07H.0:ECX[16])
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
    paging64_state.la57_supported = (ecx & (1 << 16)) != 0;
    
    // Check for PCID support (CPUID.01H:ECX[17])
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    paging64_state.pcid_supported = (ecx & (1 << 17)) != 0;
    
    // Get address size info (CPUID.80000008H)
    __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx", "ecx", "edx");
    if (max_ext >= 0x80000008) {
        __asm__ volatile("cpuid" : "=a"(eax) : "a"(0x80000008) : "ebx", "ecx", "edx");
        paging64_state.phys_addr_bits = eax & 0xFF;
        paging64_state.virt_addr_bits = (eax >> 8) & 0xFF;
    } else {
        paging64_state.phys_addr_bits = 36;
        paging64_state.virt_addr_bits = 48;
    }
    
    print("[PAGING64] CPU paging features:\n");
    print("  NX bit: ");
    print(paging64_state.nx_supported ? "yes" : "no");
    print("\n");
    print("  1GB pages: ");
    print(paging64_state.gigabyte_pages ? "yes" : "no");
    print("\n");
    print("  5-level (LA57): ");
    print(paging64_state.la57_supported ? "yes" : "no");
    print("\n");
    print("  PCID: ");
    print(paging64_state.pcid_supported ? "yes" : "no");
    print("\n");
    print("  Physical bits: ");
    print_dec(paging64_state.phys_addr_bits);
    print("\n");
    print("  Virtual bits: ");
    print_dec(paging64_state.virt_addr_bits);
    print("\n");
}

/**
 * @brief Initialize 64-bit paging
 */
memory_result_t paging64_init(void) {
    if (paging64_state.initialized) {
        return MEMORY_OK;
    }
    
    print("[PAGING64] Initializing 64-bit paging...\n");
    
    // Check CPU features
    check_cpu_features();
    
    // Allocate PML4
    paging64_state.pml4 = (uint64_t*)alloc_page_table();
    if (!paging64_state.pml4) {
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }
    
    paging64_state.kernel_pml4 = paging64_state.pml4;
    
    // Set up recursive mapping (PML4[510] -> PML4)
    paging64_state.pml4[RECURSIVE_INDEX] = make_pte(
        (uint64_t)paging64_state.pml4,
        PTE_PRESENT | PTE_WRITABLE
    );
    
    print("[PAGING64] PML4 at physical 0x");
    print_hex((uint32_t)(uint64_t)paging64_state.pml4);
    print("\n");
    
    // Identity map first 4GB for kernel
    print("[PAGING64] Identity mapping first 4GB...\n");
    for (uint64_t addr = 0; addr < 0x100000000ULL; addr += PAGE_SIZE_2M) {
        memory_result_t res = paging64_map_page_2m(
            (uint64_t)paging64_state.pml4,
            addr, addr,
            PAGE64_PRESENT | PAGE64_WRITABLE
        );
        if (res != MEMORY_OK) {
            print("[PAGING64] Warning: Failed to map 0x");
            print_hex((uint32_t)addr);
            print("\n");
        }
    }
    
    // Map higher half kernel (0xFFFFFFFF80000000 -> physical 0)
    print("[PAGING64] Mapping higher half kernel...\n");
    uint64_t higher_half = 0xFFFFFFFF80000000ULL;
    for (uint64_t offset = 0; offset < 0x40000000ULL; offset += PAGE_SIZE_2M) {
        paging64_map_page_2m(
            (uint64_t)paging64_state.pml4,
            higher_half + offset, offset,
            PAGE64_PRESENT | PAGE64_WRITABLE | PAGE64_GLOBAL
        );
    }
    
    paging64_state.initialized = true;
    print("[PAGING64] 64-bit paging initialized\n");
    
    return MEMORY_OK;
}

/**
 * @brief Map a 4KB page
 */
memory_result_t paging64_map_page_4k(uint64_t pml4_phys, uint64_t vaddr, 
                                      uint64_t paddr, uint32_t flags) {
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    // Get indices
    uint64_t pml4_idx = pml4_index(vaddr);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    uint64_t pd_idx = pd_index(vaddr);
    uint64_t pt_idx = pt_index(vaddr);
    
    // Build flags
    uint64_t pte_flags = 0;
    if (flags & PAGE64_PRESENT)   pte_flags |= PTE_PRESENT;
    if (flags & PAGE64_WRITABLE)  pte_flags |= PTE_WRITABLE;
    if (flags & PAGE64_USER)      pte_flags |= PTE_USER;
    if (flags & PAGE64_GLOBAL)    pte_flags |= PTE_GLOBAL;
    if ((flags & PAGE64_NX) && paging64_state.nx_supported) {
        pte_flags |= PTE_NO_EXECUTE;
    }
    
    // Ensure PDPT exists
    pte64_t* pdpt = ensure_table_exists(pml4, pml4_idx, 
                                         PTE_PRESENT | PTE_WRITABLE | 
                                         (flags & PAGE64_USER ? PTE_USER : 0));
    if (!pdpt) return MEMORY_ERROR_OUT_OF_MEMORY;
    
    // Ensure PD exists
    pte64_t* pd = ensure_table_exists(pdpt, pdpt_idx,
                                       PTE_PRESENT | PTE_WRITABLE |
                                       (flags & PAGE64_USER ? PTE_USER : 0));
    if (!pd) return MEMORY_ERROR_OUT_OF_MEMORY;
    
    // Ensure PT exists
    pte64_t* pt = ensure_table_exists(pd, pd_idx,
                                       PTE_PRESENT | PTE_WRITABLE |
                                       (flags & PAGE64_USER ? PTE_USER : 0));
    if (!pt) return MEMORY_ERROR_OUT_OF_MEMORY;
    
    // Check if already mapped
    if (pte_present(pt[pt_idx])) {
        return MEMORY_ERROR_ALREADY_MAPPED;
    }
    
    // Set the page table entry
    pt[pt_idx] = make_pte(paddr, pte_flags);
    
    // Invalidate TLB
    tlb_invalidate_page(vaddr);
    
    return MEMORY_OK;
}

/**
 * @brief Map a 2MB huge page
 */
memory_result_t paging64_map_page_2m(uint64_t pml4_phys, uint64_t vaddr,
                                      uint64_t paddr, uint32_t flags) {
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    // Align addresses to 2MB
    vaddr &= ~PAGE_MASK_2M;
    paddr &= ~PAGE_MASK_2M;
    
    // Get indices
    uint64_t pml4_idx = pml4_index(vaddr);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    uint64_t pd_idx = pd_index(vaddr);
    
    // Build flags
    uint64_t pte_flags = PTE_HUGE;
    if (flags & PAGE64_PRESENT)   pte_flags |= PTE_PRESENT;
    if (flags & PAGE64_WRITABLE)  pte_flags |= PTE_WRITABLE;
    if (flags & PAGE64_USER)      pte_flags |= PTE_USER;
    if (flags & PAGE64_GLOBAL)    pte_flags |= PTE_GLOBAL;
    if ((flags & PAGE64_NX) && paging64_state.nx_supported) {
        pte_flags |= PTE_NO_EXECUTE;
    }
    
    // Ensure PDPT exists
    pte64_t* pdpt = ensure_table_exists(pml4, pml4_idx,
                                         PTE_PRESENT | PTE_WRITABLE |
                                         (flags & PAGE64_USER ? PTE_USER : 0));
    if (!pdpt) return MEMORY_ERROR_OUT_OF_MEMORY;
    
    // Ensure PD exists
    pte64_t* pd = ensure_table_exists(pdpt, pdpt_idx,
                                       PTE_PRESENT | PTE_WRITABLE |
                                       (flags & PAGE64_USER ? PTE_USER : 0));
    if (!pd) return MEMORY_ERROR_OUT_OF_MEMORY;
    
    // Check if already mapped
    if (pte_present(pd[pd_idx])) {
        return MEMORY_ERROR_ALREADY_MAPPED;
    }
    
    // Set the PD entry as a 2MB page
    pd[pd_idx] = (paddr & ADDR_MASK_2M) | pte_flags;
    
    // Invalidate TLB
    tlb_invalidate_page(vaddr);
    
    return MEMORY_OK;
}

/**
 * @brief Map a 1GB huge page (if supported)
 */
memory_result_t paging64_map_page_1g(uint64_t pml4_phys, uint64_t vaddr,
                                      uint64_t paddr, uint32_t flags) {
    if (!paging64_state.gigabyte_pages) {
        return MEMORY_ERROR_NOT_INITIALIZED;
    }
    
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    // Align addresses to 1GB
    vaddr &= ~PAGE_MASK_1G;
    paddr &= ~PAGE_MASK_1G;
    
    // Get indices
    uint64_t pml4_idx = pml4_index(vaddr);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    
    // Build flags
    uint64_t pte_flags = PTE_HUGE;
    if (flags & PAGE64_PRESENT)   pte_flags |= PTE_PRESENT;
    if (flags & PAGE64_WRITABLE)  pte_flags |= PTE_WRITABLE;
    if (flags & PAGE64_USER)      pte_flags |= PTE_USER;
    if (flags & PAGE64_GLOBAL)    pte_flags |= PTE_GLOBAL;
    if ((flags & PAGE64_NX) && paging64_state.nx_supported) {
        pte_flags |= PTE_NO_EXECUTE;
    }
    
    // Ensure PDPT exists
    pte64_t* pdpt = ensure_table_exists(pml4, pml4_idx,
                                         PTE_PRESENT | PTE_WRITABLE |
                                         (flags & PAGE64_USER ? PTE_USER : 0));
    if (!pdpt) return MEMORY_ERROR_OUT_OF_MEMORY;
    
    // Check if already mapped
    if (pte_present(pdpt[pdpt_idx])) {
        return MEMORY_ERROR_ALREADY_MAPPED;
    }
    
    // Set the PDPT entry as a 1GB page
    pdpt[pdpt_idx] = (paddr & ADDR_MASK_1G) | pte_flags;
    
    // Invalidate TLB
    tlb_invalidate_page(vaddr);
    
    return MEMORY_OK;
}

/**
 * @brief Unmap a page
 */
memory_result_t paging64_unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    uint64_t pml4_idx = pml4_index(vaddr);
    if (!pte_present(pml4[pml4_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    uint64_t* pdpt = (uint64_t*)pte_to_phys(pml4[pml4_idx]);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    if (!pte_present(pdpt[pdpt_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    // Check for 1GB page
    if (pte_huge(pdpt[pdpt_idx])) {
        pdpt[pdpt_idx] = 0;
        tlb_invalidate_page(vaddr);
        return MEMORY_OK;
    }
    
    uint64_t* pd = (uint64_t*)pte_to_phys(pdpt[pdpt_idx]);
    uint64_t pd_idx = pd_index(vaddr);
    if (!pte_present(pd[pd_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    // Check for 2MB page
    if (pte_huge(pd[pd_idx])) {
        pd[pd_idx] = 0;
        tlb_invalidate_page(vaddr);
        return MEMORY_OK;
    }
    
    uint64_t* pt = (uint64_t*)pte_to_phys(pd[pd_idx]);
    uint64_t pt_idx = pt_index(vaddr);
    if (!pte_present(pt[pt_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    // Clear 4KB page
    pt[pt_idx] = 0;
    tlb_invalidate_page(vaddr);
    
    return MEMORY_OK;
}

/**
 * @brief Get physical address for virtual address
 */
uint64_t paging64_virt_to_phys(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    uint64_t pml4_idx = pml4_index(vaddr);
    if (!pte_present(pml4[pml4_idx])) {
        return 0;
    }
    
    uint64_t* pdpt = (uint64_t*)pte_to_phys(pml4[pml4_idx]);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    if (!pte_present(pdpt[pdpt_idx])) {
        return 0;
    }
    
    // 1GB page
    if (pte_huge(pdpt[pdpt_idx])) {
        return (pdpt[pdpt_idx] & ADDR_MASK_1G) | (vaddr & PAGE_MASK_1G);
    }
    
    uint64_t* pd = (uint64_t*)pte_to_phys(pdpt[pdpt_idx]);
    uint64_t pd_idx = pd_index(vaddr);
    if (!pte_present(pd[pd_idx])) {
        return 0;
    }
    
    // 2MB page
    if (pte_huge(pd[pd_idx])) {
        return (pd[pd_idx] & ADDR_MASK_2M) | (vaddr & PAGE_MASK_2M);
    }
    
    uint64_t* pt = (uint64_t*)pte_to_phys(pd[pd_idx]);
    uint64_t pt_idx = pt_index(vaddr);
    if (!pte_present(pt[pt_idx])) {
        return 0;
    }
    
    // 4KB page
    return (pt[pt_idx] & ADDR_MASK_4K) | (vaddr & PAGE_MASK_4K);
}

/**
 * @brief Load PML4 into CR3
 */
void paging64_load_pml4(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

/**
 * @brief Get current PML4 from CR3
 */
uint64_t paging64_get_current_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ADDR_MASK_4K;
}

/**
 * @brief Check if NX is supported
 */
bool paging64_nx_supported(void) {
    return paging64_state.nx_supported;
}

/**
 * @brief Check if 1GB pages are supported
 */
bool paging64_1gb_supported(void) {
    return paging64_state.gigabyte_pages;
}

/**
 * @brief Check if 5-level paging (LA57) is supported
 */
bool paging64_la57_supported(void) {
    return paging64_state.la57_supported;
}

/**
 * @brief Enable 5-level paging
 */
bool paging64_enable_la57(void) {
    if (!paging64_state.la57_supported) {
        return false;
    }
    
    // LA57 must be enabled before paging is turned on
    // or while in compatibility mode transitioning to long mode
    
    // Set CR4.LA57
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 12);  // LA57 bit
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    
    paging64_state.la57_active = true;
    
    print("[PAGING64] 5-level paging (LA57) enabled\n");
    
    return true;
}

/**
 * @brief Check if currently using 5-level paging
 */
bool paging64_is_la57_active(void) {
    return paging64_state.la57_active;
}

/**
 * @brief Create new address space
 */
uint64_t paging64_create_address_space(void) {
    uint64_t root_table = alloc_page_table();
    if (root_table == 0) {
        return 0;
    }
    
    // Copy kernel mappings (higher half) from current address space
    uint64_t* new_root = (uint64_t*)root_table;
    uint64_t* kernel_root = (uint64_t*)(paging64_state.la57_active ? 
                                         paging64_state.pml4 : 
                                         paging64_state.kernel_pml4);
    
    // Copy upper half entries (kernel space)
    for (int i = 256; i < 512; i++) {
        new_root[i] = kernel_root[i];
    }
    
    return root_table;
}

/**
 * @brief Clone address space (for fork with COW)
 */
uint64_t paging64_clone_address_space(uint64_t src_pml4) {
    uint64_t dst_pml4 = alloc_page_table();
    if (dst_pml4 == 0) {
        return 0;
    }
    
    uint64_t* src = (uint64_t*)src_pml4;
    uint64_t* dst = (uint64_t*)dst_pml4;
    
    // Copy all PML4 entries
    // User space entries (0-255) should be marked COW
    // Kernel space entries (256-511) are shared
    for (int i = 0; i < 512; i++) {
        if (i < 256 && pte_present(src[i])) {
            // User space - need deep copy with COW marking
            // For now, just copy the entry (full COW requires recursive copying)
            dst[i] = src[i];
        } else {
            // Kernel space - share directly
            dst[i] = src[i];
        }
    }
    
    return dst_pml4;
}

/**
 * @brief Free address space
 */
void paging64_free_address_space(uint64_t pml4_phys) {
    // TODO: Recursively free user page tables
    // Don't free kernel page tables as they are shared
    
    pmm_free_frame(pml4_phys);
}

/**
 * @brief Get page size at virtual address
 */
uint64_t paging64_get_page_size(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    uint64_t pml4_idx = pml4_index(vaddr);
    if (!pte_present(pml4[pml4_idx])) {
        return 0;
    }
    
    uint64_t* pdpt = (uint64_t*)pte_to_phys(pml4[pml4_idx]);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    if (!pte_present(pdpt[pdpt_idx])) {
        return 0;
    }
    
    // 1GB page
    if (pte_huge(pdpt[pdpt_idx])) {
        return PAGE_SIZE_1G;
    }
    
    uint64_t* pd = (uint64_t*)pte_to_phys(pdpt[pdpt_idx]);
    uint64_t pd_idx = pd_index(vaddr);
    if (!pte_present(pd[pd_idx])) {
        return 0;
    }
    
    // 2MB page
    if (pte_huge(pd[pd_idx])) {
        return PAGE_SIZE_2M;
    }
    
    uint64_t* pt = (uint64_t*)pte_to_phys(pd[pd_idx]);
    uint64_t pt_idx = pt_index(vaddr);
    if (!pte_present(pt[pt_idx])) {
        return 0;
    }
    
    // 4KB page
    return PAGE_SIZE_4K;
}

/**
 * @brief Change page protection
 */
memory_result_t paging64_change_protection(uint64_t pml4_phys, uint64_t vaddr, uint32_t flags) {
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    uint64_t pml4_idx = pml4_index(vaddr);
    if (!pte_present(pml4[pml4_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    uint64_t* pdpt = (uint64_t*)pte_to_phys(pml4[pml4_idx]);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    if (!pte_present(pdpt[pdpt_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    // Build new flags
    uint64_t new_flags = PTE_PRESENT;
    if (flags & PAGE64_WRITABLE) new_flags |= PTE_WRITABLE;
    if (flags & PAGE64_USER) new_flags |= PTE_USER;
    if (flags & PAGE64_GLOBAL) new_flags |= PTE_GLOBAL;
    if ((flags & PAGE64_NX) && paging64_state.nx_supported) {
        new_flags |= PTE_NO_EXECUTE;
    }
    
    // Handle 1GB page
    if (pte_huge(pdpt[pdpt_idx])) {
        uint64_t phys = pdpt[pdpt_idx] & ADDR_MASK_1G;
        pdpt[pdpt_idx] = phys | new_flags | PTE_HUGE;
        tlb_invalidate_page(vaddr);
        return MEMORY_OK;
    }
    
    uint64_t* pd = (uint64_t*)pte_to_phys(pdpt[pdpt_idx]);
    uint64_t pd_idx = pd_index(vaddr);
    if (!pte_present(pd[pd_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    // Handle 2MB page
    if (pte_huge(pd[pd_idx])) {
        uint64_t phys = pd[pd_idx] & ADDR_MASK_2M;
        pd[pd_idx] = phys | new_flags | PTE_HUGE;
        tlb_invalidate_page(vaddr);
        return MEMORY_OK;
    }
    
    uint64_t* pt = (uint64_t*)pte_to_phys(pd[pd_idx]);
    uint64_t pt_idx = pt_index(vaddr);
    if (!pte_present(pt[pt_idx])) {
        return MEMORY_ERROR_NOT_MAPPED;
    }
    
    // Handle 4KB page
    uint64_t phys = pt[pt_idx] & ADDR_MASK_4K;
    pt[pt_idx] = phys | new_flags;
    tlb_invalidate_page(vaddr);
    
    return MEMORY_OK;
}

/**
 * @brief Dump page table entry for debugging
 */
void paging64_dump_entry(uint64_t pml4_phys, uint64_t vaddr) {
    print("\nPage table walk for vaddr 0x");
    print_hex((uint32_t)(vaddr >> 32));
    print_hex((uint32_t)vaddr);
    print(":\n");
    
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    uint64_t pml4_idx = pml4_index(vaddr);
    
    print("  PML4[");
    print_dec((uint32_t)pml4_idx);
    print("] = 0x");
    print_hex((uint32_t)(pml4[pml4_idx] >> 32));
    print_hex((uint32_t)pml4[pml4_idx]);
    
    if (!pte_present(pml4[pml4_idx])) {
        print(" (not present)\n");
        return;
    }
    print("\n");
    
    uint64_t* pdpt = (uint64_t*)pte_to_phys(pml4[pml4_idx]);
    uint64_t pdpt_idx = pdpt_index(vaddr);
    
    print("  PDPT[");
    print_dec((uint32_t)pdpt_idx);
    print("] = 0x");
    print_hex((uint32_t)(pdpt[pdpt_idx] >> 32));
    print_hex((uint32_t)pdpt[pdpt_idx]);
    
    if (!pte_present(pdpt[pdpt_idx])) {
        print(" (not present)\n");
        return;
    }
    if (pte_huge(pdpt[pdpt_idx])) {
        print(" (1GB page)\n");
        return;
    }
    print("\n");
    
    uint64_t* pd = (uint64_t*)pte_to_phys(pdpt[pdpt_idx]);
    uint64_t pd_idx = pd_index(vaddr);
    
    print("  PD[");
    print_dec((uint32_t)pd_idx);
    print("] = 0x");
    print_hex((uint32_t)(pd[pd_idx] >> 32));
    print_hex((uint32_t)pd[pd_idx]);
    
    if (!pte_present(pd[pd_idx])) {
        print(" (not present)\n");
        return;
    }
    if (pte_huge(pd[pd_idx])) {
        print(" (2MB page)\n");
        return;
    }
    print("\n");
    
    uint64_t* pt = (uint64_t*)pte_to_phys(pd[pd_idx]);
    uint64_t pt_idx = pt_index(vaddr);
    
    print("  PT[");
    print_dec((uint32_t)pt_idx);
    print("] = 0x");
    print_hex((uint32_t)(pt[pt_idx] >> 32));
    print_hex((uint32_t)pt[pt_idx]);
    
    if (!pte_present(pt[pt_idx])) {
        print(" (not present)\n");
    } else {
        print(" (4KB page)\n");
    }
}

/* ============================================================================
 * x64_* API  —  pml4_t*-based interface
 *
 * These functions provide the public API described in paging64.h using a
 * typed pml4_t* rather than a raw physical address.  They walk the four
 * paging levels directly (physical == virtual assumption holds before the
 * page-table switch, matching the rest of Fern boot code).
 *
 * Virtual address bit decomposition (48-bit canonical):
 *   [47:39] PML4 index  (9 bits)
 *   [38:30] PDPT index  (9 bits)
 *   [29:21] PD index    (9 bits)
 *   [20:12] PT index    (9 bits)
 *   [11:0]  Page offset (12 bits)
 * ============================================================================ */

/* IA32_EFER MSR number */
#define IA32_EFER_MSR   0xC0000080UL
/* NXE bit in IA32_EFER */
#define IA32_EFER_NXE   (1ULL << 11)

/* Kernel higher-half virtual base (mirrors the header constant) */
#ifndef KERNEL_HIGHER_HALF_OFFSET
#define KERNEL_HIGHER_HALF_OFFSET  0xFFFFFFFF80000000ULL
#endif

/**
 * x64_alloc_table - allocate and zero a single 4KB page table frame.
 *
 * Returns the physical (== virtual, pre-paging) address of the new table,
 * or 0 on failure.
 */
static uint64_t x64_alloc_table(void) {
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return 0;
    memset((void*)frame, 0, PAGE_SIZE_4K);
    return frame;
}

/**
 * x64_get_or_create_entry - walk one level of the page hierarchy.
 *
 * Given a pointer to a parent table and the index into it, ensure the entry
 * at that index points to a valid child table.  Allocates a new child table
 * if the entry is not present.
 *
 * Returns a pointer to the child table on success, or NULL on OOM.
 *
 * @parent   Pointer to the parent table (512 entries of uint64_t).
 * @idx      Index into the parent table.
 * @usr_flag PTE_USER if the mapping is for user space, 0 otherwise.
 */
static uint64_t* x64_get_or_create_child(uint64_t* parent, uint64_t idx,
                                          uint64_t usr_flag) {
    pte64_t entry = parent[idx];

    if (pte_present(entry)) {
        /* Already exists — return pointer to child table. */
        return (uint64_t*)pte_to_phys(entry);
    }

    /* Allocate and link a new child table. */
    uint64_t child_phys = x64_alloc_table();
    if (!child_phys) return NULL;

    parent[idx] = make_pte(child_phys,
                           PTE_PRESENT | PTE_WRITABLE | usr_flag);
    return (uint64_t*)child_phys;
}

/* --------------------------------------------------------------------------
 * pml4_create
 * -------------------------------------------------------------------------- */
pml4_t* pml4_create(void) {
    uint64_t frame = x64_alloc_table();
    if (!frame) return NULL;
    return (pml4_t*)frame;
}

/* --------------------------------------------------------------------------
 * x64_map_page
 * -------------------------------------------------------------------------- */
int x64_map_page(pml4_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) return MEMORY_ERROR_NULL_PTR;

    /* Align addresses to 4KB. */
    virt &= ~(uint64_t)PAGE_MASK_4K;
    phys &= ~(uint64_t)PAGE_MASK_4K;

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);
    uint64_t pt_idx   = pt_index(virt);

    /* Determine whether user bit should be propagated through parent entries. */
    uint64_t usr = (flags & PAGE64_USER) ? PTE_USER : 0;

    /* Walk/build the four-level hierarchy. */
    uint64_t* pdpt = x64_get_or_create_child((uint64_t*)pml4, pml4_idx, usr);
    if (!pdpt) return MEMORY_ERROR_OUT_OF_MEMORY;

    uint64_t* pd = x64_get_or_create_child(pdpt, pdpt_idx, usr);
    if (!pd) return MEMORY_ERROR_OUT_OF_MEMORY;

    uint64_t* pt = x64_get_or_create_child(pd, pd_idx, usr);
    if (!pt) return MEMORY_ERROR_OUT_OF_MEMORY;

    /* Translate caller flags to internal PTE flags. */
    uint64_t pte_flags = 0;
    if (flags & PAGE64_PRESENT)  pte_flags |= PTE_PRESENT;
    if (flags & PAGE64_WRITABLE) pte_flags |= PTE_WRITABLE;
    if (flags & PAGE64_USER)     pte_flags |= PTE_USER;
    if (flags & PAGE64_PWT)      pte_flags |= PTE_WRITE_THROUGH;
    if (flags & PAGE64_PCD)      pte_flags |= PTE_CACHE_DISABLE;
    if (flags & PAGE64_GLOBAL)   pte_flags |= PTE_GLOBAL;
    if (flags & PAGE64_NX)       pte_flags |= PTE_NO_EXECUTE;

    /* If PAGE64_PRESENT was not set but the caller passed no present flag,
     * default to marking the page present (map implies present). */
    if (!(flags & (PAGE64_PRESENT | PAGE64_WRITABLE | PAGE64_USER |
                   PAGE64_GLOBAL | PAGE64_NX))) {
        /* All-zero flags: treat as present + writable. */
        pte_flags = PTE_PRESENT | PTE_WRITABLE;
    }

    /* Warn but allow re-mapping (overwrite existing entry). */
    if (pte_present(pt[pt_idx])) {
        /* Already mapped — overwrite rather than returning error, so that
         * identity_map_range does not abort on overlapping regions. */
        pt[pt_idx] = make_pte(phys, pte_flags);
        x64_invlpg(virt);
        return MEMORY_OK;
    }

    pt[pt_idx] = make_pte(phys, pte_flags);
    return MEMORY_OK;
}

/* --------------------------------------------------------------------------
 * x64_unmap_page
 * -------------------------------------------------------------------------- */
int x64_unmap_page(pml4_t* pml4, uint64_t virt) {
    if (!pml4) return MEMORY_ERROR_NULL_PTR;

    virt &= ~(uint64_t)PAGE_MASK_4K;

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);
    uint64_t pt_idx   = pt_index(virt);

    uint64_t* tbl = (uint64_t*)pml4;

    if (!pte_present(tbl[pml4_idx])) return MEMORY_ERROR_NOT_MAPPED;
    tbl = (uint64_t*)pte_to_phys(tbl[pml4_idx]);

    if (!pte_present(tbl[pdpt_idx])) return MEMORY_ERROR_NOT_MAPPED;
    tbl = (uint64_t*)pte_to_phys(tbl[pdpt_idx]);

    if (!pte_present(tbl[pd_idx])) return MEMORY_ERROR_NOT_MAPPED;
    tbl = (uint64_t*)pte_to_phys(tbl[pd_idx]);

    if (!pte_present(tbl[pt_idx])) return MEMORY_ERROR_NOT_MAPPED;

    tbl[pt_idx] = 0;
    x64_invlpg(virt);
    return MEMORY_OK;
}

/* --------------------------------------------------------------------------
 * x64_get_phys
 * -------------------------------------------------------------------------- */
uint64_t x64_get_phys(pml4_t* pml4, uint64_t virt) {
    if (!pml4) return 0;

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);
    uint64_t pt_idx   = pt_index(virt);

    uint64_t* tbl = (uint64_t*)pml4;

    if (!pte_present(tbl[pml4_idx])) return 0;
    tbl = (uint64_t*)pte_to_phys(tbl[pml4_idx]);

    if (!pte_present(tbl[pdpt_idx])) return 0;
    /* 1GB huge page at PDPT level */
    if (pte_huge(tbl[pdpt_idx]))
        return (tbl[pdpt_idx] & ADDR_MASK_1G) | (virt & PAGE_MASK_1G);
    tbl = (uint64_t*)pte_to_phys(tbl[pdpt_idx]);

    if (!pte_present(tbl[pd_idx])) return 0;
    /* 2MB huge page at PD level */
    if (pte_huge(tbl[pd_idx]))
        return (tbl[pd_idx] & ADDR_MASK_2M) | (virt & PAGE_MASK_2M);
    tbl = (uint64_t*)pte_to_phys(tbl[pd_idx]);

    if (!pte_present(tbl[pt_idx])) return 0;
    return (tbl[pt_idx] & ADDR_MASK_4K) | (virt & PAGE_MASK_4K);
}

/* --------------------------------------------------------------------------
 * x64_is_mapped
 * -------------------------------------------------------------------------- */
bool x64_is_mapped(pml4_t* pml4, uint64_t virt) {
    if (!pml4) return false;

    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx   = pd_index(virt);
    uint64_t pt_idx   = pt_index(virt);

    uint64_t* tbl = (uint64_t*)pml4;

    if (!pte_present(tbl[pml4_idx])) return false;
    tbl = (uint64_t*)pte_to_phys(tbl[pml4_idx]);

    if (!pte_present(tbl[pdpt_idx])) return false;
    if (pte_huge(tbl[pdpt_idx])) return true;  /* 1GB page — present */
    tbl = (uint64_t*)pte_to_phys(tbl[pdpt_idx]);

    if (!pte_present(tbl[pd_idx])) return false;
    if (pte_huge(tbl[pd_idx])) return true;    /* 2MB page — present */
    tbl = (uint64_t*)pte_to_phys(tbl[pd_idx]);

    return pte_present(tbl[pt_idx]);
}

/* --------------------------------------------------------------------------
 * x64_identity_map_range
 * -------------------------------------------------------------------------- */
void x64_identity_map_range(pml4_t* pml4, uint64_t start, uint64_t end,
                              uint64_t flags) {
    if (!pml4 || start >= end) return;

    /* Round start down and end up to 4KB boundaries. */
    start &= ~(uint64_t)PAGE_MASK_4K;
    end    = (end + PAGE_MASK_4K) & ~(uint64_t)PAGE_MASK_4K;

    /* Ensure present flag is always set for an identity map. */
    uint64_t map_flags = flags | PAGE64_PRESENT;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE_4K) {
        x64_map_page(pml4, addr, addr, map_flags);
    }
}

/* --------------------------------------------------------------------------
 * x64_map_kernel_higher_half
 * -------------------------------------------------------------------------- */
void x64_map_kernel_higher_half(pml4_t* pml4, uint64_t phys_start,
                                  uint64_t phys_end) {
    if (!pml4 || phys_start >= phys_end) return;

    /* Align to 4KB. */
    phys_start &= ~(uint64_t)PAGE_MASK_4K;
    phys_end    = (phys_end + PAGE_MASK_4K) & ~(uint64_t)PAGE_MASK_4K;

    /*
     * Kernel pages: present, writable, global (not user-accessible).
     * NX is intentionally not set here so code pages remain executable;
     * callers can call x64_map_page() with PAGE64_NX for data-only ranges.
     */
    uint64_t flags = PAGE64_PRESENT | PAGE64_WRITABLE | PAGE64_GLOBAL;

    uint64_t offset = 0;
    for (uint64_t phys = phys_start; phys < phys_end;
         phys += PAGE_SIZE_4K, offset += PAGE_SIZE_4K) {
        uint64_t virt = KERNEL_HIGHER_HALF_OFFSET + (phys - phys_start);
        x64_map_page(pml4, virt, phys, flags);
    }
    (void)offset;
}

/* --------------------------------------------------------------------------
 * x64_load_pml4
 *
 * Write the physical address of the PML4 table into CR3.  This also
 * implicitly flushes all non-global TLB entries.
 * -------------------------------------------------------------------------- */
void x64_load_pml4(pml4_t* pml4) {
    if (!pml4) return;
    __asm__ volatile(
        "mov %0, %%cr3"
        :
        : "r"((uint64_t)(uintptr_t)pml4)
        : "memory"
    );
}

/* --------------------------------------------------------------------------
 * x64_invlpg
 *
 * Invalidate the TLB entry for a single virtual address.
 * -------------------------------------------------------------------------- */
void x64_invlpg(uint64_t vaddr) {
    __asm__ volatile(
        "invlpg (%0)"
        :
        : "r"(vaddr)
        : "memory"
    );
}

/* --------------------------------------------------------------------------
 * x64_enable_nx
 *
 * Enable the No-Execute Enable (NXE) bit in IA32_EFER (MSR 0xC0000080).
 * This must be done before any page table entries that use bit 63 (XD) are
 * loaded; otherwise the CPU raises a #GP when it encounters such an entry.
 *
 * We first check CPUID 0x80000001:EDX[20] to confirm the CPU supports NX.
 * -------------------------------------------------------------------------- */
void x64_enable_nx(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Query extended CPU features. */
    __asm__ volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000001U), "c"(0)
    );

    /* Bit 20 of EDX = NX/XD support. */
    if (!(edx & (1U << 20))) {
        print("[PAGING64] NX not supported by CPU, skipping x64_enable_nx\n");
        return;
    }

    /* Read IA32_EFER.
     * rdmsr returns the 64-bit MSR value split across EDX (high 32 bits)
     * and EAX (low 32 bits).  On x86_64 we must use separate 32-bit
     * register constraints and reconstruct the 64-bit value manually;
     * the "=A" constraint only works as EDX:EAX in 32-bit code. */
    uint32_t efer_lo, efer_hi;
    __asm__ volatile(
        "rdmsr"
        : "=a"(efer_lo), "=d"(efer_hi)
        : "c"(IA32_EFER_MSR)
    );
    uint64_t efer = ((uint64_t)efer_hi << 32) | efer_lo;

    if (efer & IA32_EFER_NXE) {
        /* NXE already set — nothing to do. */
        return;
    }

    efer |= IA32_EFER_NXE;
    efer_lo = (uint32_t)(efer & 0xFFFFFFFFU);
    efer_hi = (uint32_t)(efer >> 32);

    /* Write IA32_EFER back (wrmsr reads from EDX:EAX, ECX = MSR index). */
    __asm__ volatile(
        "wrmsr"
        :
        : "a"(efer_lo), "d"(efer_hi), "c"(IA32_EFER_MSR)
    );

    print("[PAGING64] NX (XD) enabled via IA32_EFER\n");

    /* Keep state consistent with the feature-detection path. */
    paging64_state.nx_supported = true;
}

#endif /* __x86_64__ */
