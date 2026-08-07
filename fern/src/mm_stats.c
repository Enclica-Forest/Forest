/**
 * @file mm_stats.c
 * @brief Comprehensive Memory Statistics and Debugging
 * 
 * Provides:
 * - /proc/meminfo-style reporting
 * - Memory leak detection
 * - Fragmentation analysis
 * - Per-subsystem statistics
 * - Memory usage timeline
 * - Allocation tracking
 */

#include "include/mm_stats.h"
#include "include/memory.h"
#include "include/pmm_enhanced.h"
#include "include/kheap_enhanced.h"
#include "include/mm_swap.h"
#include "include/mm_cow.h"
#include "include/tlb.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/debuglog.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define STATS_HISTORY_SIZE      60      // Keep 60 samples
#define LEAK_TRACKER_SIZE       1024    // Track 1024 allocations for leak detection
#define FRAGMENTATION_SAMPLES   100     // Samples for fragmentation analysis

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Memory snapshot for history
 */
typedef struct {
    uint64_t timestamp;
    uint32_t total_kb;
    uint32_t free_kb;
    uint32_t used_kb;
    uint32_t cached_kb;
    uint32_t swap_used_kb;
} mem_snapshot_t;

/**
 * @brief Allocation tracker entry (for leak detection)
 */
typedef struct {
    void* ptr;
    uint32_t size;
    const char* file;
    int line;
    uint64_t timestamp;
    bool active;
} alloc_entry_t;

/**
 * @brief Memory statistics state
 */
static struct {
    bool initialized;
    
    // History
    mem_snapshot_t history[STATS_HISTORY_SIZE];
    uint32_t history_index;
    uint32_t history_count;
    
    // Leak tracking
    alloc_entry_t leak_tracker[LEAK_TRACKER_SIZE];
    uint32_t leak_tracker_index;
    bool leak_tracking_enabled;
    
    // Counters
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t alloc_bytes;
    uint64_t free_bytes;
    uint64_t peak_usage;
    
    // Lock
    spinlock_t lock;
} stats_state = { .initialized = false };

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Get current timestamp (simplified)
 */
static uint64_t get_timestamp(void) {
    static uint64_t counter = 0;
    return counter++;
}

/**
 * @brief Calculate fragmentation percentage
 */
static uint32_t calculate_fragmentation(void) {
    pmm_stats_t pmm_stats = pmm_enhanced_get_stats();
    return pmm_stats.fragmentation_percent;
}

// ============================================================================
// PUBLIC API - INITIALIZATION
// ============================================================================

/**
 * @brief Initialize memory statistics
 */
void mm_stats_init(void) {
    if (stats_state.initialized) {
        return;
    }
    
    print("[MM-STATS] Initializing memory statistics...\n");
    
    // Initialize history
    memset(stats_state.history, 0, sizeof(stats_state.history));
    stats_state.history_index = 0;
    stats_state.history_count = 0;
    
    // Initialize leak tracker
    memset(stats_state.leak_tracker, 0, sizeof(stats_state.leak_tracker));
    stats_state.leak_tracker_index = 0;
    stats_state.leak_tracking_enabled = false;
    
    // Initialize counters
    stats_state.total_allocs = 0;
    stats_state.total_frees = 0;
    stats_state.alloc_bytes = 0;
    stats_state.free_bytes = 0;
    stats_state.peak_usage = 0;
    
    // Initialize lock
    spinlock_init(&stats_state.lock, "mm_stats");
    
    stats_state.initialized = true;
    
    print("[MM-STATS] Memory statistics initialized\n");
}

// ============================================================================
// PUBLIC API - STATISTICS COLLECTION
// ============================================================================

/**
 * @brief Take a memory snapshot
 */
