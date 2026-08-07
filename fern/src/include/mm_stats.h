/**
 * @file mm_stats.h
 * @brief Memory Statistics and Debugging Interface
 */

#ifndef MM_STATS_H
#define MM_STATS_H

#include "memory.h"
#include "pmm_enhanced.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Memory info structure (like /proc/meminfo)
 */
typedef struct {
    uint32_t mem_total_kb;
    uint32_t mem_free_kb;
    uint32_t mem_available_kb;
    uint32_t buffers_kb;
    uint32_t cached_kb;
    uint32_t swap_total_kb;
    uint32_t swap_free_kb;
    uint32_t active_kb;
    uint32_t inactive_kb;
    uint32_t shared_kb;
    uint32_t heap_total_kb;
    uint32_t heap_used_kb;
} meminfo_t;

/**
 * @brief Zone information
 */
typedef struct {
    zone_type_t type;
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t failures;
} zone_info_t;

/**
 * @brief Fragmentation information
 */
typedef struct {
    uint32_t fragmentation_percent;
    uint32_t largest_free_block;
    uint32_t free_block_count;
    uint32_t average_free_size;
} frag_info_t;

/**
 * @brief Leak report entry
 */
typedef struct {
    void* ptr;
    uint32_t size;
    const char* file;
    int line;
    uint64_t age;
} leak_report_t;

/**
 * @brief Initialize memory statistics
 */
void mm_stats_init(void);

/**
 * @brief Take a memory snapshot
 */
void mm_stats_take_snapshot(void);

/**
 * @brief Record allocation (for leak tracking)
 */
void mm_stats_record_alloc(void* ptr, uint32_t size, const char* file, int line);

/**
 * @brief Record free (for leak tracking)
 */
void mm_stats_record_free(void* ptr);

/**
 * @brief Enable/disable leak tracking
 */
void mm_stats_set_leak_tracking(bool enable);

/**
 * @brief Get comprehensive memory info
 */
meminfo_t mm_stats_get_meminfo(void);

/**
 * @brief Get zone-specific statistics
 */
zone_info_t mm_stats_get_zone_info(zone_type_t type);

/**
 * @brief Get fragmentation analysis
 */
frag_info_t mm_stats_get_fragmentation(void);

/**
 * @brief Detect potential memory leaks
 */
uint32_t mm_stats_detect_leaks(leak_report_t* reports, uint32_t max_reports);

/**
 * @brief Print /proc/meminfo-style output
 */
void mm_stats_print_meminfo(void);

/**
 * @brief Print all memory statistics
 */
void mm_stats_print_all(void);

/**
 * @brief Print memory usage bar graph
 */
void mm_stats_print_usage_bar(void);

#endif /* MM_STATS_H */
