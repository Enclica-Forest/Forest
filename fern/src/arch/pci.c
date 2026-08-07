/*
 * Fern - Cross-Architecture PCI/PCIe Abstraction Layer
 * src/arch/pci.c
 *
 * Unified PCI enumeration for x86, AArch64, RISC-V, and ARM32.
 *
 * Architecture-specific ECAM base discovery:
 *   x86:     ACPI MCFG table (or fallback to Type1 port I/O)
 *   AArch64: DTB /pci or /pcie node with "reg" property
 *   RISC-V:  DTB /soc/pci node with "reg" property
 *   ARM32:   Typically no PCI; DTB /pci if present
 */

#include "pci.h"
#include "arch.h"
#include "../include/debuglog.h"
#include "../include/types.h"
#include "../fdt.h"

#if ARCH_IS_X86
#include "../include/acpi.h"
#include "../include/io_ports.h"
#include "../include/timer.h"
#endif

/* =========================================================================
 * Internal State
 * ========================================================================= */

static bool g_pci_arch_initialized = false;
static pci_access_mode_t g_access_mode = PCI_ACCESS_PORT_IO;

/* ECAM segment table */
static pci_ecam_segment_t g_ecam_segments[PCI_ARCH_MAX_SEGMENTS];
static uint32 g_ecam_segment_count = 0;

/* Port I/O base addresses for x86 Type1 access */
#define PCI_CONFIG_ADDRESS_PORT  0xCF8
#define PCI_CONFIG_DATA_PORT     0xCFC

/* Maximum devices to enumerate before giving up */
#define PCI_ARCH_ENUM_LIMIT 256

/* Static device table for enumeration results */
#define PCI_ARCH_MAX_DEVICES 128
static pci_device_t g_pci_devices[PCI_ARCH_MAX_DEVICES];
static uint32 g_pci_device_count = 0;
static uint32 g_enum_counter = 0;

/* =========================================================================
 * 1. ECAM Address Computation
 * ========================================================================= */

/**
 * pci_ecam_calc_addr - Compute the MMIO address for an ECAM config access.
 *
 * ECAM layout (PCIe spec):
 *   Bits [63:20] = Base address from MCFG/DTB
 *   Bits [19:16] = Bus number (relative to segment start_bus)
 *   Bits [15:11] = Device number
 *   Bits [10:8]  = Function number
 *   Bits [7:2]   = Register offset (dword-aligned)
 *   Bits [1:0]   = Byte enable (always 0 for dword)
 */
static volatile uint32 *pci_ecam_calc_addr(uint16 segment, uint8 bus,
                                           uint8 device, uint8 function,
                                           uint16 offset) {
    /* Find the matching ECAM segment */
    for (uint32 i = 0; i < g_ecam_segment_count; i++) {
        const pci_ecam_segment_t *seg = &g_ecam_segments[i];
        if (!seg->present) continue;
        if (seg->segment != segment) continue;
        if (bus < seg->start_bus || bus > seg->end_bus) continue;

        uint8 rel_bus = bus - seg->start_bus;
        uint64 addr = seg->base
                    + ((uint64)rel_bus << 20)
                    + ((uint64)device << 15)
                    + ((uint64)function << 12)
                    + (offset & 0xFFFC);

        return (volatile uint32 *)(uintptr_t)addr;
    }
    return 0;
}

/* =========================================================================
 * 2. x86 Type1 (Port I/O) Access
 * ========================================================================= */

#if ARCH_IS_X86

static uint32 pci_type1_build_addr(uint8 bus, uint8 device, uint8 function,
                                   uint8 offset) {
    return (uint32)(0x80000000UL
                  | ((uint32)bus << 16)
                  | ((uint32)device << 11)
                  | ((uint32)function << 8)
                  | (offset & 0xFC));
}

