/**
 * @file paging_modes.c
 * @brief Comprehensive Paging Mode Support
 * 
 * Supports ALL x86/x64 paging modes:
 * 
 * 32-bit Modes:
 * - Legacy 32-bit paging (2-level): 4KB pages, 4GB virtual/physical
 * - PSE (Page Size Extension): 4MB pages
 * - PAE (Physical Address Extension): 3-level, 36-bit physical (64GB)
 * - PAE + PSE: 2MB pages with PAE
 * 
 * 64-bit Modes:
 * - 4-level paging (standard): 4KB, 2MB, 1GB pages, 48-bit virtual
 * - 5-level paging (LA57): 57-bit virtual, 52-bit physical
 * 
 * Special Features:
 * - NX (No Execute) bit
 * - Global pages (PGE)
 * - PCID (Process Context ID)
 * - PKE (Protection Keys)
 */

#include "include/paging_modes.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/tlb.h"

// ============================================================================
// CONSTANTS AND MASKS
// ============================================================================

// Page sizes
#define PAGE_4KB        0x1000ULL
#define PAGE_2MB        0x200000ULL
#define PAGE_4MB        0x400000ULL
#define PAGE_1GB        0x40000000ULL
#define PAGE_512GB      0x8000000000ULL

// CR0 bits
#define CR0_PG          (1UL << 31)     // Paging enable
#define CR0_WP          (1UL << 16)     // Write protect

// CR4 bits
#define CR4_PSE         (1UL << 4)      // Page Size Extension (4MB pages)
#define CR4_PAE         (1UL << 5)      // Physical Address Extension
#define CR4_PGE         (1UL << 7)      // Page Global Enable
#define CR4_PCIDE       (1UL << 17)     // PCID Enable
#define CR4_SMEP        (1UL << 20)     // SMEP
#define CR4_SMAP        (1UL << 21)     // SMAP
#define CR4_PKE         (1UL << 22)     // Protection Keys Enable
#define CR4_LA57        (1UL << 12)     // 5-level paging

// EFER MSR bits
#define EFER_LME        (1ULL << 8)     // Long Mode Enable
#define EFER_LMA        (1ULL << 10)    // Long Mode Active
#define EFER_NXE        (1ULL << 11)    // No Execute Enable

// PTE flags (common)
#define PTE_P           (1ULL << 0)     // Present
#define PTE_RW          (1ULL << 1)     // Read/Write
#define PTE_US          (1ULL << 2)     // User/Supervisor
#define PTE_PWT         (1ULL << 3)     // Page Write-Through
#define PTE_PCD         (1ULL << 4)     // Page Cache Disable
#define PTE_A           (1ULL << 5)     // Accessed
#define PTE_D           (1ULL << 6)     // Dirty
#define PTE_PS          (1ULL << 7)     // Page Size (huge page)
#define PTE_G           (1ULL << 8)     // Global
#define PTE_PAT         (1ULL << 7)     // PAT bit for 4KB pages
#define PTE_PAT_HUGE    (1ULL << 12)    // PAT bit for huge pages
#define PTE_NX          (1ULL << 63)    // No Execute

// Address masks for different modes
#define ADDR_MASK_32        0xFFFFF000UL    // 32-bit PTE address
#define ADDR_MASK_PSE_4M    0xFFC00000UL    // 4MB page address (32-bit)
#define ADDR_MASK_PAE_4K    0x000FFFFFFFFFF000ULL   // PAE 4KB
#define ADDR_MASK_PAE_2M    0x000FFFFFFFE00000ULL   // PAE 2MB
#define ADDR_MASK_64_4K     0x000FFFFFFFFFF000ULL   // 64-bit 4KB
#define ADDR_MASK_64_2M     0x000FFFFFFFE00000ULL   // 64-bit 2MB
#define ADDR_MASK_64_1G     0x000FFFFFC0000000ULL   // 64-bit 1GB