void mm_stats_take_snapshot(void) {
    if (!stats_state.initialized) {
        return;
    }
    
    spinlock_acquire(&stats_state.lock);
    
    mem_snapshot_t* snapshot = &stats_state.history[stats_state.history_index];
    
    snapshot->timestamp = get_timestamp();
    
    // Get PMM stats
    pmm_stats_t pmm_stats = pmm_enhanced_get_stats();
    snapshot->total_kb = (pmm_stats.total_frames * MEMORY_PAGE_SIZE) / 1024;
    snapshot->free_kb = (pmm_stats.free_frames * MEMORY_PAGE_SIZE) / 1024;
    snapshot->used_kb = snapshot->total_kb - snapshot->free_kb;
    
    // Get swap stats
    swap_stats_t swap_stats = swap_get_stats();
    snapshot->swap_used_kb = (swap_stats.used_pages * MEMORY_PAGE_SIZE) / 1024;
    
    // TODO: Add cached memory tracking
    snapshot->cached_kb = 0;
    
    // Update history index
    stats_state.history_index = (stats_state.history_index + 1) % STATS_HISTORY_SIZE;
    if (stats_state.history_count < STATS_HISTORY_SIZE) {
        stats_state.history_count++;
    }
    
    // Track peak usage
    if (snapshot->used_kb > stats_state.peak_usage) {
        stats_state.peak_usage = snapshot->used_kb;
    }
    
    spinlock_release(&stats_state.lock);
}

/**
 * @brief Record allocation (for leak tracking)
 */
void mm_stats_record_alloc(void* ptr, uint32_t size, const char* file, int line) {
    if (!stats_state.initialized || !stats_state.leak_tracking_enabled) {
        return;
    }
    
    spinlock_acquire(&stats_state.lock);
    
    alloc_entry_t* entry = &stats_state.leak_tracker[stats_state.leak_tracker_index];
    entry->ptr = ptr;
    entry->size = size;
    entry->file = file;
    entry->line = line;
    entry->timestamp = get_timestamp();
    entry->active = true;
    
    stats_state.leak_tracker_index = 
        (stats_state.leak_tracker_index + 1) % LEAK_TRACKER_SIZE;
    
    stats_state.total_allocs++;
    stats_state.alloc_bytes += size;
    
    spinlock_release(&stats_state.lock);
}

/**
 * @brief Record free (for leak tracking)
 */
void mm_stats_record_free(void* ptr) {
    if (!stats_state.initialized || !stats_state.leak_tracking_enabled) {
        return;
    }
    
    spinlock_acquire(&stats_state.lock);
    
    // Find the allocation
    for (uint32_t i = 0; i < LEAK_TRACKER_SIZE; i++) {
        alloc_entry_t* entry = &stats_state.leak_tracker[i];
        if (entry->active && entry->ptr == ptr) {
            stats_state.free_bytes += entry->size;
            entry->active = false;
            break;
        }
    }
    
    stats_state.total_frees++;
    
    spinlock_release(&stats_state.lock);
}

/**
 * @brief Enable/disable leak tracking
 */
void mm_stats_set_leak_tracking(bool enable) {
    stats_state.leak_tracking_enabled = enable;
    
    if (enable) {
        // Clear tracker
        spinlock_acquire(&stats_state.lock);
        memset(stats_state.leak_tracker, 0, sizeof(stats_state.leak_tracker));
        stats_state.leak_tracker_index = 0;
        spinlock_release(&stats_state.lock);
    }
}

// ============================================================================
// PUBLIC API - REPORTING
// ============================================================================

/**
 * @brief Get comprehensive memory info (like /proc/meminfo)
 */
meminfo_t mm_stats_get_meminfo(void) {
    meminfo_t info = {0};
    
    // Get PMM stats
    pmm_stats_t pmm = pmm_enhanced_get_stats();
    info.mem_total_kb = (pmm.total_frames * MEMORY_PAGE_SIZE) / 1024;
    info.mem_free_kb = (pmm.free_frames * MEMORY_PAGE_SIZE) / 1024;
    info.mem_available_kb = info.mem_free_kb;  // Simplified
    
    // Get heap stats
    kheap_stats_t heap = kheap_get_stats();
    info.heap_total_kb = heap.total_size / 1024;
    info.heap_used_kb = heap.used_size / 1024;
    
    // Get swap stats
    swap_stats_t swap = swap_get_stats();
    info.swap_total_kb = (swap.total_pages * MEMORY_PAGE_SIZE) / 1024;
    info.swap_free_kb = (swap.free_pages * MEMORY_PAGE_SIZE) / 1024;
    
    // Calculated values
    info.buffers_kb = 0;  // TODO
    info.cached_kb = 0;   // TODO
    info.active_kb = 0;   // TODO
    info.inactive_kb = 0; // TODO
    
    // COW savings
    cow_stats_t cow = cow_get_stats();
    info.shared_kb = cow.memory_saved_kb;
    
    return info;
}

