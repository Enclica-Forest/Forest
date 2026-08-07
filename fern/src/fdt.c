/*
 * fdt.c - Device Tree (FDT) parser for Forest OS
 *
 * Parses a Flattened Device Tree (FDT) blob and provides functions to
 * query device nodes and properties. The parser walks the flat token
 * structure linearly on each query.
 *
 * Reference: Devicetree Specification v0.4
 */

#include "fdt.h"
#include <stdint.h>
#include <string.h>

/* Global state */
static void *fdt_base_ptr;
static uint32_t fdt_totalsize;
static uint32_t fdt_off_struct;
static uint32_t fdt_off_strings;
static uint32_t fdt_size_struct;
static uint32_t fdt_size_strings;
static const uint8_t *fdt_struct;
static const uint8_t *fdt_strings;

/* -----------------------------------------------------------------------
 * Internal: Validate the FDT blob and store pointers to key sections
 * --------------------------------------------------------------------- */
int fdt_parse(void *fdt_base)
{
    if (!fdt_base)
        return -1;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt_base;

    /* Validate magic */
    if (fdt32_to_cpu(hdr->magic) != FDT_MAGIC)
        return -1;

    /* Read header fields */
    fdt_totalsize   = fdt32_to_cpu(hdr->totalsize);
    fdt_off_struct  = fdt32_to_cpu(hdr->off_dt_struct);
    fdt_off_strings = fdt32_to_cpu(hdr->off_dt_strings);
    fdt_size_struct = fdt32_to_cpu(hdr->size_dt_struct);
    fdt_size_strings = fdt32_to_cpu(hdr->size_dt_strings);

    /* Basic sanity checks */
    if (fdt_totalsize == 0)
        return -1;
    if (fdt_off_struct >= fdt_totalsize)
        return -1;
    if (fdt_off_strings >= fdt_totalsize)
        return -1;
    if (fdt_off_struct + fdt_size_struct > fdt_totalsize)
        return -1;
    if (fdt_off_strings + fdt_size_strings > fdt_totalsize)
        return -1;

    /* Store pointers */
    fdt_base_ptr = fdt_base;
    fdt_struct   = (const uint8_t *)fdt_base + fdt_off_struct;
    fdt_strings  = (const uint8_t *)fdt_base + fdt_off_strings;

    return 0;
}

/* -----------------------------------------------------------------------
 * Internal: Read a big-endian 32-bit value from a pointer
 * --------------------------------------------------------------------- */
static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |
           ((uint32_t)p[3]);
}

/* -----------------------------------------------------------------------
 * Internal: Align a value up to the next 4-byte boundary
 * --------------------------------------------------------------------- */
static inline uint32_t align4(uint32_t v)
{
    return (v + 3) & ~3u;
}

/* -----------------------------------------------------------------------
 * Internal: Check if two path strings match
 *
 * The FDT stores node names without the full path. When walking,
 * we compare the final component of the FDT path with the target.
 * --------------------------------------------------------------------- */
static int path_component_matches(const char *fdt_name, const char *target,
                                   uint32_t target_len)
{
    /* fdt_name may contain '@' unit address; stop comparison there */
    for (uint32_t i = 0; i < target_len; i++) {
        if (target[i] == '\0')
            return 1;
        if (fdt_name[i] == '@' || fdt_name[i] == '\0')
            return 0;
        if (fdt_name[i] != target[i])
            return 0;
    }
    /* target fully consumed – check that fdt name also ends or has '@' */
    char next = fdt_name[target_len];
    return (next == '\0' || next == '@');
}

/* -----------------------------------------------------------------------
 * Internal: Walk FDT structure to find a node by full path
 *
 * Walks the flat token stream, tracking the current path via a stack
 * of path components. Returns the offset of the FDT_BEGIN_NODE token
 * for the matching node, or -1 on failure.
 * --------------------------------------------------------------------- */
struct path_component {
    char name[FDT_MAX_NODE_NAME];
};