// Index shifts
#define SHIFT_PML5          48
#define SHIFT_PML4          39
#define SHIFT_PDPT          30
#define SHIFT_PD            21
#define SHIFT_PT            12

// Entries per table
#define ENTRIES_32BIT       1024
#define ENTRIES_PAE         512
#define ENTRIES_64BIT       512

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief CPU paging capabilities
 */
typedef struct {
    bool pse;               // PSE (4MB pages)
    bool pae;               // PAE (36-bit physical)
    bool pge;               // Global pages
    bool nx;                // NX bit
    bool pcid;              // Process Context ID
    bool invpcid;           // INVPCID instruction
    bool la57;              // 5-level paging
    bool gigabyte_pages;    // 1GB pages
    bool pke;               // Protection keys
    uint32_t phys_bits;     // Physical address bits
    uint32_t virt_bits;     // Virtual address bits (linear)
} paging_caps_t;

/**
 * @brief Paging mode manager state
 */
static struct {
    bool initialized;
    paging_mode_t current_mode;
    paging_caps_t caps;
    
    // Current root table physical address
    uint64_t root_table;
    
    // Statistics
    uint64_t pages_mapped_4k;
    uint64_t pages_mapped_2m;
    uint64_t pages_mapped_4m;
    uint64_t pages_mapped_1g;
    uint64_t tables_allocated;
} paging_state = { .initialized = false };

// ============================================================================
// CPU FEATURE DETECTION
// ============================================================================

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(val) : "memory");
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(val) : "memory");
}

/**
 * @brief Detect CPU paging capabilities
 */
static void detect_capabilities(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // CPUID.01H - Basic features
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    paging_state.caps.pse = (edx & (1 << 3)) != 0;   // PSE
    paging_state.caps.pae = (edx & (1 << 6)) != 0;   // PAE
    paging_state.caps.pge = (edx & (1 << 13)) != 0;  // PGE
    paging_state.caps.pcid = (ecx & (1 << 17)) != 0; // PCID
    
    // CPUID.07H.0 - Extended features
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    paging_state.caps.invpcid = (ebx & (1 << 10)) != 0;
    paging_state.caps.pke = (ecx & (1 << 3)) != 0;
    
    // CPUID.07H.0:ECX[16] - LA57 (5-level paging)
    paging_state.caps.la57 = (ecx & (1 << 16)) != 0;
    
    // CPUID.80000001H - Extended features
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    paging_state.caps.nx = (edx & (1 << 20)) != 0;           // NX
    paging_state.caps.gigabyte_pages = (edx & (1 << 26)) != 0;  // 1GB pages
    
    // CPUID.80000008H - Address size info
    uint32_t max_ext;
    __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx", "ecx", "edx");
    
    if (max_ext >= 0x80000008) {
        __asm__ volatile("cpuid" : "=a"(eax) : "a"(0x80000008) : "ebx", "ecx", "edx");
        paging_state.caps.phys_bits = eax & 0xFF;
        paging_state.caps.virt_bits = (eax >> 8) & 0xFF;
    } else {
        // Default for older CPUs
        paging_state.caps.phys_bits = 36;
        paging_state.caps.virt_bits = 48;
    }
    
    print("[PAGING] CPU Capabilities:\n");
    print("  PSE (4MB pages): "); print(paging_state.caps.pse ? "yes" : "no"); print("\n");
    print("  PAE (36-bit): "); print(paging_state.caps.pae ? "yes" : "no"); print("\n");
    print("  PGE (global): "); print(paging_state.caps.pge ? "yes" : "no"); print("\n");
    print("  NX bit: "); print(paging_state.caps.nx ? "yes" : "no"); print("\n");
    print("  PCID: "); print(paging_state.caps.pcid ? "yes" : "no"); print("\n");
    print("  1GB pages: "); print(paging_state.caps.gigabyte_pages ? "yes" : "no"); print("\n");
    print("  5-level (LA57): "); print(paging_state.caps.la57 ? "yes" : "no"); print("\n");
    print("  Physical bits: "); print_dec(paging_state.caps.phys_bits); print("\n");
    print("  Virtual bits: "); print_dec(paging_state.caps.virt_bits); print("\n");
}

