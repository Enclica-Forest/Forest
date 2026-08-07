/**
 * @file paging64.h
 * @brief 64-bit (Long Mode) Paging Interface
 *
 * Supports:
 * - 4-level paging (PML4, PDPT, PD, PT) - standard x86_64
 * - 5-level paging (PML5, PML4, PDPT, PD, PT) - LA57
 * - Page sizes: 4KB, 2MB, 1GB
 * - NX bit, Global pages, PCID
 * - Recursive mapping
 *
 * Virtual Address Formats:
 * - 4-level (48-bit): | Sign Ext | PML4 | PDPT | PD | PT | Offset |
 * - 5-level (57-bit): | Sign Ext | PML5 | PML4 | PDPT | PD | PT | Offset |
 *
 * Entry bit layout (64-bit PTE/PDE/PDPTE/PML4E):
 *   Bit  0   : Present
 *   Bit  1   : Read/Write (1 = writable)
 *   Bit  2   : User/Supervisor (1 = user accessible)
 *   Bit  3   : PWT - Page Write-Through
 *   Bit  4   : PCD - Page Cache Disable
 *   Bit  5   : Accessed
 *   Bit  6   : Dirty (PTE only)
 *   Bit  7   : PS  - Page Size (2MB in PD, 1GB in PDPT; must be 0 in PML4)
 *   Bit  8   : Global (PTE only, requires CR4.PGE)
 *   Bits 9-11: Available to OS
 *   Bits 12-51: Physical frame address (frame_phys >> 12, 40 bits)
 *   Bits 52-62: Available to OS
 *   Bit 63   : XD - Execute Disable (requires IA32_EFER.NXE=1)
 */

#ifndef PAGING64_H
#define PAGING64_H

#include "memory.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __x86_64__

/* -------------------------------------------------------------------------
 * Page-entry type and table type
 * ------------------------------------------------------------------------- */

/** One 64-bit page table entry (used at all four levels). */
typedef uint64_t pte64_entry_t;

/**
 * @brief A single 4KB-aligned page table at any level (512 × 8 = 4096 bytes).
 *
 * pml4_t is used for the PML4, but the same layout is used for PDPT, PD,
 * and PT — they are all 512-entry arrays of 64-bit values.
 */
typedef uint64_t pml4_t[512];

/* -------------------------------------------------------------------------
 * Higher-half kernel virtual base
 * ------------------------------------------------------------------------- */

/** Kernel higher-half base: physical 0 maps to this virtual address. */
#define KERNEL_HIGHER_HALF_OFFSET  0xFFFFFFFF80000000ULL

/**
 * @brief Page flags for 64-bit paging
 */
#define PAGE64_PRESENT      0x001
#define PAGE64_WRITABLE     0x002
#define PAGE64_USER         0x004
#define PAGE64_PWT          0x008
#define PAGE64_PCD          0x010
#define PAGE64_ACCESSED     0x020
#define PAGE64_DIRTY        0x040
#define PAGE64_HUGE         0x080
#define PAGE64_GLOBAL       0x100
#define PAGE64_NX           0x8000000000000000ULL

/* Convenience aliases matching the requested API flag names.
 * memory.h already defines PAGE_PRESENT/PAGE_WRITABLE/PAGE_USER with the
 * same numeric values; only define here if they are absent. */
#ifndef PAGE_PRESENT
#define PAGE_PRESENT    PAGE64_PRESENT
#endif
#ifndef PAGE_WRITABLE
#define PAGE_WRITABLE   PAGE64_WRITABLE
#endif
#ifndef PAGE_USER
#define PAGE_USER       PAGE64_USER
#endif

/**
 * @brief Page size constants
 */
#define PAGE64_SIZE_4K      0x1000ULL
#define PAGE64_SIZE_2M      0x200000ULL
#define PAGE64_SIZE_1G      0x40000000ULL
#define PAGE64_SIZE_512G    0x8000000000ULL

/**
 * @brief Address space limits
 */
#define VADDR_MAX_4LEVEL    0x0000FFFFFFFFFFFFULL   // 48-bit
#define VADDR_MAX_5LEVEL    0x01FFFFFFFFFFFFFFULL   // 57-bit
#define PADDR_MAX_52BIT     0x000FFFFFFFFFFFFFULL   // 52-bit physical

/* -------------------------------------------------------------------------
 * Page level shift constants
 * ------------------------------------------------------------------------- */
