/**
 * @file mm_layout.c
 * @brief Advanced Memory Layout Manager
 * 
 * Handles complex and unusual memory configurations:
 * - Memory holes (ISA hole, PCI hole, ACPI reserved)
 * - Non-contiguous memory regions
 * - NUMA configurations
 * - E820 memory map parsing
 * - Hot-pluggable memory
 * - Mixed memory types
 * - Overlapping regions
 * - Memory above 4GB on 32-bit PAE systems
 */

#include "include/mm_layout.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/spinlock.h"

// ============================================================================
// CONSTANTS
// ============================================================================

#define MAX_MEMORY_REGIONS      128
#define MAX_MEMORY_HOLES        64
#define MAX_NUMA_NODES          8
#define MAX_MEMORY_TYPES        16

// Well-known memory holes
#define ISA_HOLE_START          0x000F0000      // 960KB - 1MB (ROM area)
#define ISA_HOLE_END            0x00100000
#define VGA_HOLE_START          0x000A0000      // 640KB - 768KB
#define VGA_HOLE_END            0x000C0000
#define EBDA_TYPICAL            0x0009FC00      // Extended BIOS Data Area
#define PCI_HOLE_START          0xC0000000      // 3GB - 4GB typical
#define PCI_HOLE_END            0x100000000ULL

// E820 memory types
#define E820_USABLE             1
#define E820_RESERVED           2
#define E820_ACPI_RECLAIMABLE   3
#define E820_ACPI_NVS           4
#define E820_BAD_MEMORY         5
#define E820_PMEM               7   // Persistent memory
#define E820_PRAM               12  // Persistent RAM

// SRAT (NUMA) memory affinity types
#define SRAT_MEMORY_AFFINITY    1

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Memory region descriptor
 */
typedef struct {
    uint64_t base;
    uint64_t size;
    mem_region_type_t type;
    uint32_t numa_node;
    uint32_t flags;
    const char* description;
} mem_region_desc_t;

/**
 * @brief Memory hole descriptor
 */
typedef struct {
    uint64_t base;
    uint64_t size;
    const char* reason;
} mem_hole_desc_t;

/**
 * @brief NUMA node descriptor
 */
typedef struct {
    uint32_t id;
    uint64_t total_memory;
    uint64_t free_memory;
    uint32_t cpu_mask;          // CPUs on this node
    uint32_t distance[MAX_NUMA_NODES];  // Distance to other nodes
} numa_node_t;

/**
 * @brief Memory layout state
 */
static struct {
    bool initialized;
    
    // Memory regions
    mem_region_desc_t regions[MAX_MEMORY_REGIONS];
    uint32_t region_count;
    
    // Memory holes
    mem_hole_desc_t holes[MAX_MEMORY_HOLES];
    uint32_t hole_count;
    
    // NUMA information
    numa_node_t numa_nodes[MAX_NUMA_NODES];
    uint32_t numa_node_count;
    bool numa_available;
    
    // Summary statistics
    uint64_t total_memory;
    uint64_t usable_memory;
    uint64_t reserved_memory;
    uint64_t acpi_memory;
    uint64_t hole_memory;
    
    // Memory above 4GB
    uint64_t memory_above_4g;
    bool has_memory_above_4g;
    
    // Lock
    spinlock_t lock;
} layout_state = { .initialized = false };

// ============================================================================
// E820 MEMORY MAP PARSING
// ============================================================================

/**
 * @brief Parse E820 memory type
 */
static mem_region_type_t e820_to_region_type(uint32_t e820_type) {
    switch (e820_type) {
        case E820_USABLE:           return MEM_REGION_USABLE;
        case E820_RESERVED:         return MEM_REGION_RESERVED;
        case E820_ACPI_RECLAIMABLE: return MEM_REGION_ACPI_RECLAIMABLE;
        case E820_ACPI_NVS:         return MEM_REGION_ACPI_NVS;
        case E820_BAD_MEMORY:       return MEM_REGION_BAD;
        case E820_PMEM:             return MEM_REGION_PERSISTENT;
        case E820_PRAM:             return MEM_REGION_PERSISTENT;
        default:                    return MEM_REGION_RESERVED;
    }
}

