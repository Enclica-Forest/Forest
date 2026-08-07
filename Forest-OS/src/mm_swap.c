/**
 * @file mm_swap.c
 * @brief Virtual Memory Swap Implementation
 * 
 * Provides swap space support for virtual memory:
 * - Swap file/partition management
 * - LRU page replacement
 * - Page swapping in/out
 * - Swap space allocation
 * 
 * This allows the system to use more virtual memory
 * than physical RAM by swapping inactive pages to disk.
 */

#include "include/mm_swap.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/vfs.h"
#include "include/debuglog.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define SWAP_MAX_SIZE       (1024 * 1024 * 1024)    // 1GB max swap
#define SWAP_PAGE_SIZE      MEMORY_PAGE_SIZE
#define SWAP_CLUSTER_SIZE   8                        // Pages per cluster
#define SWAP_BITMAP_SIZE    (SWAP_MAX_SIZE / SWAP_PAGE_SIZE / 8)

// LRU configuration
#define LRU_ACTIVE_RATIO    2   // Active list is 2x inactive
#define LRU_SCAN_BATCH      32  // Pages to scan per reclaim attempt

// Swap entry encoding
// Bits 0-23: Swap offset (page index in swap)
// Bits 24-31: Swap device (for multiple swap devices)
#define SWAP_OFFSET_MASK    0x00FFFFFF
#define SWAP_DEVICE_SHIFT   24

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Swap extent (contiguous region)
 */
typedef struct swap_extent {
    uint32_t start_page;
    uint32_t nr_pages;
    struct swap_extent* next;
} swap_extent_t;

/**
 * @brief Swap device descriptor
 */
typedef struct {
    bool active;
    uint32_t type;              // 0 = file, 1 = partition
    void* backing;              // File handle or device
    uint32_t size_pages;        // Total pages in swap
    uint32_t free_pages;        // Free pages
    uint32_t next_free;         // Next free cluster
    
    // Bitmap for free space tracking
    uint8_t* bitmap;
    
    // Extents for non-contiguous swap files
    swap_extent_t* extents;
    
    // Priority (higher = preferred)
    int priority;
    
    // Statistics
    uint64_t pages_in;
    uint64_t pages_out;
} swap_device_t;

/**
 * @brief LRU list node
 */
typedef struct lru_node {
    phys_addr_t phys_addr;
    uint32_t vaddr;
    page_directory_t* dir;
    uint32_t age;
    struct lru_node* prev;
    struct lru_node* next;
} lru_node_t;

/**
 * @brief LRU lists
 */
typedef struct {
    lru_node_t* active_head;
    lru_node_t* active_tail;
    uint32_t active_count;
    
    lru_node_t* inactive_head;
    lru_node_t* inactive_tail;
    uint32_t inactive_count;
} lru_lists_t;

/**
 * @brief Swap manager state
 */
static struct {
    bool initialized;
    
    // Swap devices (support multiple)
    swap_device_t devices[4];
    uint32_t device_count;
    
    // LRU tracking
    lru_lists_t lru;
    
    // Statistics
    uint64_t total_swap_pages;
    uint64_t free_swap_pages;
    uint64_t pages_swapped_in;
    uint64_t pages_swapped_out;
    
    // Lock
    spinlock_t lock;
} swap_state = { .initialized = false };

// ============================================================================
// BITMAP OPERATIONS
// ============================================================================