static int find_node_offset(const char *path)
{
    if (!fdt_struct || !path)
        return -1;

    /* Handle root node "/" */
    int is_root = (path[0] == '/' && path[1] == '\0');
    if (is_root)
        return 0;  /* offset 0 is the root node BEGIN_NODE */

    /* Parse target path into components */
    struct path_component components[FDT_MAX_DEPTH];
    int num_components = 0;

    const char *p = path;
    if (*p == '/') p++;  /* skip leading slash */

    while (*p && num_components < FDT_MAX_DEPTH) {
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);
        if (len >= FDT_MAX_NODE_NAME) len = FDT_MAX_NODE_NAME - 1;
        memcpy(components[num_components].name, start, len);
        components[num_components].name[len] = '\0';
        num_components++;
        if (*p == '/') p++;
    }

    if (num_components == 0)
        return -1;

    /* Walk the FDT structure */
    uint32_t offset = 0;
    int depth = 0;
    int component_idx = 0;

    while (offset < fdt_size_struct) {
        uint32_t token = read_be32(fdt_struct + offset);
        offset += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            /* Read node name (NUL-terminated, padded to 4 bytes) */
            const uint8_t *name_start = fdt_struct + offset;

            /* Calculate name length including NUL */
            uint32_t name_len = 0;
            while (name_start[name_len] != '\0' && name_len < 256)
                name_len++;
            name_len++;  /* include NUL */

            /* Skip to aligned boundary */
            offset += align4(name_len);

            if (depth == component_idx && component_idx < num_components) {
                if (path_component_matches((const char *)name_start,
                                            components[component_idx].name,
                                            strlen(components[component_idx].name))) {
                    component_idx++;
                    if (component_idx == num_components) {
                        /* Found the target node */
                        return (int)(offset - name_len - 4);
                    }
                }
            }
            depth++;
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (depth < component_idx - 1) {
                /* Backtracked past our target path components – not found */
                return -1;
            }
            if (depth < component_idx) {
                component_idx = depth;
            }
            break;

        case FDT_PROP: {
            uint32_t prop_len = read_be32(fdt_struct + offset);
            offset += 4;
            offset += 4;  /* skip nameoff */
            offset += align4(prop_len);
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return -1;
        default:
            return -1;
        }
    }

    return -1;
}

/* -----------------------------------------------------------------------
 * fdt_find_node - Find a node by path (public interface)
 * --------------------------------------------------------------------- */
const void *fdt_find_node(const char *path)
{
    int off = find_node_offset(path);
    if (off < 0)
        return NULL;
    return fdt_struct + off;
}

/* -----------------------------------------------------------------------
 * fdt_get_property - Get a property value from a node by path
 * --------------------------------------------------------------------- */
const void *fdt_get_property(const char *node_path, const char *prop_name,
                             uint32_t *len_out)
{
    if (!fdt_struct || !node_path || !prop_name)
        return NULL;

    int node_off = find_node_offset(node_path);
    if (node_off < 0)
        return NULL;

    /* Walk from node offset to find the property.
     * node_off points at the FDT_BEGIN_NODE token for our target node,
     * so start with depth = -1; the first BEGIN_NODE bumps it to 0. */
    uint32_t offset = (uint32_t)node_off;
    int depth = -1;

    while (offset < fdt_size_struct) {
        uint32_t token = read_be32(fdt_struct + offset);
        offset += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const uint8_t *name_start = fdt_struct + offset;
            uint32_t name_len = 0;
            while (name_start[name_len] != '\0' && name_len < 256)
                name_len++;
            name_len++;
            offset += align4(name_len);
            depth++;
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (depth < 0)
                return NULL;  /* Past the target node */
            break;

        case FDT_PROP: {
            uint32_t prop_len  = read_be32(fdt_struct + offset);
            uint32_t nameoff   = read_be32(fdt_struct + offset + 4);
            offset += 8;

            const char *name = (const char *)fdt_strings + nameoff;

            if (strcmp(name, prop_name) == 0) {
                if (len_out) *len_out = prop_len;
                return fdt_struct + offset;
            }

            offset += align4(prop_len);
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return NULL;
        default:
            return NULL;
        }
    }

    return NULL;
}

/* -----------------------------------------------------------------------
 * fdt_get_u32 - Get a u32 property value
 * --------------------------------------------------------------------- */
uint32_t fdt_get_u32(const char *node_path, const char *prop_name,
                      uint32_t default_val)
{
    uint32_t len = 0;
    const void *val = fdt_get_property(node_path, prop_name, &len);
    if (!val || len != 4)
        return default_val;
    return fdt32_to_cpu(*(const uint32_t *)val);
}

/* -----------------------------------------------------------------------
 * fdt_get_u64 - Get a u64 property value
 * --------------------------------------------------------------------- */
uint64_t fdt_get_u64(const char *node_path, const char *prop_name,
                      uint64_t default_val)
{
    uint32_t len = 0;
    const void *val = fdt_get_property(node_path, prop_name, &len);
    if (!val || len != 8)
        return default_val;
    return fdt64_to_cpu(*(const uint64_t *)val);
}

/* -----------------------------------------------------------------------
 * fdt_get_string - Get a string property value
 * --------------------------------------------------------------------- */
const char *fdt_get_string(const char *node_path, const char *prop_name,
                            const char *default_val)
{
    uint32_t len = 0;
    const void *val = fdt_get_property(node_path, prop_name, &len);
    if (!val || len == 0)
        return default_val;
    return (const char *)val;
}

