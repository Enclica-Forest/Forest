#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/tlb_manager.h"
#include "include/debuglog.h"
#include "include/spinlock.h"

// =============================================================================
// KERNEL HEAP MANAGER IMPLEMENTATION
// =============================================================================
// Simple block-based allocator for kernel dynamic memory
// Uses linked lists of free blocks with first-fit allocation strategy
// =============================================================================

#define HEAP_MAGIC      0x48454150  // "HEAP"
#define BLOCK_MAGIC     0x424C4B53  // "BLKS"
#define BLOCK_FREE      0xFEEEFEEE
#define BLOCK_USED      0xDEADBEEF

// Block header structure
typedef struct heap_block {
    uint32 magic;               // Magic number for validation
    uint32 size;               // Size of this block (including header)
    uint32 status;             // Free or used marker
    struct heap_block* next;    // Next block in chain
    struct heap_block* prev;    // Previous block in chain
} heap_block_t;

// Heap state
static struct {
    bool initialized;
    uint32 start_addr;         // Virtual start address of heap
    uint32 current_end;        // Current end of heap
    uint32 max_size;           // Maximum heap size
    heap_block_t* first_block; // First block in heap
    heap_block_t* free_list;   // Head of free block list
    uint32 total_size;         // Total heap size
    uint32 used_size;          // Used heap size  
    uint32 free_size;          // Free heap size
    uint32 block_count;        // Total number of blocks
    uint32 alloc_count;        // Number of allocations
    uint32 free_count;         // Number of frees
} heap_state = {0};

static spinlock_t heap_lock = SPINLOCK_INIT("kheap");

static phys_addr_t g_heap_prealloc_buf[512];
static bool g_heap_prealloc_busy = false;

static struct {
    uint8_t* base;
    uint32_t size;
    uint32_t used;
    bool initialized;
} g_gfx_pool = {0};

// External VMM and PMM functions
extern memory_result_t vmm_map_page(page_directory_t* dir, uint32 vaddr, uint32 paddr, uint32 flags);
extern memory_result_t vmm_unmap_page(page_directory_t* dir, uint32 vaddr);
extern bool vmm_is_mapped(page_directory_t* dir, uint32 vaddr);
extern page_directory_t* vmm_get_current_page_directory(void);
extern page_directory_t* vmm_get_kernel_page_directory(void);

// =============================================================================
// INTERNAL HELPER FUNCTIONS
// =============================================================================

// Validate block magic and structure
static bool is_valid_block(heap_block_t* block) {
    if (!block) {
        return false;
    }
    
    // Check if pointer is within heap bounds
    if ((uint32)block < heap_state.start_addr || 
        (uint32)block >= heap_state.current_end) {
        return false;
    }
    
    // Check basic alignment
    if (((uint32)block & 0x3) != 0) {
        return false;
    }
    
    // Validate magic number and status
    if (block->magic != BLOCK_MAGIC) {
        return false;
    }
    
    if (block->status != BLOCK_FREE && block->status != BLOCK_USED) {
        return false;
    }
    
    // Check minimum size
    if (block->size < sizeof(heap_block_t)) {
        return false;
    }
    
    // Check that block doesn't extend beyond heap (overflow-safe)
    if (block->size > heap_state.current_end - (uint32)block) {
        return false;
    }
    
    return true;
}

// Get data pointer from block
static void* block_to_data(heap_block_t* block) {
    return (void*)((uint32)block + sizeof(heap_block_t));
}

// Get block from data pointer
static heap_block_t* data_to_block(void* data) {
    if (!data) return NULL;
    return (heap_block_t*)((uint32)data - sizeof(heap_block_t));
}

// Calculate aligned size
static uint32 align_size(uint32 size) {
    return memory_align_up(size, sizeof(uint32));
}

