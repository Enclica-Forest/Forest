#include "include/exfat.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static exfat_fs_t g_exfat_dev;

uint32_t exfat_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors) {
    uint8_t sector[EXFAT_SECTOR_SIZE];
    
    g_exfat_dev.dev_data = dev_data;
    g_exfat_dev.read_sector = read_sector;
    g_exfat_dev.total_sectors = total_sectors;
    
    if (!read_sector(dev_data, 0, sector)) {
        return 0;
    }
    
    exfat_bootsector_t* bs = (exfat_bootsector_t*)sector;
    
    if (memcmp(bs->fsid, "EXFAT   ", 8) != 0) {
        return 0;
    }
    
    debuglog(DEBUG_INFO, "[exFAT] Found exFAT filesystem\n");
    return 90;
}

bool exfat_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
                 uint32_t (*write_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t sector[EXFAT_SECTOR_SIZE];
    if (!read_sector(dev_data, 0, sector)) {
        debuglog(DEBUG_ERROR, "[exFAT] Failed to read boot sector\n");
        return false;
    }
    
    exfat_bootsector_t* bs = (exfat_bootsector_t*)sector;
    
    if (memcmp(bs->fsid, "EXFAT   ", 8) != 0) {
        debuglog(DEBUG_ERROR, "[exFAT] Invalid filesystem ID\n");
        return false;
    }
    
    exfat_fs_t* fs = (exfat_fs_t*)enhanced_heap_alloc(sizeof(exfat_fs_t), "exfat_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(exfat_fs_t));
    fs->dev_data = dev_data;
    fs->read_sector = read_sector;
    fs->write_sector = write_sector;
    fs->total_sectors = total_sectors;
    fs->fat_offset = fs_read32_le((uint8_t*)&bs->fat_offset);
    fs->fat_length = fs_read32_le((uint8_t*)&bs->fat_length);
    fs->cluster_heap_offset = fs_read32_le((uint8_t*)&bs->cluster_heap_offset);
    fs->cluster_count = fs_read32_le((uint8_t*)&bs->cluster_count);
    fs->bytes_per_sector = 1 << bs->bytes_per_sector_shift;
    fs->sectors_per_cluster = 1 << bs->sectors_per_cluster_shift;
    fs->root_dir_cluster = fs_read32_le((uint8_t*)&bs->root_dir_cluster);
    
    debuglog(DEBUG_INFO, "[exFAT] Mounted: clusters=%u, cluster_size=%u\n", 
             fs->cluster_count, fs->bytes_per_sector * fs->sectors_per_cluster);
    
    *sb_out = fs;
    return true;
}

bool exfat_umount(void* sb) {
    if (!sb) return false;
    exfat_fs_t* fs = (exfat_fs_t*)sb;
    enhanced_heap_free(fs, "exfat_fs");
    return true;
}
