/**
 * @file mm_cow.h
 * @brief Copy-on-Write Interface
 */

#ifndef MM_COW_H
#define MM_COW_H

#include "memory.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief COW fault result
 */
typedef enum {
    COW_NOT_SHARED,     /**< Page was not COW shared */
    COW_COPIED,         /**< Page was copied */
    COW_OUT_OF_MEMORY   /**< Failed to allocate new page */
} cow_result_t;

/**
 * @brief COW statistics
 */
typedef struct {
    uint64_t total_shared;
    uint64_t total_copied;
    uint64_t pages_saved;
    uint32_t memory_saved_kb;
} cow_stats_t;

/**
 * @brief Initialize COW subsystem
 */
memory_result_t cow_init(void);

/**
 * @brief Mark a page as COW shared
 */
memory_result_t cow_mark_shared(phys_addr_t phys_addr, uint32_t flags);

/**
 * @brief Handle COW page fault
 */
cow_result_t cow_handle_fault(page_directory_t* dir, uint32_t vaddr,
                               phys_addr_t old_phys, phys_addr_t* new_phys);

/**
 * @brief Release COW reference
 */
memory_result_t cow_release(phys_addr_t phys_addr);

/**
 * @brief Get COW reference count
 */
uint32_t cow_get_refcount(phys_addr_t phys_addr);

/**
 * @brief Check if page is COW shared
 */
bool cow_is_shared(phys_addr_t phys_addr);

/**
 * @brief Fork address space with COW
 */
page_directory_t* cow_fork_address_space(page_directory_t* parent);

/**
 * @brief Get COW statistics
 */
cow_stats_t cow_get_stats(void);

/**
 * @brief Dump COW statistics
 */
void cow_dump_stats(void);

#endif /* MM_COW_H */