static uint32 pci_type1_read(uint8 bus, uint8 device, uint8 function,
                             uint16 offset) {
    uint32 addr = pci_type1_build_addr(bus, device, function, (uint8)offset);
    outportd(PCI_CONFIG_ADDRESS_PORT, addr);
    /* Small delay for device response */
    for (volatile int i = 0; i < 10; i++) {
        __asm__ volatile("nop");
    }
    return inportd(PCI_CONFIG_DATA_PORT);
}

static void pci_type1_write(uint8 bus, uint8 device, uint8 function,
                            uint16 offset, uint32 value) {
    uint32 addr = pci_type1_build_addr(bus, device, function, (uint8)offset);
    outportd(PCI_CONFIG_ADDRESS_PORT, addr);
    outportd(PCI_CONFIG_DATA_PORT, value);
}

/* ACPI MCFG discovery */
static void pci_x86_discover_ecam(void) {
    const acpi_mcfg_table_t *mcfg = acpi_get_mcfg();
    if (!mcfg) return;

    uint32 entry_bytes = mcfg->header.length - sizeof(acpi_mcfg_table_t);
    uint32 entry_count = entry_bytes / sizeof(acpi_mcfg_entry_t);
    if (entry_count > PCI_ARCH_MAX_SEGMENTS)
        entry_count = PCI_ARCH_MAX_SEGMENTS;

    const acpi_mcfg_entry_t *entry =
        (const acpi_mcfg_entry_t *)((const uint8 *)mcfg
                                   + sizeof(acpi_mcfg_table_t));

    for (uint32 i = 0; i < entry_count; i++, entry++) {
        if (entry->base_address == 0) continue;

        g_ecam_segments[g_ecam_segment_count].base = entry->base_address;
        g_ecam_segments[g_ecam_segment_count].segment = entry->segment_group;
        g_ecam_segments[g_ecam_segment_count].start_bus = entry->start_bus;
        g_ecam_segments[g_ecam_segment_count].end_bus = entry->end_bus;
        g_ecam_segments[g_ecam_segment_count].present = true;
        g_ecam_segment_count++;
    }
}

#else /* ARCH_IS_X86 */

/* Stub: no Type1 port I/O on non-x86 */
static uint32 pci_type1_read(uint8 bus, uint8 device, uint8 function,
                             uint16 offset) {
    (void)bus; (void)device; (void)function; (void)offset;
    return 0xFFFFFFFF;
}

static void pci_type1_write(uint8 bus, uint8 device, uint8 function,
                            uint16 offset, uint32 value) {
    (void)bus; (void)device; (void)function; (void)offset; (void)value;
}

#endif /* ARCH_IS_X86 */

/* =========================================================================
 * 3. DTB ECAM Discovery (AArch64 / RISC-V / ARM32)
 * ========================================================================= */

#if !ARCH_IS_X86

/**
 * pci_dtb_discover_ecam - Parse DTB for PCIe ECAM nodes.
 *
 * Scans known DTB paths for PCIe controller nodes and extracts the ECAM
 * base address from the "reg" property.
 *
 * Typical DTB layout:
 *   AArch64 (QEMU virt):
 *     /pci@3f000000  { reg = <0x0 0x3f000000  0x0 0x10000000>; ... }
 *   RISC-V (QEMU virt):
 *     /soc/pci@30000000  { reg = <0x0 0x30000000  0x0 0x10000000>; ... }
 */