// Add block to free list in address-sorted order and coalesce
static void add_to_free_list(heap_block_t* block) {
    block->status = BLOCK_FREE;

    heap_block_t *current = heap_state.free_list;
    heap_block_t *prev = NULL;

    // Find the correct position to insert the block to keep the list sorted by address
    while (current != NULL && (uint32)current < (uint32)block) {
        // Validate current block before dereferencing
        if (!is_valid_block(current)) {
            print("[HEAP] ERROR: Corrupted block in free list at 0x"); print_hex((uint32)current); print("\n");
            // Detach only this corrupt node; don't destroy the whole list.
            if (prev == NULL) {
                heap_state.free_list = NULL;
            } else {
                prev->next = NULL;
            }
            break;
        }
        prev = current;
        current = current->next;
    }

    // Insert the block into the list
    if (prev == NULL) { // Insert at head
        heap_state.free_list = block;
    } else {
        prev->next = block;
    }
    block->prev = prev;
    block->next = current;
    if (current != NULL) {
        current->prev = block;
    }

    // Coalesce with the next block if it's adjacent and free
    if (block->next != NULL && is_valid_block(block->next) && 
        (uint32)block + block->size == (uint32)block->next) {
        heap_block_t* next_block = block->next;
        block->size += next_block->size;
        block->next = next_block->next;
        if (next_block->next != NULL) {
            next_block->next->prev = block;
        }
        heap_state.block_count--;
    }

    // Coalesce with the previous block if it's adjacent and free
    if (block->prev != NULL && is_valid_block(block->prev) && 
        (uint32)block->prev + block->prev->size == (uint32)block) {
        heap_block_t* prev_block = block->prev;
        prev_block->size += block->size;
        prev_block->next = block->next;
        if (block->next != NULL) {
            block->next->prev = prev_block;
        }
        heap_state.block_count--;
    }
}

// Remove block from free list
static void remove_from_free_list(heap_block_t* block) {
    if (!is_valid_block(block)) {
        print("[HEAP] ERROR: Attempting to remove invalid block\n");
        return;
    }
    
    if (block->prev) {
        if (is_valid_block(block->prev)) {
            block->prev->next = block->next;
        } else {
            print("[HEAP] ERROR: Corrupted prev pointer\n");
        }
    } else {
        heap_state.free_list = block->next;
    }
    
    if (block->next) {
        if (is_valid_block(block->next)) {
            block->next->prev = block->prev;
        } else {
            print("[HEAP] ERROR: Corrupted next pointer\n");
        }
    }
    
    block->next = NULL;
    block->prev = NULL;
    block->status = BLOCK_USED;
}

// Find free block of at least given size
static heap_block_t* find_free_block(uint32 size) {
    heap_block_t* block = heap_state.free_list;
    
    int i = 0;
    while (block) {
        // Validate block before accessing its members
        if (!is_valid_block(block)) {
            print("[HEAP] ERROR: Invalid block in free list at 0x"); print_hex((uint32)block); print("\n");
            // Remove corrupted block from free list
            if (block == heap_state.free_list) {
                heap_state.free_list = NULL;
            }
            return NULL;
        }
        
        // Check if virtual address is within valid heap range
        if ((uint32)block < heap_state.start_addr || 
            (uint32)block >= heap_state.current_end) {
            print("[HEAP] ERROR: Block outside heap range at 0x"); print_hex((uint32)block); print("\n");
            return NULL;
        }

        if (block->size >= size) {
            return block;
        }
        block = block->next;
        i++;
        
        // Prevent infinite loops
        if (i > 1000) {
            print("[HEAP] ERROR: Infinite loop detected in free list\n");
            return NULL;
        }
    }
    return NULL;
}

// Split a block if it's large enough
static void split_block(heap_block_t* block, uint32 size) {
    if (!is_valid_block(block)) {
        print("[HEAP] ERROR: Attempting to split invalid block\n");
        return;
    }
    
    uint32 remaining = block->size - size;
    
    // Only split if remaining space is large enough for a new block
    if (remaining >= sizeof(heap_block_t) + 16) {
        heap_block_t* new_block = (heap_block_t*)((uint32)block + size);
        
        // Ensure new block doesn't exceed heap bounds
        if ((uint32)new_block + remaining > heap_state.current_end) {
            print("[HEAP] ERROR: Split would exceed heap bounds\n");
            return;
        }
        
        new_block->magic = BLOCK_MAGIC;
        new_block->size = remaining;
        new_block->status = BLOCK_FREE;
        new_block->next = NULL;
        new_block->prev = NULL;
        
        block->size = size;
        
        add_to_free_list(new_block);
        heap_state.block_count++;
    }
}



