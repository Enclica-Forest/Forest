/**
 * Cross-Architecture Filesystem Layer
 * src/arch/fs.c
 *
 * Provides a unified filesystem initialization path for all architectures:
 *   - x86:     initrd via multiboot + AHCI/ATA block devices (PCI scan)
 *   - AArch64: initrd via DTB + virtio-blk (DTB scan)
 *   - RISC-V:  initrd via DTB + virtio-blk (DTB scan)
 *   - ARM32:   initrd via DTB/ATAGS only (no block device support)
 */

#include "arch/arch.h"
#include "arch/fs.h"
#include "../include/vfs.h"
#include "../include/ramdisk.h"
#include "../include/debuglog.h"
#include "../include/string.h"
#include "../include/enhanced_heap.h"

/* ---------------------------------------------------------------------------
 * Block device registry
 * --------------------------------------------------------------------------- */

static arch_blockdev_t  g_blockdevs[ARCH_FS_MAX_DEVICES];
static uint32           g_blockdev_count = 0;

/* ---------------------------------------------------------------------------
 * Filesystem type registry
 * --------------------------------------------------------------------------- */

static const arch_fs_type_t* g_fs_types[ARCH_FS_MAX_FS_TYPES];
static uint32                 g_fs_type_count = 0;

/* ---------------------------------------------------------------------------
 * Forward declarations for per-architecture helpers
 * --------------------------------------------------------------------------- */

static bool initrd_init(void);
static void arch_probe_block_devices(void);

/* =========================================================================
 * Public API – Block Device Registry
 * ========================================================================= */

arch_blockdev_t* arch_fs_get_blockdev(uint32 index) {
    if (index >= ARCH_FS_MAX_DEVICES) return NULL;
    if (!g_blockdevs[index].present) return NULL;
    return &g_blockdevs[index];
}

uint32 arch_fs_get_blockdev_count(void) {
    return g_blockdev_count;
}

static arch_blockdev_t* arch_fs_alloc_blockdev(void) {
    if (g_blockdev_count >= ARCH_FS_MAX_DEVICES) return NULL;
    arch_blockdev_t* dev = &g_blockdevs[g_blockdev_count];
    memset(dev, 0, sizeof(arch_blockdev_t));
    return dev;
}

__attribute__((unused)) static void arch_fs_register_blockdev(arch_blockdev_t* dev) {
    if (!dev || dev->name[0] == '\0') return;
    /* Avoid duplicates by name */
    for (uint32 i = 0; i < g_blockdev_count; i++) {
        if (strcmp(g_blockdevs[i].name, dev->name) == 0) {
            /* Update existing entry */
            g_blockdevs[i] = *dev;
            return;
        }
    }
    if (g_blockdev_count < ARCH_FS_MAX_DEVICES) {
        g_blockdevs[g_blockdev_count] = *dev;
        g_blockdev_count++;
        debuglog(DEBUG_INFO, "[ARCH-FS] Block device registered: %s (%llu sectors)\n",
                 dev->name, (unsigned long long)dev->total_sectors);
    }
}

/* =========================================================================
 * Public API – Filesystem Type Registry
 * ========================================================================= */

int arch_fs_register_type(const arch_fs_type_t* type) {
    if (!type || !type->name) return -1;
    if (g_fs_type_count >= ARCH_FS_MAX_FS_TYPES) return -1;

    /* Avoid duplicates */
    for (uint32 i = 0; i < g_fs_type_count; i++) {
        if (strcmp(g_fs_types[i]->name, type->name) == 0) {
            g_fs_types[i] = type;
            debuglog(DEBUG_INFO, "[ARCH-FS] Filesystem type updated: %s\n", type->name);
            return 0;
        }
    }

    g_fs_types[g_fs_type_count++] = type;
    debuglog(DEBUG_INFO, "[ARCH-FS] Filesystem type registered: %s\n", type->name);
    return 0;
}

const arch_fs_type_t* arch_fs_get_type(const char* name) {
    if (!name) return NULL;
    for (uint32 i = 0; i < g_fs_type_count; i++) {
        if (strcmp(g_fs_types[i]->name, name) == 0) {
            return g_fs_types[i];
        }
    }
    return NULL;
}

/* =========================================================================
 * Initrd Initialization
 *
 * All architectures share the same ramdisk.c parser, but the way the
 * initrd memory range is discovered differs:
 *   - x86: multiboot module list (ramdisk_init() handles this)
 *   - AArch64/RISC-V/ARM32: DTB /chosen node (linux,initrd-start/end)
 *
 * ramdisk_init() already handles x86 multiboot; for non-x86 we parse the
 * DTB and feed the range to the ramdisk parser through a separate path.
 * ========================================================================= */