/* -----------------------------------------------------------------------
 * fdt_node_exists - Check if a device tree node exists
 * --------------------------------------------------------------------- */
int fdt_node_exists(const char *path)
{
    return find_node_offset(path) >= 0;
}

/* -----------------------------------------------------------------------
 * Internal: Extract the parent path from a full node path
 *
 * E.g. "/soc/uart@10000000" -> "/soc"
 *      "/cpus/cpu@0"        -> "/cpus"
 *      "/memory@80000000"   -> "/"
 * --------------------------------------------------------------------- */
static int get_parent_path(const char *path, char *parent_out, uint32_t max_len)
{
    if (!path || path[0] != '/')
        return -1;

    /* Find last '/' */
    int last_slash = 0;
    int i;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/')
            last_slash = i;
    }

    /* For direct children of root, parent is "/" */
    if (last_slash == 0) {
        if (max_len < 2) return -1;
        parent_out[0] = '/';
        parent_out[1] = '\0';
        return 0;
    }

    uint32_t len = (uint32_t)last_slash;
    if (len >= max_len) len = max_len - 1;
    memcpy(parent_out, path, len);
    parent_out[len] = '\0';
    return 0;
}

/* -----------------------------------------------------------------------
 * Internal: Parse #address-cells or #size-cells from a node
 * Returns the cell count, or a default if not found or on error.
 * --------------------------------------------------------------------- */
static uint32_t parse_cell_count(const char *node_path, const char *prop_name,
                                  uint32_t default_val)
{
    uint32_t len = 0;
    const void *val = fdt_get_property(node_path, prop_name, &len);
    if (!val || len != 4)
        return default_val;
    return fdt32_to_cpu(*(const uint32_t *)val);
}

/* -----------------------------------------------------------------------
 * fdt_get_address - Parse a "reg" property using parent address/size cells
 *
 * Reads #address-cells and #size-cells from the parent node, then decodes
 * the "reg" property of the specified node.
 *
 * @node_path:  Full path to the node containing "reg"
 * @prop_name:  Property name (usually "reg")
 * @addr_out:   Receives the decoded base address
 * @size_out:   Receives the decoded size (0 if not requested)
 *
 * Returns 0 on success, -1 on error.
 * --------------------------------------------------------------------- */