static void pci_dtb_discover_ecam(void) {
    /* Paths to search for PCIe controller nodes */
    static const char *pci_paths[] = {
        "/pci",
        "/pcie",
        "/soc/pci",
        "/soc/pcie",
        "/soc/pci@30000000",
        "/soc/pcie@30000000",
        "/pcie@3f000000",
        0
    };

    for (int p = 0; pci_paths[p] != 0; p++) {
        uint32_t reg_len = 0;
        const void *reg = fdt_get_property(pci_paths[p], "reg", &reg_len);
        if (!reg || reg_len < 16) continue;

        /*
         * The "reg" property encodes address cells + size cells.
         * We need to determine #address-cells and #size-cells from the
         * parent node.  For simplicity, we try the common 2-cell (32-bit)
         * and 4-cell (64-bit) formats.
         */
        uint64 base = 0;
        uint64 size = 0;

        if (reg_len >= 16) {
            /* Assume #address-cells=2, #size-cells=2 (most common) */
            const uint32_t *cells = (const uint32_t *)reg;
            uint32 cell0 = fdt32_to_cpu(cells[0]);
            uint32 cell1 = fdt32_to_cpu(cells[1]);
            uint32 cell2 = fdt32_to_cpu(cells[2]);
            uint32 cell3 = fdt32_to_cpu(cells[3]);
            base = ((uint64)cell0 << 32) | cell1;
            size = ((uint64)cell2 << 32) | cell3;
        }

        if (base == 0 || size == 0) continue;

        debuglog(DEBUG_INFO, "[PCI-DTB] Found ECAM at 0x%llx (size 0x%llx) from %s\n",
                 (unsigned long long)base, (unsigned long long)size,
                 pci_paths[p]);

        g_ecam_segments[g_ecam_segment_count].base = base;
        g_ecam_segments[g_ecam_segment_count].segment = 0;
        g_ecam_segments[g_ecam_segment_count].start_bus = 0;
        g_ecam_segments[g_ecam_segment_count].end_bus = 255;
        g_ecam_segments[g_ecam_segment_count].present = true;
        g_ecam_segment_count++;
    }

    /* Also try platform.h defaults if DTB didn't provide anything */
#if ARCH_ARM64 && defined(PLATFORM_PCI_ECAM_BASE)
    if (g_ecam_segment_count == 0) {
        g_ecam_segments[g_ecam_segment_count].base = PLATFORM_PCI_ECAM_BASE;
        g_ecam_segments[g_ecam_segment_count].segment = 0;
        g_ecam_segments[g_ecam_segment_count].start_bus = 0;
        g_ecam_segments[g_ecam_segment_count].end_bus = 255;
        g_ecam_segments[g_ecam_segment_count].present = true;
        g_ecam_segment_count++;
        debuglog(DEBUG_INFO, "[PCI-DTB] Using platform ECAM base 0x%x\n",
                 PLATFORM_PCI_ECAM_BASE);
    }
#endif
}

#else /* ARCH_IS_X86 */

__attribute__((unused)) static void pci_dtb_discover_ecam(void) {
    /* No DTB on x86 (ACPI is used instead) */
}

#endif /* !ARCH_IS_X86 */

/* =========================================================================
 * 4. Unified Config Space Access
 * ========================================================================= */

/**
 * pci_arch_read32_aligned - Read a dword from config space (4-byte aligned).
 */
static uint32 pci_arch_read32_aligned(uint16 segment, uint8 bus, uint8 device,
                                      uint8 function, uint16 offset) {
    /* Try ECAM first */
    if (g_access_mode != PCI_ACCESS_PORT_IO) {
        volatile uint32 *addr = pci_ecam_calc_addr(segment, bus, device,
                                                   function, offset);
        if (addr) {
            return *addr;
        }
    }

    /* Fall back to Type1 (x86 only, ignores segment) */
    if (g_access_mode == PCI_ACCESS_PORT_IO || g_access_mode == PCI_ACCESS_ECAM) {
        return pci_type1_read(bus, device, function, offset);
    }

    return 0xFFFFFFFF;
}

/**
 * pci_arch_write32_aligned - Write a dword to config space (4-byte aligned).
 */
