#ifndef ARCH_FS_H
#define ARCH_FS_H

#include "../include/types.h"
#include "../include/vfs.h"
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================
 * Block Device I/O
 *
 * Every architecture exposes block devices through a uniform interface.
 * The read_sector / write_sector callbacks are set during device probe and
 * passed to filesystem drivers so they remain decoupled from the hardware
 * layer (AHCI, virtio-blk, ATA PIO, etc.).
 * ========================================================================= */

#define ARCH_FS_SECTOR_SIZE      512
#define ARCH_FS_MAX_DEVICES      8
#define ARCH_FS_MAX_FS_TYPES     8
#define ARCH_FS_MAX_FILESYSTEMS  4

typedef struct arch_blockdev {
    char     name[32];        /* e.g. "ahci0", "virtio-blk0" */
    uint32   major;           /* VFS major number */
    uint32   minor;           /* VFS minor number */
    uint64   total_sectors;   /* device capacity in sectors */
    uint32   sector_size;     /* usually 512 */
    void*    driver_data;     /* opaque pointer to driver state */

    /* Sector-level I/O.  Returns number of bytes transferred, or 0 on error. */
    uint32 (*read_sector)(void* driver_data, uint64 lba, uint32 count, uint8* buf);
    uint32 (*write_sector)(void* driver_data, uint64 lba, uint32 count, const uint8* buf);

    bool     present;         /* true once probed successfully */
} arch_blockdev_t;

/* =========================================================================
 * Filesystem Type Registry
 *
 * Each architecture registers the filesystem types it supports.  At minimum
 * every architecture supports "initrd" (ramdisk).  Block-based filesystems
 * (fat32, ext2, iso9660, etc.) are registered when their drivers are
 * compiled in.
 * ========================================================================= */

typedef struct arch_fs_type {
    const char* name;         /* "initrd", "fat32", "ext2", ... */
    bool (*probe)(arch_blockdev_t* dev);
    bool (*mount)(arch_blockdev_t* dev, vfs_node_t** root_out);
    void (*unmount)(void* fs_data);
} arch_fs_type_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * arch_fs_init - Initialize the cross-architecture filesystem layer.
 *
 * Called once during kernel startup after memory and device subsystems are
 * ready.  Performs the following in order:
 *   1. Initializes the initrd (ramdisk) from bootloader-provided data.
 *   2. Enumerates block devices (PCI scan on x86; DTB on ARM/RISC-V).
 *   3. Registers the built-in "initrd" filesystem type.
 *   4. Attempts to probe and mount additional filesystems on detected
 *      block devices.
 *
 * Returns true on success.
 */
bool arch_fs_init(void);

/**
 * arch_fs_register_type - Register a filesystem type.
 *
 * @type: Pointer to a statically-allocated arch_fs_type_t.
 *
 * Returns 0 on success, -1 if the registry is full.
 */
int arch_fs_register_type(const arch_fs_type_t* type);

/**
 * arch_fs_get_type - Look up a filesystem type by name.
 *
 * Returns NULL if not found.
 */
const arch_fs_type_t* arch_fs_get_type(const char* name);

/**
 * arch_fs_get_blockdev - Get a block device by index.
 *
 * @index: 0-based index (up to ARCH_FS_MAX_DEVICES - 1).
 *
 * Returns NULL if the index is out of range or the device is not present.
 */
arch_blockdev_t* arch_fs_get_blockdev(uint32 index);

/**
 * arch_fs_get_blockdev_count - Return the number of detected block devices.
 */
uint32 arch_fs_get_blockdev_count(void);

/**
 * arch_fs_probe_devices - (Re-)enumerate block devices.
 *
 * On x86 this scans PCI for AHCI/ATA controllers.
 * On AArch64/RISC-V this scans the DTB for virtio-blk nodes.
 * On ARM32 this is a no-op (no block device support).
 */
void arch_fs_probe_devices(void);

/**
 * arch_fs_try_mount - Attempt to mount a filesystem on a block device.
 *
 * Iterates registered filesystem types and calls each type's probe()
 * callback.  On the first successful probe, calls mount() and creates
 * a VFS mount at the given mountpoint.
 *
 * @dev:        Block device to mount.
 * @mountpoint: VFS path (e.g. "/mnt/disk").
 *
 * Returns 0 on success, -1 if no filesystem could be mounted.
 */
int arch_fs_try_mount(arch_blockdev_t* dev, const char* mountpoint);

#endif /* ARCH_FS_H */
