/*
 * fdt.h - Device Tree (FDT) parser for Forest OS
 *
 * Provides functions to parse a Flattened Device Tree (FDT) blob and
 * query device nodes and properties. Used during early boot on ARM32,
 * AArch64, and RISC-V platforms.
 *
 * Reference: Devicetree Specification v0.4
 */

#ifndef FDT_H
#define FDT_H

#include <stdint.h>

/* FDT magic value */
#define FDT_MAGIC 0xD00DFEED

/* FDT token types */
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

/* FDT header structure (big-endian on disk, fields stored in BE32) */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

/* FDT property structure */
struct fdt_prop {
    uint32_t len;
    uint32_t nameoff;
};

/* Maximum path depth for node traversal */
#define FDT_MAX_DEPTH 32

/* Maximum node name length */
#define FDT_MAX_NODE_NAME 64

/* Maximum property value size */
#define FDT_MAX_PROP_SIZE 4096

/*
 * fdt_parse - Validate and store a pointer to the FDT blob
 *
 * @fdt_base: Pointer to the FDT blob in memory
 *
 * Returns 0 on success, -1 on error (invalid magic, size, etc.)
 */
int fdt_parse(void *fdt_base);

/*
 * fdt_find_node - Find a device tree node by path
 *
 * @path: Full path to the node (e.g., "/cpus/cpu@0")
 *
 * Returns pointer to node begin token on success, NULL on not found.
 * The returned pointer can be passed to fdt_get_property() with an offset.
 */
const void *fdt_find_node(const char *path);

/*
 * fdt_get_property - Get a property value from a device tree node
 *
 * @node_path: Full path to the node (e.g., "/cpus/cpu@0")
 * @prop_name: Property name (e.g., "reg")
 * @len_out: If non-NULL, receives the length of the property value
 *
 * Returns pointer to property data on success, NULL if not found.
 * Data is in big-endian format; use fdt32_to_cpu/fdt64_to_cpu for conversion.
 */
const void *fdt_get_property(const char *node_path, const char *prop_name,
                             uint32_t *len_out);

/*
 * fdt_get_u32 - Get a u32 property value
 *
 * @node_path: Full path to the node
 * @prop_name: Property name
 * @default_val: Value returned if property not found or wrong size
 *
 * Returns property value in native (little-endian) byte order.
 */
uint32_t fdt_get_u32(const char *node_path, const char *prop_name,
                      uint32_t default_val);

/*
 * fdt_get_u64 - Get a u64 property value
 *
 * @node_path: Full path to the node
 * @prop_name: Property name
 * @default_val: Value returned if property not found or wrong size
 *
 * Returns property value in native (little-endian) byte order.
 */
uint64_t fdt_get_u64(const char *node_path, const char *prop_name,
                      uint64_t default_val);

/*
 * fdt_get_string - Get a string property value
 *
 * @node_path: Full path to the node
 * @prop_name: Property name
 * @default_val: String returned if property not found
 *
 * Returns pointer to the string (NUL-terminated within the FDT blob).
 * The string is valid as long as the FDT blob remains mapped.
 */
const char *fdt_get_string(const char *node_path, const char *prop_name,
                            const char *default_val);

/*
 * fdt_node_exists - Check if a device tree node exists
 *
 * @path: Full path to the node
 *
 * Returns 1 if node found, 0 otherwise.
 */
int fdt_node_exists(const char *path);

/*
 * fdt_get_address - Parse a "reg" property using parent address/size cells
 *
 * Reads #address-cells and #size-cells from the parent node, then decodes
 * the specified property of the given node.
 *
 * @node_path:  Full path to the node containing the address property
 * @prop_name:  Property name (usually "reg")
 * @addr_out:   Receives the decoded base address (may be NULL)
 * @size_out:   Receives the decoded size (may be NULL)
 *
 * Returns 0 on success, -1 on error.
 */
int fdt_get_address(const char *node_path, const char *prop_name,
                    uint64_t *addr_out, uint64_t *size_out);

/*
 * fdt_get_interrupt - Parse interrupt information from a node
 *
 * Reads the "interrupts" property and resolves #interrupt-cells.
 *
 * @node_path: Full path to the node
 * @irq_out:   Receives the IRQ number
 * @type_out:  Receives the type (0=level-high, 1=edge-high,
 *             2=level-low, 3=edge-low)
 *
 * Returns 0 on success, -1 on error.
 */
int fdt_get_interrupt(const char *node_path, uint32_t *irq_out,
                      uint32_t *type_out);

/*
 * fdt_for_each_child - Iterate over all direct children of a node
 *
 * @parent_path: Full path to the parent node
 * @callback:    Function called with each child's path string
 */
void fdt_for_each_child(const char *parent_path,
                        void (*callback)(const char *child_path));

/*
 * fdt_get_memory - Get RAM base and size from /memory node
 *
 * @base_out: Receives physical base address of RAM
 * @size_out: Receives size of RAM in bytes
 *
 * Returns 0 on success, -1 if /memory not found.
 */
int fdt_get_memory(uint64_t *base_out, uint64_t *size_out);

/*
 * fdt_is_qemu - Check if the DTB was generated by QEMU
 *
 * Returns 1 if QEMU detected, 0 otherwise.
 */
int fdt_is_qemu(void);

/*
 * fdt_get_machine - Get the machine type string from the DTB
 *
 * Returns pointer to the "model" or "compatible" string, or NULL.
 * The string is valid as long as the FDT blob remains mapped.
 */
const char *fdt_get_machine(void);

/*
 * Byte-order helpers for FDT (big-endian on disk).
 * On little-endian systems (ARM32, AArch64, RISC-V), we need to swap.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline uint32_t fdt32_to_cpu(uint32_t v) { return v; }
static inline uint64_t fdt64_to_cpu(uint64_t v) { return v; }
static inline uint32_t cpu_to_fdt32(uint32_t v) { return v; }
static inline uint64_t cpu_to_fdt64(uint64_t v) { return v; }
#else
static inline uint32_t fdt32_to_cpu(uint32_t v)
{
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}
static inline uint64_t fdt64_to_cpu(uint64_t v)
{
    return ((v & 0xFF00000000000000ull) >> 56) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x000000FF00000000ull) >>  8) |
           ((v & 0x00000000FF000000ull) <<  8) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x00000000000000FFull) << 56);
}
static inline uint32_t cpu_to_fdt32(uint32_t v) { return fdt32_to_cpu(v); }
static inline uint64_t cpu_to_fdt64(uint64_t v) { return fdt64_to_cpu(v); }
#endif

#endif /* FDT_H */
