#include "include/memory.h"
#include "include/screen.h"
#include "include/system.h" // For cpu_get_cr0, cpu_set_cr0, etc.
#include "include/panic.h"  // For kernel_panic
#include "include/string.h" // For memset
#include "include/debuglog.h"
#include "include/mm_cow.h" // For cow_get_refcount()/cow_release() - COW-aware frame teardown

#define VMM_DEFAULT_IDENTITY_LIMIT_BYTES (64 * 1024 * 1024)   // Map first 64MB identity for firmware tables
#define KERNEL_HIGHER_HALF_BASE   0xC0000000

// Temporary mapping area for page table access.
//
// Keep this in kernel high-half space so it cannot collide with user mappings
// or low identity-mapped regions that may be repurposed by other subsystems.
// 0xD0000000 is above user space and away from kernel image base (0xC0000000).
#define VMM_TEMP_MAP_BASE         0xD0000000
#define VMM_TEMP_MAP_SIZE         0x400000    // 4MB for temporary mappings
#define VMM_TEMP_MAP_PAGES        (VMM_TEMP_MAP_SIZE / MEMORY_PAGE_SIZE)  // 1024 pages
#define VMM_TEMP_MAP_DATA_PAGES   (VMM_TEMP_MAP_PAGES - 1)  // reserve last page for PT alias
#define VMM_TEMP_PT_ALIAS_VADDR   (VMM_TEMP_MAP_BASE + VMM_TEMP_MAP_SIZE - MEMORY_PAGE_SIZE)

#define VMM_DEBUG_LOG 0

extern char kernel_start;
extern char kernel_end;

static inline uint32 align_up(uint32 value, uint32 align) {
    return (value + align - 1) & ~(align - 1);
}

static inline bool vmm_is_addr_valid(uint32 addr) {
    return addr <= MEMORY_MAX_ADDR;
}

static inline void vmm_pretouch_identity_page(uint32 addr) {
    if (addr < MEMORY_PRETOUCH_LIMIT_BYTES) {
        volatile uint8_t* ptr = (volatile uint8_t*)addr;
        (void)*ptr;
    }
}