/**
 * @brief Get description for E820 type
 */
static const char* e820_type_name(uint32_t type) {
    switch (type) {
        case E820_USABLE:           return "Usable RAM";
        case E820_RESERVED:         return "Reserved";
        case E820_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
        case E820_ACPI_NVS:         return "ACPI NVS";
        case E820_BAD_MEMORY:       return "Bad Memory";
        case E820_PMEM:             return "Persistent Memory";
        case E820_PRAM:             return "Persistent RAM";
        default:                    return "Unknown";
    }
}

/**
 * @brief Add a memory region
 */
static bool add_region(uint64_t base, uint64_t size, mem_region_type_t type, 
                       uint32_t numa_node, const char* desc) {
    if (layout_state.region_count >= MAX_MEMORY_REGIONS) {
        return false;
    }
    
    mem_region_desc_t* region = &layout_state.regions[layout_state.region_count++];
    region->base = base;
    region->size = size;
    region->type = type;
    region->numa_node = numa_node;
    region->flags = 0;
    region->description = desc;
    
    // Update statistics
    layout_state.total_memory += size;
    
    switch (type) {
        case MEM_REGION_USABLE:
            layout_state.usable_memory += size;
            break;
        case MEM_REGION_RESERVED:
            layout_state.reserved_memory += size;
            break;
        case MEM_REGION_ACPI_RECLAIMABLE:
        case MEM_REGION_ACPI_NVS:
            layout_state.acpi_memory += size;
            break;
        default:
            break;
    }
    
    // Check for memory above 4GB
    if (base >= 0x100000000ULL) {
        layout_state.has_memory_above_4g = true;
        layout_state.memory_above_4g += size;
    } else if (base + size > 0x100000000ULL) {
        layout_state.has_memory_above_4g = true;
        layout_state.memory_above_4g += (base + size) - 0x100000000ULL;
    }
    
    return true;
}

/**
 * @brief Add a memory hole
 */
static bool add_hole(uint64_t base, uint64_t size, const char* reason) {
    if (layout_state.hole_count >= MAX_MEMORY_HOLES) {
        return false;
    }
    
    mem_hole_desc_t* hole = &layout_state.holes[layout_state.hole_count++];
    hole->base = base;
    hole->size = size;
    hole->reason = reason;
    
    layout_state.hole_memory += size;
    
    return true;
}

// ============================================================================
// WELL-KNOWN REGIONS
// ============================================================================

/**
 * @brief Add well-known x86 memory regions
 */
static void add_well_known_regions(void) {
    // Real mode IVT and BDA (0x00000 - 0x00500)
    add_hole(0x00000, 0x00500, "IVT + BDA");
    
    // Conventional memory (0x00500 - 0x9FC00)
    // This is typically usable
    
    // Extended BIOS Data Area (usually at 0x9FC00)
    add_hole(EBDA_TYPICAL, ISA_HOLE_START - EBDA_TYPICAL, "EBDA");
    
    // VGA/ROM area (0xA0000 - 0xC0000)
    add_hole(VGA_HOLE_START, VGA_HOLE_END - VGA_HOLE_START, "VGA Buffer");
    
    // ROM area (0xC0000 - 0x100000)
    add_hole(VGA_HOLE_END, ISA_HOLE_END - VGA_HOLE_END, "ROM/BIOS");
}

/**
 * @brief Detect PCI memory hole
 */
static void detect_pci_hole(void) {
    // The PCI hole is typically from ~3GB to 4GB
    // Actual location depends on chipset and installed RAM
    // For now, assume standard location
    
    // Check if we have memory regions above 4GB to infer the hole
    uint64_t highest_below_4g = 0;
    
    for (uint32_t i = 0; i < layout_state.region_count; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        if (region->type == MEM_REGION_USABLE) {
            uint64_t end = region->base + region->size;
            if (end <= 0x100000000ULL && end > highest_below_4g) {
                highest_below_4g = end;
            }
        }
    }
    
    // If memory doesn't reach 4GB, there's a hole
    if (highest_below_4g > 0 && highest_below_4g < PCI_HOLE_START) {
        // Memory below typical PCI hole start
    } else if (highest_below_4g > 0 && highest_below_4g < 0x100000000ULL) {
        add_hole(highest_below_4g, 0x100000000ULL - highest_below_4g, "PCI MMIO Hole");
    }
}

