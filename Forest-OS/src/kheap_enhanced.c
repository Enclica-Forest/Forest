/**
 * @file kheap_enhanced.c
 * @brief Enhanced Kernel Heap Implementation
 * 
 * Features:
 * - Per-CPU caches for reduced contention
 * - Size-class pools for common allocation sizes
 * - Coalescing to reduce fragmentation
 * - Detailed statistics and debugging
 * - Memory leak detection
 * 
 * Based on dlmalloc and SLAB allocator concepts.
 */

#include "include/kheap_enhanced.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/debuglog.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

// Size classes for small allocations (bytes)
static const uint32_t SIZE_CLASSES[] = {
    16, 32, 48, 64, 96, 128, 192, 256,
    384, 512, 768, 1024, 1536, 2048, 3072, 4096
};
#define NUM_SIZE_CLASSES    16

// Per-CPU cache size
#define CACHE_SIZE          32

// Minimum allocation from main heap
#define MIN_ALLOC_SIZE      16

// Block header magic (for debugging)
#define BLOCK_MAGIC         0xDEADBEEF
#define FREE_MAGIC          0xFEEDFACE

// Maximum heap size
#define MAX_HEAP_SIZE       (256 * 1024 * 1024)  // 256MB

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Block header for allocated/free blocks
 */
typedef struct block_header {
    uint32_t magic;                 // Debug magic
    uint32_t size;                  // Size of data (not including header)
    uint32_t flags;                 // Block flags
    struct block_header* prev;      // Previous block in memory
    struct block_header* next;      // Next block in memory
    struct block_header* prev_free; // Previous free block (if free)
    struct block_header* next_free; // Next free block (if free)
#ifdef HEAP_DEBUG
    const char* file;               // Allocation file
    int line;                       // Allocation line
#endif
} block_header_t;

// Block flags
#define BLOCK_FREE          0x01
#define BLOCK_LAST          0x02

/**
 * @brief Size class pool
 */
typedef struct {
    block_header_t* free_list;
    uint32_t size;
    uint32_t count;
    uint32_t alloc_count;
    uint32_t free_count;
    spinlock_t lock;
} size_class_t;

/**
 * @brief Per-CPU cache
 */
typedef struct {
    void* cache[NUM_SIZE_CLASSES][CACHE_SIZE];
    uint32_t counts[NUM_SIZE_CLASSES];
} percpu_cache_t;

/**
 * @brief Heap state
 */
static struct {
    bool initialized;
    
    // Heap bounds
    uint32_t heap_start;
    uint32_t heap_end;
    uint32_t heap_current;
    uint32_t heap_max;
    
    // Free list for large allocations
    block_header_t* free_list;
    
    // Size class pools
    size_class_t size_classes[NUM_SIZE_CLASSES];
    
    // Per-CPU caches (simplified: single cache for now)
    percpu_cache_t percpu_cache;
    
    // Statistics
    uint64_t total_allocated;
    uint64_t total_freed;
    uint64_t current_used;
    uint64_t peak_used;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t realloc_count;
    
    // Main heap lock
    spinlock_t heap_lock;
} heap_state = { .initialized = false };

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Get size class index for a given size
 */
static int get_size_class_index(uint32_t size) {
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (size <= SIZE_CLASSES[i]) {
            return i;
        }
    }
    return -1;  // Too large for size classes
}

/**
 * @brief Align size to minimum alignment
 */
static inline uint32_t align_size(uint32_t size) {
    return (size + MIN_ALLOC_SIZE - 1) & ~(MIN_ALLOC_SIZE - 1);
}

/**
 * @brief Get data pointer from block header
 */
static inline void* block_to_data(block_header_t* block) {
    return (void*)((uint8_t*)block + sizeof(block_header_t));
}

/**
 * @brief Get block header from data pointer
 */