static void pci_arch_write32_aligned(uint16 segment, uint8 bus, uint8 device,
                                     uint8 function, uint16 offset,
                                     uint32 value) {
    /* Try ECAM first */
    if (g_access_mode != PCI_ACCESS_PORT_IO) {
        volatile uint32 *addr = pci_ecam_calc_addr(segment, bus, device,
                                                   function, offset);
        if (addr) {
            *addr = value;
            return;
        }
    }

    /* Fall back to Type1 */
    if (g_access_mode == PCI_ACCESS_PORT_IO || g_access_mode == PCI_ACCESS_ECAM) {
        pci_type1_write(bus, device, function, offset, value);
    }
}

/* =========================================================================
 * 5. Public API: Low-Level Config Access
 * ========================================================================= */

uint32 arch_pci_read_config(uint8 bus, uint8 device, uint8 function,
                            uint16 offset, uint8 size) {
    if (!g_pci_arch_initialized) return 0xFFFFFFFF;

    /* For byte/word accesses, read the containing dword and extract */
    uint16 aligned_off = offset & ~0x3;
    uint32 value = pci_arch_read32_aligned(0, bus, device, function, aligned_off);

    uint8 shift = (offset & 3) * 8;
    switch (size) {
    case 1:  return (value >> shift) & 0xFF;
    case 2:  return (value >> shift) & 0xFFFF;
    case 4:  return value;
    default: return 0xFFFFFFFF;
    }
}

void arch_pci_write_config(uint8 bus, uint8 device, uint8 function,
                           uint16 offset, uint32 value, uint8 size) {
    if (!g_pci_arch_initialized) return;

    uint16 aligned_off = offset & ~0x3;

    if (size == 4 && (offset & 3) == 0) {
        pci_arch_write32_aligned(0, bus, device, function, aligned_off, value);
        return;
    }

    /* Read-modify-write for sub-dword accesses */
    uint32 old = pci_arch_read32_aligned(0, bus, device, function, aligned_off);
    uint8 shift = (offset & 3) * 8;

    switch (size) {
    case 1:
        old &= ~(0xFF << shift);
        old |= (value & 0xFF) << shift;
        break;
    case 2:
        old &= ~(0xFFFF << shift);
        old |= (value & 0xFFFF) << shift;
        break;
    default:
        return;
    }
    pci_arch_write32_aligned(0, bus, device, function, aligned_off, old);
}

/* =========================================================================
 * 6. Device Filling
 * ========================================================================= */

static void pci_fill_device_info(uint8 bus, uint8 device, uint8 function,
                                 pci_device_t *out) {
    out->bus = bus;
    out->device = device;
    out->function = function;
    out->segment = 0;

    out->vendor_id = (uint16)arch_pci_read_config(bus, device, function,
                                                   0x00, 2);
    if (out->vendor_id == 0xFFFF) return;

    out->device_id = (uint16)arch_pci_read_config(bus, device, function,
                                                   0x02, 2);
    out->revision_id = (uint8)arch_pci_read_config(bus, device, function,
                                                    0x08, 1);
    out->prog_if = (uint8)arch_pci_read_config(bus, device, function,
                                                0x09, 1);
    out->subclass = (uint8)arch_pci_read_config(bus, device, function,
                                                 0x0A, 1);
    out->class_code = (uint8)arch_pci_read_config(bus, device, function,
                                                   0x0B, 1);
    out->header_type = (uint8)arch_pci_read_config(bus, device, function,
                                                    0x0E, 1);

    /* Read BARs */
    for (uint8 i = 0; i < PCI_BAR_COUNT; i++) {
        out->bar[i] = arch_pci_read_config_dword(bus, device, function,
                                                  0x10 + (i * 4));
    }

    /* Read interrupt info */
    out->irq_line = (uint8)arch_pci_read_config(bus, device, function,
                                                 0x3C, 1);
    out->irq_pin = (uint8)arch_pci_read_config(bus, device, function,
                                                0x3D, 1);

    /* PCIe capability detection */
    out->is_pcie = false;
    out->pcie_cap_offset = 0;
    out->pcie_device_port_type = 0;
    out->pcie_link_speed = 0;
    out->pcie_link_width = 0;

    uint8 cap_ptr = (uint8)arch_pci_read_config(bus, device, function,
                                                 0x34, 1);
    cap_ptr &= 0xFC;
    for (int i = 0; i < 48 && cap_ptr != 0; i++) {
        uint8 cap_id = (uint8)arch_pci_read_config(bus, device, function,
                                                    cap_ptr, 1);
        if (cap_id == 0x10) { /* PCIe capability */
            out->is_pcie = true;
            out->pcie_cap_offset = cap_ptr;

            uint16 pcie_cap = (uint16)arch_pci_read_config(bus, device, function,
                                                            cap_ptr, 2);
            out->pcie_device_port_type = (pcie_cap >> 4) & 0xF;

            uint32 link_cap = arch_pci_read_config_dword(bus, device, function,
                                                         cap_ptr + 0x0C);
            out->pcie_link_speed = link_cap & 0xF;
            out->pcie_link_width = (link_cap >> 4) & 0x3F;

            uint16 link_sts = (uint16)arch_pci_read_config(bus, device, function,
                                                            cap_ptr + 0x10, 2);
            uint8 cur_speed = link_sts & 0xF;
            uint8 cur_width = (link_sts >> 4) & 0x3F;
            if (cur_speed) out->pcie_link_speed = cur_speed;
            if (cur_width) out->pcie_link_width = cur_width;
            break;
        }
        cap_ptr = (uint8)arch_pci_read_config(bus, device, function,
                                               cap_ptr + 1, 1);
        cap_ptr &= 0xFC;
    }
}

