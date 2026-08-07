/*
 * Block Device Drivers for Fern
 * Implements standard block devices: /dev/sd*, /dev/loop*, backed by the
 * real ATA PIO driver (src/ata.c) and their MBR partitions.
 *
 * This registers against devfs.c's devfs_register_device() - the device
 * registry actually wired into the live boot sequence (see kernel.c's
 * devfs_init() call). An earlier version of this file was written against
 * device_fs.c's device_register()/device_register_class() API instead: a
 * second, parallel device-filesystem implementation in this tree that is
 * never initialized at boot, so nothing it registers was ever reachable.
 *
 * NVMe is intentionally not registered here: there is no real NVMe driver
 * in this tree (no MMIO/queue implementation), so previously this file
 * unconditionally advertised two fake nvme0n1/nvme1n1 devices that never
 * existed in hardware. Advertising devices with no backing driver is worse
 * than not advertising them - callers (lsblk, mkfs, an installer) would
 * "succeed" against a device that silently does nothing.
 */

#include "include/devfs.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/ata.h"
#include "include/block_devices.h"

/* Block device structure */
typedef struct block_device {
    char name[32];
    uint32_t sector_size;
    uint64_t num_sectors;
    bool removable;
    bool readonly;
    ata_device_t *ata_dev; /* backing ATA device; NULL for non-ATA devices (loop) */
    uint64_t start_lba;    /* partition start offset in sectors; 0 for whole disk */
} block_device_t;

/* Major number for all block devices this driver registers (sd*, loop*).
 * devfs_register_device() doesn't validate majors against a pre-registered
 * class table (unlike the unused device_fs.c registry), so this only needs
 * to be internally consistent - picked to match the traditional Linux SCSI
 * disk major (8) for familiarity, not because anything else here reads it. */
#define BLOCK_DEV_MAJOR 8

/* Global block devices */
#define MAX_BLOCK_DEVICES 64  /* Increased for partitions */
static block_device_t g_block_devices[MAX_BLOCK_DEVICES];
static uint32_t g_num_block_devices = 0;

static const char* get_scsi_disk_prefix(void) { return "sd"; }
static const char* get_loop_prefix(void) { return "loop"; }

static dev_ops_t sd_ops;
static dev_ops_t loop_ops;

/* Every dev_ops_t callback here receives the vfs_node_t that devfs_open()
 * built for this device file; node->internal_data is the dev_node_t devfs
 * allocated at registration time, and dev_node_t->private_data is exactly
 * the pointer passed as the last argument to devfs_register_device() below
 * (see devfs_open() in devfs.c for where this wiring happens). */
static block_device_t* blkdev_from_node(vfs_node_t* node) {
    if (!node || !node->internal_data) {
        return NULL;
    }
    dev_node_t* dev = (dev_node_t*)node->internal_data;
    return (block_device_t*)dev->private_data;
}

static uint32 sd_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    block_device_t* blkdev = blkdev_from_node(node);
    if (!blkdev || !blkdev->ata_dev || !buffer || size == 0) {
        return 0;
    }
    if (blkdev->sector_size == 0 ||
        (offset % blkdev->sector_size) != 0 || (size % blkdev->sector_size) != 0) {
        return 0;
    }

    uint64_t start_sector = offset / blkdev->sector_size;
    uint32_t sectors = size / blkdev->sector_size;
    if (start_sector + sectors > blkdev->num_sectors) {
        return 0;
    }

    int bytes = ata_read_sectors(blkdev->ata_dev, blkdev->start_lba + start_sector, sectors, buffer);
    return (bytes > 0) ? (uint32)bytes : 0;
}

static uint32 sd_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    block_device_t* blkdev = blkdev_from_node(node);
    if (!blkdev || !blkdev->ata_dev || blkdev->readonly || !buffer || size == 0) {
        return 0;
    }
    if (blkdev->sector_size == 0 ||
        (offset % blkdev->sector_size) != 0 || (size % blkdev->sector_size) != 0) {
        return 0;
    }

    uint64_t start_sector = offset / blkdev->sector_size;
    uint32_t sectors = size / blkdev->sector_size;
    if (start_sector + sectors > blkdev->num_sectors) {
        return 0;
    }

    int bytes = ata_write_sectors(blkdev->ata_dev, blkdev->start_lba + start_sector, sectors, buffer);
    return (bytes > 0) ? (uint32)bytes : 0;
}

/* ioctl(fd, BLOCK_IOCTL_GET_SIZE, uint64_t *out_bytes) - the only way for
 * userspace (lsblk/blkid/an installer) to learn a block device's size,
 * since devfs has no stat()-equivalent for device files. */
#define BLOCK_IOCTL_GET_SIZE 1

