/**
 * @file pmm_enhanced.c
 * @brief Enhanced Physical Memory Manager
 * 
 * Comprehensive PMM with:
 * - Multi-zone support (DMA, NORMAL, HIGHMEM)
 * - Hybrid allocation (fast stack + bitmap for special cases)
 * - Detailed statistics tracking
 * - Fragmentation analysis
 * - Memory watermarks and pressure handling
 * 
 * Based on OSDev wiki recommendations and Linux-style memory management.
 */

#include "include/pmm_enhanced.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/debuglog.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define PMM_MAX_ZONES           4
#define PMM_STACK_SIZE          1024    // Free frames cached per zone
#define PMM_BITMAP_GRANULARITY  1       // 1 bit per page
#define PMM_MAX_ORDER           11      // Max 2^11 = 2048 pages = 8MB

// Zone boundaries (physical addresses)
#define PMM_DMA_START           0x00000000
#define PMM_DMA_END             0x01000000  // 16MB
#define PMM_NORMAL_START        0x01000000  // 16MB
#define PMM_NORMAL_END          0x38000000  // 896MB (typical)
#define PMM_HIGHMEM_START       0x38000000  // 896MB

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Free frame stack for fast allocation
 */
typedef struct {
    phys_addr_t frames[PMM_STACK_SIZE];
    uint32_t top;
    spinlock_t lock;
} pmm_stack_t;

/**
 * @brief Buddy free list for a specific order
 */
typedef struct {
    phys_addr_t head;       // Head of free list (frame address)
    uint32_t count;         // Number of free blocks at this order
} pmm_buddy_list_t;

/**
 * @brief Memory zone descriptor
 */
typedef struct {
    zone_type_t type;
    const char* name;
    
    // Physical address range
    phys_addr_t start;
    phys_addr_t end;
    
    // Frame counts
    frame_count_t total_frames;
    frame_count_t free_frames;
    frame_count_t reserved_frames;
    
    // Fast stack for single page allocations
    pmm_stack_t stack;
    
    // Buddy lists for multi-page allocations
    pmm_buddy_list_t buddy[PMM_MAX_ORDER + 1];
    
    // Bitmap for tracking (1 bit per page)
    uint8_t* bitmap;
    uint32_t bitmap_size;
    
    // Watermarks
    frame_count_t watermark_min;
    frame_count_t watermark_low;
    frame_count_t watermark_high;
    
    // Statistics
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_failures;
    
    // Zone lock
    spinlock_t lock;
} pmm_zone_t;

/**
 * @brief PMM global state
 */
static struct {
    bool initialized;
    
    // Zones
    pmm_zone_t zones[PMM_MAX_ZONES];
    uint32_t zone_count;
    
    // Global statistics
    frame_count_t total_memory_frames;
    frame_count_t total_free_frames;
    frame_count_t total_reserved_frames;
    
    // Bitmap storage (placed after kernel)
    uint8_t* bitmap_base;
    uint32_t bitmap_total_size;
    
    // Global lock
    spinlock_t global_lock;
} pmm_state = { .initialized = false };

// ============================================================================
// BITMAP OPERATIONS
// ============================================================================