// Expand heap by allocating more pages.
// On success, *out_block (if non-NULL) is set to the free block that now
// covers the newly-expanded space (it may be a coalesced predecessor if the
// previous free-list tail was adjacent to the new pages).  Callers that need
// a large block can use this pointer directly instead of re-scanning the whole
// free list from the beginning (which would have to skip all small fragments).
static memory_result_t expand_heap(uint32 needed_size, heap_block_t** out_block) {
    if (out_block) *out_block = NULL;

    // Validate expansion parameters
    if (needed_size == 0 || needed_size > heap_state.max_size) {
        return MEMORY_ERROR_INVALID_SIZE;
    }

    uint32 pages_needed = (needed_size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
    uint32 expand_size = pages_needed * MEMORY_PAGE_SIZE;

    if (heap_state.current_end + expand_size > heap_state.start_addr + heap_state.max_size) {
        return MEMORY_ERROR_OUT_OF_MEMORY; // Hit heap size limit
    }

    print("[HEAP] expand_heap: pages_needed="); print_dec(pages_needed);
    print(" current_end=0x"); print_hex(heap_state.current_end); print("\n");
    debuglog(DEBUG_WARN, "[HEAP] expand_heap: pages=%u end=0x%x\n", pages_needed, heap_state.current_end);

    // Ensure TLB entries for this range are clean before mapping
    tlb_safe_heap_expand(heap_state.current_end, pages_needed);

    page_directory_t* current_dir = vmm_get_current_page_directory();
    page_directory_t* kernel_dir = vmm_get_kernel_page_directory();
    if (!kernel_dir) {
        kernel_dir = current_dir;
    }
    print("[HEAP] expand_heap: got current_dir=0x"); print_hex((uint32)current_dir); print("\n");
    debuglog(DEBUG_WARN, "[HEAP] expand_heap: dir=0x%x\n", (uint32)current_dir);
    if (kernel_dir != current_dir) {
        debuglog(DEBUG_WARN, "[HEAP] expand_heap: kernel_dir=0x%x\n", (uint32)kernel_dir);
    }

    // Allocate and map new pages
    for (uint32 i = 0; i < pages_needed; i++) {
        uint32 vaddr = heap_state.current_end + (i * MEMORY_PAGE_SIZE);

        if (i == 0) {
            print("[HEAP] expand_heap: mapping first page vaddr=0x"); print_hex(vaddr); print("\n");
        }

        /*
         * Kernel heap virtual addresses must always be mapped in the canonical
         * kernel page directory. If we only map them in the active task CR3,
         * heap metadata diverges across tasks and eventually corrupts global
         * kernel state.
         */
        uint32 phys_frame = 0;
        bool created_kernel_mapping = false;

        if (vmm_is_mapped(kernel_dir, vaddr)) {
            phys_frame = vmm_get_physical_addr(kernel_dir, vaddr);
            // The page is identity-mapped (kernel boot mapping covers low RAM).
            // PMM was not consulted, so the frame is still marked free in the
            // bitmap. Reserve it now so pmm_alloc_frame cannot hand the same
            // physical frame back as a page directory or other allocation.
            if (phys_frame) {
                pmm_reserve_range(phys_frame, phys_frame + MEMORY_PAGE_SIZE);
            }
        } else {
            phys_frame = pmm_alloc_frame();
            if (phys_frame == 0) {
                return MEMORY_ERROR_OUT_OF_MEMORY;
            }

            memory_result_t kmap_res = vmm_map_page(kernel_dir,
                                                    vaddr,
                                                    phys_frame,
                                                    PAGE_PRESENT | PAGE_WRITABLE);
            if (kmap_res == MEMORY_ERROR_ALREADY_MAPPED) {
                uint32 existing = vmm_get_physical_addr(kernel_dir, vaddr);
                if (existing != 0) {
                    pmm_free_frame(phys_frame);
                    phys_frame = existing;
                } else {
                    pmm_free_frame(phys_frame);
                    return MEMORY_ERROR_ALREADY_MAPPED;
                }
            } else if (kmap_res != MEMORY_OK) {
                pmm_free_frame(phys_frame);
                return kmap_res;
            } else {
                created_kernel_mapping = true;
            }
        }

        /* Mirror the same mapping into the currently active address space. */
        if (current_dir && current_dir != kernel_dir && !vmm_is_mapped(current_dir, vaddr)) {
            memory_result_t cmap_res = vmm_map_page(current_dir,
                                                    vaddr,
                                                    phys_frame,
                                                    PAGE_PRESENT | PAGE_WRITABLE);
            if (cmap_res != MEMORY_OK && cmap_res != MEMORY_ERROR_ALREADY_MAPPED) {
                if (created_kernel_mapping) {
                    vmm_unmap_page(kernel_dir, vaddr);
                    pmm_free_frame(phys_frame);
                }
                return cmap_res;
            }
        }

        tlb_invalidate_page(vaddr);
    }
    print("[HEAP] expand_heap: all pages mapped\n");

    // Record the address we are about to use for the new block header before
    // updating current_end, so we can locate the owning entry after coalescing.
    uint32 new_block_addr = heap_state.current_end;

    // Update current_end FIRST so that is_valid_block() accepts the new block
    // (it checks that block+size <= current_end).
    heap_state.current_end += expand_size;
    heap_state.total_size  += expand_size;
    heap_state.free_size   += expand_size;
    heap_state.block_count++;

    heap_block_t* new_block = (heap_block_t*)new_block_addr;
    new_block->magic  = BLOCK_MAGIC;
    new_block->size   = expand_size;
    new_block->status = BLOCK_FREE;
    new_block->next   = NULL;
    new_block->prev   = NULL;

    add_to_free_list(new_block);

    // Determine which free-list entry now owns the new pages (handles coalescing
    // where the block was absorbed into its predecessor).
    if (out_block) {
        heap_block_t* candidate = heap_state.free_list;
        heap_block_t* owner = NULL;
        int guard = 0;
        while (candidate && guard < 1000) {
            if (is_valid_block(candidate) &&
                (uint32)candidate <= new_block_addr &&
                (uint32)candidate + candidate->size > new_block_addr) {
                owner = candidate;
                break;
            }
            candidate = candidate->next;
            guard++;
        }
        // Fallback: new_block itself was not coalesced
        if (!owner && is_valid_block(new_block)) {
            owner = new_block;
        }
        *out_block = owner;
    }

    return MEMORY_OK;
}

static heap_block_t* expand_heap_with_prealloc(uint32 needed_size,
                                                phys_addr_t* frames,
                                                uint32 frame_count,
                                                uint32 total_pages) {
    uint32 expand_size = total_pages * MEMORY_PAGE_SIZE;
    if (heap_state.current_end + expand_size > heap_state.start_addr + heap_state.max_size) {
        return NULL;
    }

    tlb_safe_heap_expand(heap_state.current_end, total_pages);

    page_directory_t* current_dir = vmm_get_current_page_directory();
    page_directory_t* kernel_dir = vmm_get_kernel_page_directory();
    if (!kernel_dir) kernel_dir = current_dir;

    for (uint32 i = 0; i < total_pages; i++) {
        uint32 vaddr = heap_state.current_end + (i * MEMORY_PAGE_SIZE);
        phys_addr_t phys_frame = (i < frame_count) ? frames[i] : 0;

        if (vmm_is_mapped(kernel_dir, vaddr)) {
            uint32 existing = vmm_get_physical_addr(kernel_dir, vaddr);
            if (existing) {
                pmm_reserve_range(existing, existing + MEMORY_PAGE_SIZE);
                if (phys_frame && phys_frame != existing) {
                    pmm_free_frame(phys_frame);
                }
                phys_frame = existing;
                if (i < frame_count) frames[i] = 0;
            }
        } else {
            if (!phys_frame) {
                phys_frame = pmm_alloc_frame();
                if (phys_frame == 0) {
                    for (uint32 j = i; j < frame_count; j++) {
                        if (frames[j]) { pmm_free_frame(frames[j]); frames[j] = 0; }
                    }
                    return NULL;
                }
            }
            memory_result_t res = vmm_map_page(kernel_dir, vaddr, phys_frame,
                                                PAGE_PRESENT | PAGE_WRITABLE);
            if (res == MEMORY_ERROR_ALREADY_MAPPED) {
                uint32 existing = vmm_get_physical_addr(kernel_dir, vaddr);
                if (existing) {
                    pmm_free_frame(phys_frame);
                    phys_frame = existing;
                } else {
                    pmm_free_frame(phys_frame);
                    for (uint32 j = i + 1; j < frame_count; j++) {
                        if (frames[j]) { pmm_free_frame(frames[j]); frames[j] = 0; }
                    }
                    return NULL;
                }
            } else if (res != MEMORY_OK) {
                pmm_free_frame(phys_frame);
                for (uint32 j = i + 1; j < frame_count; j++) {
                    if (frames[j]) { pmm_free_frame(frames[j]); frames[j] = 0; }
                }
                return NULL;
            }
            if (i < frame_count) frames[i] = 0;
        }

        if (current_dir && current_dir != kernel_dir && !vmm_is_mapped(current_dir, vaddr)) {
            memory_result_t res = vmm_map_page(current_dir, vaddr, phys_frame,
                                                PAGE_PRESENT | PAGE_WRITABLE);
            if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
                for (uint32 j = i + 1; j < frame_count; j++) {
                    if (frames[j]) { pmm_free_frame(frames[j]); frames[j] = 0; }
                }
                return NULL;
            }
        }

        tlb_invalidate_page(vaddr);
    }

    uint32 new_block_addr = heap_state.current_end;
    heap_state.current_end += expand_size;
    heap_state.total_size += expand_size;
    heap_state.free_size += expand_size;
    heap_state.block_count++;

    heap_block_t* new_block = (heap_block_t*)new_block_addr;
    new_block->magic = BLOCK_MAGIC;
    new_block->size = expand_size;
    new_block->status = BLOCK_FREE;
    new_block->next = NULL;
    new_block->prev = NULL;

    add_to_free_list(new_block);

    heap_block_t* candidate = heap_state.free_list;
    heap_block_t* owner = NULL;
    int guard = 0;
    while (candidate && guard < 1000) {
        if (is_valid_block(candidate) &&
            (uint32)candidate <= new_block_addr &&
            (uint32)candidate + candidate->size > new_block_addr) {
            owner = candidate;
            break;
        }
        candidate = candidate->next;
        guard++;
    }
    if (!owner && is_valid_block(new_block)) {
        owner = new_block;
    }
    return owner;
}