/* x86 path – ramdisk_init() already handles multiboot1/2 + cached fallback */
#if ARCH_X86_32 || ARCH_X86_64
#include "../include/multiboot.h"
static bool initrd_init(void) {
    /* ramdisk_init() is called elsewhere in the x86 boot path (kernel_main);
     * we just verify that it succeeded.  If files are present, we're good. */
    if (ramdisk_file_count() > 0) {
        debuglog(DEBUG_INFO, "[ARCH-FS] x86 initrd already loaded (%u files)\n",
                 ramdisk_file_count());
        return true;
    }
    debuglog(DEBUG_WARN, "[ARCH-FS] x86 initrd not loaded via multiboot\n");
    return false;
}
#endif

/* DTB-based path (AArch64, RISC-V, ARM32) */
#if ARCH_ARM64 || ARCH_RISCV64 || ARCH_ARM32
#include "../fdt.h"

/* ramdisk.c exposes an internal loader for raw address ranges.  We declare
 * it here so arch/fs.c can feed DTB-discovered ranges.  The symbol is
 * provided by ramdisk.c when compiled for non-x86 targets. */
extern bool ramdisk_init_from_range(uint32 base, uint32 size);

static bool initrd_init(void) {
    /* Check if ramdisk is already loaded (e.g. early boot stub did it) */
    if (ramdisk_file_count() > 0) {
        debuglog(DEBUG_INFO, "[ARCH-FS] initrd already loaded (%u files)\n",
                 ramdisk_file_count());
        return true;
    }

    /* Try to locate initrd via DTB /chosen node */
    uint64_t start = 0, end = 0;
    const void* prop_start = fdt_get_property("/chosen", "linux,initrd-start", NULL);
    const void* prop_end   = fdt_get_property("/chosen", "linux,initrd-end",   NULL);

    if (prop_start && prop_end) {
        start = fdt32_to_cpu(*(const uint32_t*)prop_start);
        end   = fdt32_to_cpu(*(const uint32_t*)prop_end);

        /* AArch64 may use 64-bit properties */
        if (start == 0 && end == 0) {
            prop_start = fdt_get_property("/chosen", "linux,initrd-start", NULL);
            prop_end   = fdt_get_property("/chosen", "linux,initrd-end",   NULL);
            /* Try as 64-bit if the property is 8 bytes */
            if (prop_start) {
                const uint64_t* s64 = (const uint64_t*)prop_start;
                start = fdt64_to_cpu(*s64);
            }
            if (prop_end) {
                const uint64_t* e64 = (const uint64_t*)prop_end;
                end = fdt64_to_cpu(*e64);
            }
        }
    }

    if (start != 0 && end > start) {
        debuglog(DEBUG_INFO, "[ARCH-FS] DTB initrd range: 0x%llx - 0x%llx (%llu bytes)\n",
                 (unsigned long long)start, (unsigned long long)end,
                 (unsigned long long)(end - start));
        return ramdisk_init_from_range((uint32)start, (uint32)(end - start));
    }

    /* Fallback: check for "initrd" node directly */
    if (fdt_node_exists("/chosen/initrd")) {
        const void* reg = fdt_get_property("/chosen/initrd", "reg", NULL);
        if (reg) {
            /* Parse reg property (address + size) */
            uint64_t addr = 0, size = 0;
            const uint64_t* reg64 = (const uint64_t*)reg;
            addr = fdt64_to_cpu(reg64[0]);
            size = fdt64_to_cpu(reg64[1]);
            if (addr != 0 && size != 0) {
                debuglog(DEBUG_INFO, "[ARCH-FS] DTB /chosen/initrd: 0x%llx size 0x%llx\n",
                         (unsigned long long)addr, (unsigned long long)size);
                return ramdisk_init_from_range((uint32)addr, (uint32)size);
            }
        }
    }

    debuglog(DEBUG_WARN, "[ARCH-FS] No initrd found in DTB\n");
    return false;
}
#endif

/* =========================================================================
 * Block Device Probing (per-architecture)
 * ========================================================================= */

/*
 * x86: Scan PCI for AHCI / ATA controllers.
 * The existing AHCI driver and PCI enumeration handle this.  We just
 * create arch_blockdev_t entries for each detected drive.
 */
#if ARCH_X86_32 || ARCH_X86_64
#include "../include/ahci.h"