#define PML5_SHIFT          48
#define PML4_SHIFT_4L       39
#define PDPT_SHIFT_4L       30
#define PD_SHIFT_4L         21
#define PT_SHIFT_4L         12

/* 5-level VA decomposition */
#define PML5_INDEX(va)      (((uint64_t)(va) >> PML5_SHIFT) & 0x1FF)

/**
 * @brief Initialize 64-bit paging
 * @return MEMORY_OK on success
 */
memory_result_t paging64_init(void);

/**
 * @brief Map a 4KB page
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address
 * @param paddr Physical address
 * @param flags Page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_map_page_4k(uint64_t pml4_phys, uint64_t vaddr,
                                      uint64_t paddr, uint32_t flags);

/**
 * @brief Map a 2MB huge page
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address (2MB aligned)
 * @param paddr Physical address (2MB aligned)
 * @param flags Page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_map_page_2m(uint64_t pml4_phys, uint64_t vaddr,
                                      uint64_t paddr, uint32_t flags);

/**
 * @brief Map a 1GB huge page (if supported)
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address (1GB aligned)
 * @param paddr Physical address (1GB aligned)
 * @param flags Page flags
 * @return MEMORY_OK on success, MEMORY_ERROR_NOT_INITIALIZED if not supported
 */
memory_result_t paging64_map_page_1g(uint64_t pml4_phys, uint64_t vaddr,
                                      uint64_t paddr, uint32_t flags);

/**
 * @brief Unmap a page (any size)
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address
 * @return MEMORY_OK on success
 */
memory_result_t paging64_unmap_page(uint64_t pml4_phys, uint64_t vaddr);

/**
 * @brief Translate virtual to physical address
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address
 * @return Physical address, or 0 if not mapped
 */
uint64_t paging64_virt_to_phys(uint64_t pml4_phys, uint64_t vaddr);

/**
 * @brief Load PML4 into CR3
 * @param pml4_phys Physical address of PML4
 */
void paging64_load_pml4(uint64_t pml4_phys);

/**
 * @brief Get current PML4 from CR3
 * @return Physical address of current PML4
 */
uint64_t paging64_get_current_pml4(void);

/**
 * @brief Check if NX bit is supported
 */
bool paging64_nx_supported(void);

/**
 * @brief Check if 1GB pages are supported
 */
bool paging64_1gb_supported(void);

/**
 * @brief Check if 5-level paging (LA57) is supported
 */
bool paging64_la57_supported(void);

/**
 * @brief Enable 5-level paging
 * Must be called before paging is enabled
 * @return true if successfully enabled
 */
bool paging64_enable_la57(void);

/**
 * @brief Check if currently using 5-level paging
 */
bool paging64_is_la57_active(void);

/**
 * @brief Get the root table level (4 or 5) based on LA57 state
 * @return 5 if 5-level paging is active, 4 otherwise
 */
int paging64_get_root_level(void);

/**
 * @brief Map a 4KB page using 5-level paging
 * @param pml5_phys Physical address of PML5
 * @param vaddr Virtual address
 * @param paddr Physical address
 * @param flags Page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_map_page_5level(uint64_t pml5_phys, uint64_t vaddr,
                                         uint64_t paddr, uint32_t flags);

/**
 * @brief Map a 2MB page using 5-level paging
 * @param pml5_phys Physical address of PML5
 * @param vaddr Virtual address (2MB aligned)
 * @param paddr Physical address (2MB aligned)
 * @param flags Page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_map_page_2m_5level(uint64_t pml5_phys, uint64_t vaddr,
                                            uint64_t paddr, uint32_t flags);

/**
 * @brief Map a 1GB page using 5-level paging
 * @param pml5_phys Physical address of PML5
 * @param vaddr Virtual address (1GB aligned)
 * @param paddr Physical address (1GB aligned)
 * @param flags Page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_map_page_1g_5level(uint64_t pml5_phys, uint64_t vaddr,
                                            uint64_t paddr, uint32_t flags);

/**
 * @brief Translate virtual to physical address using 5-level paging
 * @param pml5_phys Physical address of PML5
 * @param vaddr Virtual address
 * @return Physical address, or 0 if not mapped
 */
uint64_t paging64_virt_to_phys_5level(uint64_t pml5_phys, uint64_t vaddr);

/**
 * @brief Unmap a page using 5-level paging
 * @param pml5_phys Physical address of PML5
 * @param vaddr Virtual address
 * @return MEMORY_OK on success
 */