// ============================================================================
// MULTIBOOT PARSING
// ============================================================================

/**
 * @brief Parse Multiboot1 memory map
 */
void mm_layout_parse_multiboot1(void* mmap_addr, uint32_t mmap_length) {
    uint8_t* ptr = (uint8_t*)mmap_addr;
    uint8_t* end = ptr + mmap_length;
    
    while (ptr < end) {
        uint32_t entry_size = *(uint32_t*)ptr;
        uint64_t base = *(uint64_t*)(ptr + 4);
        uint64_t length = *(uint64_t*)(ptr + 12);
        uint32_t type = *(uint32_t*)(ptr + 20);
        
        add_region(base, length, e820_to_region_type(type), 0, e820_type_name(type));
        
        ptr += entry_size + 4;
    }
}

/**
 * @brief Parse Multiboot2 memory map
 */
void mm_layout_parse_multiboot2(void* tag) {
    // Multiboot2 memory map tag structure
    // tag_type (4), tag_size (4), entry_size (4), entry_version (4)
    // Then entries...
    
    uint32_t* header = (uint32_t*)tag;
    uint32_t tag_size = header[1];
    uint32_t entry_size = header[2];
    
    uint8_t* entries = (uint8_t*)tag + 16;
    uint8_t* end = (uint8_t*)tag + tag_size;
    
    while (entries < end) {
        uint64_t base = *(uint64_t*)entries;
        uint64_t length = *(uint64_t*)(entries + 8);
        uint32_t type = *(uint32_t*)(entries + 16);
        
        add_region(base, length, e820_to_region_type(type), 0, e820_type_name(type));
        
        entries += entry_size;
    }
}

// ============================================================================
// NUMA SUPPORT
// ============================================================================

/**
 * @brief Add NUMA node
 */
bool mm_layout_add_numa_node(uint32_t id, uint64_t total_mem, uint32_t cpu_mask) {
    if (layout_state.numa_node_count >= MAX_NUMA_NODES) {
        return false;
    }
    
    numa_node_t* node = &layout_state.numa_nodes[layout_state.numa_node_count++];
    node->id = id;
    node->total_memory = total_mem;
    node->free_memory = total_mem;
    node->cpu_mask = cpu_mask;
    
    // Initialize distance (10 = local, higher = remote)
    for (uint32_t i = 0; i < MAX_NUMA_NODES; i++) {
        node->distance[i] = (i == id) ? 10 : 20;
    }
    
    layout_state.numa_available = true;
    
    return true;
}

/**
 * @brief Set NUMA distance
 */
void mm_layout_set_numa_distance(uint32_t from, uint32_t to, uint32_t distance) {
    if (from < MAX_NUMA_NODES && to < MAX_NUMA_NODES) {
        layout_state.numa_nodes[from].distance[to] = distance;
    }
}

/**
 * @brief Get NUMA node for address
 */
uint32_t mm_layout_get_numa_node(uint64_t addr) {
    for (uint32_t i = 0; i < layout_state.region_count; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        if (addr >= region->base && addr < region->base + region->size) {
            return region->numa_node;
        }
    }
    return 0;  // Default to node 0
}

/**
 * @brief Get best NUMA node for CPU
 */
uint32_t mm_layout_best_numa_for_cpu(uint32_t cpu_id) {
    uint32_t cpu_bit = 1 << cpu_id;
    
    for (uint32_t i = 0; i < layout_state.numa_node_count; i++) {
        if (layout_state.numa_nodes[i].cpu_mask & cpu_bit) {
            return i;
        }
    }
    return 0;
}

// ============================================================================
// MEMORY TYPE QUERIES
// ============================================================================

/**
 * @brief Check if address is in usable memory
 */
bool mm_layout_is_usable(uint64_t addr) {
    for (uint32_t i = 0; i < layout_state.region_count; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        if (addr >= region->base && addr < region->base + region->size) {
            return region->type == MEM_REGION_USABLE;
        }
    }
    return false;
}