/* =========================================================================
 * 7. Public API: Enumeration
 * ========================================================================= */

static void pci_scan_bus(uint16 segment, uint8 start_bus, uint8 end_bus,
                         arch_pci_enum_callback_t callback, void *context) {
    (void)segment;
    for (uint16 bus = start_bus; bus <= end_bus; bus++) {
        for (uint8 dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            if (g_enum_counter >= PCI_ARCH_ENUM_LIMIT) return;

            uint16 vendor = (uint16)arch_pci_read_config(bus, dev, 0, 0x00, 2);
            if (vendor == 0xFFFF) continue;

            g_enum_counter++;

            uint8 header = (uint8)arch_pci_read_config(bus, dev, 0, 0x0E, 1);
            uint8 functions = (header & 0x80) ? PCI_MAX_FUNCTION : 1;

            for (uint8 fn = 0; fn < functions; fn++) {
                pci_device_t dev_info;
                pci_fill_device_info(bus, dev, fn, &dev_info);

                if (dev_info.vendor_id == 0xFFFF) continue;

                /* Store in device table if room */
                if (g_pci_device_count < PCI_ARCH_MAX_DEVICES) {
                    g_pci_devices[g_pci_device_count++] = dev_info;
                }

                if (callback && !callback(&dev_info, context)) {
                    return;
                }
            }
        }
    }
}

void arch_pci_enumerate(arch_pci_enum_callback_t callback, void *context) {
    g_enum_counter = 0;

    if (g_ecam_segment_count > 0 && g_access_mode != PCI_ACCESS_PORT_IO) {
        for (uint32 i = 0; i < g_ecam_segment_count; i++) {
            const pci_ecam_segment_t *seg = &g_ecam_segments[i];
            if (!seg->present) continue;
            pci_scan_bus(seg->segment, seg->start_bus, seg->end_bus,
                         callback, context);
            if (g_enum_counter >= PCI_ARCH_ENUM_LIMIT) return;
        }
    } else {
        /* Scan all 256 buses on bus 0 */
        pci_scan_bus(0, 0, 255, callback, context);
    }
}

/* =========================================================================
 * 8. Public API: Device Search
 * ========================================================================= */

typedef struct {
    uint16 target_vendor;
    uint16 target_device;
    pci_device_t *out;
    bool found;
} pci_find_dev_ctx_t;

