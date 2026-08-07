/**
 * @file kheap_enhanced.h
 * @brief Enhanced Kernel Heap Interface
 * 
 * Features per-CPU caches, size classes, and detailed statistics.
 */

#ifndef KHEAP_ENHANCED_H
#define KHEAP_ENHANCED_H

#include "memory.h"
#include <stdint.h>

/**
 * @brief Heap statistics
 */
typedef struct {
    uint32_t total_size;
    uint32_t used_size;
    uint32_t free_size;
    uint32_t peak_used;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t realloc_count;
} kheap_stats_t;

/**
 * @brief Initialize enhanced kernel heap
 * @param start Start address of heap
 * @param initial_size Initial heap size
 * @return MEMORY_OK on success
 */
memory_result_t kheap_enhanced_init(uint32_t start, uint32_t initial_size);

/**
 * @brief Allocate memory
 * @param size Size in bytes
 * @return Pointer to allocated memory, or NULL
 */
void* kheap_alloc(uint32_t size);

/**
 * @brief Allocate zeroed memory
 * @param size Size in bytes
 * @return Pointer to zeroed memory, or NULL
 */
void* kheap_zalloc(uint32_t size);

/**
 * @brief Free memory
 * @param ptr Pointer to free
 */
void kheap_free(void* ptr);

/**
 * @brief Reallocate memory
 * @param ptr Pointer to existing allocation
 * @param new_size New size in bytes
 * @return Pointer to new allocation, or NULL
 */
void* kheap_realloc(void* ptr, uint32_t new_size);

/**
 * @brief Get heap statistics
 * @return Heap statistics structure
 */
kheap_stats_t kheap_get_stats(void);

/**
 * @brief Dump heap statistics to console
 */
void kheap_dump_stats(void);

#endif /* KHEAP_ENHANCED_H */
