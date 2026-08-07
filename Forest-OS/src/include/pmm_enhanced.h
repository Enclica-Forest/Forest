/**
 * @file pmm_enhanced.h
 * @brief Enhanced Physical Memory Manager Interface
 * 
 * Multi-zone PMM with:
 * - DMA, NORMAL, HIGHMEM zones
 * - Fast stack-based allocation
 * - Detailed statistics
 * - Fragmentation analysis
 */

#ifndef PMM_ENHANCED_H
#define PMM_ENHANCED_H

#include "memory.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Memory zone types
 */
#ifndef ZONE_TYPE_T_DEFINED
typedef enum {
    ZONE_DMA = 0,       /**< DMA-capable memory (0-16MB) */
    ZONE_NORMAL = 1,    /**< Normal memory (16MB-896MB) */
    ZONE_HIGHMEM = 2,   /**< High memory (896MB+) */
    ZONE_MOVABLE = 3,   /**< Movable memory for migration */
    ZONE_INVALID = 255  /**< Invalid zone marker */
} zone_type_t;
#define ZONE_TYPE_T_DEFINED 1
#endif

/**
 * @brief PMM statistics (use pmm_stats_t from bitmap_pmm.h if already defined)
 */
#ifndef PMM_STATS_T_DEFINED
typedef struct pmm_stats {
    uint32_t total_frames;
    uint32_t free_frames;
    uint32_t used_frames;
    uint32_t reserved_frames;
    uint32_t zone_count;
    uint32_t fragmentation_percent;
} pmm_stats_t;
#define PMM_STATS_T_DEFINED 1
#endif

/**
 * @brief Zone-specific statistics
 */
typedef struct {
    zone_type_t type;
    frame_count_t total_frames;
    frame_count_t free_frames;
    frame_count_t reserved_frames;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_failures;
    uint32_t stack_depth;
} pmm_zone_stats_t;

/**
 * @brief Initialize the enhanced PMM
 * @param regions Memory regions from bootloader
 * @param region_count Number of regions
 * @return MEMORY_OK on success
 */
memory_result_t pmm_enhanced_init(memory_region_t* regions, uint32_t region_count);

/**
 * @brief Allocate a single page frame (from NORMAL zone)
 * @return Physical address of frame, or 0 on failure
 */
phys_addr_t pmm_enhanced_alloc_frame(void);

/**
 * @brief Allocate frame from specific zone
 * @param preferred_zone Zone to allocate from
 * @return Physical address of frame, or 0 on failure
 */
phys_addr_t pmm_enhanced_alloc_frame_zone(zone_type_t preferred_zone);

/**
 * @brief Free a page frame
 * @param frame_addr Physical address of frame
 * @return MEMORY_OK on success
 */
memory_result_t pmm_enhanced_free_frame(phys_addr_t frame_addr);

/**
 * @brief Allocate contiguous frames
 * @param count Number of contiguous frames needed
 * @param zone_type Zone to allocate from
 * @return Physical address of first frame, or 0 on failure
 */
phys_addr_t pmm_enhanced_alloc_contiguous(uint32_t count, zone_type_t zone_type);

/**
 * @brief Free contiguous frames
 * @param addr Physical address of first frame
 * @param count Number of frames
 * @return MEMORY_OK on success
 */
memory_result_t pmm_enhanced_free_contiguous(phys_addr_t addr, uint32_t count);

/**
 * @brief Reserve a physical memory range
 * @param start Start address
 * @param end End address
 */
void pmm_enhanced_reserve_range(phys_addr_t start, phys_addr_t end);

/**
 * @brief Check if frame is free
 * @param addr Physical address
 * @return true if free
 */
bool pmm_enhanced_is_frame_free(phys_addr_t addr);

/**
 * @brief Get total number of frames
 */
frame_count_t pmm_enhanced_get_total_frames(void);

/**
 * @brief Get number of free frames
 */
frame_count_t pmm_enhanced_get_free_frames(void);

/**
 * @brief Get detailed PMM statistics
 */
pmm_stats_t pmm_enhanced_get_stats(void);

/**
 * @brief Get zone-specific statistics
 */
pmm_zone_stats_t pmm_enhanced_get_zone_stats(zone_type_t type);

/**
 * @brief Dump PMM information to console
 */
void pmm_enhanced_dump_info(void);

#endif /* PMM_ENHANCED_H */