static inline block_header_t* data_to_block(void* ptr) {
    return (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
}

/**
 * @brief Check if block is valid
 */
static bool is_valid_block(block_header_t* block) {
    if (!block) return false;
    return (block->magic == BLOCK_MAGIC || block->magic == FREE_MAGIC);
}

// ============================================================================
// SIZE CLASS ALLOCATION
// ============================================================================

/**
 * @brief Allocate from size class pool
 */
static void* alloc_from_size_class(int class_idx) {
    size_class_t* sc = &heap_state.size_classes[class_idx];
    
    spinlock_acquire(&sc->lock);
    
    if (sc->free_list) {
        block_header_t* block = sc->free_list;
        sc->free_list = block->next_free;
        block->magic = BLOCK_MAGIC;
        block->flags &= ~BLOCK_FREE;
        sc->alloc_count++;
        sc->count--;
        
        spinlock_release(&sc->lock);
        return block_to_data(block);
    }
    
    spinlock_release(&sc->lock);
    return NULL;
}

/**
 * @brief Free to size class pool
 */
static bool free_to_size_class(block_header_t* block) {
    int class_idx = get_size_class_index(block->size);
    if (class_idx < 0) {
        return false;
    }
    
    size_class_t* sc = &heap_state.size_classes[class_idx];
    
    spinlock_acquire(&sc->lock);
    
    block->magic = FREE_MAGIC;
    block->flags |= BLOCK_FREE;
    block->next_free = sc->free_list;
    sc->free_list = block;
    sc->free_count++;
    sc->count++;
    
    spinlock_release(&sc->lock);
    return true;
}

// ============================================================================
// PER-CPU CACHE
// ============================================================================

/**
 * @brief Allocate from per-CPU cache
 */
static void* alloc_from_cache(int class_idx) {
    percpu_cache_t* cache = &heap_state.percpu_cache;
    
    if (cache->counts[class_idx] > 0) {
        cache->counts[class_idx]--;
        return cache->cache[class_idx][cache->counts[class_idx]];
    }
    
    return NULL;
}

/**
 * @brief Free to per-CPU cache
 */
static bool free_to_cache(void* ptr, int class_idx) {
    percpu_cache_t* cache = &heap_state.percpu_cache;
    
    if (cache->counts[class_idx] < CACHE_SIZE) {
        cache->cache[class_idx][cache->counts[class_idx]] = ptr;
        cache->counts[class_idx]++;
        return true;
    }
    
    return false;
}

// ============================================================================
// MAIN HEAP OPERATIONS
// ============================================================================

/**
 * @brief Find a free block of sufficient size
 */
static block_header_t* find_free_block(uint32_t size) {
    block_header_t* best = NULL;
    uint32_t best_size = UINT32_MAX;
    
    // Best-fit search
    for (block_header_t* current = heap_state.free_list; 
         current != NULL; 
         current = current->next_free) {
        if (current->size >= size && current->size < best_size) {
            best = current;
            best_size = current->size;
            
            // Perfect fit
            if (best_size == size) {
                break;
            }
        }
    }
    
    return best;
}

/**
 * @brief Remove block from free list
 */
static void remove_from_free_list(block_header_t* block) {
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        heap_state.free_list = block->next_free;
    }
    
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    
    block->prev_free = NULL;
    block->next_free = NULL;
}

/**
 * @brief Add block to free list
 */
static void add_to_free_list(block_header_t* block) {
    block->prev_free = NULL;
    block->next_free = heap_state.free_list;
    
    if (heap_state.free_list) {
        heap_state.free_list->prev_free = block;
    }
    
    heap_state.free_list = block;
}

/**
 * @brief Split a block if it's too large
 */
static void split_block(block_header_t* block, uint32_t size) {
    uint32_t remaining = block->size - size - sizeof(block_header_t);
    
    // Only split if remainder is large enough
    if (remaining < MIN_ALLOC_SIZE + sizeof(block_header_t)) {
        return;
    }
    
    // Create new block for remainder
    block_header_t* new_block = (block_header_t*)((uint8_t*)block + 
                                sizeof(block_header_t) + size);
    new_block->magic = FREE_MAGIC;
    new_block->size = remaining;
    new_block->flags = BLOCK_FREE;
    new_block->prev = block;
    new_block->next = block->next;
    
    if (block->next) {
        block->next->prev = new_block;
    }
    
    block->next = new_block;
    block->size = size;
    
    // Add remainder to free list
    add_to_free_list(new_block);
}

/**
 * @brief Coalesce with adjacent free blocks
 */
static block_header_t* coalesce(block_header_t* block) {
    // Coalesce with next block
    if (block->next && (block->next->flags & BLOCK_FREE)) {
        block_header_t* next = block->next;
        remove_from_free_list(next);
        
        block->size += sizeof(block_header_t) + next->size;
        block->next = next->next;
        
        if (next->next) {
            next->next->prev = block;
        }
    }
    
    // Coalesce with previous block
    if (block->prev && (block->prev->flags & BLOCK_FREE)) {
        block_header_t* prev = block->prev;
        remove_from_free_list(prev);
        
        prev->size += sizeof(block_header_t) + block->size;
        prev->next = block->next;
        
        if (block->next) {
            block->next->prev = prev;
        }
        
        block = prev;
    }
    
    return block;
}