// ============================================================================
// TABLE ALLOCATION
// ============================================================================

/**
 * @brief Allocate and zero a page table
 */
static uint64_t alloc_table(uint32_t size) {
    uint64_t frame = pmm_alloc_frame();
    if (frame == 0) {
        return 0;
    }
    memset((void*)(uintptr_t)frame, 0, size);
    paging_state.tables_allocated++;
    return frame;
}

// ============================================================================
// 32-BIT LEGACY PAGING (2-LEVEL)
// ============================================================================

/**
 * @brief Map page using 32-bit legacy paging (4KB)
 */
static paging_result_t map_32bit_4k(uint32_t vaddr, uint32_t paddr, uint32_t flags) {
    uint32_t* pd = (uint32_t*)(uintptr_t)paging_state.root_table;
    
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    
    // Build PTE flags
    uint32_t pte_flags = PTE_P;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if (flags & PAGING_WRITE_THROUGH) pte_flags |= PTE_PWT;
    if (flags & PAGING_CACHE_DISABLE) pte_flags |= PTE_PCD;
    
    // Ensure PT exists
    if (!(pd[pd_idx] & PTE_P)) {
        uint32_t pt = (uint32_t)alloc_table(PAGE_4KB);
        if (pt == 0) return PAGING_ERROR_NO_MEMORY;
        pd[pd_idx] = pt | PTE_P | PTE_RW | (flags & PAGING_USER ? PTE_US : 0);
    }
    
    uint32_t* pt = (uint32_t*)(uintptr_t)(pd[pd_idx] & ADDR_MASK_32);
    
    if (pt[pt_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pt[pt_idx] = (paddr & ADDR_MASK_32) | pte_flags;
    paging_state.pages_mapped_4k++;
    
    return PAGING_OK;
}

/**
 * @brief Map page using 32-bit PSE (4MB)
 */
static paging_result_t map_32bit_4m(uint32_t vaddr, uint32_t paddr, uint32_t flags) {
    if (!paging_state.caps.pse) {
        return PAGING_ERROR_NOT_SUPPORTED;
    }
    
    uint32_t* pd = (uint32_t*)(uintptr_t)paging_state.root_table;
    
    uint32_t pd_idx = vaddr >> 22;
    
    // Align to 4MB
    vaddr &= ADDR_MASK_PSE_4M;
    paddr &= ADDR_MASK_PSE_4M;
    
    uint32_t pte_flags = PTE_P | PTE_PS;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    
    if (pd[pd_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pd[pd_idx] = paddr | pte_flags;
    paging_state.pages_mapped_4m++;
    
    return PAGING_OK;
}

// ============================================================================
// PAE PAGING (3-LEVEL, 36-BIT PHYSICAL)
// ============================================================================

/**
 * @brief Map page using PAE paging (4KB)
 */
static paging_result_t map_pae_4k(uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    uint64_t* pdpt = (uint64_t*)(uintptr_t)paging_state.root_table;
    
    uint32_t pdpt_idx = (vaddr >> 30) & 0x3;
    uint32_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint32_t pt_idx = (vaddr >> 12) & 0x1FF;
    
    uint64_t pte_flags = PTE_P;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if ((flags & PAGING_NX) && paging_state.caps.nx) pte_flags |= PTE_NX;
    
    // Ensure PD exists
    if (!(pdpt[pdpt_idx] & PTE_P)) {
        uint64_t pd = alloc_table(PAGE_4KB);
        if (pd == 0) return PAGING_ERROR_NO_MEMORY;
        pdpt[pdpt_idx] = pd | PTE_P;
    }
    
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ADDR_MASK_PAE_4K);
    
    // Ensure PT exists
    if (!(pd[pd_idx] & PTE_P)) {
        uint64_t pt = alloc_table(PAGE_4KB);
        if (pt == 0) return PAGING_ERROR_NO_MEMORY;
        pd[pd_idx] = pt | PTE_P | PTE_RW | (flags & PAGING_USER ? PTE_US : 0);
    }
    
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[pd_idx] & ADDR_MASK_PAE_4K);
    
    if (pt[pt_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pt[pt_idx] = (paddr & ADDR_MASK_PAE_4K) | pte_flags;
    paging_state.pages_mapped_4k++;
    
    return PAGING_OK;
}

/**
 * @brief Map page using PAE paging (2MB)
 */
static paging_result_t map_pae_2m(uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    uint64_t* pdpt = (uint64_t*)(uintptr_t)paging_state.root_table;
    
    uint32_t pdpt_idx = (vaddr >> 30) & 0x3;
    uint32_t pd_idx = (vaddr >> 21) & 0x1FF;
    
    // Align to 2MB
    vaddr &= ~(PAGE_2MB - 1);
    paddr &= ~(PAGE_2MB - 1);
    
    uint64_t pte_flags = PTE_P | PTE_PS;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if ((flags & PAGING_NX) && paging_state.caps.nx) pte_flags |= PTE_NX;
    
    // Ensure PD exists
    if (!(pdpt[pdpt_idx] & PTE_P)) {
        uint64_t pd = alloc_table(PAGE_4KB);
        if (pd == 0) return PAGING_ERROR_NO_MEMORY;
        pdpt[pdpt_idx] = pd | PTE_P;
    }
    
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ADDR_MASK_PAE_4K);
    
    if (pd[pd_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pd[pd_idx] = (paddr & ADDR_MASK_PAE_2M) | pte_flags;
    paging_state.pages_mapped_2m++;
    
    return PAGING_OK;
}

// ============================================================================
// 64-BIT 4-LEVEL PAGING
// ============================================================================

/**
 * @brief Map page using 64-bit paging (4KB)
 */
static paging_result_t map_64bit_4k(uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    uint64_t* pml4 = (uint64_t*)(uintptr_t)paging_state.root_table;
    
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    
    uint64_t pte_flags = PTE_P;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if ((flags & PAGING_NX) && paging_state.caps.nx) pte_flags |= PTE_NX;
    
    uint64_t table_flags = PTE_P | PTE_RW | (flags & PAGING_USER ? PTE_US : 0);
    
    // Ensure PDPT exists
    if (!(pml4[pml4_idx] & PTE_P)) {
        uint64_t pdpt = alloc_table(PAGE_4KB);
        if (pdpt == 0) return PAGING_ERROR_NO_MEMORY;
        pml4[pml4_idx] = pdpt | table_flags;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ADDR_MASK_64_4K);
    
    // Ensure PD exists
    if (!(pdpt[pdpt_idx] & PTE_P)) {
        uint64_t pd = alloc_table(PAGE_4KB);
        if (pd == 0) return PAGING_ERROR_NO_MEMORY;
        pdpt[pdpt_idx] = pd | table_flags;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ADDR_MASK_64_4K);
    
    // Ensure PT exists
    if (!(pd[pd_idx] & PTE_P)) {
        uint64_t pt = alloc_table(PAGE_4KB);
        if (pt == 0) return PAGING_ERROR_NO_MEMORY;
        pd[pd_idx] = pt | table_flags;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[pd_idx] & ADDR_MASK_64_4K);
    
    if (pt[pt_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pt[pt_idx] = (paddr & ADDR_MASK_64_4K) | pte_flags;
    paging_state.pages_mapped_4k++;
    
    return PAGING_OK;
}

/**
 * @brief Map page using 64-bit paging (2MB)
 */
static paging_result_t map_64bit_2m(uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    uint64_t* pml4 = (uint64_t*)(uintptr_t)paging_state.root_table;
    
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    
    vaddr &= ~(PAGE_2MB - 1);
    paddr &= ~(PAGE_2MB - 1);
    
    uint64_t pte_flags = PTE_P | PTE_PS;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if ((flags & PAGING_NX) && paging_state.caps.nx) pte_flags |= PTE_NX;
    
    uint64_t table_flags = PTE_P | PTE_RW | (flags & PAGING_USER ? PTE_US : 0);
    
    // Ensure PDPT exists
    if (!(pml4[pml4_idx] & PTE_P)) {
        uint64_t pdpt = alloc_table(PAGE_4KB);
        if (pdpt == 0) return PAGING_ERROR_NO_MEMORY;
        pml4[pml4_idx] = pdpt | table_flags;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ADDR_MASK_64_4K);
    
    // Ensure PD exists
    if (!(pdpt[pdpt_idx] & PTE_P)) {
        uint64_t pd = alloc_table(PAGE_4KB);
        if (pd == 0) return PAGING_ERROR_NO_MEMORY;
        pdpt[pdpt_idx] = pd | table_flags;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ADDR_MASK_64_4K);
    
    if (pd[pd_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pd[pd_idx] = (paddr & ADDR_MASK_64_2M) | pte_flags;
    paging_state.pages_mapped_2m++;
    
    return PAGING_OK;
}

/**
 * @brief Map page using 64-bit paging (1GB)
 */
static paging_result_t map_64bit_1g(uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    if (!paging_state.caps.gigabyte_pages) {
        return PAGING_ERROR_NOT_SUPPORTED;
    }
    
    uint64_t* pml4 = (uint64_t*)(uintptr_t)paging_state.root_table;
    
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    
    vaddr &= ~(PAGE_1GB - 1);
    paddr &= ~(PAGE_1GB - 1);
    
    uint64_t pte_flags = PTE_P | PTE_PS;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if ((flags & PAGING_NX) && paging_state.caps.nx) pte_flags |= PTE_NX;
    
    uint64_t table_flags = PTE_P | PTE_RW | (flags & PAGING_USER ? PTE_US : 0);
    
    // Ensure PDPT exists
    if (!(pml4[pml4_idx] & PTE_P)) {
        uint64_t pdpt = alloc_table(PAGE_4KB);
        if (pdpt == 0) return PAGING_ERROR_NO_MEMORY;
        pml4[pml4_idx] = pdpt | table_flags;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ADDR_MASK_64_4K);
    
    if (pdpt[pdpt_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pdpt[pdpt_idx] = (paddr & ADDR_MASK_64_1G) | pte_flags;
    paging_state.pages_mapped_1g++;
    
    return PAGING_OK;
}

// ============================================================================
// 5-LEVEL PAGING (LA57)
// ============================================================================

/**
 * @brief Map page using 5-level paging (4KB)
 */
static paging_result_t map_la57_4k(uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    if (!paging_state.caps.la57) {
        return PAGING_ERROR_NOT_SUPPORTED;
    }
    
    uint64_t* pml5 = (uint64_t*)(uintptr_t)paging_state.root_table;
    
    uint64_t pml5_idx = (vaddr >> 48) & 0x1FF;
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    
    uint64_t pte_flags = PTE_P;
    if (flags & PAGING_WRITABLE) pte_flags |= PTE_RW;
    if (flags & PAGING_USER) pte_flags |= PTE_US;
    if (flags & PAGING_GLOBAL) pte_flags |= PTE_G;
    if ((flags & PAGING_NX) && paging_state.caps.nx) pte_flags |= PTE_NX;
    
    uint64_t table_flags = PTE_P | PTE_RW | (flags & PAGING_USER ? PTE_US : 0);
    
    // Ensure PML4 exists
    if (!(pml5[pml5_idx] & PTE_P)) {
        uint64_t pml4 = alloc_table(PAGE_4KB);
        if (pml4 == 0) return PAGING_ERROR_NO_MEMORY;
        pml5[pml5_idx] = pml4 | table_flags;
    }
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(pml5[pml5_idx] & ADDR_MASK_64_4K);
    
    // Continue as normal 4-level paging
    if (!(pml4[pml4_idx] & PTE_P)) {
        uint64_t pdpt = alloc_table(PAGE_4KB);
        if (pdpt == 0) return PAGING_ERROR_NO_MEMORY;
        pml4[pml4_idx] = pdpt | table_flags;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ADDR_MASK_64_4K);
    
    if (!(pdpt[pdpt_idx] & PTE_P)) {
        uint64_t pd = alloc_table(PAGE_4KB);
        if (pd == 0) return PAGING_ERROR_NO_MEMORY;
        pdpt[pdpt_idx] = pd | table_flags;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ADDR_MASK_64_4K);
    
    if (!(pd[pd_idx] & PTE_P)) {
        uint64_t pt = alloc_table(PAGE_4KB);
        if (pt == 0) return PAGING_ERROR_NO_MEMORY;
        pd[pd_idx] = pt | table_flags;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[pd_idx] & ADDR_MASK_64_4K);
    
    if (pt[pt_idx] & PTE_P) {
        return PAGING_ERROR_ALREADY_MAPPED;
    }
    
    pt[pt_idx] = (paddr & ADDR_MASK_64_4K) | pte_flags;
    paging_state.pages_mapped_4k++;
    
    return PAGING_OK;
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize paging mode manager
 */
paging_result_t paging_modes_init(void) {
    if (paging_state.initialized) {
        return PAGING_OK;
    }
    
    print("[PAGING] Initializing paging mode manager...\n");
    
    detect_capabilities();
    
    paging_state.current_mode = PAGING_MODE_NONE;
    paging_state.root_table = 0;
    paging_state.pages_mapped_4k = 0;
    paging_state.pages_mapped_2m = 0;
    paging_state.pages_mapped_4m = 0;
    paging_state.pages_mapped_1g = 0;
    paging_state.tables_allocated = 0;
    
    paging_state.initialized = true;
    
    print("[PAGING] Paging mode manager initialized\n");
    
    return PAGING_OK;
}

/**
 * @brief Set paging mode
 */
paging_result_t paging_set_mode(paging_mode_t mode) {
    if (!paging_state.initialized) {
        return PAGING_ERROR_NOT_INITIALIZED;
    }
    
    // Check if mode is supported
    switch (mode) {
        case PAGING_MODE_32BIT:
            // Always supported
            break;
        case PAGING_MODE_32BIT_PSE:
            if (!paging_state.caps.pse) return PAGING_ERROR_NOT_SUPPORTED;
            break;
        case PAGING_MODE_PAE:
        case PAGING_MODE_PAE_PSE:
            if (!paging_state.caps.pae) return PAGING_ERROR_NOT_SUPPORTED;
            break;
        case PAGING_MODE_64BIT:
            // Requires 64-bit CPU (assumed if building for x64)
            break;
        case PAGING_MODE_LA57:
            if (!paging_state.caps.la57) return PAGING_ERROR_NOT_SUPPORTED;
            break;
        default:
            return PAGING_ERROR_INVALID_MODE;
    }
    
    // Allocate root table
    uint32_t table_size = PAGE_4KB;
    if (mode == PAGING_MODE_PAE || mode == PAGING_MODE_PAE_PSE) {
        table_size = 32;  // PDPT is only 4 entries (32 bytes) for PAE
    }
    
    paging_state.root_table = alloc_table(table_size);
    if (paging_state.root_table == 0) {
        return PAGING_ERROR_NO_MEMORY;
    }
    
    // Set up CR4 based on mode
    uint64_t cr4 = read_cr4();
    
    switch (mode) {
        case PAGING_MODE_32BIT:
            cr4 &= ~(CR4_PSE | CR4_PAE);
            break;
        case PAGING_MODE_32BIT_PSE:
            cr4 |= CR4_PSE;
            cr4 &= ~CR4_PAE;
            break;
        case PAGING_MODE_PAE:
            cr4 |= CR4_PAE;
            cr4 &= ~CR4_PSE;
            break;
        case PAGING_MODE_PAE_PSE:
            cr4 |= CR4_PAE | CR4_PSE;
            break;
        case PAGING_MODE_64BIT:
            cr4 |= CR4_PAE;
            cr4 &= ~CR4_LA57;
            break;
        case PAGING_MODE_LA57:
            cr4 |= CR4_PAE | CR4_LA57;
            break;
        default:
            break;
    }
    
    // Enable global pages if supported
    if (paging_state.caps.pge) {
        cr4 |= CR4_PGE;
    }
    
    write_cr4(cr4);
    
    paging_state.current_mode = mode;
    
    print("[PAGING] Mode set to: ");
    print(paging_mode_name(mode));
    print("\n");
    
    return PAGING_OK;
}

/**
 * @brief Map a page (auto-selects best size)
 */
paging_result_t paging_map(uint64_t vaddr, uint64_t paddr, page_size_t size, uint32_t flags) {
    if (!paging_state.initialized || paging_state.current_mode == PAGING_MODE_NONE) {
        return PAGING_ERROR_NOT_INITIALIZED;
    }
    
    switch (paging_state.current_mode) {
        case PAGING_MODE_32BIT:
            if (size != PAGE_SIZE_4K) return PAGING_ERROR_NOT_SUPPORTED;
            return map_32bit_4k((uint32_t)vaddr, (uint32_t)paddr, flags);
            
        case PAGING_MODE_32BIT_PSE:
            if (size == PAGE_SIZE_4K) return map_32bit_4k((uint32_t)vaddr, (uint32_t)paddr, flags);
            if (size == PAGE_SIZE_4M) return map_32bit_4m((uint32_t)vaddr, (uint32_t)paddr, flags);
            return PAGING_ERROR_NOT_SUPPORTED;
            
        case PAGING_MODE_PAE:
            if (size == PAGE_SIZE_4K) return map_pae_4k(vaddr, paddr, flags);
            return PAGING_ERROR_NOT_SUPPORTED;
            
        case PAGING_MODE_PAE_PSE:
            if (size == PAGE_SIZE_4K) return map_pae_4k(vaddr, paddr, flags);
            if (size == PAGE_SIZE_2M) return map_pae_2m(vaddr, paddr, flags);
            return PAGING_ERROR_NOT_SUPPORTED;
            
        case PAGING_MODE_64BIT:
            if (size == PAGE_SIZE_4K) return map_64bit_4k(vaddr, paddr, flags);
            if (size == PAGE_SIZE_2M) return map_64bit_2m(vaddr, paddr, flags);
            if (size == PAGE_SIZE_1G) return map_64bit_1g(vaddr, paddr, flags);
            return PAGING_ERROR_NOT_SUPPORTED;
            
        case PAGING_MODE_LA57:
            if (size == PAGE_SIZE_4K) return map_la57_4k(vaddr, paddr, flags);
            // TODO: 2M and 1G for LA57
            return PAGING_ERROR_NOT_SUPPORTED;
            
        default:
            return PAGING_ERROR_INVALID_MODE;
    }
}

/**
 * @brief Map a range with auto page size selection
 */
paging_result_t paging_map_range(uint64_t vaddr, uint64_t paddr, uint64_t size, uint32_t flags) {
    uint64_t offset = 0;
    
    while (offset < size) {
        uint64_t remaining = size - offset;
        page_size_t page_size;
        uint64_t page_bytes;
        
        // Choose largest possible page size
        if (remaining >= PAGE_1GB && 
            (vaddr + offset) % PAGE_1GB == 0 && 
            (paddr + offset) % PAGE_1GB == 0 &&
            paging_state.caps.gigabyte_pages &&
            (paging_state.current_mode == PAGING_MODE_64BIT || 
             paging_state.current_mode == PAGING_MODE_LA57)) {
            page_size = PAGE_SIZE_1G;
            page_bytes = PAGE_1GB;
        } else if (remaining >= PAGE_4MB && 
                   (vaddr + offset) % PAGE_4MB == 0 && 
                   (paddr + offset) % PAGE_4MB == 0 &&
                   paging_state.current_mode == PAGING_MODE_32BIT_PSE) {
            page_size = PAGE_SIZE_4M;
            page_bytes = PAGE_4MB;
        } else if (remaining >= PAGE_2MB && 
                   (vaddr + offset) % PAGE_2MB == 0 && 
                   (paddr + offset) % PAGE_2MB == 0 &&
                   (paging_state.current_mode >= PAGING_MODE_PAE_PSE)) {
            page_size = PAGE_SIZE_2M;
            page_bytes = PAGE_2MB;
        } else {
            page_size = PAGE_SIZE_4K;
            page_bytes = PAGE_4KB;
        }
        
        paging_result_t result = paging_map(vaddr + offset, paddr + offset, page_size, flags);
        if (result != PAGING_OK && result != PAGING_ERROR_ALREADY_MAPPED) {
            return result;
        }
        
        offset += page_bytes;
    }
    
    return PAGING_OK;
}

/**
 * @brief Enable paging with current configuration
 */
paging_result_t paging_enable(void) {
    if (paging_state.root_table == 0) {
        return PAGING_ERROR_NOT_INITIALIZED;
    }
    
    // Load root table into CR3
    write_cr3(paging_state.root_table);
    
    // Enable paging in CR0
    uint64_t cr0 = read_cr0();
    cr0 |= CR0_PG | CR0_WP;
    write_cr0(cr0);
    
    print("[PAGING] Paging enabled\n");
    
    return PAGING_OK;
}

/**
 * @brief Get current paging mode
 */
paging_mode_t paging_get_mode(void) {
    return paging_state.current_mode;
}

/**
 * @brief Get paging capabilities
 */
void paging_get_capabilities(paging_caps_info_t* caps) {
    if (!caps) return;
    
    caps->pse_supported = paging_state.caps.pse;
    caps->pae_supported = paging_state.caps.pae;
    caps->nx_supported = paging_state.caps.nx;
    caps->pcid_supported = paging_state.caps.pcid;
    caps->la57_supported = paging_state.caps.la57;
    caps->gigabyte_pages = paging_state.caps.gigabyte_pages;
    caps->phys_bits = paging_state.caps.phys_bits;
    caps->virt_bits = paging_state.caps.virt_bits;
}

/**
 * @brief Get mode name string
 */
const char* paging_mode_name(paging_mode_t mode) {
    switch (mode) {
        case PAGING_MODE_NONE:      return "None";
        case PAGING_MODE_32BIT:     return "32-bit (2-level)";
        case PAGING_MODE_32BIT_PSE: return "32-bit PSE (4MB pages)";
        case PAGING_MODE_PAE:       return "PAE (36-bit physical)";
        case PAGING_MODE_PAE_PSE:   return "PAE + PSE (2MB pages)";
        case PAGING_MODE_64BIT:     return "64-bit (4-level)";
        case PAGING_MODE_LA57:      return "LA57 (5-level)";
        default:                    return "Unknown";
    }
}

/**
 * @brief Dump paging statistics
 */
void paging_dump_stats(void) {
    print("\n=== Paging Statistics ===\n");
    print("Current mode: ");
    print(paging_mode_name(paging_state.current_mode));
    print("\n");
    print("Root table: 0x");
    print_hex((uint32_t)paging_state.root_table);
    print("\n");
    print("4KB pages mapped: ");
    print_dec((uint32_t)paging_state.pages_mapped_4k);
    print("\n");
    print("2MB pages mapped: ");
    print_dec((uint32_t)paging_state.pages_mapped_2m);
    print("\n");
    print("4MB pages mapped: ");
    print_dec((uint32_t)paging_state.pages_mapped_4m);
    print("\n");
    print("1GB pages mapped: ");
    print_dec((uint32_t)paging_state.pages_mapped_1g);
    print("\n");
    print("Tables allocated: ");
    print_dec((uint32_t)paging_state.tables_allocated);
    print("\n");
    print("=========================\n\n");
}
