/**
 * @file tlb.h
 * @brief Translation Lookaside Buffer (TLB) Management Interface
 * 
 * TLB management including:
 * - Single page invalidation
 * - Full TLB flush
 * - SMP TLB shootdown
 * - PCID support
 * 
 * Note: On 32-bit builds, use tlb_manager.h instead for compatibility.
 * This header is for 64-bit-specific TLB operations.
 */

#ifndef TLB_H
#define TLB_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize TLB manager (always available)
 */
void tlb_init(void);

/**
 * @brief TLB statistics
 */
#ifndef TLB_STATS_T_DEFINED
typedef struct {
    uint64_t invlpg_count;      /**< Number of INVLPG calls */
    uint64_t flush_count;       /**< Number of full TLB flushes */
    uint64_t shootdown_count;   /**< Number of TLB shootdowns */
    bool global_pages_supported;
    bool pcid_supported;
} tlb_stats_t;
#define TLB_STATS_T_DEFINED 1
#endif

/**
 * @brief Get TLB statistics
 * @return TLB statistics structure
 */
tlb_stats_t tlb_get_stats(void);

/* If tlb_manager.h is included, don't redeclare conflicting functions */
#ifndef __TLB_MANAGER_H__

#if defined(__x86_64__)
/**
 * @brief Invalidate a single page in TLB (64-bit)
 * @param vaddr Virtual address of page to invalidate
 */
void tlb_invalidate_page(uint64_t vaddr);

/**
 * @brief Invalidate a range of pages (64-bit)
 * @param start Start virtual address
 * @param end End virtual address
 */
void tlb_invalidate_range(uint64_t start, uint64_t end);

/**
 * @brief Request TLB shootdown for single page (SMP, 64-bit)
 * @param vaddr Virtual address to invalidate on all CPUs
 */
void tlb_shootdown_page(uint64_t vaddr);
#endif /* __x86_64__ */

/**
 * @brief Flush entire TLB (excluding global pages)
 */
void tlb_flush(void);

/**
 * @brief Flush entire TLB including global pages
 */
void tlb_flush_all(void);

/**
 * @brief Flush TLB on address space switch
 * @param new_cr3 New CR3 value (page directory base)
 */
#if defined(__x86_64__)
void tlb_switch_address_space(uint64_t new_cr3);
#else
void tlb_switch_address_space(uint32_t new_cr3);
#endif

/**
 * @brief Request full TLB shootdown (SMP)
 */
void tlb_shootdown_all(void);

/**
 * @brief Handle TLB shootdown IPI
 * 
 * Called from the IPI handler on each CPU when
 * a shootdown request is received.
 */
void tlb_shootdown_ipi_handler(void);

/**
 * @brief Register a new CPU with TLB manager
 * @param cpu_id CPU identifier
 */
void tlb_register_cpu(uint32_t cpu_id);

/**
 * @brief Set current CPU ID
 * @param cpu_id Current CPU identifier
 */
void tlb_set_current_cpu(uint32_t cpu_id);

/**
 * @brief Dump TLB statistics to console
 */
void tlb_dump_stats(void);

#endif /* __TLB_MANAGER_H__ */

#endif /* TLB_H */