int fdt_get_address(const char *node_path, const char *prop_name,
                    uint64_t *addr_out, uint64_t *size_out)
{
    if (!node_path || !prop_name)
        return -1;

    /* Get parent path and parse address/size cell counts */
    char parent_path[256];
    if (get_parent_path(node_path, parent_path, sizeof(parent_path)) < 0)
        return -1;

    uint32_t addr_cells = parse_cell_count(parent_path, "#address-cells", 1);
    uint32_t size_cells = parse_cell_count(parent_path, "#size-cells", 1);

    /* Clamp to reasonable values */
    if (addr_cells > 2) addr_cells = 2;
    if (size_cells > 2) size_cells = 2;

    uint32_t reg_cells = addr_cells + size_cells;
    if (reg_cells == 0)
        return -1;

    uint32_t len = 0;
    const void *val = fdt_get_property(node_path, prop_name, &len);
    if (!val || len < reg_cells * 4)
        return -1;

    const uint8_t *p = (const uint8_t *)val;

    /* Decode address */
    if (addr_out) {
        uint64_t addr = 0;
        for (uint32_t i = 0; i < addr_cells; i++) {
            addr = (addr << 32) | fdt32_to_cpu(*(const uint32_t *)p);
            p += 4;
        }
        *addr_out = addr;
    } else {
        p += addr_cells * 4;
    }

    /* Decode size */
    if (size_out) {
        uint64_t sz = 0;
        for (uint32_t i = 0; i < size_cells; i++) {
            sz = (sz << 32) | fdt32_to_cpu(*(const uint32_t *)p);
            p += 4;
        }
        *size_out = sz;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * fdt_get_interrupt - Parse interrupt information from a node
 *
 * Reads "interrupts" property and resolves the interrupt parent to
 * determine the IRQ number and type (level/edge).
 *
 * @node_path: Full path to the node
 * @irq_out:   Receives the IRQ number
 * @type_out:  Receives the interrupt type (0=level-high, 1=edge-high,
 *             2=level-low, 3=edge-low per DT spec)
 *
 * Returns 0 on success, -1 on error.
 * --------------------------------------------------------------------- */
int fdt_get_interrupt(const char *node_path, uint32_t *irq_out,
                      uint32_t *type_out)
{
    if (!node_path)
        return -1;

    uint32_t len = 0;
    const void *val = fdt_get_property(node_path, "interrupts", &len);
    if (!val || len < 4)
        return -1;

    /* Determine #interrupt-cells from the interrupt parent */
    uint32_t int_cells = 2; /* default: IRQ + type */
    const char *int_parent = fdt_get_string(node_path, "interrupt-parent", NULL);
    if (int_parent && fdt_node_exists(int_parent)) {
        int_cells = parse_cell_count(int_parent, "#interrupt-cells", 2);
    }

    if (int_cells > 4) int_cells = 4;
    if (len < int_cells * 4)
        return -1;

    const uint8_t *p = (const uint8_t *)val;

    /* First cell is typically the IRQ number */
    if (irq_out)
        *irq_out = fdt32_to_cpu(*(const uint32_t *)p);

    /* Second cell (if present) is the flags/type */
    if (type_out) {
        if (int_cells >= 2)
            *type_out = fdt32_to_cpu(*(const uint32_t *)(p + 4));
        else
            *type_out = 0;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * fdt_for_each_child - Iterate over all direct children of a node
 *
 * Walks the FDT structure from the given parent node and calls the
 * callback for each direct child (depth == parent_depth + 1).
 *
 * @parent_path: Full path to the parent node
 * @callback:    Function called with the child's full path
 * --------------------------------------------------------------------- */
void fdt_for_each_child(const char *parent_path,
                        void (*callback)(const char *child_path))
{
    if (!fdt_struct || !parent_path || !callback)
        return;

    int parent_off = find_node_offset(parent_path);
    if (parent_off < 0)
        return;

    /* Build the current path prefix from parent_path */
    uint32_t prefix_len = strlen(parent_path);

    /* Walk from parent node offset */
    uint32_t offset = (uint32_t)parent_off;
    int depth = -1;
    char child_path[512];

    while (offset < fdt_size_struct) {
        uint32_t token = read_be32(fdt_struct + offset);
        offset += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const uint8_t *name_start = fdt_struct + offset;
            uint32_t name_len = 0;
            while (name_start[name_len] != '\0' && name_len < 256)
                name_len++;
            name_len++; /* include NUL */
            offset += align4(name_len);

            depth++;

            if (depth == 1) {
                /* Direct child – build its full path */
                uint32_t pos = prefix_len;
                if (prefix_len > 1) {
                    /* parent is not root; append separator */
                    if (pos < sizeof(child_path) - 1)
                        child_path[pos++] = '/';
                }
                /* Copy child name (up to '@' or NUL) */
                for (uint32_t i = 0; i < name_len - 1 && pos < sizeof(child_path) - 1; i++) {
                    if (name_start[i] == '@')
                        break;
                    child_path[pos++] = (char)name_start[i];
                }
                child_path[pos] = '\0';

                callback(child_path);
            }
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (depth < 0)
                return; /* done with this subtree */
            break;
        case FDT_PROP: {
            uint32_t prop_len = read_be32(fdt_struct + offset);
            offset += 8; /* skip len + nameoff */
            offset += align4(prop_len);
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return;
        default:
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * fdt_get_memory - Get RAM base address and size from /memory node
 *
 * Reads the "reg" property of the /memory node using standard
 * #address-cells / #size-cells from the root node.
 *
 * @base_out: Receives the physical base address of RAM
 * @size_out: Receives the size of RAM in bytes
 *
 * Returns 0 on success, -1 if /memory not found.
 * --------------------------------------------------------------------- */
int fdt_get_memory(uint64_t *base_out, uint64_t *size_out)
{
    /* Try common /memory node paths */
    const char *paths[] = {
        "/memory@80000000",
        "/memory@40000000",
        "/memory@0",
        "/memory",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        if (fdt_node_exists(paths[i]))
            return fdt_get_address(paths[i], "reg", base_out, size_out);
    }

    return -1;
}

/* -----------------------------------------------------------------------
 * fdt_is_qemu - Check if the DTB was generated by QEMU
 *
 * Checks the root "/compatible" property for a "qemu" string.
 *
 * Returns 1 if QEMU detected, 0 otherwise.
 * --------------------------------------------------------------------- */
int fdt_is_qemu(void)
{
    const char *compat = fdt_get_string("/", "compatible", NULL);
    if (!compat)
        return 0;

    /* Search for "qemu" in the compatible string (may be multi-string) */
    const char *p = compat;
    while (*p) {
        if (strcmp(p, "qemu") == 0)
            return 1;
        p += strlen(p) + 1;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * fdt_get_machine - Get the machine type string from the DTB
 *
 * Returns the "model" property from "/" if present, otherwise the
 * first string from the root "compatible" property.
 *
 * Returns pointer to string (valid while FDT blob is mapped), or NULL.
 * --------------------------------------------------------------------- */
const char *fdt_get_machine(void)
{
    const char *model = fdt_get_string("/", "model", NULL);
    if (model && model[0])
        return model;

    return fdt_get_string("/", "compatible", NULL);
}