/**
 * @brief Get zone-specific statistics
 */
zone_info_t mm_stats_get_zone_info(zone_type_t type) {
    zone_info_t info = {0};
    
    pmm_zone_stats_t stats = pmm_enhanced_get_zone_stats(type);
    
    info.type = stats.type;
    info.total_pages = stats.total_frames;
    info.free_pages = stats.free_frames;
    info.used_pages = info.total_pages - info.free_pages;
    info.alloc_count = stats.alloc_count;
    info.free_count = stats.free_count;
    info.failures = stats.alloc_failures;
    
    return info;
}

/**
 * @brief Get fragmentation analysis
 */
frag_info_t mm_stats_get_fragmentation(void) {
    frag_info_t info = {0};
    
    pmm_stats_t pmm = pmm_enhanced_get_stats();
    
    info.fragmentation_percent = pmm.fragmentation_percent;
    info.largest_free_block = 0;  // TODO: Track this
    info.free_block_count = 0;    // TODO: Track this
    info.average_free_size = 0;   // TODO: Track this
    
    return info;
}

/**
 * @brief Detect potential memory leaks
 */
uint32_t mm_stats_detect_leaks(leak_report_t* reports, uint32_t max_reports) {
    if (!stats_state.leak_tracking_enabled || !reports || max_reports == 0) {
        return 0;
    }
    
    uint32_t count = 0;
    
    spinlock_acquire(&stats_state.lock);
    
    for (uint32_t i = 0; i < LEAK_TRACKER_SIZE && count < max_reports; i++) {
        alloc_entry_t* entry = &stats_state.leak_tracker[i];
        if (entry->active) {
            reports[count].ptr = entry->ptr;
            reports[count].size = entry->size;
            reports[count].file = entry->file;
            reports[count].line = entry->line;
            reports[count].age = get_timestamp() - entry->timestamp;
            count++;
        }
    }
    
    spinlock_release(&stats_state.lock);
    
    return count;
}

// ============================================================================
// PUBLIC API - DISPLAY
// ============================================================================

/**
 * @brief Print /proc/meminfo-style output
 */
void mm_stats_print_meminfo(void) {
    meminfo_t info = mm_stats_get_meminfo();
    
    print("\n");
    print("MemTotal:       "); print_dec(info.mem_total_kb); print(" kB\n");
    print("MemFree:        "); print_dec(info.mem_free_kb); print(" kB\n");
    print("MemAvailable:   "); print_dec(info.mem_available_kb); print(" kB\n");
    print("Buffers:        "); print_dec(info.buffers_kb); print(" kB\n");
    print("Cached:         "); print_dec(info.cached_kb); print(" kB\n");
    print("SwapCached:     0 kB\n");
    print("Active:         "); print_dec(info.active_kb); print(" kB\n");
    print("Inactive:       "); print_dec(info.inactive_kb); print(" kB\n");
    print("SwapTotal:      "); print_dec(info.swap_total_kb); print(" kB\n");
    print("SwapFree:       "); print_dec(info.swap_free_kb); print(" kB\n");
    print("Shared:         "); print_dec(info.shared_kb); print(" kB\n");
    print("KernelHeap:     "); print_dec(info.heap_used_kb); print(" kB\n");
    print("\n");
}

/**
 * @brief Print all memory statistics
 */
