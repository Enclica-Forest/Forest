/**
 * @file mem_protect.h
 * @brief Memory Protection Features Interface
 * 
 * NX, SMEP, SMAP, and PAT support.
 */

#ifndef MEM_PROTECT_H
#define MEM_PROTECT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Memory types for PAT
 */
typedef enum {
    MEM_TYPE_WB,        /**< Write Back (default, fully cached) */
    MEM_TYPE_WT,        /**< Write Through */
    MEM_TYPE_UC,        /**< Uncacheable */
    MEM_TYPE_WC,        /**< Write Combining (good for framebuffers) */
    MEM_TYPE_UC_MINUS   /**< Uncacheable, can be overridden by MTRRs */
} mem_type_t;

/**
 * @brief Memory protection status
 */
typedef struct {
    bool nx_supported;
    bool nx_enabled;
    bool smep_supported;
    bool smep_enabled;
    bool smap_supported;
    bool smap_enabled;
    bool pat_supported;
} mem_protect_status_t;

/**
 * @brief Initialize memory protection features
 */
void mem_protect_init(void);

/**
 * @brief Check if NX is enabled
 */
bool mem_protect_nx_enabled(void);

/**
 * @brief Check if SMEP is enabled
 */
bool mem_protect_smep_enabled(void);

/**
 * @brief Check if SMAP is enabled
 */
bool mem_protect_smap_enabled(void);

/**
 * @brief Enable/disable SMAP at runtime
 */
void mem_protect_set_smap(bool enable);

/**
 * @brief Temporarily disable SMAP for user memory access
 * Call before accessing user memory from kernel.
 */
void mem_protect_stac(void);

/**
 * @brief Re-enable SMAP after user memory access
 * Call after accessing user memory from kernel.
 */
void mem_protect_clac(void);

/**
 * @brief Get PAT index for memory type
 * @param type Memory type
 * @return PAT index (0-7)
 */
uint32_t mem_protect_get_pat_index(mem_type_t type);

/**
 * @brief Get page table flags for memory type
 * @param type Memory type
 * @return Page flags (PWT, PCD bits)
 */
uint32_t mem_protect_get_type_flags(mem_type_t type);

/**
 * @brief Get protection status
 */
mem_protect_status_t mem_protect_get_status(void);

/**
 * @brief Dump protection status to console
 */
void mem_protect_dump_status(void);

#endif /* MEM_PROTECT_H */