static int sd_ioctl(vfs_node_t* node, uint32 request, void* arg) {
    block_device_t* blkdev = blkdev_from_node(node);
    if (!blkdev) {
        return -1;
    }
    if (request == BLOCK_IOCTL_GET_SIZE && arg) {
        *(uint64_t*)arg = blkdev->num_sectors * blkdev->sector_size;
        return 0;
    }
    return -1;
}

/* Loop device operations - map a backing file to a block device.
 * Not yet implemented (needs a VFS-file-backed read/write path); kept as
 * registered-but-unsupported so /dev/loop* exists for future losetup-style
 * wiring instead of silently absent nodes. */
static uint32 loop_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static uint32 loop_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

static int loop_ioctl(vfs_node_t* node, uint32 request, void* arg) {
    block_device_t* blkdev = blkdev_from_node(node);
    if (!blkdev) {
        return -1;
    }
    if (request == BLOCK_IOCTL_GET_SIZE && arg) {
        *(uint64_t*)arg = blkdev->num_sectors * blkdev->sector_size;
        return 0;
    }
    return -1;
}

static int register_block_device(const char *name, uint16_t major, uint16_t minor,
                                dev_ops_t *ops, block_device_t *blkdev) {
    if (g_num_block_devices >= MAX_BLOCK_DEVICES) {
        return -1;
    }
    if (!devfs_register_device(name, DEV_TYPE_BLOCK, major, minor, ops, blkdev)) {
        return -1;
    }
    g_num_block_devices++;
    return 0;
}

/* Parse a classic MBR partition table (4 primary entries at offset 0x1BE,
 * boot signature 0x55 0xAA at 510-511) from a just-read sector-0 buffer.
 * Registers one /dev/sd<letter><N> block device per non-empty entry.
 * GPT is not handled here - a GPT disk's protective MBR (type 0xEE) is
 * still parsed as a single "partition" spanning the disk, which is at
 * least not silently wrong, but real GPT entries aren't read. */
static void register_mbr_partitions(ata_device_t *ata_dev, const char *disk_name,
                                     char disk_letter, uint16_t disk_minor,
                                     const uint8_t *mbr) {
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        debuglog(DEBUG_INFO, "BLOCK: %s: no valid MBR signature, skipping partition scan\n", disk_name);
        return;
    }

    for (int p = 0; p < 4; p++) {
        const uint8_t *entry = mbr + 0x1BE + p * 16;
        uint8_t part_type = entry[4];
        if (part_type == 0x00) {
            continue; /* unused entry */
        }

        uint32_t lba_start = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                              ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        uint32_t sector_count = (uint32_t)entry[12] | ((uint32_t)entry[13] << 8) |
                                 ((uint32_t)entry[14] << 16) | ((uint32_t)entry[15] << 24);
        if (sector_count == 0) {
            continue;
        }

        char name[16];
        string_format(name, sizeof(name), "%s%c%d", get_scsi_disk_prefix(), disk_letter, p + 1);

        if (g_num_block_devices >= MAX_BLOCK_DEVICES) {
            debuglog(DEBUG_INFO, "BLOCK: partition table full, dropping %s\n", name);
            return;
        }
        block_device_t *partdev = &g_block_devices[g_num_block_devices];
        strncpy(partdev->name, name, sizeof(partdev->name) - 1);
        partdev->sector_size = ATA_SECTOR_SIZE;
        partdev->num_sectors = sector_count;
        partdev->removable = false;
        partdev->readonly = false;
        partdev->ata_dev = ata_dev;
        partdev->start_lba = lba_start;

        /* Linux-style minor numbering: disk_minor * 16 + partition_number */
        uint16_t part_minor = (uint16_t)(disk_minor * 16 + p);
        if (register_block_device(name, BLOCK_DEV_MAJOR, part_minor, &sd_ops, partdev) != 0) {
            debuglog(DEBUG_INFO, "BLOCK: Failed to register %s\n", name);
        } else {
            debuglog(DEBUG_INFO, "BLOCK: %s: type=0x%x start=%u sectors=%u\n",
                        name, part_type, lba_start, sector_count);
        }
    }
}

