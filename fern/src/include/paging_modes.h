/**
 * @file paging_modes.h
 * @brief Comprehensive Paging Mode Support Interface
 * 
 * Supports all x86/x64 paging modes:
 * - 32-bit 2-level (4KB pages)
 * - 32-bit PSE (4MB pages)
 * - PAE 3-level (4KB, 36-bit physical)
 * - PAE + PSE (2MB pages)
 * - 64-bit 4-level (4KB, 2MB, 1GB)
 * - 5-level LA57 (57-bit virtual)
 */

#ifndef PAGING_MODES_H
#define PAGING_MODES_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Paging modes
 */
typedef enum {
    PAGING_MODE_NONE,           /**< Paging disabled */
    PAGING_MODE_32BIT,          /**< 32-bit 2-level (4KB only) */
    PAGING_MODE_32BIT_PSE,      /**< 32-bit with PSE (4KB + 4MB) */
    PAGING_MODE_PAE,            /**< PAE 3-level (4KB, 36-bit phys) */
    PAGING_MODE_PAE_PSE,        /**< PAE with 2MB pages */
    PAGING_MODE_64BIT,          /**< 64-bit 4-level (4KB/2MB/1GB) */
    PAGING_MODE_LA57            /**< 5-level paging (57-bit virtual) */
} paging_mode_t;

/**
 * @brief Page sizes
 */
typedef enum {
    PAGE_SIZE_4K,
    PAGE_SIZE_2M,
    PAGE_SIZE_4M,
    PAGE_SIZE_1G
} page_size_t;

/**
 * @brief Paging result codes
 */
typedef enum {
    PAGING_OK,
    PAGING_ERROR_NO_MEMORY,
    PAGING_ERROR_NOT_SUPPORTED,
    PAGING_ERROR_ALREADY_MAPPED,
    PAGING_ERROR_NOT_MAPPED,
    PAGING_ERROR_INVALID_MODE,
    PAGING_ERROR_NOT_INITIALIZED
} paging_result_t;

/**
 * @brief Page mapping flags
 */
#define PAGING_PRESENT          0x0001
#define PAGING_WRITABLE         0x0002
#define PAGING_USER             0x0004
#define PAGING_WRITE_THROUGH    0x0008
#define PAGING_CACHE_DISABLE    0x0010
#define PAGING_GLOBAL           0x0020
#define PAGING_NX               0x0040

/**
 * @brief Paging capabilities info
 */
typedef struct {
    bool pse_supported;
    bool pae_supported;
    bool nx_supported;
    bool pcid_supported;
    bool la57_supported;
    bool gigabyte_pages;
    uint32_t phys_bits;
    uint32_t virt_bits;
} paging_caps_info_t;

/**
 * @brief Initialize paging mode manager
 */
paging_result_t paging_modes_init(void);

/**
 * @brief Set paging mode
 */
paging_result_t paging_set_mode(paging_mode_t mode);

/**
 * @brief Map a page
 */
paging_result_t paging_map(uint64_t vaddr, uint64_t paddr, page_size_t size, uint32_t flags);

/**
 * @brief Map a range with auto page size
 */
paging_result_t paging_map_range(uint64_t vaddr, uint64_t paddr, uint64_t size, uint32_t flags);

/**
 * @brief Enable paging
 */
paging_result_t paging_enable(void);

/**
 * @brief Get current paging mode
 */
paging_mode_t paging_get_mode(void);

/**
 * @brief Get paging capabilities
 */
void paging_get_capabilities(paging_caps_info_t* caps);

/**
 * @brief Get mode name string
 */
const char* paging_mode_name(paging_mode_t mode);

/**
 * @brief Dump paging statistics
 */
void paging_dump_stats(void);

#endif /* PAGING_MODES_H */