static bool pci_find_dev_cb(const pci_device_t *dev, void *ctx) {
    pci_find_dev_ctx_t *c = (pci_find_dev_ctx_t *)ctx;
    if (dev->vendor_id == c->target_vendor &&
        dev->device_id == c->target_device) {
        *c->out = *dev;
        c->found = true;
        return false;
    }
    return true;
}

bool arch_pci_find_device(uint16 vendor_id, uint16 device_id,
                          pci_device_t *out_device) {
    pci_find_dev_ctx_t ctx = { vendor_id, device_id, out_device, false };
    arch_pci_enumerate(pci_find_dev_cb, &ctx);
    return ctx.found;
}

typedef struct {
    uint8 target_class;
    uint8 target_subclass;
    pci_device_t *out;
    bool found;
} pci_find_class_ctx_t;

static bool pci_find_class_cb(const pci_device_t *dev, void *ctx) {
    pci_find_class_ctx_t *c = (pci_find_class_ctx_t *)ctx;
    if (dev->class_code == c->target_class &&
        dev->subclass == c->target_subclass) {
        *c->out = *dev;
        c->found = true;
        return false;
    }
    return true;
}

bool arch_pci_find_class(uint8 class_code, uint8 subclass,
                         pci_device_t *out_device) {
    pci_find_class_ctx_t ctx = { class_code, subclass, out_device, false };
    arch_pci_enumerate(pci_find_class_cb, &ctx);
    return ctx.found;
}

/* =========================================================================
 * 9. Public API: Initialization
 * ========================================================================= */

bool arch_pci_init(void) {
    if (g_pci_arch_initialized) return true;

    g_ecam_segment_count = 0;
    g_pci_device_count = 0;
    g_access_mode = PCI_ACCESS_PORT_IO;

#if ARCH_IS_X86
    /* x86: Try ACPI MCFG for ECAM, fall back to Type1 */
    pci_x86_discover_ecam();
    if (g_ecam_segment_count > 0) {
        g_access_mode = PCI_ACCESS_ECAM;
        debuglog(DEBUG_INFO, "[PCI] x86: Using ECAM (%u segment(s))\n",
                 g_ecam_segment_count);
    } else {
        g_access_mode = PCI_ACCESS_PORT_IO;
        debuglog(DEBUG_INFO, "[PCI] x86: Using Type1 port I/O (CF8/CFC)\n");
    }
#else
    /* ARM/RISC-V: Try DTB for ECAM base */
    pci_dtb_discover_ecam();
    if (g_ecam_segment_count > 0) {
        g_access_mode = PCI_ACCESS_DT_ECAM;
        debuglog(DEBUG_INFO, "[PCI] %s: Using DTB ECAM (%u segment(s))\n",
                 arch_get_name(), g_ecam_segment_count);
    } else {
        debuglog(DEBUG_INFO, "[PCI] %s: No PCIe/ECAM found\n",
                 arch_get_name());
    }
#endif

    g_pci_arch_initialized = true;

    /* Enumerate and store devices */
    arch_pci_enumerate(0, 0);
    debuglog(DEBUG_INFO, "[PCI] Enumerated %u device(s)\n", g_pci_device_count);

    return true;
}

/* =========================================================================
 * 10. Public API: Query Functions
 * ========================================================================= */

bool arch_pci_has_ecam(void) {
    return g_ecam_segment_count > 0;
}

pci_access_mode_t arch_pci_get_access_mode(void) {
    return g_access_mode;
}

uint32 arch_pci_get_ecam_segments(pci_ecam_segment_t *segments,
                                  uint32 max_count) {
    uint32 count = g_ecam_segment_count;
    if (count > max_count) count = max_count;
    for (uint32 i = 0; i < count; i++) {
        segments[i] = g_ecam_segments[i];
    }
    return count;
}