/* From ahci.c / ahci.h */
extern ahci_controller_t g_ahci_controller;

static uint32 x86_ahci_read(void* driver_data, uint64 lba, uint32 count, uint8* buf) {
    ahci_port_t* port = (ahci_port_t*)driver_data;
    if (!port) return 0;
    return (uint32)ahci_read_sectors(port, (uint64_t)lba, (uint32_t)count, (void*)buf);
}

static uint32 x86_ahci_write(void* driver_data, uint64 lba, uint32 count, const uint8* buf) {
    ahci_port_t* port = (ahci_port_t*)driver_data;
    if (!port) return 0;
    return (uint32)ahci_write_sectors(port, (uint64_t)lba, (uint32_t)count, (const void*)buf);
}

static void arch_probe_block_devices(void) {
    for (uint32 i = 0; i < g_ahci_controller.port_count && g_blockdev_count < ARCH_FS_MAX_DEVICES; i++) {
        ahci_port_t* port = &g_ahci_controller.ports[i];
        if (!port->info.present) continue;

        arch_blockdev_t* dev = arch_fs_alloc_blockdev();
        if (!dev) break;

        string_format(dev->name, sizeof(dev->name), "ahci%u", (uint32)port->port_number);
        dev->major        = 8;        /* SCSI disk major */
        dev->minor        = (uint32)port->port_number;
        dev->total_sectors = port->info.sectors;
        dev->sector_size  = port->info.sector_size ? port->info.sector_size : 512;
        dev->driver_data  = port;
        dev->read_sector  = x86_ahci_read;
        dev->write_sector = x86_ahci_write;
        dev->present      = true;

        debuglog(DEBUG_INFO, "[ARCH-FS] AHCI port %u: %llu sectors (%s)\n",
                 (uint32)port->port_number,
                 (unsigned long long)dev->total_sectors,
                 port->info.model);
    }
}
#endif

/*
 * AArch64: Scan DTB for virtio-mmio block devices.
 * QEMU "virt" machine exposes virtio-blk at virtio_mmio@0x0a000000+.
 */
#if ARCH_ARM64
#include "../fdt.h"

/* virtio-mmio register layout (same as Linux DT binding) */
#define VIRTIO_MMIO_MAGIC     0x000
#define VIRTIO_MMIO_VERSION   0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_STATUS    0x070

#define VIRTIO_DEVICE_ID_BLOCK 2

static uint32 aarch64_virtio_mmio_read32(uint64 base, uint32 offset) {
    volatile uint32_t* addr = (volatile uint32_t*)(uintptr_t)(base + offset);
    return *addr;
}

static void aarch64_virtio_blk_probe_node(const char* path) {
    if (!fdt_node_exists(path)) return;

    /* Check compatible string */
    const char* compat = fdt_get_string(path, "compatible", NULL);
    if (!compat) return;

    /* Look for "virtio,mmio" in the compatible string */
    if (!strstr(compat, "virtio,mmio")) return;

    /* Get MMIO base address from "reg" property */
    const void* reg = fdt_get_property(path, "reg", NULL);
    if (!reg) return;

    /* QEMU virt typically uses 2-cell reg (address + size) */
    const uint32_t* cells = (const uint32_t*)reg;
    uint64_t mmio_base = ((uint64_t)fdt32_to_cpu(cells[0]) << 32) | fdt32_to_cpu(cells[1]);

    /* Verify virtio magic */
    uint32_t magic = aarch64_virtio_mmio_read32(mmio_base, VIRTIO_MMIO_MAGIC);
    if (magic != 0x74726976) { /* "virt" */
        debuglog(DEBUG_WARN, "[ARCH-FS] virtio-mmio at 0x%llx: bad magic 0x%x\n",
                 (unsigned long long)mmio_base, magic);
        return;
    }

    uint32_t dev_id = aarch64_virtio_mmio_read32(mmio_base, VIRTIO_MMIO_DEVICE_ID);
    if (dev_id != VIRTIO_DEVICE_ID_BLOCK) {
        /* Not a block device (could be net, console, etc.) */
        return;
    }

    /* Found a virtio-blk device */
    arch_blockdev_t* dev = arch_fs_alloc_blockdev();
    if (!dev) return;

    string_format(dev->name, sizeof(dev->name), "virtio-blk%u", g_blockdev_count);
    dev->major        = 253;      /* virtio block major */
    dev->minor        = g_blockdev_count;
    dev->sector_size  = 512;
    dev->total_sectors = 0;       /* will be filled by virtio-blk driver */
    dev->driver_data  = (void*)(uintptr_t)mmio_base;
    dev->present      = true;

    debuglog(DEBUG_INFO, "[ARCH-FS] Found virtio-blk at 0x%llx\n",
             (unsigned long long)mmio_base);
}

