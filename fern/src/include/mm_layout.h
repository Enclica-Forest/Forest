/**
 * @file mm_layout.h
 * @brief Advanced Memory Layout Manager Interface
 * 
 * Handles:
 * - Memory holes (ISA, PCI, ACPI)
 * - Non-contiguous regions
 * - NUMA configurations
 * - E820 memory maps
 * - Hot-plug memory
 * - Persistent memory
 */

#ifndef MM_LAYOUT_H
#define MM_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Memory region types
 */
typedef enum {
    MEM_REGION_UNKNOWN,
    MEM_REGION_USABLE,
    MEM_REGION_RESERVED,
    MEM_REGION_ACPI_RECLAIMABLE,
    MEM_REGION_ACPI_NVS,
    MEM_REGION_BAD,
    MEM_REGION_PERSISTENT
} mem_region_type_t;

/**
 * @brief Memory region flags
 */
#define MEM_FLAG_HOTPLUG        0x01
#define MEM_FLAG_PERSISTENT     0x02
#define MEM_FLAG_ECC            0x04

/**
 * @brief Memory layout summary
 */
typedef struct {
    uint64_t total_memory;
    uint64_t usable_memory;
    uint64_t reserved_memory;
    uint64_t acpi_memory;
    uint64_t hole_memory;
    uint64_t memory_above_4g;
    bool has_memory_above_4g;
    bool numa_available;
    uint32_t numa_node_count;
    uint32_t region_count;
    uint32_t hole_count;
} mm_layout_summary_t;

/**
 * @brief Initialize memory layout manager
 */
void mm_layout_init(void);

/**
 * @brief Finalize layout after adding regions
 */
void mm_layout_finalize(void);

/**
 * @brief Parse Multiboot1 memory map
 */
void mm_layout_parse_multiboot1(void* mmap_addr, uint32_t mmap_length);

/**
 * @brief Parse Multiboot2 memory map
 */
void mm_layout_parse_multiboot2(void* tag);

/**
 * @brief Add NUMA node
 */
bool mm_layout_add_numa_node(uint32_t id, uint64_t total_mem, uint32_t cpu_mask);

/**
 * @brief Set NUMA distance
 */
void mm_layout_set_numa_distance(uint32_t from, uint32_t to, uint32_t distance);

/**
 * @brief Get NUMA node for address
 */
uint32_t mm_layout_get_numa_node(uint64_t addr);

/**
 * @brief Get best NUMA node for CPU
 */
uint32_t mm_layout_best_numa_for_cpu(uint32_t cpu_id);

/**
 * @brief Check if address is usable
 */
bool mm_layout_is_usable(uint64_t addr);

/**
 * @brief Check if address is in a hole
 */
bool mm_layout_is_hole(uint64_t addr);

/**
 * @brief Get region type for address
 */
mem_region_type_t mm_layout_get_type(uint64_t addr);

/**
 * @brief Handle memory remapping
 */
void mm_layout_handle_remap(uint64_t remap_base, uint64_t remap_size,
                            uint64_t original_base);

/**
 * @brief Mark memory as hot-pluggable
 */
void mm_layout_mark_hotplug(uint64_t base, uint64_t size);

/**
 * @brief Add persistent memory
 */
void mm_layout_add_persistent(uint64_t base, uint64_t size);

/**
 * @brief Get layout summary
 */
mm_layout_summary_t mm_layout_get_summary(void);

/**
 * @brief Iterate over usable regions
 */
void mm_layout_foreach_usable(void (*callback)(uint64_t base, uint64_t size, uint32_t numa_node));

/**
 * @brief Dump memory layout
 */
void mm_layout_dump(void);

/**
 * @brief Get region type name
 */
const char* mm_layout_type_name(mem_region_type_t type);

#endif /* MM_LAYOUT_H */