/**
 * @brief Check if address is in a hole
 */
bool mm_layout_is_hole(uint64_t addr) {
    for (uint32_t i = 0; i < layout_state.hole_count; i++) {
        mem_hole_desc_t* hole = &layout_state.holes[i];
        if (addr >= hole->base && addr < hole->base + hole->size) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Get region type for address
 */
mem_region_type_t mm_layout_get_type(uint64_t addr) {
    for (uint32_t i = 0; i < layout_state.region_count; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        if (addr >= region->base && addr < region->base + region->size) {
            return region->type;
        }
    }
    return MEM_REGION_UNKNOWN;
}

// ============================================================================
// SPECIAL MEMORY CONFIGURATIONS
// ============================================================================

/**
 * @brief Handle memory remapping (for systems with >4GB RAM on 32-bit)
 */
void mm_layout_handle_remap(uint64_t remap_base, uint64_t remap_size, 
                            uint64_t original_base) {
    (void)original_base;
    // Some BIOSes remap memory above 4GB that was stolen by the PCI hole
    // e.g., If you have 8GB RAM and PCI hole is at 3-4GB:
    // - 0-3GB: Normal RAM
    // - 3-4GB: PCI hole
    // - 4-8GB: Remapped RAM (originally 3-4GB) + additional RAM
    
    add_region(remap_base, remap_size, MEM_REGION_USABLE, 0, "Remapped RAM");
    
    print("[MM-LAYOUT] Memory remapped: ");
    print_hex((uint32_t)(remap_base >> 32));
    print_hex((uint32_t)remap_base);
    print(" (");
    print_dec((uint32_t)(remap_size / (1024*1024)));
    print(" MB)\n");
}

/**
 * @brief Mark memory as hot-pluggable
 */
void mm_layout_mark_hotplug(uint64_t base, uint64_t size) {
    for (uint32_t i = 0; i < layout_state.region_count; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        if (region->base == base && region->size == size) {
            region->flags |= MEM_FLAG_HOTPLUG;
            return;
        }
    }
}

/**
 * @brief Add persistent memory region (NVDIMM)
 */
void mm_layout_add_persistent(uint64_t base, uint64_t size) {
    add_region(base, size, MEM_REGION_PERSISTENT, 0, "Persistent Memory (NVDIMM)");
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize memory layout manager
 */
void mm_layout_init(void) {
    if (layout_state.initialized) {
        return;
    }
    
    print("[MM-LAYOUT] Initializing memory layout manager...\n");
    
    // Reset state
    memset(&layout_state, 0, sizeof(layout_state));
    spinlock_init(&layout_state.lock, "mm_layout");
    
    // Add well-known regions
    add_well_known_regions();
    
    layout_state.initialized = true;
    
    print("[MM-LAYOUT] Memory layout manager initialized\n");
}

/**
 * @brief Finalize layout after all regions added
 */
void mm_layout_finalize(void) {
    detect_pci_hole();
    
    // Sort regions by base address
    for (uint32_t i = 0; i < layout_state.region_count - 1; i++) {
        for (uint32_t j = i + 1; j < layout_state.region_count; j++) {
            if (layout_state.regions[j].base < layout_state.regions[i].base) {
                mem_region_desc_t temp = layout_state.regions[i];
                layout_state.regions[i] = layout_state.regions[j];
                layout_state.regions[j] = temp;
            }
        }
    }
}

/**
 * @brief Get layout summary
 */
mm_layout_summary_t mm_layout_get_summary(void) {
    mm_layout_summary_t summary;
    
    summary.total_memory = layout_state.total_memory;
    summary.usable_memory = layout_state.usable_memory;
    summary.reserved_memory = layout_state.reserved_memory;
    summary.acpi_memory = layout_state.acpi_memory;
    summary.hole_memory = layout_state.hole_memory;
    summary.memory_above_4g = layout_state.memory_above_4g;
    summary.has_memory_above_4g = layout_state.has_memory_above_4g;
    summary.numa_available = layout_state.numa_available;
    summary.numa_node_count = layout_state.numa_node_count;
    summary.region_count = layout_state.region_count;
    summary.hole_count = layout_state.hole_count;
    
    return summary;
}

/**
 * @brief Iterate over usable regions
 */
void mm_layout_foreach_usable(void (*callback)(uint64_t base, uint64_t size, uint32_t numa_node)) {
    for (uint32_t i = 0; i < layout_state.region_count; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        if (region->type == MEM_REGION_USABLE) {
            callback(region->base, region->size, region->numa_node);
        }
    }
}

/**
 * @brief Dump memory layout
 */
void mm_layout_dump(void) {
    print("\n================= Memory Layout =================\n\n");
    
    // Summary
    print("Summary:\n");
    print("  Total Memory:   ");
    print_dec((uint32_t)(layout_state.total_memory / (1024*1024)));
    print(" MB\n");
    print("  Usable Memory:  ");
    print_dec((uint32_t)(layout_state.usable_memory / (1024*1024)));
    print(" MB\n");
    print("  Reserved:       ");
    print_dec((uint32_t)(layout_state.reserved_memory / (1024*1024)));
    print(" MB\n");
    print("  ACPI Memory:    ");
    print_dec((uint32_t)(layout_state.acpi_memory / 1024));
    print(" KB\n");
    print("  Memory Holes:   ");
    print_dec((uint32_t)(layout_state.hole_memory / (1024*1024)));
    print(" MB\n");
    
    if (layout_state.has_memory_above_4g) {
        print("  Above 4GB:      ");
        print_dec((uint32_t)(layout_state.memory_above_4g / (1024*1024)));
        print(" MB\n");
    }
    
    // Regions
    print("\nMemory Regions (");
    print_dec(layout_state.region_count);
    print("):\n");
    
    for (uint32_t i = 0; i < layout_state.region_count && i < 20; i++) {
        mem_region_desc_t* region = &layout_state.regions[i];
        
        print("  0x");
        print_hex((uint32_t)(region->base >> 32));
        print_hex((uint32_t)region->base);
        print(" - 0x");
        uint64_t end = region->base + region->size - 1;
        print_hex((uint32_t)(end >> 32));
        print_hex((uint32_t)end);
        print(" : ");
        print(region->description ? region->description : "Unknown");
        print("\n");
    }
    
    if (layout_state.region_count > 20) {
        print("  ... and ");
        print_dec(layout_state.region_count - 20);
        print(" more regions\n");
    }
    
    // Holes
    print("\nMemory Holes (");
    print_dec(layout_state.hole_count);
    print("):\n");
    
    for (uint32_t i = 0; i < layout_state.hole_count; i++) {
        mem_hole_desc_t* hole = &layout_state.holes[i];
        
        print("  0x");
        print_hex((uint32_t)(hole->base >> 32));
        print_hex((uint32_t)hole->base);
        print(" (");
        print_dec((uint32_t)(hole->size / 1024));
        print(" KB): ");
        print(hole->reason ? hole->reason : "Unknown");
        print("\n");
    }
    
    // NUMA info
    if (layout_state.numa_available) {
        print("\nNUMA Nodes (");
        print_dec(layout_state.numa_node_count);
        print("):\n");
        
        for (uint32_t i = 0; i < layout_state.numa_node_count; i++) {
            numa_node_t* node = &layout_state.numa_nodes[i];
            
            print("  Node ");
            print_dec(node->id);
            print(": ");
            print_dec((uint32_t)(node->total_memory / (1024*1024)));
            print(" MB, CPUs: 0x");
            print_hex(node->cpu_mask);
            print("\n");
        }
    }
    
    print("\n=================================================\n\n");
}

/**
 * @brief Get region type name
 */
const char* mm_layout_type_name(mem_region_type_t type) {
    switch (type) {
        case MEM_REGION_USABLE:             return "Usable";
        case MEM_REGION_RESERVED:           return "Reserved";
        case MEM_REGION_ACPI_RECLAIMABLE:   return "ACPI Reclaimable";
        case MEM_REGION_ACPI_NVS:           return "ACPI NVS";
        case MEM_REGION_BAD:                return "Bad Memory";
        case MEM_REGION_PERSISTENT:         return "Persistent";
        case MEM_REGION_UNKNOWN:            return "Unknown";
        default:                            return "Invalid";
    }
}