static void arch_probe_block_devices(void) {
    /* Scan DTB for virtio-mmio nodes under /soc */
    static const char* virtio_paths[] = {
        "/soc/virtio_mmio@0a000000",
        "/soc/virtio_mmio@0a000800",
        "/soc/virtio_mmio@0a001000",
        "/soc/virtio_mmio@0a001800",
        "/soc/virtio_mmio@0a002000",
        "/soc/virtio_mmio@0a002800",
        "/soc/virtio_mmio@0a003000",
        "/soc/virtio_mmio@0a003800",
    };
    for (uint32 i = 0; i < sizeof(virtio_paths)/sizeof(virtio_paths[0]); i++) {
        aarch64_virtio_blk_probe_node(virtio_paths[i]);
    }

    /* Also try iterating DTB children of /soc if fixed paths didn't match */
    if (g_blockdev_count == 0 && fdt_node_exists("/soc")) {
        /* Fallback: iterate /soc children looking for virtio,mmio */
        fdt_for_each_child("/soc", aarch64_virtio_blk_probe_node);
    }
}
#endif

/*
 * RISC-V: Scan DTB for virtio-mmio block devices.
 * Same layout as AArch64 QEMU virt, but at different MMIO addresses.
 */
#if ARCH_RISCV64
#include "../fdt.h"

#define RISCV64_VIRTIO_MMIO_MAGIC     0x000
#define RISCV64_VIRTIO_MMIO_DEVICE_ID 0x008

#define RISCV64_VIRTIO_DEVICE_ID_BLOCK 2

static uint32 riscv64_virtio_mmio_read32(uint64 base, uint32 offset) {
    volatile uint32_t* addr = (volatile uint32_t*)(uintptr_t)(base + offset);
    return *addr;
}

static void riscv64_virtio_blk_probe_node(const char* path) {
    if (!fdt_node_exists(path)) return;

    const char* compat = fdt_get_string(path, "compatible", NULL);
    if (!compat || !strstr(compat, "virtio,mmio")) return;

    const void* reg = fdt_get_property(path, "reg", NULL);
    if (!reg) return;

    const uint32_t* cells = (const uint32_t*)reg;
    uint64_t mmio_base = ((uint64_t)fdt32_to_cpu(cells[0]) << 32) | fdt32_to_cpu(cells[1]);

    uint32_t magic = riscv64_virtio_mmio_read32(mmio_base, RISCV64_VIRTIO_MMIO_MAGIC);
    if (magic != 0x74726976) return;

    uint32_t dev_id = riscv64_virtio_mmio_read32(mmio_base, RISCV64_VIRTIO_MMIO_DEVICE_ID);
    if (dev_id != RISCV64_VIRTIO_DEVICE_ID_BLOCK) return;

    arch_blockdev_t* dev = arch_fs_alloc_blockdev();
    if (!dev) return;

    string_format(dev->name, sizeof(dev->name), "virtio-blk%u", g_blockdev_count);
    dev->major        = 253;
    dev->minor        = g_blockdev_count;
    dev->sector_size  = 512;
    dev->total_sectors = 0;
    dev->driver_data  = (void*)(uintptr_t)mmio_base;
    dev->present      = true;

    debuglog(DEBUG_INFO, "[ARCH-FS] Found virtio-blk at 0x%llx\n",
             (unsigned long long)mmio_base);
}

static void arch_probe_block_devices(void) {
    static const char* virtio_paths[] = {
        "/soc/virtio_mmio@10000000",
        "/soc/virtio_mmio@10000800",
        "/soc/virtio_mmio@10001000",
        "/soc/virtio_mmio@10001800",
        "/soc/virtio_mmio@10002000",
        "/soc/virtio_mmio@10002800",
        "/soc/virtio_mmio@10003000",
        "/soc/virtio_mmio@10003800",
    };
    for (uint32 i = 0; i < sizeof(virtio_paths)/sizeof(virtio_paths[0]); i++) {
        riscv64_virtio_blk_probe_node(virtio_paths[i]);
    }

    if (g_blockdev_count == 0 && fdt_node_exists("/soc")) {
        fdt_for_each_child("/soc", riscv64_virtio_blk_probe_node);
    }
}
#endif