/**
 * @brief Expand heap
 */
static block_header_t* expand_heap(uint32_t size) {
    uint32_t needed = size + sizeof(block_header_t);
    needed = (needed + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
    
    if (heap_state.heap_current + needed > heap_state.heap_max) {
        return NULL;  // Out of heap space
    }
    
    // Allocate new pages
    for (uint32_t addr = heap_state.heap_current; 
         addr < heap_state.heap_current + needed; 
         addr += MEMORY_PAGE_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return NULL;
        }
        
        page_directory_t* dir = vmm_get_current_page_directory();
        vmm_map_page(dir, addr, frame, PAGE_PRESENT | PAGE_WRITABLE);
    }
    
    // Create new block
    block_header_t* block = (block_header_t*)heap_state.heap_current;
    block->magic = FREE_MAGIC;
    block->size = needed - sizeof(block_header_t);
    block->flags = BLOCK_FREE | BLOCK_LAST;
    block->prev = NULL;
    block->next = NULL;
    block->prev_free = NULL;
    block->next_free = NULL;
    
    heap_state.heap_current += needed;
    
    // Find last block and link
    block_header_t* last = heap_state.free_list;
    while (last && last->next) {
        last = last->next;
    }
    
    if (last) {
        last->flags &= ~BLOCK_LAST;
        last->next = block;
        block->prev = last;
    }
    
    add_to_free_list(block);
    
    return block;
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize enhanced kernel heap
 */
memory_result_t kheap_enhanced_init(uint32_t start, uint32_t initial_size) {
    if (heap_state.initialized) {
        return MEMORY_OK;
    }
    
    print("[KHEAP-E] Initializing enhanced kernel heap at 0x");
    print_hex(start);
    print("...\n");
    
    // Initialize state
    heap_state.heap_start = start;
    heap_state.heap_end = start + initial_size;
    heap_state.heap_current = start;
    heap_state.heap_max = start + MAX_HEAP_SIZE;
    heap_state.free_list = NULL;
    
    // Initialize locks
    spinlock_init(&heap_state.heap_lock, "kheap_main");
    
    // Initialize size classes
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        heap_state.size_classes[i].size = SIZE_CLASSES[i];
        heap_state.size_classes[i].free_list = NULL;
        heap_state.size_classes[i].count = 0;
        heap_state.size_classes[i].alloc_count = 0;
        heap_state.size_classes[i].free_count = 0;
        spinlock_init(&heap_state.size_classes[i].lock, "size_class");
    }
    
    // Initialize per-CPU cache
    memset(&heap_state.percpu_cache, 0, sizeof(percpu_cache_t));
    
    // Initialize statistics
    heap_state.total_allocated = 0;
    heap_state.total_freed = 0;
    heap_state.current_used = 0;
    heap_state.peak_used = 0;
    heap_state.alloc_count = 0;
    heap_state.free_count = 0;
    heap_state.realloc_count = 0;
    
    // Create initial heap block
    expand_heap(initial_size - sizeof(block_header_t));
    
    heap_state.initialized = true;
    
    print("[KHEAP-E] Enhanced kernel heap initialized (");
    print_dec(initial_size / 1024);
    print(" KB initial)\n");
    
    return MEMORY_OK;
}

/**
 * @brief Allocate memory
 */
void* kheap_alloc(uint32_t size) {
    if (!heap_state.initialized || size == 0) {
        return NULL;
    }
    
    size = align_size(size);
    
    // Try per-CPU cache first
    int class_idx = get_size_class_index(size);
    if (class_idx >= 0) {
        void* ptr = alloc_from_cache(class_idx);
        if (ptr) {
            heap_state.alloc_count++;
            heap_state.current_used += SIZE_CLASSES[class_idx];
            if (heap_state.current_used > heap_state.peak_used) {
                heap_state.peak_used = heap_state.current_used;
            }
            return ptr;
        }
        
        // Try size class pool
        ptr = alloc_from_size_class(class_idx);
        if (ptr) {
            heap_state.alloc_count++;
            heap_state.current_used += SIZE_CLASSES[class_idx];
            if (heap_state.current_used > heap_state.peak_used) {
                heap_state.peak_used = heap_state.current_used;
            }
            return ptr;
        }
    }
    
    // Fall back to main heap
    spinlock_acquire(&heap_state.heap_lock);
    
    block_header_t* block = find_free_block(size);
    if (!block) {
        block = expand_heap(size);
        if (!block) {
            spinlock_release(&heap_state.heap_lock);
            return NULL;
        }
    }
    
    remove_from_free_list(block);
    split_block(block, size);
    
    block->magic = BLOCK_MAGIC;
    block->flags &= ~BLOCK_FREE;
    
    heap_state.total_allocated += block->size;
    heap_state.current_used += block->size;
    heap_state.alloc_count++;
    
    if (heap_state.current_used > heap_state.peak_used) {
        heap_state.peak_used = heap_state.current_used;
    }
    
    spinlock_release(&heap_state.heap_lock);
    
    return block_to_data(block);
}

