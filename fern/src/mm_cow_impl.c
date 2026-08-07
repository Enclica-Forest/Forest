/**
 * @file mm_cow_impl.c
 * @brief Copy-on-Write Implementation
 * 
 * Full COW support for:
 * - fork() system call
 * - Shared memory regions
 * - Memory-mapped files
 * - Anonymous page sharing
 * 
 * When a page is marked COW:
 * 1. It is marked read-only in the page table
 * 2. A reference count is maintained
 * 3. On write fault, a copy is made if refcount > 1
 */

#include "include/mm_cow.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/tlb.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

// cow_init() was never actually reaching this code until the duplicate
// cow_init() in mm_cow.c got renamed off the real symbol name (it was
// silently winning the link, so this eager pool allocation had never run
// in practice). At the original 1GB-of-pages sizing, the 4MB kmalloc()
// this needs stalls the boot-time heap expander on a modest-RAM VM. Sized
// down to what this kernel's actual fork() usage needs (a handful of
// forked shells' worth of COW'd pages at a time), not a theoretical max.
#define COW_MAX_PAGES       4096            // Support up to 16MB of COW pages
#define COW_HASH_SIZE       1024

// Page flags for COW
#define COW_FLAG_SHARED     0x01
#define COW_FLAG_ANON       0x02
#define COW_FLAG_MAPPED     0x04

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief COW page descriptor
 */
typedef struct cow_page {
    phys_addr_t phys_addr;          // Physical address
    uint32_t refcount;              // Reference count
    uint32_t flags;                 // COW flags
    struct cow_page* hash_next;     // Hash chain
    struct cow_page* free_next;     // Free-list chain (valid only once unlinked from the hash)
} cow_page_t;

/**
 * @brief COW manager state
 */
static struct {
    bool initialized;
    
    // Page descriptors pool
    cow_page_t* pages;
    uint32_t page_count;
    uint32_t next_free;

    // Free-list of descriptors released back by cow_release() (refcount
    // reached 0). Without this, the pool only ever grows via next_free and
    // permanently exhausts after COW_MAX_PAGES total fork-shared pages have
    // ever been fully released, even though none are in use at any given
    // moment -- a slow-motion failure that only shows up after days of
    // uptime with many short-lived forked processes.
    cow_page_t* free_list;

    // Hash table for fast lookup
    cow_page_t* hash_table[COW_HASH_SIZE];
    
    // Statistics
    uint64_t total_shared;
    uint64_t total_copied;
    uint64_t pages_saved;
    
    // Lock
    spinlock_t lock;
} cow_state = { .initialized = false };

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Hash function for physical address
 */
static inline uint32_t cow_hash(phys_addr_t addr) {
    return (addr >> MEMORY_PAGE_SHIFT) % COW_HASH_SIZE;
}

/**
 * @brief Find COW descriptor for physical address
 */
static cow_page_t* cow_find(phys_addr_t addr) {
    uint32_t hash = cow_hash(addr);
    cow_page_t* page = cow_state.hash_table[hash];
    
    while (page) {
        if (page->phys_addr == addr) {
            return page;
        }
        page = page->hash_next;
    }
    
    return NULL;
}

/**
 * @brief Allocate COW descriptor
 */
static cow_page_t* cow_alloc_descriptor(void) {
    if (cow_state.free_list) {
        cow_page_t* page = cow_state.free_list;
        cow_state.free_list = page->free_next;
        return page;
    }

    if (cow_state.next_free >= cow_state.page_count) {
        return NULL;
    }

    return &cow_state.pages[cow_state.next_free++];
}

/**
 * @brief Return a descriptor (already unlinked from the hash table) to the
 * free list so cow_alloc_descriptor() can reuse it.
 */
static void cow_free_descriptor(cow_page_t* page) {
    page->free_next = cow_state.free_list;
    cow_state.free_list = page;
}

/**
 * @brief Add page to hash table
 */
static void cow_hash_insert(cow_page_t* page) {
    uint32_t hash = cow_hash(page->phys_addr);
    page->hash_next = cow_state.hash_table[hash];
    cow_state.hash_table[hash] = page;
}

/**
 * @brief Remove page from hash table
 */