/*
 * ARM32: No block device support.  Initrd only.
 */
#if ARCH_ARM32
static void arch_probe_block_devices(void) {
    debuglog(DEBUG_INFO, "[ARCH-FS] ARM32: no block device support (initrd only)\n");
}
#endif

/* =========================================================================
 * Built-in Filesystem Types
 * ========================================================================= */

/**
 * initrd_filesystem - The "initrd" filesystem type.
 *
 * Every architecture supports this.  The root node is provided by the
 * existing VFS initrd implementation in vfs.c.
 */
static bool initrd_probe(arch_blockdev_t* dev) {
    (void)dev;
    /* initrd doesn't need a block device */
    return (ramdisk_file_count() > 0);
}

static bool initrd_mount(arch_blockdev_t* dev, vfs_node_t** root_out) {
    (void)dev;
    /* The initrd is already mounted as root by vfs_init().  We just
     * confirm it exists and return the root node. */
    if (ramdisk_file_count() == 0) return false;
    /* root_out is filled by the caller from vfs_open("/") */
    if (root_out) *root_out = vfs_open("/", 0);
    return (*root_out != NULL);
}

static void initrd_unmount(void* fs_data) {
    (void)fs_data;
    /* Nothing to unmount for initrd */
}

static const arch_fs_type_t g_initrd_fs_type = {
    .name    = "initrd",
    .probe   = initrd_probe,
    .mount   = initrd_mount,
    .unmount = initrd_unmount,
};

/* =========================================================================
 * Mount Helper
 * ========================================================================= */

int arch_fs_try_mount(arch_blockdev_t* dev, const char* mountpoint) {
    if (!dev || !mountpoint) return -1;

    for (uint32 i = 0; i < g_fs_type_count; i++) {
        const arch_fs_type_t* type = g_fs_types[i];
        if (type->probe && type->probe(dev)) {
            vfs_node_t* root = NULL;
            if (type->mount && type->mount(dev, &root)) {
                /* Register as a VFS mount */
                vfs_filesystem_t* vfs_fs = (vfs_filesystem_t*)
                    enhanced_heap_alloc(sizeof(vfs_filesystem_t), "arch_vfs_mount");
                if (vfs_fs) {
                    memset(vfs_fs, 0, sizeof(vfs_filesystem_t));
                    vfs_fs->name = type->name;
                    vfs_register_filesystem(vfs_fs);
                }
                debuglog(DEBUG_INFO, "[ARCH-FS] Mounted %s on %s\n",
                         type->name, mountpoint);
                return 0;
            }
        }
    }

    debuglog(DEBUG_WARN, "[ARCH-FS] No filesystem matched for %s\n", dev->name);
    return -1;
}

/* =========================================================================
 * Public API – Device Probe
 * ========================================================================= */

void arch_fs_probe_devices(void) {
    debuglog(DEBUG_INFO, "[ARCH-FS] Probing block devices...\n");
    arch_probe_block_devices();
    debuglog(DEBUG_INFO, "[ARCH-FS] %u block device(s) found\n", g_blockdev_count);
}

/* =========================================================================
 * Public API – Initialization
 * ========================================================================= */

bool arch_fs_init(void) {
    debuglog(DEBUG_INFO, "[ARCH-FS] Initializing cross-arch filesystem layer\n");

    /* 1. Initialize initrd */
    bool initrd_ok = initrd_init();
    if (initrd_ok) {
        debuglog(DEBUG_INFO, "[ARCH-FS] initrd loaded: %u files\n",
                 ramdisk_file_count());
    } else {
        debuglog(DEBUG_WARN, "[ARCH-FS] initrd not available\n");
    }

    /* 2. Register built-in filesystem types */
    arch_fs_register_type(&g_initrd_fs_type);

    /* Additional filesystem types can be registered here based on
     * compile-time feature flags (ENABLE_FAT32, ENABLE_EXT2, etc.) */

    /* 3. Probe block devices */
    arch_fs_probe_devices();

    /* 4. Attempt to mount initrd as root (if not already done by vfs_init) */
    if (initrd_ok && ramdisk_file_count() > 0) {
        debuglog(DEBUG_INFO, "[ARCH-FS] Filesystem layer ready (%s)\n",
#if ARCH_X86_32 || ARCH_X86_64
                 "x86"
#elif ARCH_ARM64
                 "aarch64"
#elif ARCH_RISCV64
                 "riscv64"
#elif ARCH_ARM32
                 "arm32"
#else
                 "unknown"
#endif
        );
    }

    return true;
}