/**
 * @brief Allocate zeroed memory
 */
void* kheap_zalloc(uint32_t size) {
    void* ptr = kheap_alloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/**
 * @brief Free memory
 */
void kheap_free(void* ptr) {
    if (!heap_state.initialized || !ptr) {
        return;
    }
    
    block_header_t* block = data_to_block(ptr);
    
    if (!is_valid_block(block)) {
        print("[KHEAP-E] WARNING: Invalid free at 0x");
        print_hex((uint32_t)ptr);
        print("\n");
        return;
    }
    
    if (block->flags & BLOCK_FREE) {
        print("[KHEAP-E] WARNING: Double free at 0x");
        print_hex((uint32_t)ptr);
        print("\n");
        return;
    }
    
    uint32_t size = block->size;
    
    // Try per-CPU cache first
    int class_idx = get_size_class_index(size);
    if (class_idx >= 0) {
        if (free_to_cache(ptr, class_idx)) {
            heap_state.free_count++;
            heap_state.current_used -= SIZE_CLASSES[class_idx];
            return;
        }
        
        // Try size class pool
        if (free_to_size_class(block)) {
            heap_state.free_count++;
            heap_state.current_used -= SIZE_CLASSES[class_idx];
            return;
        }
    }
    
    // Return to main heap
    spinlock_acquire(&heap_state.heap_lock);
    
    block->magic = FREE_MAGIC;
    block->flags |= BLOCK_FREE;
    
    block = coalesce(block);
    add_to_free_list(block);
    
    heap_state.total_freed += size;
    heap_state.current_used -= size;
    heap_state.free_count++;
    
    spinlock_release(&heap_state.heap_lock);
}

/**
 * @brief Reallocate memory
 */
void* kheap_realloc(void* ptr, uint32_t new_size) {
    if (!ptr) {
        return kheap_alloc(new_size);
    }
    
    if (new_size == 0) {
        kheap_free(ptr);
        return NULL;
    }
    
    block_header_t* block = data_to_block(ptr);
    if (!is_valid_block(block)) {
        return NULL;
    }
    
    // If new size fits in current block, just return
    if (new_size <= block->size) {
        return ptr;
    }
    
    // Allocate new block and copy
    void* new_ptr = kheap_alloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    
    memcpy(new_ptr, ptr, block->size);
    kheap_free(ptr);
    
    heap_state.realloc_count++;
    
    return new_ptr;
}

/**
 * @brief Get heap statistics
 */
kheap_stats_t kheap_get_stats(void) {
    kheap_stats_t stats;
    
    stats.total_size = heap_state.heap_current - heap_state.heap_start;
    stats.used_size = heap_state.current_used;
    stats.free_size = stats.total_size - stats.used_size;
    stats.peak_used = heap_state.peak_used;
    stats.alloc_count = heap_state.alloc_count;
    stats.free_count = heap_state.free_count;
    stats.realloc_count = heap_state.realloc_count;
    
    return stats;
}

/**
 * @brief Dump heap statistics
 */
void kheap_dump_stats(void) {
    kheap_stats_t stats = kheap_get_stats();
    
    print("\n=== Enhanced Kernel Heap ===\n");
    print("Total Size: ");
    print_dec(stats.total_size / 1024);
    print(" KB\n");
    print("Used: ");
    print_dec(stats.used_size / 1024);
    print(" KB\n");
    print("Free: ");
    print_dec(stats.free_size / 1024);
    print(" KB\n");
    print("Peak: ");
    print_dec(stats.peak_used / 1024);
    print(" KB\n");
    print("Allocations: ");
    print_dec((uint32_t)stats.alloc_count);
    print("\n");
    print("Frees: ");
    print_dec((uint32_t)stats.free_count);
    print("\n");
    print("============================\n\n");
}