/* Initialize block devices */
int block_devices_init_real(void) {
    debuglog(DEBUG_INFO, "BLOCK: Initializing block devices\n");

    sd_ops.read = sd_read;
    sd_ops.write = sd_write;
    sd_ops.open = NULL;
    sd_ops.close = NULL;
    sd_ops.ioctl = sd_ioctl;
    sd_ops.poll = NULL;

    loop_ops.read = loop_read;
    loop_ops.write = loop_write;
    loop_ops.open = NULL;
    loop_ops.close = NULL;
    loop_ops.ioctl = loop_ioctl;
    loop_ops.poll = NULL;

    /* Real ATA/SATA (legacy PIO controller) disks and their partitions.
     * Iterates hardware actually detected by ata_detect_devices() - nothing
     * here is registered unless a real drive answered IDENTIFY. */
    int disk_index = 0;
    static const ata_channel_t channels[ATA_MAX_CHANNELS] = {ATA_CHANNEL_PRIMARY, ATA_CHANNEL_SECONDARY};
    static const ata_device_select_t selects[ATA_MAX_DEVICES_PER_CHANNEL] = {ATA_DEV_MASTER, ATA_DEV_SLAVE};

    for (int c = 0; c < ATA_MAX_CHANNELS; c++) {
        for (int s = 0; s < ATA_MAX_DEVICES_PER_CHANNEL; s++) {
            ata_device_t *ata_dev = ata_get_device(channels[c], selects[s]);
            if (!ata_dev || !ata_dev->info.exists || ata_dev->info.type != ATA_DEV_TYPE_ATA) {
                continue; /* no drive, or an ATAPI/optical drive (not a block device here) */
            }

            char disk_letter = (char)('a' + disk_index);
            char name[16];
            string_format(name, sizeof(name), "%s%c", get_scsi_disk_prefix(), disk_letter);

            if (g_num_block_devices >= MAX_BLOCK_DEVICES) {
                debuglog(DEBUG_INFO, "BLOCK: device table full, dropping %s\n", name);
                break;
            }
            block_device_t *blkdev = &g_block_devices[g_num_block_devices];
            strncpy(blkdev->name, name, sizeof(blkdev->name) - 1);
            blkdev->sector_size = ata_dev->info.sector_size ? ata_dev->info.sector_size : ATA_SECTOR_SIZE;
            blkdev->num_sectors = ata_dev->info.sectors;
            blkdev->removable = false;
            blkdev->readonly = false;
            blkdev->ata_dev = ata_dev;
            blkdev->start_lba = 0;

            uint16_t disk_minor = (uint16_t)(disk_index * 16);
            if (register_block_device(name, BLOCK_DEV_MAJOR, disk_minor, &sd_ops, blkdev) != 0) {
                debuglog(DEBUG_INFO, "BLOCK: Failed to register %s\n", name);
                continue;
            }
            debuglog(DEBUG_INFO, "BLOCK: %s: %llu sectors x %u bytes (%s)\n",
                        name, (unsigned long long)blkdev->num_sectors, blkdev->sector_size,
                        ata_dev->info.model);

            /* Read sector 0 to find real partitions - failure just means no
             * partitions get registered for this disk, the whole-disk node
             * (sdX) is still usable. */
            uint8_t mbr[ATA_SECTOR_SIZE];
            if (ata_read_sectors(ata_dev, 0, 1, mbr) == (int)sizeof(mbr)) {
                register_mbr_partitions(ata_dev, name, disk_letter, disk_minor, mbr);
            }

            disk_index++;
        }
    }

    if (disk_index == 0) {
        debuglog(DEBUG_INFO, "BLOCK: No ATA disks detected\n");
    }

    /* Loop devices (backing-file not yet wired up - see loop_read/write).
     * Share BLOCK_DEV_MAJOR with the sd* devices above (it's the only
     * block device class this build actually has room for) but offset the
     * minor range well past anything sd (or sdN) could reach. */
    for (int i = 0; i < 8; i++) {
        char name[16];
        string_format(name, sizeof(name), "%s%d", get_loop_prefix(), i);

        if (g_num_block_devices >= MAX_BLOCK_DEVICES) {
            break;
        }
        block_device_t *blkdev = &g_block_devices[g_num_block_devices];
        strncpy(blkdev->name, name, sizeof(blkdev->name) - 1);
        blkdev->sector_size = 512;
        blkdev->num_sectors = 0; /* Size determined by backing file, once implemented */
        blkdev->removable = false;
        blkdev->readonly = false;
        blkdev->ata_dev = NULL;
        blkdev->start_lba = 0;

        if (register_block_device(name, BLOCK_DEV_MAJOR, 240 + i, &loop_ops, blkdev) != 0) {
            debuglog(DEBUG_INFO, "BLOCK: Failed to register %s\n", name);
        }
    }

    debuglog(DEBUG_INFO, "BLOCK: Block devices initialized (%d devices, %d real ATA disk(s))\n",
                g_num_block_devices, disk_index);
    return 0;
}

/* Cleanup block devices */
void block_devices_cleanup_real(void) {
    debuglog(DEBUG_INFO, "BLOCK: Cleaning up block devices\n");
    g_num_block_devices = 0;
}