// =============================================================================
// PUBLIC INTERFACE IMPLEMENTATION
// =============================================================================

memory_result_t heap_init(uint32 start_addr, uint32 initial_size) {
    spinlock_acquire(&heap_lock);
    print("[HEAP] heap_init: start=0x"); print_hex(start_addr); 
    print(" size="); print_dec(initial_size); print("\n");
    
    // Validate parameters
    if (start_addr == 0 || (start_addr & MEMORY_PAGE_MASK) != 0) {
        print("[HEAP] ERROR: Invalid start address\n");
        spinlock_release(&heap_lock);
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    if (initial_size < MEMORY_PAGE_SIZE) {
        initial_size = MEMORY_PAGE_SIZE;
    }
    
    // Align initial size to page boundary
    initial_size = memory_align_up(initial_size, MEMORY_PAGE_SIZE);
    print("[HEAP] Aligned size: "); print_dec(initial_size); print("\n");
    
    heap_state.start_addr = start_addr;
    heap_state.current_end = start_addr;
    heap_state.max_size = MEMORY_KERNEL_HEAP_MAX_SIZE;
    heap_state.total_size = 0;
    heap_state.used_size = 0;
    heap_state.free_size = 0;
    heap_state.block_count = 0;
    heap_state.alloc_count = 0;
    heap_state.free_count = 0;
    heap_state.first_block = NULL;
    heap_state.free_list = NULL;
    
    // Allocate initial heap space (no need to capture the block pointer here)
    memory_result_t result = expand_heap(initial_size, NULL);
    if (result != MEMORY_OK) {
        spinlock_release(&heap_lock);
        return result;
    }
    
    // The first block is already set up by expand_heap() and added to free_list
    heap_state.first_block = heap_state.free_list;
    heap_state.initialized = true;
    spinlock_release(&heap_lock);
    
    return MEMORY_OK;
}

void* kmalloc(size_t size) {
    spinlock_acquire(&heap_lock);
    if (!heap_state.initialized || size == 0) {
        spinlock_release(&heap_lock);
        return NULL;
    }
    
    // Prevent allocations that can never fit in the heap (account for header)
    uint32 max_alloc = heap_state.max_size > sizeof(heap_block_t)
        ? heap_state.max_size - sizeof(heap_block_t)
        : 0;
    if (size > (size_t)max_alloc) {
        spinlock_release(&heap_lock);
        return NULL;
    }
    
    // Calculate total size needed (including header)
    uint32 total_size = align_size((uint32)(sizeof(heap_block_t) + size));

    if (total_size >= 256 * 1024) {
        debuglog(DEBUG_INFO, "[HEAP] kmalloc large: req=%u total=%u free=%u end=0x%x pages=%u\n",
                 (uint32)size, total_size, heap_state.free_size, heap_state.current_end,
                 (total_size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE);
    }
    
    // Find suitable free block
    heap_block_t* block = find_free_block(total_size);
    
    if (!block) {
        uint32 pages_needed = (total_size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;

        if (pages_needed > 32 && pages_needed <= 512 && !g_heap_prealloc_busy) {
            g_heap_prealloc_busy = true;
            spinlock_release(&heap_lock);

            for (uint32 i = 0; i < pages_needed; i++) {
                g_heap_prealloc_buf[i] = pmm_alloc_frame();
                if (g_heap_prealloc_buf[i] == 0) {
                    for (uint32 j = 0; j < i; j++) {
                        pmm_free_frame(g_heap_prealloc_buf[j]);
                    }
                    g_heap_prealloc_busy = false;
                    debuglog(DEBUG_WARN, "[HEAP] OOM: pre-alloc %u frames for %u byte alloc\n",
                             pages_needed, (uint32)size);
                    return NULL;
                }
            }

            spinlock_acquire(&heap_lock);

            block = find_free_block(total_size);
            if (!block) {
                block = expand_heap_with_prealloc(total_size, g_heap_prealloc_buf,
                                                  pages_needed, pages_needed);
            } else {
                for (uint32 i = 0; i < pages_needed; i++) {
                    if (g_heap_prealloc_buf[i]) {
                        pmm_free_frame(g_heap_prealloc_buf[i]);
                    }
                    g_heap_prealloc_buf[i] = 0;
                }
            }
            g_heap_prealloc_busy = false;

            if (!block) {
                for (uint32 i = 0; i < pages_needed; i++) {
                    if (g_heap_prealloc_buf[i]) {
                        pmm_free_frame(g_heap_prealloc_buf[i]);
                        g_heap_prealloc_buf[i] = 0;
                    }
                }
                spinlock_release(&heap_lock);
                return NULL;
            }
        } else {
            heap_block_t* expanded_block = NULL;
            if (expand_heap(total_size, &expanded_block) != MEMORY_OK) {
                spinlock_release(&heap_lock);
                return NULL;
            }

            if (expanded_block && is_valid_block(expanded_block) &&
                expanded_block->size >= total_size &&
                expanded_block->status == BLOCK_FREE) {
                block = expanded_block;
            } else {
                block = find_free_block(total_size);
            }

            if (!block) {
                spinlock_release(&heap_lock);
                return NULL;
            }
        }
    }
    
    // Double-check block validity before use
    if (!is_valid_block(block)) {
        spinlock_release(&heap_lock);
        return NULL;
    }
    
    // Remove from free list
    remove_from_free_list(block);
    
    // Split block if it's too large
    split_block(block, total_size);
    
    // Update statistics
    heap_state.used_size += block->size;
    heap_state.free_size -= block->size;
    heap_state.alloc_count++;

    void* out = block_to_data(block);
    spinlock_release(&heap_lock);
    return out;
}

#define KMALLOC_ALIGNED_MAGIC 0xA11CEDUL

void* kmalloc_aligned(size_t size, uint32 alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return kmalloc(size);
    }

    /* Over-allocate: room for magic+raw_ptr overhead + up to (alignment-1) slide.
     * Layout inside the allocated block's data region:
     *   [magic:uint32][raw_data_ptr:uintptr_t][padding bytes][aligned data begins]
     * kfree detects the magic and recovers raw_data_ptr. */
    uint32 overhead = sizeof(uint32) + sizeof(uintptr_t);
    uint32 extra_size = size + alignment + overhead;
    void* raw_ptr = kmalloc(extra_size);
    if (!raw_ptr) {
        return NULL;
    }

    /* Find next aligned address after raw_ptr + overhead */
    uintptr_t start = (uintptr_t)raw_ptr + overhead;
    uintptr_t aligned_addr = (start + alignment - 1) & ~((uintptr_t)(alignment - 1));

    /* Write magic and original pointer in the two slots before aligned_addr */
    ((uint32*)aligned_addr)[-2]    = KMALLOC_ALIGNED_MAGIC;
    ((uintptr_t*)aligned_addr)[-1] = (uintptr_t)raw_ptr;

    return (void*)aligned_addr;
}

void* kzalloc(size_t size) {
    void* ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void* ptr) {
    spinlock_acquire(&heap_lock);
    if (!heap_state.initialized || !ptr) {
        spinlock_release(&heap_lock);
        return;
    }

    /* Detect aligned allocation: magic+raw_ptr stored before ptr.
     * Check magic only when there's enough room before ptr. */
    uint32 overhead = sizeof(uint32) + sizeof(uintptr_t);
    if ((uint32)ptr >= heap_state.start_addr + overhead) {
        uint32 maybe_magic = ((uint32*)ptr)[-2];
        if (maybe_magic == KMALLOC_ALIGNED_MAGIC) {
            /* Recover original raw pointer and free that instead */
            void* raw_ptr = (void*)((uintptr_t*)ptr)[-1];
            if ((uint32)raw_ptr >= heap_state.start_addr + sizeof(heap_block_t) &&
                (uint32)raw_ptr < heap_state.current_end) {
                ptr = raw_ptr;
            }
        }
    }

    // Validate pointer is within heap bounds
    if ((uint32)ptr < heap_state.start_addr + sizeof(heap_block_t) ||
        (uint32)ptr >= heap_state.current_end) {
        print("[HEAP] Error: Pointer outside heap bounds: 0x");
        print_hex((uint32)ptr);
        print("\n");
        spinlock_release(&heap_lock);
        return;
    }

    heap_block_t* block = data_to_block(ptr);
    
    if (!is_valid_block(block) || block->status != BLOCK_USED) {
        print("[HEAP] Error: Invalid free() on ");
        print_hex((uint32)ptr);
        print(" (block at 0x");
        print_hex((uint32)block);
        print(")\n");
        spinlock_release(&heap_lock);
        return;
    }
    
    // Update statistics
    heap_state.used_size -= block->size;
    heap_state.free_size += block->size;
    heap_state.free_count++;
    
    // Add to free list (which now handles coalescing)
    add_to_free_list(block);
    spinlock_release(&heap_lock);
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    // Validate input pointer bounds
    if ((uint32)ptr < heap_state.start_addr + sizeof(heap_block_t) || 
        (uint32)ptr >= heap_state.current_end) {
        print("[HEAP] ERROR: realloc on invalid pointer 0x"); print_hex((uint32)ptr); print("\n");
        return NULL;
    }
    
    heap_block_t* block = data_to_block(ptr);
    
    if (!is_valid_block(block) || block->status != BLOCK_USED) {
        print("[HEAP] ERROR: realloc on invalid or free block\n");
        return NULL;
    }
    
    uint32 old_data_size = block->size - sizeof(heap_block_t);
    
    // If new size fits in current block, just return the same pointer
    if (new_size <= old_data_size) {
        return ptr;
    }
    
    // Need to allocate a new larger block
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    
    // Copy old data
    memcpy(new_ptr, ptr, old_data_size);
    
    // Free old block
    kfree(ptr);
    
    return new_ptr;
}

void heap_get_stats(uint32* total_size, uint32* used_size, uint32* free_size) {
    spinlock_acquire(&heap_lock);
    if (total_size) *total_size = heap_state.total_size;
    if (used_size) *used_size = heap_state.used_size;
    if (free_size) *free_size = heap_state.free_size;
    spinlock_release(&heap_lock);
}

uint32_t kheap_get_free_memory(void) {
    return heap_state.free_size;
}

bool kheap_check_memory_pressure(uint32_t needed) {
    return heap_state.free_size >= needed;
}

memory_result_t kheap_graphics_pool_init(uint32_t pool_size) {
    if (g_gfx_pool.initialized) return MEMORY_OK;

    spinlock_acquire(&heap_lock);
    uint32 free = heap_state.free_size;
    spinlock_release(&heap_lock);

    if (free < pool_size + 256 * 1024) {
        debuglog(DEBUG_WARN, "[HEAP] Graphics pool skipped: free=%u need=%u\n", free, pool_size);
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }

    g_gfx_pool.base = (uint8_t*)kmalloc(pool_size);
    if (!g_gfx_pool.base) {
        debuglog(DEBUG_WARN, "[HEAP] Graphics pool alloc failed: %u bytes\n", pool_size);
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }

    g_gfx_pool.size = pool_size;
    g_gfx_pool.used = 0;
    g_gfx_pool.initialized = true;

    debuglog(DEBUG_INFO, "[HEAP] Graphics pool: %u KB at %p\n", pool_size / 1024, g_gfx_pool.base);
    return MEMORY_OK;
}

void* kheap_graphics_alloc(uint32_t size) {
    if (!g_gfx_pool.initialized || size == 0) return kmalloc(size);

    uint32_t aligned = (size + 15) & ~15;

    spinlock_acquire(&heap_lock);
    if (g_gfx_pool.used + aligned > g_gfx_pool.size) {
        spinlock_release(&heap_lock);
        debuglog(DEBUG_WARN, "[HEAP] Graphics pool full (need=%u free=%u), fallback kmalloc\n",
                 aligned, g_gfx_pool.size - g_gfx_pool.used);
        return kmalloc(size);
    }
    void* ptr = g_gfx_pool.base + g_gfx_pool.used;
    g_gfx_pool.used += aligned;
    spinlock_release(&heap_lock);

    debuglog(DEBUG_INFO, "[HEAP] gfx_alloc: %u bytes -> %p (pool used=%u/%u)\n",
             size, ptr, g_gfx_pool.used, g_gfx_pool.size);
    return ptr;
}

void kheap_graphics_free(void* ptr) {
    if (!ptr || !g_gfx_pool.initialized) return;

    if ((uint8_t*)ptr >= g_gfx_pool.base &&
        (uint8_t*)ptr < g_gfx_pool.base + g_gfx_pool.size) {
        return;
    }
    kfree(ptr);
}