memory_result_t paging64_unmap_page_5level(uint64_t pml5_phys, uint64_t vaddr);

/**
 * @brief Map a 512GB region (PML5 level, LA57 only)
 * @param pml5_phys Physical address of PML5 (only for LA57)
 * @param vaddr Virtual address (512GB aligned)
 * @param paddr Physical address (512GB aligned)
 * @param flags Page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_map_region_512g(uint64_t pml5_phys, uint64_t vaddr,
                                          uint64_t paddr, uint32_t flags);

/**
 * @brief Create new address space
 * @return Physical address of new PML4/PML5, or 0 on failure
 */
uint64_t paging64_create_address_space(void);

/**
 * @brief Clone address space (for fork)
 * @param src_pml4 Source PML4/PML5
 * @return Physical address of cloned structure
 */
uint64_t paging64_clone_address_space(uint64_t src_pml4);

/**
 * @brief Free address space
 * @param pml4_phys Physical address of PML4/PML5 to free
 */
void paging64_free_address_space(uint64_t pml4_phys);

/**
 * @brief Get page size at virtual address
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address
 * @return Page size (4K, 2M, or 1G), or 0 if not mapped
 */
uint64_t paging64_get_page_size(uint64_t pml4_phys, uint64_t vaddr);

/**
 * @brief Change page protection
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address
 * @param flags New page flags
 * @return MEMORY_OK on success
 */
memory_result_t paging64_change_protection(uint64_t pml4_phys, uint64_t vaddr, uint32_t flags);

/**
 * @brief Dump page table entry for debugging
 * @param pml4_phys Physical address of PML4
 * @param vaddr Virtual address
 */
void paging64_dump_entry(uint64_t pml4_phys, uint64_t vaddr);

/* =========================================================================
 * New pml4_t* -based API (x64_* prefix)
 *
 * These functions accept a typed pml4_t* instead of a raw uint64_t
 * physical address.  They wrap the lower-level paging64_* functions and
 * add the standalone helpers (invlpg, enable_nx) that were missing.
 * ========================================================================= */

/**
 * @brief Allocate and zero a new PML4 table (4KB aligned frame).
 * @return Pointer to the new PML4 (physical == virtual before paging), or
 *         NULL on allocation failure.
 */
pml4_t* pml4_create(void);

/**
 * @brief Map a 4KB virtual page to a physical frame.
 * @param pml4  Pointer to the PML4 table.
 * @param virt  Virtual address (will be 4KB-aligned internally).
 * @param phys  Physical address (will be 4KB-aligned internally).
 * @param flags Combination of PAGE64_PRESENT | PAGE64_WRITABLE | PAGE64_USER
 *              | PAGE64_GLOBAL | PAGE64_NX | PAGE64_PWT | PAGE64_PCD etc.
 * @return MEMORY_OK on success, or a MEMORY_ERROR_* code.
 */