static inline uint32 vmm_get_active_cr3_phys(void) {
    uint32 cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

#if VMM_DEBUG_LOG
static inline void vmm_log_text(const char* text) {
    if (debuglog_is_ready()) {
        debuglog_write(text);
    }
}

static inline void vmm_log_hex(uint32 value) {
    if (debuglog_is_ready()) {
        debuglog_write_hex(value);
    }
}

static inline void vmm_log_dec(uint32 value) {
    if (debuglog_is_ready()) {
        debuglog_write_dec(value);
    }
}

#define print(text) do { vmm_log_text(text); } while (0)
#define print_hex(value) do { vmm_log_hex(value); } while (0)
#define print_dec(value) do { vmm_log_dec(value); } while (0)
#else
#define print(text) do {} while (0)
#define print_hex(value) do {} while (0)
#define print_dec(value) do {} while (0)
#endif

// VMM internal state
static struct {
    bool initialized;
    page_directory_t* kernel_directory; // Physical address of the kernel page directory
    page_directory_t* current_directory; // Physical address of the current active page directory
    uint32 temp_map_next; // Next available temporary mapping slot
    page_table_t* temp_map_pt_alias; // Stable kernel virtual alias to temp-map PT
    uint32 temp_map_pt_phys; // Physical frame backing the temp-map page table
    uint32 temp_map_pd_index; // PDE index containing the temp-map window
    bool paging_enabled;  // Track if paging is enabled
} vmm_state = {0};

// Highest physical address (exclusive) that the kernel's boot-time identity
// map actually covers. Defaults to MEMORY_USER_START to preserve prior
// behavior until memory.c calls vmm_set_identity_map_limit() with the real
// boundary (kernel_map_end, which is typically far below MEMORY_USER_START -
// e.g. ~200MB vs. the 1GB MEMORY_USER_START). Physical frames pmm_alloc_frame()
// hands out beyond the real boot identity map (e.g. new user-space page
// tables allocated from high physical memory) are NOT identity mapped in any
// page directory, so treating them as valid kernel virtual pointers here
// causes a kernel-mode page fault the first time such a frame is used as a
// page table. See get_page_entry()'s vmm_temp_map_page() fallback, which
// this limit exists to route those frames through instead.
static uint32 vmm_identity_map_actual_limit = MEMORY_USER_START;

void vmm_set_identity_map_limit(uint32 limit) {
    vmm_identity_map_actual_limit = limit;
}

static inline page_table_t* vmm_phys_pt_as_virt(uint32 pt_phys) {
    // Kernel identity maps low physical memory up to whatever the boot-time
    // identity map actually covers (see vmm_set_identity_map_limit()).
    if (pt_phys == 0 || pt_phys >= vmm_identity_map_actual_limit) {
        return NULL;
    }
    return (page_table_t*)pt_phys;
}

// =============================================================================
// TEMPORARY MAPPING FOR PAGE TABLE ACCESS
// =============================================================================

// Temporarily map a physical page to a virtual address for access.
//
// The temp-map page table is also mapped at a stable kernel alias
// (VMM_TEMP_PT_ALIAS_VADDR) during vmm_init, so paging-on access does not
// depend on identity-mapped physical frames.
void* vmm_temp_map_page(uint32 phys_addr) {
    if (!vmm_state.paging_enabled) {
        // Paging not yet on — physical == virtual, access directly.
        return (void*)phys_addr;
    }

    // Round-robin slot in the temporary mapping window.
    uint32 slot = __sync_fetch_and_add(&vmm_state.temp_map_next, 1) % VMM_TEMP_MAP_DATA_PAGES;

    uint32 temp_vaddr = VMM_TEMP_MAP_BASE + (slot * MEMORY_PAGE_SIZE);

    uint32 page_num = temp_vaddr / MEMORY_PAGE_SIZE;
    uint32 pd_index = page_num / 1024;
    uint32 pt_index = page_num % 1024;

    page_entry_t* pde = &(*vmm_state.current_directory)[pd_index];

    if (!pde->present) {
        // The temp-map PDE must be present when paging is enabled.
        return NULL;
    }

    uint32 pt_phys = ((uint32)pde->frame) << MEMORY_PAGE_SHIFT;

    page_table_t* pt = vmm_phys_pt_as_virt(pt_phys);
    if (!pt) {
        // Fallback for non-identity-mapped PTs (should not happen in early boot).
        pt = vmm_state.temp_map_pt_alias;
        if (!pt) {
            return NULL;
        }
    }

    // Keep the reserved alias entry repaired in case unrelated corruption cleared it.
    if (vmm_state.temp_map_pt_phys != 0) {
        page_table_t* temp_pt_phys = vmm_phys_pt_as_virt(vmm_state.temp_map_pt_phys);
        if (temp_pt_phys) {
            page_entry_t* alias_pte = &(*temp_pt_phys)[VMM_TEMP_MAP_PAGES - 1];
            uint32 alias_phys = ((uint32)alias_pte->frame) << MEMORY_PAGE_SHIFT;
            if (!alias_pte->present || alias_phys != vmm_state.temp_map_pt_phys) {
                alias_pte->frame = vmm_state.temp_map_pt_phys >> MEMORY_PAGE_SHIFT;
                alias_pte->present = 1;
                alias_pte->writable = 1;
                alias_pte->user = 0;
                alias_pte->pwt = 0;
                alias_pte->pcd = 0;
                if (debuglog_is_ready()) {
                    debuglog(DEBUG_WARN,
                             "[VMM] Repaired temp-map alias entry: pt=0x%08x\n",
                             vmm_state.temp_map_pt_phys);
                }
            }
        }
    }
    page_entry_t* pte = &(*pt)[pt_index];

    pte->frame = phys_addr >> MEMORY_PAGE_SHIFT;
    pte->present = 1;
    pte->writable = 1;
    pte->user = 0;

    __asm__ __volatile__("invlpg (%0)" :: "r"(temp_vaddr) : "memory");

    return (void*)temp_vaddr;
}

// Unmap a temporarily mapped page (clears its PTE and flushes TLB).
// See vmm_temp_map_page for the identity-mapped invariant on page table access.
void vmm_temp_unmap_page(void* vaddr) {
    if (!vmm_state.paging_enabled) {
        return;
    }

    uint32 temp_vaddr = (uint32)vaddr;

    if (temp_vaddr < VMM_TEMP_MAP_BASE ||
        temp_vaddr >= VMM_TEMP_MAP_BASE + VMM_TEMP_MAP_SIZE) {
        return;
    }

    // Never unmap the reserved temp PT alias slot.
    if (temp_vaddr >= VMM_TEMP_PT_ALIAS_VADDR) {
        return;
    }

        uint32 page_num = temp_vaddr / MEMORY_PAGE_SIZE;
    uint32 pd_index = page_num / 1024;
    uint32 pt_index = page_num % 1024;

    page_entry_t* pde = &(*vmm_state.current_directory)[pd_index];
    if (!pde->present) {
        return;
    }

    uint32 pt_phys = ((uint32)pde->frame) << MEMORY_PAGE_SHIFT;
    page_table_t* pt = vmm_phys_pt_as_virt(pt_phys);
    if (!pt) {
        // Fallback for non-identity-mapped PTs.
        pt = vmm_state.temp_map_pt_alias;
        if (!pt) {
            return;
        }
    }
    page_entry_t* pte = &(*pt)[pt_index];

    if (pt_index >= (VMM_TEMP_MAP_PAGES - 1)) {
        return;
    }

    pte->present = 0;

    __asm__ __volatile__("invlpg (%0)" :: "r"(temp_vaddr) : "memory");
}

// Helper function to get a page table entry for a given virtual address
// If make is true, and the page table doesn't exist, it allocates one.
static page_entry_t* get_page_entry(uint32 vaddr, bool make, page_directory_t* dir) {
    print("[VMM_DBG] get_page_entry: vaddr=0x"); print_hex(vaddr); print(", make="); print_dec(make); print("\n");

    if (!dir) {
        return NULL;
    }

    uint32 page_num = vaddr / MEMORY_PAGE_SIZE; // Convert to page number
    uint32 pd_index = page_num / 1024; // Page Directory Index
    uint32 pt_index = page_num % 1024; // Page Table Index

    if (pd_index >= 1024 || pt_index >= 1024) {
        return NULL;
    }

    // Check if the page directory entry exists
    page_entry_t* pde = &(*dir)[pd_index];

    if (!pde->present) { // Page table not present
        if (!make) {
            return NULL; // Don't create if not requested
        }

        // Allocate a new page table
        uint32 pt_phys_addr = pmm_alloc_frame();
        if (pt_phys_addr == 0) {
            debuglog(DEBUG_ERROR, "[VMM] CRITICAL: Failed to allocate frame for new page table!\n");
            return NULL; // Out of physical memory
        }

        // Clear the new page table using temporary mapping
        void* temp_pt = vmm_temp_map_page(pt_phys_addr);
        if (!temp_pt) {
            pmm_free_frame(pt_phys_addr);
            return NULL;
        }
        memset(temp_pt, 0, MEMORY_PAGE_SIZE);
        vmm_temp_unmap_page(temp_pt);

        // Set up the page directory entry
        pde->frame = pt_phys_addr >> MEMORY_PAGE_SHIFT;
        pde->present = 1;
        pde->writable = 1; // Default to writable
        pde->user = 0;     // Default to kernel access
        pde->pwt = 0;
        pde->pcd = 0;
    }

    // Now, the page table should exist (either pre-existing or newly created).
    // Prefer direct identity mapping for low physical memory page tables because
    // it is stable across IRQ preemption. Falling back to rotating temp-map
    // aliases here can return pointers that become stale mid-call.
    uint32 pt_phys_addr = ((uint32)pde->frame) << MEMORY_PAGE_SHIFT;
    page_table_t* pt = vmm_phys_pt_as_virt(pt_phys_addr);
    if (!pt) {
        void* temp_pt = vmm_temp_map_page(pt_phys_addr);
        if (!temp_pt) {
            return NULL;
        }
        pt = (page_table_t*)temp_pt;
    }

    return &(*pt)[pt_index];
}

// Map a virtual address to a physical address
// NOTE: For identity mapping (vaddr == paddr), we allow high virtual addresses
// like kernel higher-half (0xC0000000+)
memory_result_t vmm_map_page(page_directory_t* dir, uint32 vaddr, uint32 paddr, uint32 flags) {
    if (!dir) {
        return MEMORY_ERROR_NULL_PTR;
    }

    // Only validate physical address - virtual can be any 32-bit value
    // This allows identity mapping of kernel higher-half
    if (!vmm_is_addr_valid(paddr)) {
        return MEMORY_ERROR_INVALID_ADDR;
    }

    // Ensure addresses are page-aligned
    if ((vaddr & MEMORY_PAGE_MASK) != 0 || (paddr & MEMORY_PAGE_MASK) != 0) {
        print("[VMM_DBG] vmm_map_page: Address not page-aligned. Returning INVALID_ADDR.\n");
        return MEMORY_ERROR_INVALID_ADDR;
    }

    // Ensure the page directory entry allows user-mode access when requested.
    uint32 page_num = vaddr / MEMORY_PAGE_SIZE;
    uint32 pd_index = page_num / 1024;
    if (pd_index >= 1024) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    page_entry_t* pde = &(*dir)[pd_index];

    page_entry_t* page = get_page_entry(vaddr, true, dir);
    if (page == NULL) {
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }

    if (page->present) {
        // If already mapped to the exact same physical address with the same flags, it's OK.
        if ((((uint32)page->frame) << MEMORY_PAGE_SHIFT) == paddr) {
            // Check if flags differ (e.g. PCD/PWT for MMIO re-mapping) — if so, fall
            // through to update the PTE rather than silently keeping stale flags.
            bool same_flags =
                (page->writable == ((flags & PAGE_WRITABLE) ? 1u : 0u)) &&
                (page->user     == ((flags & PAGE_USER)     ? 1u : 0u)) &&
                (page->pwt      == ((flags & PAGE_WRITE_THROUGH) ? 1u : 0u)) &&
                (page->pcd      == ((flags & PAGE_CACHE_DISABLE) ? 1u : 0u));
            if (same_flags) {
                return MEMORY_ERROR_ALREADY_MAPPED;
            }
            // Flags differ — fall through to update the PTE.
        }
        // Different physical address or different flags: overwrite the mapping.
        // Caller must ensure the TLB is flushed after this call.
    }

    page->frame = paddr >> MEMORY_PAGE_SHIFT;
    page->present = (flags & PAGE_PRESENT) ? 1 : 0;
    page->writable = (flags & PAGE_WRITABLE) ? 1 : 0;
    page->user = (flags & PAGE_USER) ? 1 : 0;
    if (flags & PAGE_USER) {
        pde->user = 1; // Without this, ring 3 cannot access the PTE even if page->user is set.
    }
    page->pwt = (flags & PAGE_WRITE_THROUGH) ? 1 : 0;
    page->pcd = (flags & PAGE_CACHE_DISABLE) ? 1 : 0;
    page->accessed = (flags & PAGE_ACCESSED) ? 1 : 0;
    page->dirty = (flags & PAGE_DIRTY) ? 1 : 0;

    // Invalidate TLB for this virtual address if paging is enabled.
    // Without this, stale TLB entries can cause writes to go to old physical
    // frames — catastrophic for the ELF loader's temp-mapping copy path.
    if (vmm_state.paging_enabled) {
        __asm__ __volatile__("invlpg (%0)" :: "r"(vaddr) : "memory");
    }

    return MEMORY_OK;
}

// Unmap a virtual address
memory_result_t vmm_unmap_page(page_directory_t* dir, uint32 vaddr) {
    if (!dir) {
        return MEMORY_ERROR_NULL_PTR;
    }

    if (!vmm_is_addr_valid(vaddr)) {
        return MEMORY_ERROR_INVALID_ADDR;
    }

    if ((vaddr & MEMORY_PAGE_MASK) != 0) {
        return MEMORY_ERROR_INVALID_ADDR;
    }

    page_entry_t* page = get_page_entry(vaddr, false, dir);
    if (page == NULL || !page->present) {
        return MEMORY_ERROR_NOT_MAPPED; // Page not mapped or page table not present
    }

    page->present = 0; // Mark as not present
    // Clear other flags if necessary, but hardware usually ignores if not present

    // Invalidate TLB so the CPU doesn't use the stale mapping.
    if (vmm_state.paging_enabled) {
        __asm__ __volatile__("invlpg (%0)" :: "r"(vaddr) : "memory");
    }

    // TODO: if page table becomes empty, free its frame

    return MEMORY_OK;
}

// Get physical address for a virtual address
uint32 vmm_get_physical_addr(page_directory_t* dir, uint32 vaddr) {
    if (!dir) {
        return 0;
    }

    if (!vmm_is_addr_valid(vaddr)) {
        return 0;
    }

    if ((vaddr & MEMORY_PAGE_MASK) != 0) {
        return 0; // Not page-aligned
    }

    page_entry_t* page = get_page_entry(vaddr, false, dir);
    if (page == NULL || !page->present) {
        return 0; // Not mapped
    }

    return ((((uint32)page->frame) << MEMORY_PAGE_SHIFT) | (vaddr & MEMORY_PAGE_MASK));
}

// Set up the initial kernel page directory and enable paging
memory_result_t vmm_init(void) {
    print("[VMM] Initializing Virtual Memory Manager (new)...\n");

    // Allocate a frame for the kernel page directory
    uint32 kernel_dir_phys = pmm_alloc_frame();
    if (kernel_dir_phys == 0) {
        kernel_panic("VMM: Failed to allocate frame for kernel page directory!");
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }

    // Point vmm_state.kernel_directory to the physical address
    vmm_state.kernel_directory = (page_directory_t*)kernel_dir_phys;
    // Clear the new page directory
    memset((void*)vmm_state.kernel_directory, 0, MEMORY_PAGE_SIZE);

    vmm_state.current_directory = vmm_state.kernel_directory;

    // Identity map a reasonable range of low memory (default 64MB)
    // This covers the kernel, PMM bitmap, and early data structures.
    // Large page tables will be accessed via temporary mapping.
    uint32 identity_limit_kb = memory_get_usable_kb();
    if (identity_limit_kb == 0) {
        identity_limit_kb = VMM_DEFAULT_IDENTITY_LIMIT_BYTES / 1024;
    }

    if (identity_limit_kb < MEMORY_BOOTSTRAP_MIN_IDENTITY_KB) {
        identity_limit_kb = MEMORY_BOOTSTRAP_MIN_IDENTITY_KB;
    }
    if (identity_limit_kb > MEMORY_BOOTSTRAP_MAX_IDENTITY_KB) {
        identity_limit_kb = MEMORY_BOOTSTRAP_MAX_IDENTITY_KB;
    }

    uint32 identity_limit = identity_limit_kb * 1024;

    // Also scan ALL memory regions (including reserved/ACPI) to find the
    // highest physical address.  ACPI tables reside in reserved regions that
    // sit just above the last usable RAM region, so memory_get_usable_kb()
    // underestimates the range that needs identity mapping.  For example, on
    // a 512MB QEMU guest the usable limit is ~0x1FFDE000 but the RSDT is at
    // ~0x1FFE2000 (reserved region 0x1FFE0000-0x20000000).
    {
        uint32 region_count = 0;
        memory_region_t* regions = memory_get_regions(&region_count);
        if (regions && region_count > 0) {
            for (uint32 i = 0; i < region_count; i++) {
                uint64_t end = regions[i].base_address + regions[i].length;
                // Only consider regions below 4GB (we're 32-bit)
                if (end > 0x100000000ULL) {
                    end = 0x100000000ULL;
                }
                uint32 end32 = (uint32)(end & 0xFFFFFFFFULL);
                // Saturate to avoid wrap-around (end32 == 0 means exactly 4GB)
                if (end32 == 0 && end == 0x100000000ULL) {
                    end32 = 0xFFFFFFFF;
                }
                uint32 region_limit_kb = end32 / 1024;
                if (region_limit_kb > identity_limit_kb) {
                    identity_limit_kb = region_limit_kb;
                }
            }
            // Re-clamp after expansion
            if (identity_limit_kb > MEMORY_BOOTSTRAP_MAX_IDENTITY_KB) {
                identity_limit_kb = MEMORY_BOOTSTRAP_MAX_IDENTITY_KB;
            }
            identity_limit = identity_limit_kb * 1024;
        }
    }

    // Ensure identity limit covers all possible physical frame allocations.
    // The heap and PMM bitmap may be pushed above 128MB by a large initrd,
    // so we must identity-map ALL usable RAM.  The ceiling is MEMORY_USER_START
    // (1GB) — everything below that is kernel virtual space available for
    // identity mapping; user space begins above it.
    if (identity_limit < 0x04000000) {
        identity_limit = 0x04000000; // always map at least first 64MB
    }
    if (identity_limit > MEMORY_USER_START) {
        identity_limit = MEMORY_USER_START; // don't overlap user virtual space
    }
    identity_limit = (identity_limit + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);

    print("[VMM] Identity mapping first ");
    print_hex(identity_limit);
    print(" bytes...\n");
    for (uint32 addr = 0; addr < identity_limit; addr += MEMORY_PAGE_SIZE) {
        memory_result_t res = vmm_map_page(vmm_state.kernel_directory, addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
        if (res == MEMORY_ERROR_ALREADY_MAPPED) {
            continue;
        }
        if (res != MEMORY_OK) {
            kernel_panic("VMM: Failed to identity map low memory!");
            return res;
        }

        vmm_pretouch_identity_page(addr);
    }

    // Explicitly identity map VGA text buffer for direct access
    print("[VMM] Identity mapping VGA text buffer 0xB8000...\n");
    memory_result_t res_vga_id = vmm_map_page(vmm_state.kernel_directory, 0xB8000, 0xB8000, PAGE_PRESENT | PAGE_WRITABLE);
    if (res_vga_id != MEMORY_OK && res_vga_id != MEMORY_ERROR_ALREADY_MAPPED) {
        kernel_panic("VMM: Failed to identity map VGA text buffer!");
        return res_vga_id;
    }

    // Map the actual kernel image into the higher half starting at 0xC0000000.
    uint32 kernel_phys_start = (uint32)&kernel_start;
    uint32 kernel_phys_end = align_up((uint32)&kernel_end, MEMORY_PAGE_SIZE);
    uint32 kernel_size = kernel_phys_end - kernel_phys_start;
    print("[VMM] Mapping kernel higher-half. phys_start=0x");
    print_hex(kernel_phys_start);
    print(", phys_end=0x");
    print_hex(kernel_phys_end);
    print("\n");
    for (uint32 offset = 0; offset < kernel_size; offset += MEMORY_PAGE_SIZE) {
        uint32 virt = KERNEL_HIGHER_HALF_BASE + offset;
        uint32 phys = kernel_phys_start + offset;
        memory_result_t map_res = vmm_map_page(
            vmm_state.kernel_directory,
            virt,
            phys,
            PAGE_PRESENT | PAGE_WRITABLE);
        if (map_res != MEMORY_OK && map_res != MEMORY_ERROR_ALREADY_MAPPED) {
            kernel_panic("VMM: Failed to map kernel into higher half!");
            return map_res;
        }
    }

    // Explicitly map VGA text buffer to its higher-half address (e.g., 0xC00B8000)
    print("[VMM] Mapping VGA text buffer to higher-half 0xC00B8000...\n");
    memory_result_t res_vga_hh = vmm_map_page(vmm_state.kernel_directory, 0xC00B8000, 0xB8000, PAGE_PRESENT | PAGE_WRITABLE);
    if (res_vga_hh != MEMORY_OK && res_vga_hh != MEMORY_ERROR_ALREADY_MAPPED) {
        kernel_panic("VMM: Failed to higher-half map VGA text buffer!");
        return res_vga_hh;
    }


    // Set up temporary mapping area for page table access
    // We need to do this manually to avoid circular dependency with get_page_entry
    print("[VMM] Setting up temporary mapping area...\n");

    // Calculate page directory and page table indices for temp mapping area
    uint32 temp_start_page = VMM_TEMP_MAP_BASE / MEMORY_PAGE_SIZE;
    uint32 temp_pd_index = temp_start_page / 1024;
    uint32 temp_pages = VMM_TEMP_MAP_SIZE / MEMORY_PAGE_SIZE;

    // Allocate page tables for the temporary mapping area
    uint32 temp_pt_phys = 0;
    for (uint32 i = 0; i < (temp_pages + 1023) / 1024; i++) {
        uint32 pd_index = temp_pd_index + i;

        if (pd_index >= 1024) {
            kernel_panic("VMM: Temporary mapping area too large!");
            return MEMORY_ERROR_INVALID_ADDR;
        }

        page_entry_t* pde = &(*vmm_state.kernel_directory)[pd_index];

        if (!pde->present) {
            uint32 pt_phys_addr = pmm_alloc_frame();
            if (pt_phys_addr == 0) {
                kernel_panic("VMM: Failed to allocate page table for temporary mapping area!");
                return MEMORY_ERROR_OUT_OF_MEMORY;
            }

            // Clear the page table directly (before paging is enabled)
            memset((void*)pt_phys_addr, 0, MEMORY_PAGE_SIZE);

            pde->frame = pt_phys_addr >> MEMORY_PAGE_SHIFT;
            pde->present = 1;
            pde->writable = 1;
            pde->user = 0;
            if (i == 0) {
                temp_pt_phys = pt_phys_addr;
            }
        } else if (i == 0) {
            temp_pt_phys = ((uint32)pde->frame) << MEMORY_PAGE_SHIFT;
        }
    }

    if (temp_pt_phys == 0) {
        kernel_panic("VMM: Failed to resolve temp-map page table physical address!");
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }

    vmm_state.temp_map_pt_phys = temp_pt_phys;
    vmm_state.temp_map_pd_index = temp_pd_index;

    /* Reserve the last temp-map slot (index 1023) as a self-alias of the
     * temp-map page table frame so runtime accesses never require physical-as-
     * virtual assumptions. This lives in the same PDE as temp-map slots. */
    {
        page_table_t* temp_pt = (page_table_t*)temp_pt_phys; // safe before paging is enabled
        page_entry_t* alias_pte = &(*temp_pt)[VMM_TEMP_MAP_PAGES - 1];
        alias_pte->frame = temp_pt_phys >> MEMORY_PAGE_SHIFT;
        alias_pte->present = 1;
        alias_pte->writable = 1;
        alias_pte->user = 0;
        alias_pte->pwt = 0;
        alias_pte->pcd = 0;
    }
    vmm_state.temp_map_pt_alias = (page_table_t*)VMM_TEMP_PT_ALIAS_VADDR;

    vmm_state.initialized = true;
    vmm_state.temp_map_next = 0;
    print("[VMM] VMM Initialized. Kernel directory at 0x");
    print_hex((uint32)vmm_state.kernel_directory);
    print("\n");

    return MEMORY_OK;
}

// Enable paging for the current_directory
void vmm_enable_paging(void) {
    print("[VMM] Attempting to enable paging.\n");
    if (!vmm_state.initialized) {
        kernel_panic("VMM: Attempt to enable paging before VMM initialization!");
    }

    // Load the physical address of the current page directory into CR3
    cpu_set_cr3((uintptr_t)vmm_state.current_directory);

    // Enable PAE (if needed, but for 32-bit non-PAE, it's not)
    // For now, assuming 32-bit non-PAE protected mode without explicit PAE.
    // If PAE were needed, it would be:
    // uint32 cr4 = cpu_get_cr4();
    // cr4 |= (1 << 5); // Set PAE bit
    // cpu_set_cr4(cr4);

    // Enable paging (PG bit in CR0) and Protected Mode (PE bit in CR0)
    uint32 cr0 = cpu_get_cr0();
    cr0 |= (1 << 31); // Set PG bit (Paging Enable)
    cr0 |= (1 << 0);  // Set PE bit (Protected Mode Enable) - ensure it's set
    cpu_set_cr0(cr0);

    // Mark paging as enabled for the VMM state
    vmm_state.paging_enabled = true;

    print("[VMM] Paging enabled!\n");
}

// Switch the current page directory
void vmm_switch_page_directory(page_directory_t* dir) {
    if (!dir) {
        kernel_panic("VMM: Attempt to switch to NULL page directory!");
    }
    // If paging is already enabled, update CR3
    if ((cpu_get_cr0() & (1 << 31)) != 0) {
        cpu_set_cr3((uintptr_t)dir);
    }
    vmm_state.current_directory = dir;
}

// Get the current page directory
page_directory_t* vmm_get_current_page_directory(void) {
    if (!vmm_state.paging_enabled) {
        return vmm_state.current_directory;
    }

    uint32 active_cr3 = vmm_get_active_cr3_phys() & ~MEMORY_PAGE_MASK;
    page_directory_t* active_dir = (page_directory_t*)(uintptr_t)active_cr3;
    if (active_dir) {
        vmm_state.current_directory = active_dir;
    }
    return vmm_state.current_directory;
}

page_directory_t* vmm_get_kernel_page_directory(void) {
    return vmm_state.kernel_directory;
}

// Update the software-tracked current directory without switching CR3.
// Used by the task switcher to keep vmm_state in sync with the hardware CR3.
void vmm_set_current_directory(page_directory_t* dir) {
    if (dir) {
        vmm_state.current_directory = dir;
    }
}

page_directory_t* vmm_create_page_directory(void) {
    if (!vmm_state.initialized) {
        return NULL;
    }
    uint32 dir_phys = pmm_alloc_frame();
    if (!dir_phys) {
        return NULL;
    }

    // The new directory's physical frame must be accessible in the CURRENT
    // address space before we can write to it.  Identity-map it first.
    vmm_map_page(vmm_state.current_directory,
                 dir_phys, dir_phys,
                 PAGE_PRESENT | PAGE_WRITABLE);

    page_directory_t* new_dir = (page_directory_t*)dir_phys;
    memset(new_dir, 0, MEMORY_PAGE_SIZE);

    // Copy the ENTIRE kernel directory into the new one.
    // This is a shallow copy — both directories share the same physical page
    // tables for every PDE that was present at copy time.  New kernel heap
    // allocations that go into an existing page table are automatically visible
    // in both directories (they share the PT frame).  New allocations that
    // require a *new* page table (new PDE) will NOT be visible until we
    // explicitly sync that PDE — see task_switch for the runtime sync path.
    //
    // IMPORTANT: copy from kernel_directory, not current_directory. If a user
    // task is currently active (current_directory points at a user PD), copying
    // from it would inherit user-space PDEs and miss kernel PDEs that were
    // added after that user task was created.
    page_directory_t* src_dir = vmm_state.kernel_directory
                                   ? vmm_state.kernel_directory
                                   : vmm_state.current_directory;
    memcpy(new_dir, src_dir, MEMORY_PAGE_SIZE);

    // CRITICAL — ensure the new directory maps its OWN physical frame so that
    // kernel writes through this CR3 (during ELF segment loading) don't fault.
    vmm_identity_map_range(new_dir,
                           dir_phys,
                           dir_phys + MEMORY_PAGE_SIZE,
                           PAGE_WRITABLE);

    // Identity-map page-table frames referenced by kernel-space PDEs so that
    // get_page_entry() can access them via the temp-map mechanism (which needs
    // page tables to be reachable through the active CR3).
    //
    // IMPORTANT: only identity-map frames whose physical address falls below
    // MEMORY_USER_START.  On a typical 512MB system all physical frames are
    // below 0x20000000 so this is always true.  Skipping frames above the
    // user virtual base avoids accidentally clobbering ELF / user-space PTEs
    // that will be created later.
    page_entry_t* src_pde = (page_entry_t*)src_dir;
    for (int i = 0; i < 1024; i++) {
        if (src_pde[i].present) {
            uint32 pt_phys = ((uint32)src_pde[i].frame) << MEMORY_PAGE_SHIFT;
            if (pt_phys >= MEMORY_USER_START) {
                continue; // Don't identity-map into user virtual range
            }
            memory_result_t res = vmm_map_page(new_dir, pt_phys, pt_phys,
                                               PAGE_PRESENT | PAGE_WRITABLE);
            if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
                // Cannot map page table — clean up and fail.
                pmm_free_frame(dir_phys);
                return NULL;
            }
        }
    }

    // RUNTIME PDE SYNC NOTE:
    // Because this is a shallow copy, any kernel heap allocation made AFTER
    // this point that causes a NEW page table to be created (new PDE) will
    // only appear in the kernel's directory, not here.  The task_switch path
    // must sync all kernel PDEs into the active task's directory before
    // calling task_switch_asm.  See task_sync_kernel_pdes() below.

    return new_dir;
}

// Synchronise all kernel-space PDEs (those used for the kernel heap and other
// kernel-only mappings) from the current kernel directory into a task's
// directory.  Call this in task_switch() before task_switch_asm() to ensure
// any kernel heap pages allocated after the task was created are visible.
//
// "Kernel space" is defined as any PDE whose source entry is present AND whose
// corresponding virtual range (i >= kernel_pde_start) falls above the user
// space boundary.  We use a conservative range: PDE 0 through the identity
// limit are already synced via the initial memcpy; we resync ALL PDEs to be
// safe (cost is one pass of 1024 comparisons — negligible).
void vmm_sync_kernel_pdes(page_directory_t* task_dir) {
    if (!task_dir) {
        return;
    }

    page_directory_t* kernel_dir = vmm_state.kernel_directory;
    if (!kernel_dir) {
        return;
    }

    page_entry_t* kernel_pde = (page_entry_t*)kernel_dir;
    page_entry_t* task_pde   = (page_entry_t*)task_dir;

    // Only sync KERNEL-SPACE PDEs (below MEMORY_USER_START and at/above
    // MEMORY_USER_END).  User-space PDEs (indices covering the range
    // [MEMORY_USER_START, MEMORY_USER_END)) belong to the task and must NOT
    // be overwritten — doing so would clobber ELF segment and user stack
    // mappings that were explicitly created for this task.
    uint32 user_pde_start = MEMORY_USER_START / (4 * 1024 * 1024); // PDE index for first user 4MB
    uint32 user_pde_end   = MEMORY_USER_END   / (4 * 1024 * 1024); // PDE index past last user 4MB

    // Start at i=0: PDE 0 covers virtual 0x00000000-0x003FFFFF, which includes
    // kernel code (notably task_start_usermode_asm at ~0x22c000). Skipping
    // PDE 0 here meant kernel code could become unreachable in a task PD
    // after heap/PT churn, causing "TASK SWITCH: unmapped task_start_usermode_asm".
    for (int i = 0; i < 1024; i++) {
        // Skip user-space PDE range — those belong to the task.
        if ((uint32)i >= user_pde_start && (uint32)i < user_pde_end) {
            continue;
        }

        if (kernel_pde[i].present) {
            // Ensure the task's PD has this kernel PDE
            if (!task_pde[i].present) {
                task_pde[i] = kernel_pde[i];
            }
            // Also ensure PDE permissions and frame numbers are consistent with kernel
            if (task_pde[i].present && task_pde[i].frame != kernel_pde[i].frame) {
                task_pde[i] = kernel_pde[i];
            }
        }
    }
}

void vmm_destroy_page_directory(page_directory_t* dir) {
    if (!dir || dir == vmm_state.kernel_directory) {
        return;
    }

    // Real page directories are always page-frame-aligned (allocated via
    // pmm_alloc_frame(), see vmm_create_page_directory()); a stale/corrupt
    // pointer never is. There's an unresolved task-lifecycle bug elsewhere
    // (some path can double-destroy/re-free a task, leaving its struct --
    // and this field -- holding kfree()'s freed-memory poison pattern by
    // the time this runs) that this doesn't fix, but walking and freeing
    // frames through a garbage pointer here is what actually crashes the
    // kernel, so refuse to do that regardless of how `dir` went bad.
    if (((uintptr_t)dir & MEMORY_PAGE_MASK) != 0) {
        debuglog(DEBUG_ERROR,
                 "[VMM] vmm_destroy_page_directory: refusing non-page-aligned dir=0x%x (already freed/corrupt?)\n",
                 (uint32)(uintptr_t)dir);
        return;
    }

    /* Walk page directory: free user-space PT frames and their data frames.
     * Skip kernel-space PDEs (those are shared with kernel_directory). */
    uint32 user_pde_start = MEMORY_USER_START / (4 * 1024 * 1024);
    uint32 user_pde_end   = MEMORY_USER_END   / (4 * 1024 * 1024);

    page_entry_t* pde_arr = (page_entry_t*)dir;
    for (uint32 i = user_pde_start; i < user_pde_end && i < 1024; i++) {
        if (!pde_arr[i].present) {
            continue;
        }
        uint32 pt_phys = ((uint32)pde_arr[i].frame) << MEMORY_PAGE_SHIFT;
        /* Access page table through temp map */
        void* pt_virt = vmm_temp_map_page(pt_phys);
        if (!pt_virt) {
            continue;
        }
        page_table_t* pt = (page_table_t*)pt_virt;
        /* Free user data frames. A frame shared via fork()'s COW mechanism
         * (cow_fork_address_space()) is still mapped into at least one
         * sibling task's address space, so it must never be handed back to
         * the PMM directly here -- that would let the sibling keep running
         * against a physical frame the allocator has already reissued to
         * someone else. Route through cow_release(), which only frees the
         * frame once every sharer has released its reference; a frame that
         * was never COW-tracked (refcount reads back as 1) is this task's
         * sole owner and is freed directly as before. */
        for (uint32 j = 0; j < 1024; j++) {
            if ((*pt)[j].present && (*pt)[j].user) {
                uint32 frame_phys = ((uint32)(*pt)[j].frame) << MEMORY_PAGE_SHIFT;
                if (cow_get_refcount(frame_phys) > 1) {
                    cow_release(frame_phys);
                } else {
                    pmm_free_frame(frame_phys);
                }
            }
        }
        vmm_temp_unmap_page(pt_virt);
        /* Free the page table frame itself */
        pmm_free_frame(pt_phys);
        pde_arr[i].present = 0;
    }

    pmm_free_frame((uint32)dir);
}

bool vmm_is_mapped(page_directory_t* dir, uint32 vaddr) {
    if (!dir) {
        return false;
    }
    page_entry_t* page = get_page_entry(vaddr, false, dir);
    return page && page->present;
}

memory_result_t vmm_identity_map_range(page_directory_t* dir, uint32 start, uint32 end, uint32 flags) {
    if (!dir || start > end) {
        return MEMORY_ERROR_INVALID_ADDR;
    }

    // Skip vmm_is_addr_valid check for identity mapping.
    // Identity mapping maps virtual = physical, so if physical memory is valid (below 4GB),
    // the virtual should be allowed too. This fixes crashes when mapping kernel
    // higher-half (0xC0000000+) in user page directories.

    uint32 aligned_start = start & ~MEMORY_PAGE_MASK;
    uint32 aligned_end = (end + MEMORY_PAGE_MASK) & ~MEMORY_PAGE_MASK;
    for (uint32 addr = aligned_start; addr < aligned_end; addr += MEMORY_PAGE_SIZE) {
        memory_result_t res = vmm_map_page(dir, addr, addr, flags | PAGE_PRESENT);
        if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
            return res;
        }
    }
    return MEMORY_OK;
}