void mm_stats_print_all(void) {
    print("\n");
    print("=============================================================\n");
    print("           FOREST OS MEMORY SYSTEM STATUS REPORT\n");
    print("=============================================================\n\n");
    
    // Memory Info
    print("--- Memory Info (like /proc/meminfo) ---\n");
    mm_stats_print_meminfo();
    
    // Zone Info
    print("--- Zone Information ---\n");
    const char* zone_names[] = {"DMA", "NORMAL", "HIGHMEM"};
    zone_type_t zone_types[] = {ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM};
    
    for (int i = 0; i < 3; i++) {
        zone_info_t zone = mm_stats_get_zone_info(zone_types[i]);
        if (zone.total_pages > 0) {
            print("  ");
            print(zone_names[i]);
            print(": ");
            print_dec((uint32_t)zone.free_pages);
            print("/");
            print_dec((uint32_t)zone.total_pages);
            print(" pages free (");
            print_dec((uint32_t)(zone.free_pages * 100 / zone.total_pages));
            print("%)\n");
        }
    }
    print("\n");
    
    // Fragmentation
    print("--- Fragmentation Analysis ---\n");
    frag_info_t frag = mm_stats_get_fragmentation();
    print("  Fragmentation: ");
    print_dec(frag.fragmentation_percent);
    print("%\n\n");
    
    // Allocation Stats
    print("--- Allocation Statistics ---\n");
    print("  Total Allocations: ");
    print_dec((uint32_t)stats_state.total_allocs);
    print("\n");
    print("  Total Frees: ");
    print_dec((uint32_t)stats_state.total_frees);
    print("\n");
    print("  Bytes Allocated: ");
    print_dec((uint32_t)(stats_state.alloc_bytes / 1024));
    print(" KB\n");
    print("  Bytes Freed: ");
    print_dec((uint32_t)(stats_state.free_bytes / 1024));
    print(" KB\n");
    print("  Peak Usage: ");
    print_dec((uint32_t)stats_state.peak_usage);
    print(" KB\n\n");
    
    // TLB Stats
    print("--- TLB Statistics ---\n");
    tlb_stats_t tlb = tlb_get_stats();
    print("  INVLPG calls: ");
    print_dec((uint32_t)tlb.invlpg_count);
    print("\n");
    print("  Full flushes: ");
    print_dec((uint32_t)tlb.flush_count);
    print("\n\n");
    
    // Swap Stats
    print("--- Swap Statistics ---\n");
    swap_stats_t swap = swap_get_stats();
    print("  Total: ");
    print_dec((uint32_t)(swap.total_pages * 4));
    print(" KB\n");
    print("  Used: ");
    print_dec((uint32_t)((swap.total_pages - swap.free_pages) * 4));
    print(" KB\n");
    print("  Pages In: ");
    print_dec((uint32_t)swap.pages_in);
    print("\n");
    print("  Pages Out: ");
    print_dec((uint32_t)swap.pages_out);
    print("\n\n");
    
    // COW Stats
    print("--- Copy-on-Write Statistics ---\n");
    cow_stats_t cow = cow_get_stats();
    print("  Total Shared: ");
    print_dec((uint32_t)cow.total_shared);
    print("\n");
    print("  Total Copied: ");
    print_dec((uint32_t)cow.total_copied);
    print("\n");
    print("  Memory Saved: ");
    print_dec(cow.memory_saved_kb);
    print(" KB\n\n");
    
    // Leak Detection
    if (stats_state.leak_tracking_enabled) {
        print("--- Potential Memory Leaks ---\n");
        leak_report_t leaks[10];
        uint32_t leak_count = mm_stats_detect_leaks(leaks, 10);
        if (leak_count == 0) {
            print("  No leaks detected\n");
        } else {
            for (uint32_t i = 0; i < leak_count; i++) {
                print("  0x");
                print_hex((uint32_t)leaks[i].ptr);
                print(" (");
                print_dec(leaks[i].size);
                print(" bytes) - ");
                if (leaks[i].file) {
                    print(leaks[i].file);
                    print(":");
                    print_dec(leaks[i].line);
                }
                print("\n");
            }
        }
        print("\n");
    }
    
    print("=============================================================\n\n");
}

/**
 * @brief Print memory usage bar graph
 */
void mm_stats_print_usage_bar(void) {
    meminfo_t info = mm_stats_get_meminfo();
    
    if (info.mem_total_kb == 0) {
        return;
    }
    
    uint32_t used_percent = ((info.mem_total_kb - info.mem_free_kb) * 100) / 
                            info.mem_total_kb;
    uint32_t bar_width = 50;
    uint32_t filled = (used_percent * bar_width) / 100;
    
    print("Memory: [");
    for (uint32_t i = 0; i < bar_width; i++) {
        if (i < filled) {
            print("#");
        } else {
            print("-");
        }
    }
    print("] ");
    print_dec(used_percent);
    print("% used\n");
}
