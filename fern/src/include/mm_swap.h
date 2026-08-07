/**
 * @file mm_swap.h
 * @brief Virtual Memory Swap Interface
 */

#ifndef MM_SWAP_H
#define MM_SWAP_H

#include "memory.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Swap entry (encodes device + offset)
 */
typedef uint32_t swap_entry_t;

#define SWAP_ENTRY_INVALID  0xFFFFFFFF

/**
 * @brief Swap statistics
 */
typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t pages_in;
    uint64_t pages_out;
    uint32_t device_count;
} swap_stats_t;

/**
 * @brief Initialize swap subsystem
 */
memory_result_t swap_init(void);

/**
 * @brief Add swap device
 */
memory_result_t swap_add_device(const char* path, uint32_t size_kb, int priority);

/**
 * @brief Swap out a page
 */
swap_entry_t swap_out_page(page_directory_t* dir, uint32_t vaddr);

/**
 * @brief Swap in a page
 */
memory_result_t swap_in_page(page_directory_t* dir, uint32_t vaddr, swap_entry_t entry);

/**
 * @brief Try to reclaim memory
 */
uint32_t swap_reclaim_memory(uint32_t pages_needed);

/**
 * @brief Check if swap is available
 */
bool swap_is_available(void);

/**
 * @brief Get swap statistics
 */
swap_stats_t swap_get_stats(void);

/**
 * @brief Dump swap statistics
 */
void swap_dump_stats(void);

#endif /* MM_SWAP_H */