static inline void bitmap_set(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static inline void bitmap_clear(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool bitmap_test(uint8_t* bitmap, uint32_t index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

static inline void bitmap_set_range(uint8_t* bitmap, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        bitmap_set(bitmap, start + i);
    }
}

static inline void bitmap_clear_range(uint8_t* bitmap, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        bitmap_clear(bitmap, start + i);
    }
}

// ============================================================================
// ZONE OPERATIONS
// ============================================================================

/**
 * @brief Find zone containing a physical address
 */
static pmm_zone_t* find_zone_for_addr(phys_addr_t addr) {
    for (uint32_t i = 0; i < pmm_state.zone_count; i++) {
        pmm_zone_t* zone = &pmm_state.zones[i];
        if (addr >= zone->start && addr < zone->end) {
            return zone;
        }
    }
    return NULL;
}

/**
 * @brief Find zone by type
 */
static pmm_zone_t* find_zone_by_type(zone_type_t type) {
    for (uint32_t i = 0; i < pmm_state.zone_count; i++) {
        if (pmm_state.zones[i].type == type) {
            return &pmm_state.zones[i];
        }
    }
    return NULL;
}

/**
 * @brief Get zone fallback order
 */
static zone_type_t get_fallback_zone(zone_type_t current) {
    switch (current) {
        case ZONE_DMA:
            return ZONE_NORMAL;
        case ZONE_NORMAL:
            return ZONE_HIGHMEM;
        case ZONE_HIGHMEM:
        default:
            return ZONE_INVALID;
    }
}

/**
 * @brief Convert frame address to zone-local index
 */
static inline uint32_t addr_to_zone_index(pmm_zone_t* zone, phys_addr_t addr) {
    return (addr - zone->start) / MEMORY_PAGE_SIZE;
}

/**
 * @brief Convert zone-local index to frame address
 */
static inline phys_addr_t zone_index_to_addr(pmm_zone_t* zone, uint32_t index) {
    return zone->start + (index * MEMORY_PAGE_SIZE);
}

// ============================================================================
// STACK-BASED FAST ALLOCATION
// ============================================================================

/**
 * @brief Push frame onto zone's fast stack
 */
static bool stack_push(pmm_zone_t* zone, phys_addr_t frame) {
    if (zone->stack.top >= PMM_STACK_SIZE) {
        return false;
    }
    zone->stack.frames[zone->stack.top++] = frame;
    return true;
}

/**
 * @brief Pop frame from zone's fast stack
 */
static phys_addr_t stack_pop(pmm_zone_t* zone) {
    if (zone->stack.top == 0) {
        return 0;
    }
    return zone->stack.frames[--zone->stack.top];
}

/**
 * @brief Refill fast stack from bitmap
 */
static void stack_refill(pmm_zone_t* zone) {
    uint32_t target = PMM_STACK_SIZE / 2;  // Refill to half capacity
    
    for (uint32_t i = 0; i < zone->total_frames && zone->stack.top < target; i++) {
        if (!bitmap_test(zone->bitmap, i)) {
            // Found free frame
            phys_addr_t addr = zone_index_to_addr(zone, i);
            bitmap_set(zone->bitmap, i);
            stack_push(zone, addr);
        }
    }
}

// ============================================================================
// BUDDY ALLOCATION
// ============================================================================

/**
 * @brief Calculate buddy address
 */
static inline phys_addr_t calc_buddy_addr(phys_addr_t addr, uint32_t order) {
    return addr ^ ((1UL << order) * MEMORY_PAGE_SIZE);
}

/**
 * @brief Allocate from buddy system
 */
__attribute__((unused)) static phys_addr_t buddy_alloc(pmm_zone_t* zone, uint32_t order) {
    if (order > PMM_MAX_ORDER) {
        return 0;
    }
    
    // Find smallest available block
    for (uint32_t current_order = order; current_order <= PMM_MAX_ORDER; current_order++) {
        if (zone->buddy[current_order].count > 0) {
            // Found a block
            phys_addr_t block = zone->buddy[current_order].head;
            zone->buddy[current_order].head = 0;  // Simple: just take head
            zone->buddy[current_order].count--;
            
            // Split if necessary
            while (current_order > order) {
                current_order--;
                phys_addr_t buddy = calc_buddy_addr(block, current_order);
                
                // Add buddy to free list
                zone->buddy[current_order].head = buddy;
                zone->buddy[current_order].count++;
            }
            
            // Mark as allocated in bitmap
            uint32_t num_pages = 1 << order;
            uint32_t start_index = addr_to_zone_index(zone, block);
            bitmap_set_range(zone->bitmap, start_index, num_pages);
            
            return block;
        }
    }
    
    return 0;
}

/**
 * @brief Free to buddy system
 */
__attribute__((unused)) static void buddy_free(pmm_zone_t* zone, phys_addr_t addr, uint32_t order) {
    if (order > PMM_MAX_ORDER) {
        return;
    }
    
    uint32_t num_pages = 1 << order;
    uint32_t start_index = addr_to_zone_index(zone, addr);
    
    // Clear bitmap
    bitmap_clear_range(zone->bitmap, start_index, num_pages);
    
    // Try to coalesce with buddy
    while (order < PMM_MAX_ORDER) {
        phys_addr_t buddy_addr = calc_buddy_addr(addr, order);
        
        // Check if buddy is within zone
        if (buddy_addr < zone->start || buddy_addr >= zone->end) {
            break;
        }
        
        // Check if buddy is free
        uint32_t buddy_index = addr_to_zone_index(zone, buddy_addr);
        bool buddy_free = true;
        for (uint32_t i = 0; i < num_pages; i++) {
            if (bitmap_test(zone->bitmap, buddy_index + i)) {
                buddy_free = false;
                break;
            }
        }
        
        if (!buddy_free) {
            break;
        }
        
        // Coalesce
        if (buddy_addr < addr) {
            addr = buddy_addr;
        }
        order++;
        num_pages = 1 << order;
    }
    
    // Add to free list
    zone->buddy[order].head = addr;
    zone->buddy[order].count++;
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize the enhanced PMM
 */
memory_result_t pmm_enhanced_init(memory_region_t* regions, uint32_t region_count) {
    if (pmm_state.initialized) {
        return MEMORY_OK;
    }
    
    print("[PMM-E] Initializing Enhanced Physical Memory Manager...\n");
    
    // Initialize global lock
    spinlock_init(&pmm_state.global_lock, "pmm_global");
    
    // Calculate total memory and determine zones
    phys_addr_t max_addr = 0;
    frame_count_t total_frames = 0;
    
    for (uint32_t i = 0; i < region_count; i++) {
        if (regions[i].type == MEMORY_REGION_AVAILABLE) {
            phys_addr_t end = regions[i].base_address + regions[i].length;
            if (end > max_addr) {
                max_addr = end;
            }
            total_frames += regions[i].length / MEMORY_PAGE_SIZE;
        }
    }
    
    pmm_state.total_memory_frames = total_frames;
    
    print("[PMM-E] Total frames: ");
    print_dec((uint32_t)total_frames);
    print(", Max address: 0x");
    print_hex((uint32_t)max_addr);
    print("\n");
    
    // Set up zones based on memory layout
    pmm_state.zone_count = 0;
    
    // Zone 0: DMA (0-16MB)
    if (max_addr > PMM_DMA_START) {
        pmm_zone_t* zone = &pmm_state.zones[pmm_state.zone_count++];
        zone->type = ZONE_DMA;
        zone->name = "DMA";
        zone->start = PMM_DMA_START;
        zone->end = (max_addr < PMM_DMA_END) ? max_addr : PMM_DMA_END;
        zone->total_frames = (zone->end - zone->start) / MEMORY_PAGE_SIZE;
        zone->free_frames = 0;
        zone->reserved_frames = 0;
        spinlock_init(&zone->lock, "zone_dma");
        spinlock_init(&zone->stack.lock, "stack_dma");
        zone->stack.top = 0;
        
        // Set watermarks
        zone->watermark_min = zone->total_frames / 64;
        zone->watermark_low = zone->watermark_min * 2;
        zone->watermark_high = zone->watermark_min * 4;
        
        print("[PMM-E] Zone DMA: 0x");
        print_hex((uint32_t)zone->start);
        print(" - 0x");
        print_hex((uint32_t)zone->end);
        print("\n");
    }
    
    // Zone 1: NORMAL (16MB-896MB)
    if (max_addr > PMM_NORMAL_START) {
        pmm_zone_t* zone = &pmm_state.zones[pmm_state.zone_count++];
        zone->type = ZONE_NORMAL;
        zone->name = "NORMAL";
        zone->start = PMM_NORMAL_START;
        zone->end = (max_addr < PMM_NORMAL_END) ? max_addr : PMM_NORMAL_END;
        zone->total_frames = (zone->end - zone->start) / MEMORY_PAGE_SIZE;
        zone->free_frames = 0;
        zone->reserved_frames = 0;
        spinlock_init(&zone->lock, "zone_normal");
        spinlock_init(&zone->stack.lock, "stack_normal");
        zone->stack.top = 0;
        
        zone->watermark_min = zone->total_frames / 64;
        zone->watermark_low = zone->watermark_min * 2;
        zone->watermark_high = zone->watermark_min * 4;
        
        print("[PMM-E] Zone NORMAL: 0x");
        print_hex((uint32_t)zone->start);
        print(" - 0x");
        print_hex((uint32_t)zone->end);
        print("\n");
    }
    
    // Zone 2: HIGHMEM (896MB+)
    if (max_addr > PMM_HIGHMEM_START) {
        pmm_zone_t* zone = &pmm_state.zones[pmm_state.zone_count++];
        zone->type = ZONE_HIGHMEM;
        zone->name = "HIGHMEM";
        zone->start = PMM_HIGHMEM_START;
        zone->end = max_addr;
        zone->total_frames = (zone->end - zone->start) / MEMORY_PAGE_SIZE;
        zone->free_frames = 0;
        zone->reserved_frames = 0;
        spinlock_init(&zone->lock, "zone_highmem");
        spinlock_init(&zone->stack.lock, "stack_highmem");
        zone->stack.top = 0;
        
        zone->watermark_min = zone->total_frames / 64;
        zone->watermark_low = zone->watermark_min * 2;
        zone->watermark_high = zone->watermark_min * 4;
        
        print("[PMM-E] Zone HIGHMEM: 0x");
        print_hex((uint32_t)zone->start);
        print(" - 0x");
        print_hex((uint32_t)zone->end);
        print("\n");
    }
    
    // Allocate bitmaps for each zone
    uint32_t total_bitmap_size = 0;
    for (uint32_t i = 0; i < pmm_state.zone_count; i++) {
        pmm_zone_t* zone = &pmm_state.zones[i];
        zone->bitmap_size = (zone->total_frames + 7) / 8;
        total_bitmap_size += zone->bitmap_size;
    }
    
    // Place bitmap after kernel (using existing PMM start)
    pmm_state.bitmap_base = (uint8_t*)memory_get_pmm_start();
    pmm_state.bitmap_total_size = total_bitmap_size;
    
    // Assign bitmap pointers to zones
    uint8_t* bitmap_ptr = pmm_state.bitmap_base;
    for (uint32_t i = 0; i < pmm_state.zone_count; i++) {
        pmm_zone_t* zone = &pmm_state.zones[i];
        zone->bitmap = bitmap_ptr;
        memset(zone->bitmap, 0xFF, zone->bitmap_size);  // Mark all as used initially
        bitmap_ptr += zone->bitmap_size;
    }
    
    // Mark available regions as free
    for (uint32_t r = 0; r < region_count; r++) {
        if (regions[r].type != MEMORY_REGION_AVAILABLE) {
            continue;
        }
        
        phys_addr_t start = (regions[r].base_address + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
        phys_addr_t end = (regions[r].base_address + regions[r].length) & ~(MEMORY_PAGE_SIZE - 1);
        
        for (phys_addr_t addr = start; addr < end; addr += MEMORY_PAGE_SIZE) {
            pmm_zone_t* zone = find_zone_for_addr(addr);
            if (zone) {
                uint32_t index = addr_to_zone_index(zone, addr);
                bitmap_clear(zone->bitmap, index);
                zone->free_frames++;
                pmm_state.total_free_frames++;
            }
        }
    }
    
    // Reserve first 1MB (low memory, BIOS, etc.)
    pmm_zone_t* dma_zone = find_zone_by_type(ZONE_DMA);
    if (dma_zone) {
        for (phys_addr_t addr = 0; addr < 0x100000; addr += MEMORY_PAGE_SIZE) {
            uint32_t index = addr_to_zone_index(dma_zone, addr);
            if (!bitmap_test(dma_zone->bitmap, index)) {
                bitmap_set(dma_zone->bitmap, index);
                dma_zone->free_frames--;
                dma_zone->reserved_frames++;
                pmm_state.total_free_frames--;
                pmm_state.total_reserved_frames++;
            }
        }
    }
    
    // Pre-fill fast stacks
    for (uint32_t i = 0; i < pmm_state.zone_count; i++) {
        stack_refill(&pmm_state.zones[i]);
    }
    
    pmm_state.initialized = true;
    
    print("[PMM-E] Initialized. Free frames: ");
    print_dec((uint32_t)pmm_state.total_free_frames);
    print("\n");
    
    return MEMORY_OK;
}

/**
 * @brief Allocate a single page frame
 */
phys_addr_t pmm_enhanced_alloc_frame(void) {
    return pmm_enhanced_alloc_frame_zone(ZONE_NORMAL);
}

/**
 * @brief Allocate frame from specific zone
 */
phys_addr_t pmm_enhanced_alloc_frame_zone(zone_type_t preferred_zone) {
    if (!pmm_state.initialized) {
        return 0;
    }
    
    zone_type_t zone_type = preferred_zone;
    
    // Try zones with fallback
    while (zone_type != ZONE_INVALID) {
        pmm_zone_t* zone = find_zone_by_type(zone_type);
        if (zone && zone->free_frames > 0) {
            spinlock_acquire(&zone->lock);
            
            // Try fast stack first
            phys_addr_t frame = stack_pop(zone);
            if (frame != 0) {
                zone->free_frames--;
                zone->alloc_count++;
                pmm_state.total_free_frames--;
                spinlock_release(&zone->lock);
                return frame;
            }
            
            // Try bitmap scan
            for (uint32_t i = 0; i < zone->total_frames; i++) {
                if (!bitmap_test(zone->bitmap, i)) {
                    bitmap_set(zone->bitmap, i);
                    zone->free_frames--;
                    zone->alloc_count++;
                    pmm_state.total_free_frames--;
                    spinlock_release(&zone->lock);
                    return zone_index_to_addr(zone, i);
                }
            }
            
            spinlock_release(&zone->lock);
        }
        
        zone_type = get_fallback_zone(zone_type);
    }
    
    // Allocation failed
    pmm_zone_t* zone = find_zone_by_type(preferred_zone);
    if (zone) {
        zone->alloc_failures++;
    }
    
    return 0;
}

/**
 * @brief Free a page frame
 */
memory_result_t pmm_enhanced_free_frame(phys_addr_t frame_addr) {
    if (!pmm_state.initialized) {
        return MEMORY_ERROR_NOT_INITIALIZED;
    }
    
    if ((frame_addr & MEMORY_PAGE_MASK) != 0) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    pmm_zone_t* zone = find_zone_for_addr(frame_addr);
    if (!zone) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    uint32_t index = addr_to_zone_index(zone, frame_addr);
    if (index >= zone->total_frames) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    spinlock_acquire(&zone->lock);
    
    if (!bitmap_test(zone->bitmap, index)) {
        // Already free - double free
        spinlock_release(&zone->lock);
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    // Try to add to fast stack
    if (!stack_push(zone, frame_addr)) {
        // Stack full, just clear bitmap
        bitmap_clear(zone->bitmap, index);
    }
    
    zone->free_frames++;
    zone->free_count++;
    pmm_state.total_free_frames++;
    
    spinlock_release(&zone->lock);
    
    return MEMORY_OK;
}

/**
 * @brief Allocate contiguous frames
 */
phys_addr_t pmm_enhanced_alloc_contiguous(uint32_t count, zone_type_t zone_type) {
    if (!pmm_state.initialized || count == 0) {
        return 0;
    }
    
    pmm_zone_t* zone = find_zone_by_type(zone_type);
    if (!zone || zone->free_frames < count) {
        return 0;
    }
    
    spinlock_acquire(&zone->lock);
    
    // Search for contiguous free block
    uint32_t consecutive = 0;
    uint32_t start_index = 0;
    
    for (uint32_t i = 0; i < zone->total_frames; i++) {
        if (!bitmap_test(zone->bitmap, i)) {
            if (consecutive == 0) {
                start_index = i;
            }
            consecutive++;
            
            if (consecutive >= count) {
                // Found enough consecutive frames
                bitmap_set_range(zone->bitmap, start_index, count);
                zone->free_frames -= count;
                zone->alloc_count++;
                pmm_state.total_free_frames -= count;
                
                spinlock_release(&zone->lock);
                return zone_index_to_addr(zone, start_index);
            }
        } else {
            consecutive = 0;
        }
    }
    
    spinlock_release(&zone->lock);
    zone->alloc_failures++;
    
    return 0;
}

/**
 * @brief Free contiguous frames
 */
memory_result_t pmm_enhanced_free_contiguous(phys_addr_t addr, uint32_t count) {
    if (!pmm_state.initialized) {
        return MEMORY_ERROR_NOT_INITIALIZED;
    }
    
    pmm_zone_t* zone = find_zone_for_addr(addr);
    if (!zone) {
        return MEMORY_ERROR_INVALID_ADDR;
    }
    
    uint32_t start_index = addr_to_zone_index(zone, addr);
    if (start_index + count > zone->total_frames) {
        return MEMORY_ERROR_INVALID_SIZE;
    }
    
    spinlock_acquire(&zone->lock);
    
    bitmap_clear_range(zone->bitmap, start_index, count);
    zone->free_frames += count;
    zone->free_count++;
    pmm_state.total_free_frames += count;
    
    spinlock_release(&zone->lock);
    
    return MEMORY_OK;
}

/**
 * @brief Reserve a physical memory range
 */
void pmm_enhanced_reserve_range(phys_addr_t start, phys_addr_t end) {
    if (!pmm_state.initialized) {
        return;
    }
    
    start = start & ~MEMORY_PAGE_MASK;
    end = (end + MEMORY_PAGE_SIZE - 1) & ~MEMORY_PAGE_MASK;
    
    for (phys_addr_t addr = start; addr < end; addr += MEMORY_PAGE_SIZE) {
        pmm_zone_t* zone = find_zone_for_addr(addr);
        if (zone) {
            uint32_t index = addr_to_zone_index(zone, addr);
            spinlock_acquire(&zone->lock);
            
            if (!bitmap_test(zone->bitmap, index)) {
                bitmap_set(zone->bitmap, index);
                zone->free_frames--;
                zone->reserved_frames++;
                pmm_state.total_free_frames--;
                pmm_state.total_reserved_frames++;
            }
            
            spinlock_release(&zone->lock);
        }
    }
}

/**
 * @brief Check if frame is free
 */
bool pmm_enhanced_is_frame_free(phys_addr_t addr) {
    pmm_zone_t* zone = find_zone_for_addr(addr);
    if (!zone) {
        return false;
    }
    
    uint32_t index = addr_to_zone_index(zone, addr);
    return !bitmap_test(zone->bitmap, index);
}

/**
 * @brief Get total frames
 */
frame_count_t pmm_enhanced_get_total_frames(void) {
    return pmm_state.total_memory_frames;
}

/**
 * @brief Get free frames
 */
frame_count_t pmm_enhanced_get_free_frames(void) {
    return pmm_state.total_free_frames;
}

/**
 * @brief Get detailed statistics
 */
pmm_stats_t pmm_enhanced_get_stats(void) {
    pmm_stats_t stats = {0};
    
    stats.total_frames = pmm_state.total_memory_frames;
    stats.free_frames = pmm_state.total_free_frames;
    stats.used_frames = stats.total_frames - stats.free_frames;
    stats.reserved_frames = pmm_state.total_reserved_frames;
    stats.zone_count = pmm_state.zone_count;
    
    // Calculate fragmentation (ratio of largest free block to total free)
    frame_count_t largest_free = 0;
    for (uint32_t z = 0; z < pmm_state.zone_count; z++) {
        pmm_zone_t* zone = &pmm_state.zones[z];
        frame_count_t consecutive = 0;
        
        for (uint32_t i = 0; i < zone->total_frames; i++) {
            if (!bitmap_test(zone->bitmap, i)) {
                consecutive++;
                if (consecutive > largest_free) {
                    largest_free = consecutive;
                }
            } else {
                consecutive = 0;
            }
        }
    }
    
    if (stats.free_frames > 0) {
        stats.fragmentation_percent = 100 - (largest_free * 100 / stats.free_frames);
    }
    
    return stats;
}

/**
 * @brief Get zone statistics
 */
pmm_zone_stats_t pmm_enhanced_get_zone_stats(zone_type_t type) {
    pmm_zone_stats_t stats = {0};
    
    pmm_zone_t* zone = find_zone_by_type(type);
    if (zone) {
        stats.type = zone->type;
        stats.total_frames = zone->total_frames;
        stats.free_frames = zone->free_frames;
        stats.reserved_frames = zone->reserved_frames;
        stats.alloc_count = zone->alloc_count;
        stats.free_count = zone->free_count;
        stats.alloc_failures = zone->alloc_failures;
        stats.stack_depth = zone->stack.top;
    }
    
    return stats;
}

/**
 * @brief Dump PMM information
 */
void pmm_enhanced_dump_info(void) {
    print("\n=== Enhanced PMM Status ===\n");
    
    pmm_stats_t stats = pmm_enhanced_get_stats();
    print("Total Frames: ");
    print_dec((uint32_t)stats.total_frames);
    print(" (");
    print_dec((uint32_t)(stats.total_frames * 4));
    print(" KB)\n");
    
    print("Free Frames: ");
    print_dec((uint32_t)stats.free_frames);
    print("\n");
    
    print("Used Frames: ");
    print_dec((uint32_t)stats.used_frames);
    print("\n");
    
    print("Reserved Frames: ");
    print_dec((uint32_t)stats.reserved_frames);
    print("\n");
    
    print("Fragmentation: ");
    print_dec(stats.fragmentation_percent);
    print("%\n");
    
    print("\nZone Details:\n");
    for (uint32_t i = 0; i < pmm_state.zone_count; i++) {
        pmm_zone_t* zone = &pmm_state.zones[i];
        print("  ");
        print(zone->name);
        print(": ");
        print_dec((uint32_t)zone->free_frames);
        print("/");
        print_dec((uint32_t)zone->total_frames);
        print(" free (stack: ");
        print_dec(zone->stack.top);
        print(")\n");
    }
    
    print("===========================\n\n");
}