static void cow_hash_remove(cow_page_t* page) {
    uint32_t hash = cow_hash(page->phys_addr);
    cow_page_t** pp = &cow_state.hash_table[hash];
    
    while (*pp) {
        if (*pp == page) {
            *pp = page->hash_next;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize COW subsystem
 */
memory_result_t cow_init(void) {
    if (cow_state.initialized) {
        return MEMORY_OK;
    }
    
    print("[COW] Initializing Copy-on-Write subsystem...\n");
    
    // Allocate page descriptors pool
    uint32_t pool_size = COW_MAX_PAGES * sizeof(cow_page_t);
    cow_state.pages = (cow_page_t*)kmalloc(pool_size);
    if (!cow_state.pages) {
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }
    
    memset(cow_state.pages, 0, pool_size);
    cow_state.page_count = COW_MAX_PAGES;
    cow_state.next_free = 0;
    
    // Initialize hash table
    memset(cow_state.hash_table, 0, sizeof(cow_state.hash_table));
    
    // Initialize statistics
    cow_state.total_shared = 0;
    cow_state.total_copied = 0;
    cow_state.pages_saved = 0;
    
    // Initialize lock
    spinlock_init(&cow_state.lock, "cow");
    
    cow_state.initialized = true;
    
    print("[COW] COW subsystem initialized\n");
    
    return MEMORY_OK;
}

/**
 * @brief Mark a page as COW (shared)
 * 
 * Called when fork() shares a page between parent and child.
 */
memory_result_t cow_mark_shared(phys_addr_t phys_addr, uint32_t flags) {
    if (!cow_state.initialized) {
        return MEMORY_ERROR_NOT_INITIALIZED;
    }
    
    spinlock_acquire(&cow_state.lock);
    
    cow_page_t* page = cow_find(phys_addr);
    
    if (page) {
        // Already tracked, increment refcount
        page->refcount++;
    } else {
        // New COW page
        page = cow_alloc_descriptor();
        if (!page) {
            spinlock_release(&cow_state.lock);
            return MEMORY_ERROR_OUT_OF_MEMORY;
        }
        
        page->phys_addr = phys_addr;
        page->refcount = 2;  // Original + new reference
        page->flags = flags | COW_FLAG_SHARED;
        
        cow_hash_insert(page);
    }
    
    cow_state.total_shared++;
    
    spinlock_release(&cow_state.lock);
    
    return MEMORY_OK;
}

/**
 * @brief Handle COW page fault
 * 
 * Called when a write occurs to a COW page.
 * Returns the physical address to use (new or original).
 */
cow_result_t cow_handle_fault(page_directory_t* dir, uint32_t vaddr, 
                               phys_addr_t old_phys, phys_addr_t* new_phys) {
    if (!cow_state.initialized) {
        *new_phys = old_phys;
        return COW_NOT_SHARED;
    }
    
    spinlock_acquire(&cow_state.lock);
    
    cow_page_t* page = cow_find(old_phys);
    
    if (!page || page->refcount <= 1) {
        // Not a COW page or last reference
        // Just make it writable
        spinlock_release(&cow_state.lock);
        *new_phys = old_phys;
        return COW_NOT_SHARED;
    }
    
    // Multiple references - need to copy
    phys_addr_t new_frame = pmm_alloc_frame();
    if (new_frame == 0) {
        spinlock_release(&cow_state.lock);
        return COW_OUT_OF_MEMORY;
    }
    
    // Copy the page contents. Neither frame is guaranteed to be
    // identity-mapped, so both need a temporary virtual alias.
    void* src = vmm_temp_map_page(old_phys);
    void* dst = vmm_temp_map_page(new_frame);
    if (!src || !dst) {
        if (src) vmm_temp_unmap_page(src);
        if (dst) vmm_temp_unmap_page(dst);
        pmm_free_frame(new_frame);
        spinlock_release(&cow_state.lock);
        return COW_OUT_OF_MEMORY;
    }
    memcpy(dst, src, MEMORY_PAGE_SIZE);
    vmm_temp_unmap_page(src);
    vmm_temp_unmap_page(dst);
    
    // Decrement original page refcount
    page->refcount--;
    
    if (page->refcount == 1) {
        // No longer shared, remove from COW tracking
        cow_hash_remove(page);
    }
    
    cow_state.total_copied++;
    cow_state.pages_saved++;
    
    spinlock_release(&cow_state.lock);
    
    *new_phys = new_frame;
    return COW_COPIED;
}

/**
 * @brief Release COW reference
 * 
 * Called when a process exits or unmaps a COW page.
 */
memory_result_t cow_release(phys_addr_t phys_addr) {
    if (!cow_state.initialized) {
        return MEMORY_OK;
    }
    
    spinlock_acquire(&cow_state.lock);
    
    cow_page_t* page = cow_find(phys_addr);
    
    if (page) {
        page->refcount--;

        if (page->refcount == 0) {
            // No more references, free the page
            cow_hash_remove(page);
            cow_free_descriptor(page);
            pmm_free_frame(phys_addr);
        }
    }
    
    spinlock_release(&cow_state.lock);
    
    return MEMORY_OK;
}

/**
 * @brief Get COW reference count
 */
uint32_t cow_get_refcount(phys_addr_t phys_addr) {
    if (!cow_state.initialized) {
        return 1;
    }
    
    spinlock_acquire(&cow_state.lock);
    
    cow_page_t* page = cow_find(phys_addr);
    uint32_t refcount = page ? page->refcount : 1;
    
    spinlock_release(&cow_state.lock);
    
    return refcount;
}

/**
 * @brief Check if page is COW shared
 */
bool cow_is_shared(phys_addr_t phys_addr) {
    return cow_get_refcount(phys_addr) > 1;
}

/**
 * @brief Fork address space with COW
 * 
 * Creates a new page directory that shares all pages with the
 * parent using COW semantics.
 */
page_directory_t* cow_fork_address_space(page_directory_t* parent) {
    if (!parent) {
        return NULL;
    }
    
    // Create new page directory
    page_directory_t* child = vmm_create_page_directory();
    if (!child) {
        return NULL;
    }
    
    // Iterate through parent's page tables
    for (uint32_t pd_idx = 0; pd_idx < 1024; pd_idx++) {
        page_entry_t* pde = &(*parent)[pd_idx];
        
        if (!pde->present) {
            continue;
        }
        
        // Skip kernel space: identity-mapped low memory below
        // MEMORY_USER_START (kernel heap/code/page tables) as well as the
        // high kernel region at/above MEMORY_USER_END. Only the
        // [MEMORY_USER_START, MEMORY_USER_END) range is this task's own
        // user address space and needs COW treatment.
        uint32_t vaddr_base = pd_idx * 1024 * MEMORY_PAGE_SIZE;
        if (vaddr_base < MEMORY_USER_START || vaddr_base >= MEMORY_USER_END) {
            // Copy kernel mappings directly (shared, not COW)
            (*child)[pd_idx] = *pde;
            continue;
        }
        
        // User space - mark pages as COW.
        // pde->frame is a *physical* frame number; PMM allocations are not
        // guaranteed to fall in identity-mapped low memory, so this frame
        // can only be safely accessed through a temporary virtual alias
        // (see elf.c's copy_to_user_space for the same pattern).
        phys_addr_t pt_phys = pde->frame << MEMORY_PAGE_SHIFT;
        page_table_t* pt = (page_table_t*)vmm_temp_map_page(pt_phys);
        if (!pt) {
            continue;
        }

        for (uint32_t pt_idx = 0; pt_idx < 1024; pt_idx++) {
            page_entry_t* pte = &(*pt)[pt_idx];

            if (!pte->present) {
                continue;
            }

            phys_addr_t page_phys = pte->frame << MEMORY_PAGE_SHIFT;
            uint32_t vaddr = vaddr_base + pt_idx * MEMORY_PAGE_SIZE;

            // Mark as COW-shared (refcount=2: parent's existing mapping +
            // child's new one). Both mappings must become read-only:
            // leaving the parent's mapping writable lets the parent keep
            // writing straight into the still-shared physical page (e.g.
            // its own stack, as it continues past fork()) with no fault to
            // intercept it, silently clobbering data the child's copy of
            // the same page depends on (its stack frames, saved return
            // addresses, etc.) before the child ever gets to run. Either
            // side's next write now faults into cow_handle_fault(), which
            // splits off a private copy for whichever task actually wrote,
            // leaving the other's view of the page untouched.
            cow_mark_shared(page_phys, COW_FLAG_ANON);
            pte->writable = 0;

            // Map in child as read-only
            vmm_map_page(child, vaddr, page_phys,
                        PAGE_PRESENT | PAGE_USER);
        }

        vmm_temp_unmap_page(pt);
    }

    // Flush TLB
    tlb_flush();

    return child;
}

/**
 * @brief Get COW statistics
 */
cow_stats_t cow_get_stats(void) {
    cow_stats_t stats;
    stats.total_shared = cow_state.total_shared;
    stats.total_copied = cow_state.total_copied;
    stats.pages_saved = cow_state.pages_saved;
    stats.memory_saved_kb = (cow_state.pages_saved * MEMORY_PAGE_SIZE) / 1024;
    return stats;
}

/**
 * @brief Dump COW statistics
 */
void cow_dump_stats(void) {
    cow_stats_t stats = cow_get_stats();
    
    print("\n=== Copy-on-Write Statistics ===\n");
    print("Total shared: ");
    print_dec((uint32_t)stats.total_shared);
    print("\n");
    print("Total copied: ");
    print_dec((uint32_t)stats.total_copied);
    print("\n");
    print("Pages saved: ");
    print_dec((uint32_t)stats.pages_saved);
    print("\n");
    print("Memory saved: ");
    print_dec(stats.memory_saved_kb);
    print(" KB\n");
    print("================================\n\n");
}