static inline void swap_bitmap_set(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static inline void swap_bitmap_clear(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool swap_bitmap_test(uint8_t* bitmap, uint32_t index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

// ============================================================================
// SWAP SPACE ALLOCATION
// ============================================================================

/**
 * @brief Allocate swap slot
 */
static uint32_t swap_alloc_slot(swap_device_t* dev) {
    if (dev->free_pages == 0) {
        return SWAP_ENTRY_INVALID;
    }
    
    // Search for free slot starting from next_free
    uint32_t start = dev->next_free;
    uint32_t slot = start;
    
    do {
        if (!swap_bitmap_test(dev->bitmap, slot)) {
            // Found free slot
            swap_bitmap_set(dev->bitmap, slot);
            dev->free_pages--;
            dev->next_free = (slot + 1) % dev->size_pages;
            return slot;
        }
        slot = (slot + 1) % dev->size_pages;
    } while (slot != start);
    
    return SWAP_ENTRY_INVALID;
}

/**
 * @brief Free swap slot
 */
static void swap_free_slot(swap_device_t* dev, uint32_t slot) {
    if (slot >= dev->size_pages) {
        return;
    }
    
    if (swap_bitmap_test(dev->bitmap, slot)) {
        swap_bitmap_clear(dev->bitmap, slot);
        dev->free_pages++;
    }
}

// ============================================================================
// LRU MANAGEMENT
// ============================================================================

/**
 * @brief Add page to LRU active list
 */
static void lru_add_active(phys_addr_t phys, uint32_t vaddr, page_directory_t* dir) {
    lru_node_t* node = (lru_node_t*)kmalloc(sizeof(lru_node_t));
    if (!node) return;
    
    node->phys_addr = phys;
    node->vaddr = vaddr;
    node->dir = dir;
    node->age = 0;
    
    // Add to head of active list
    node->prev = NULL;
    node->next = swap_state.lru.active_head;
    
    if (swap_state.lru.active_head) {
        swap_state.lru.active_head->prev = node;
    }
    swap_state.lru.active_head = node;
    
    if (!swap_state.lru.active_tail) {
        swap_state.lru.active_tail = node;
    }
    
    swap_state.lru.active_count++;
}

/**
 * @brief Move page from active to inactive list
 */
static void lru_demote_page(lru_node_t* node) {
    // Remove from active list
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        swap_state.lru.active_head = node->next;
    }
    
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        swap_state.lru.active_tail = node->prev;
    }
    
    swap_state.lru.active_count--;
    
    // Add to head of inactive list
    node->prev = NULL;
    node->next = swap_state.lru.inactive_head;
    
    if (swap_state.lru.inactive_head) {
        swap_state.lru.inactive_head->prev = node;
    }
    swap_state.lru.inactive_head = node;
    
    if (!swap_state.lru.inactive_tail) {
        swap_state.lru.inactive_tail = node;
    }
    
    swap_state.lru.inactive_count++;
}

/**
 * @brief Get least recently used page for swapping
 */
static lru_node_t* lru_get_victim(void) {
    // Try inactive list first
    if (swap_state.lru.inactive_tail) {
        return swap_state.lru.inactive_tail;
    }
    
    // Fall back to active list
    if (swap_state.lru.active_tail) {
        return swap_state.lru.active_tail;
    }
    
    return NULL;
}

// ============================================================================
// SWAP I/O
// ============================================================================

/**
 * @brief Write page to swap
 */
static bool swap_write_page(swap_device_t* dev, uint32_t slot, void* page_data) {
    if (!dev->active || !dev->backing) {
        return false;
    }
    
    // For now, simulate swap I/O
    // In a real implementation, this would write to disk
    dev->pages_out++;
    
    return true;
}

/**
 * @brief Read page from swap
 */
static bool swap_read_page(swap_device_t* dev, uint32_t slot, void* page_data) {
    if (!dev->active || !dev->backing) {
        return false;
    }
    
    // For now, simulate swap I/O
    // In a real implementation, this would read from disk
    dev->pages_in++;
    
    return true;
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize swap subsystem
 */
memory_result_t swap_init(void) {
    if (swap_state.initialized) {
        return MEMORY_OK;
    }
    
    print("[SWAP] Initializing swap subsystem...\n");
    
    // Initialize state
    memset(&swap_state.devices, 0, sizeof(swap_state.devices));
    swap_state.device_count = 0;
    
    // Initialize LRU lists
    memset(&swap_state.lru, 0, sizeof(swap_state.lru));
    
    // Initialize statistics
    swap_state.total_swap_pages = 0;
    swap_state.free_swap_pages = 0;
    swap_state.pages_swapped_in = 0;
    swap_state.pages_swapped_out = 0;
    
    // Initialize lock
    spinlock_init(&swap_state.lock, "swap");
    
    swap_state.initialized = true;
    
    print("[SWAP] Swap subsystem initialized\n");
    
    return MEMORY_OK;
}

/**
 * @brief Add swap device
 */
memory_result_t swap_add_device(const char* path, uint32_t size_kb, int priority) {
    if (!swap_state.initialized) {
        return MEMORY_ERROR_NOT_INITIALIZED;
    }
    
    if (swap_state.device_count >= 4) {
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }
    
    spinlock_acquire(&swap_state.lock);
    
    swap_device_t* dev = &swap_state.devices[swap_state.device_count];
    
    dev->active = true;
    dev->type = 0;  // File-based swap
    dev->backing = NULL;  // Would open file here
    dev->size_pages = (size_kb * 1024) / SWAP_PAGE_SIZE;
    dev->free_pages = dev->size_pages;
    dev->next_free = 0;
    dev->priority = priority;
    dev->pages_in = 0;
    dev->pages_out = 0;
    
    // Allocate bitmap
    uint32_t bitmap_size = (dev->size_pages + 7) / 8;
    dev->bitmap = (uint8_t*)kmalloc(bitmap_size);
    if (!dev->bitmap) {
        spinlock_release(&swap_state.lock);
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }
    memset(dev->bitmap, 0, bitmap_size);
    
    swap_state.total_swap_pages += dev->size_pages;
    swap_state.free_swap_pages += dev->size_pages;
    swap_state.device_count++;
    
    spinlock_release(&swap_state.lock);
    
    print("[SWAP] Added swap device: ");
    print(path);
    print(" (");
    print_dec(size_kb / 1024);
    print(" MB)\n");
    
    return MEMORY_OK;
}

/**
 * @brief Swap out a page
 */
swap_entry_t swap_out_page(page_directory_t* dir, uint32_t vaddr) {
    if (!swap_state.initialized || swap_state.device_count == 0) {
        return SWAP_ENTRY_INVALID;
    }
    
    spinlock_acquire(&swap_state.lock);
    
    // Get physical address
    phys_addr_t phys = vmm_get_physical_addr(dir, vaddr);
    if (phys == 0) {
        spinlock_release(&swap_state.lock);
        return SWAP_ENTRY_INVALID;
    }
    
    // Find best swap device (by priority)
    swap_device_t* best_dev = NULL;
    int best_priority = -1;
    uint32_t dev_index = 0;
    
    for (uint32_t i = 0; i < swap_state.device_count; i++) {
        swap_device_t* dev = &swap_state.devices[i];
        if (dev->active && dev->free_pages > 0 && dev->priority > best_priority) {
            best_dev = dev;
            best_priority = dev->priority;
            dev_index = i;
        }
    }
    
    if (!best_dev) {
        spinlock_release(&swap_state.lock);
        return SWAP_ENTRY_INVALID;
    }
    
    // Allocate swap slot
    uint32_t slot = swap_alloc_slot(best_dev);
    if (slot == SWAP_ENTRY_INVALID) {
        spinlock_release(&swap_state.lock);
        return SWAP_ENTRY_INVALID;
    }
    
    // Write page to swap
    if (!swap_write_page(best_dev, slot, (void*)phys)) {
        swap_free_slot(best_dev, slot);
        spinlock_release(&swap_state.lock);
        return SWAP_ENTRY_INVALID;
    }
    
    // Unmap page and free physical frame
    vmm_unmap_page(dir, vaddr);
    pmm_free_frame(phys);
    
    // Update statistics
    swap_state.free_swap_pages--;
    swap_state.pages_swapped_out++;
    
    spinlock_release(&swap_state.lock);
    
    // Encode swap entry
    return (dev_index << SWAP_DEVICE_SHIFT) | slot;
}

/**
 * @brief Swap in a page
 */
memory_result_t swap_in_page(page_directory_t* dir, uint32_t vaddr, swap_entry_t entry) {
    if (!swap_state.initialized) {
        return MEMORY_ERROR_NOT_INITIALIZED;
    }
    
    uint32_t dev_index = entry >> SWAP_DEVICE_SHIFT;
    uint32_t slot = entry & SWAP_OFFSET_MASK;
    
    if (dev_index >= swap_state.device_count) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    spinlock_acquire(&swap_state.lock);
    
    swap_device_t* dev = &swap_state.devices[dev_index];
    if (!dev->active || slot >= dev->size_pages) {
        spinlock_release(&swap_state.lock);
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    // Allocate new physical frame
    phys_addr_t new_frame = pmm_alloc_frame();
    if (new_frame == 0) {
        spinlock_release(&swap_state.lock);
        return MEMORY_ERROR_OUT_OF_MEMORY;
    }
    
    // Read page from swap
    if (!swap_read_page(dev, slot, (void*)new_frame)) {
        pmm_free_frame(new_frame);
        spinlock_release(&swap_state.lock);
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    // Map new frame
    memory_result_t result = vmm_map_page(dir, vaddr, new_frame, 
                                          PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    if (result != MEMORY_OK) {
        pmm_free_frame(new_frame);
        spinlock_release(&swap_state.lock);
        return result;
    }
    
    // Free swap slot
    swap_free_slot(dev, slot);
    
    // Update statistics
    swap_state.free_swap_pages++;
    swap_state.pages_swapped_in++;
    
    spinlock_release(&swap_state.lock);
    
    return MEMORY_OK;
}

/**
 * @brief Try to reclaim memory by swapping
 */
uint32_t swap_reclaim_memory(uint32_t pages_needed) {
    if (!swap_state.initialized || swap_state.device_count == 0) {
        return 0;
    }
    
    uint32_t reclaimed = 0;
    
    spinlock_acquire(&swap_state.lock);
    
    while (reclaimed < pages_needed) {
        lru_node_t* victim = lru_get_victim();
        if (!victim) {
            break;
        }
        
        swap_entry_t entry = swap_out_page(victim->dir, victim->vaddr);
        if (entry == SWAP_ENTRY_INVALID) {
            break;
        }
        
        // Remove from LRU
        // (simplified - real implementation would store swap entry in PTE)
        reclaimed++;
    }
    
    spinlock_release(&swap_state.lock);
    
    return reclaimed;
}

/**
 * @brief Check if swap is available
 */
bool swap_is_available(void) {
    return swap_state.initialized && swap_state.free_swap_pages > 0;
}

/**
 * @brief Get swap statistics
 */
swap_stats_t swap_get_stats(void) {
    swap_stats_t stats;
    
    stats.total_pages = swap_state.total_swap_pages;
    stats.free_pages = swap_state.free_swap_pages;
    stats.used_pages = stats.total_pages - stats.free_pages;
    stats.pages_in = swap_state.pages_swapped_in;
    stats.pages_out = swap_state.pages_swapped_out;
    stats.device_count = swap_state.device_count;
    
    return stats;
}

/**
 * @brief Dump swap statistics
 */
void swap_dump_stats(void) {
    swap_stats_t stats = swap_get_stats();
    
    print("\n=== Swap Statistics ===\n");
    print("Total: ");
    print_dec((uint32_t)(stats.total_pages * 4));
    print(" KB\n");
    print("Used: ");
    print_dec((uint32_t)(stats.used_pages * 4));
    print(" KB\n");
    print("Free: ");
    print_dec((uint32_t)(stats.free_pages * 4));
    print(" KB\n");
    print("Swapped in: ");
    print_dec((uint32_t)stats.pages_in);
    print(" pages\n");
    print("Swapped out: ");
    print_dec((uint32_t)stats.pages_out);
    print(" pages\n");
    print("Devices: ");
    print_dec(stats.device_count);
    print("\n");
    print("=======================\n\n");
}