int x64_map_page(pml4_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * @brief Unmap a 4KB virtual page (clears the Present bit).
 * @param pml4  Pointer to the PML4 table.
 * @param virt  Virtual address of the page to unmap.
 * @return MEMORY_OK on success, MEMORY_ERROR_NOT_MAPPED if not mapped.
 */
int x64_unmap_page(pml4_t* pml4, uint64_t virt);

/**
 * @brief Translate a virtual address to its physical address.
 * @param pml4  Pointer to the PML4 table.
 * @param virt  Virtual address to translate.
 * @return Physical address, or 0 if the page is not mapped.
 */
uint64_t x64_get_phys(pml4_t* pml4, uint64_t virt);

/**
 * @brief Check whether a virtual address is currently mapped.
 * @param pml4  Pointer to the PML4 table.
 * @param virt  Virtual address to check.
 * @return true if mapped and present, false otherwise.
 */
bool x64_is_mapped(pml4_t* pml4, uint64_t virt);

/**
 * @brief Identity-map a physical range [start, end) so that virt == phys.
 *
 * Maps every 4KB page in [start, end) with the supplied flags.  Both
 * start and end are rounded to 4KB boundaries.
 *
 * @param pml4  Pointer to the PML4 table.
 * @param start Physical (and virtual) start address.
 * @param end   Physical (and virtual) end address (exclusive).
 * @param flags Page flags (e.g. PAGE64_PRESENT | PAGE64_WRITABLE).
 */
void x64_identity_map_range(pml4_t* pml4, uint64_t start, uint64_t end,
                             uint64_t flags);

/**
 * @brief Map the kernel into the higher half of virtual address space.
 *
 * Maps physical [phys_start, phys_end) to virtual addresses starting at
 * KERNEL_HIGHER_HALF_OFFSET (0xFFFFFFFF80000000).  Uses 4KB pages so that
 * individual sections can later have their permissions refined.
 *
 * @param pml4       Pointer to the PML4 table.
 * @param phys_start Start of the physical kernel range (inclusive).
 * @param phys_end   End of the physical kernel range (exclusive).
 */
void x64_map_kernel_higher_half(pml4_t* pml4, uint64_t phys_start,
                                 uint64_t phys_end);

/**
 * @brief Load a PML4 into CR3, flushing the entire TLB.
 * @param pml4  Pointer to the PML4 table (must be physically accessible,
 *              i.e. identity-mapped or pre-paging).
 */
void x64_load_pml4(pml4_t* pml4);

/**
 * @brief Invalidate a single page in the TLB using the INVLPG instruction.
 * @param vaddr Virtual address of the page to invalidate.
 */
void x64_invlpg(uint64_t vaddr);

/* --------------------------------------------------------------------------
 * 5-level paging (LA57) variants of x64_* API
 * -------------------------------------------------------------------------- */

/**
 * @brief Map a 4KB page using 5-level paging via pml5_t*.
 * @param pml5  Pointer to the PML5 table.
 * @param virt  Virtual address (4KB-aligned internally).
 * @param phys  Physical address (4KB-aligned internally).
 * @param flags PAGE64_* flag combination.
 * @return MEMORY_OK on success, or MEMORY_ERROR_* code.
 */
int x64_map_page_5level(void* pml5, uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * @brief Unmap a 4KB page using 5-level paging.
 * @param pml5  Pointer to the PML5 table.
 * @param virt  Virtual address to unmap.
 * @return MEMORY_OK on success.
 */
int x64_unmap_page_5level(void* pml5, uint64_t virt);

/**
 * @brief Translate virtual to physical using 5-level paging.
 * @param pml5  Pointer to the PML5 table.
 * @param virt  Virtual address to translate.
 * @return Physical address, or 0 if not mapped.
 */
uint64_t x64_get_phys_5level(void* pml5, uint64_t virt);

/**
 * @brief Check whether a virtual address is mapped using 5-level paging.
 * @param pml5  Pointer to the PML5 table.
 * @param virt  Virtual address to check.
 * @return true if mapped, false otherwise.
 */
bool x64_is_mapped_5level(void* pml5, uint64_t virt);

/**
 * @brief Identity-map a physical range using 5-level paging.
 * @param pml5  Pointer to the PML5 table.
 * @param start Physical (and virtual) start address.
 * @param end   Physical (and virtual) end address (exclusive).
 * @param flags PAGE64_* flag combination.
 */
void x64_identity_map_range_5level(void* pml5, uint64_t start, uint64_t end,
                                   uint64_t flags);

/**
 * @brief Map the kernel into the higher half using 5-level paging.
 * @param pml5       Pointer to the PML5 table.
 * @param phys_start Start of physical kernel range.
 * @param phys_end   End of physical kernel range.
 */
void x64_map_kernel_higher_half_5level(void* pml5, uint64_t phys_start,
                                       uint64_t phys_end);

/**
 * @brief Enable the No-Execute (NX / XD) bit via IA32_EFER MSR.
 *
 * Sets bit 11 (NXE) in IA32_EFER (MSR 0xC0000080).  Must be called before
 * loading page tables that have PAGE64_NX set on any entry; otherwise a
 * general-protection fault will occur when those entries are used.
 *
 * Does nothing if the CPU does not advertise NX support (CPUID
 * 0x80000001:EDX[20]).
 */
void x64_enable_nx(void);

/* =========================================================================
 * High-level pml4_* management API (requested interface)
 *
 * These wrap the lower-level paging64_* / x64_* functions behind the
 * short names used by the rest of the kernel (scheduler, fork, exec).
 * ========================================================================= */

/**
 * @brief Allocate a fresh, zeroed PML4 and copy the kernel (upper-half)
 *        entries from the current address space into it.
 *
 * Returns a pointer to the new PML4 (for the user portion to be filled in
 * by the caller) or NULL on OOM.  Alias of pml4_create() with kernel
 *        mappings pre-populated.
 */
pml4_t* pml4_alloc(void);

/**
 * @brief Map a 4KB page in the given PML4.
 *
 * Thin wrapper around x64_map_page() that returns 0 on success and
 * -1 on failure, matching the convention used by the scheduler.
 *
 * @param pml4   PML4 table pointer.
 * @param vaddr  Virtual address (page-aligned internally).
 * @param paddr  Physical address (page-aligned internally).
 * @param flags  PAGE64_* flag combination.
 * @return 0 on success, -1 on failure.
 */
int pml4_map(pml4_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/**
 * @brief Unmap a 4KB page from the given PML4.
 * @return 0 on success, -1 if not mapped.
 */
int pml4_unmap(pml4_t* pml4, uint64_t vaddr);

/**
 * @brief Clone an address space for fork(2).
 *
 * Copies the user-space (lower-half) PML4 entries from @p src into a newly
 * allocated PML4.  Kernel (upper-half) entries are shared by reference.
 *
 * The clone currently performs a shallow copy of user PML4 entries; a
 * full copy-on-write implementation would mark the copied entries read-
 * only and wire up a #PF handler.  The infrastructure is in place — see
 * paging64_clone_address_space() for the physical-address variant.
 *
 * @param src  Source PML4 table pointer.
 * @return New PML4 pointer, or NULL on OOM.
 */
pml4_t* pml4_clone(pml4_t* src);

/**
 * @brief Switch the active address space by loading @p pml4 into CR3.
 *
 * Honours PCID when ENABLE_PCID is on and CR4.PCIDE is set: the caller
 * may pass a 12-bit PCID in the high bits of @p pml4 (CR3[11:0] is the
 * PCID when PCIDE is enabled).  Otherwise behaves as a plain CR3 write
 * which flushes non-global TLB entries.
 *
 * @param pml4  PML4 table pointer (or PCID-tagged address).
 */
void pml4_switch(pml4_t* pml4);

/* =========================================================================
 * PCID (Process Context ID) support
 *
 * When CR4.PCIDE is set, CR3[11:0] carries a 12-bit ASID/PCID that tags
 * TLB entries so a context switch does not require a full TLB flush.
 * ========================================================================= */

#if ENABLE_PCID
#define X86_64_PCID_BITS        12
#define X86_64_PCID_MASK        ((1ULL << X86_64_PCID_BITS) - 1)
#define X86_64_PCID_MAX         X86_64_PCID_MASK

/**
 * @brief Allocate a fresh PCID (1..4095).  0 is reserved for the kernel.
 * @return A valid PCID, or 0 if all are in use.
 */
uint16_t paging64_pcid_alloc(void);

/**
 * @brief Release a PCID previously allocated with paging64_pcid_alloc().
 */
void paging64_pcid_free(uint16_t pcid);

/**
 * @brief Build a CR3 value that combines @p pml4_phys with @p pcid.
 */
static inline uint64_t paging64_cr3_with_pcid(uint64_t pml4_phys, uint16_t pcid)
{
    return (pml4_phys & ~X86_64_PCID_MASK) | (pcid & X86_64_PCID_MASK);
}
#endif /* ENABLE_PCID */

/* =========================================================================
 * Kernel ASLR (ENABLE_KASLR)
 *
 * When enabled, paging64_kaslr_offset() returns a random 2 MiB-aligned
 * delta applied to the kernel's high-half virtual base at boot.  The
 * delta is chosen from [0, 512 MiB) using RDTSC as the entropy source.
 *
 * NOTE: full KASLR requires the kernel image to be position-independent
 * or to carry a relocation table that the early boot code applies.  The
 * helper here only provides the offset and mapping; relocation itself is
 * the responsibility of the loader/boot code.
 * ========================================================================= */

#if ENABLE_KASLR
/** Maximum randomisation range for the kernel high-half base (512 MiB). */
#define KASLR_RANGE_BYTES   0x20000000ULL

/**
 * @brief Compute the KASLR offset to add to KERNEL_HIGHER_HALF_OFFSET.
 * @return A 2 MiB-aligned offset in [0, KASLR_RANGE_BYTES).
 */
uint64_t paging64_kaslr_offset(void);

/**
 * @brief Effective kernel virtual base (KERNEL_HIGHER_HALF_OFFSET +
 *        kaslr offset).
 */
uint64_t paging64_kernel_base(void);
#endif

#endif /* __x86_64__ */

#endif /* PAGING64_H */
