#include "include/iso9660.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static iso9660_fs_t g_iso_dev;

uint32_t iso9660_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors) {
    uint8_t sector[ISO9660_SECTOR_SIZE];
    
    g_iso_dev.dev_data = dev_data;
    g_iso_dev.read_sector = read_sector;
    g_iso_dev.total_sectors = total_sectors;
    
    if (!read_sector(dev_data, 16, sector)) {
        return 0;
    }
    
    iso_volume_descriptor_t* vd = (iso_volume_descriptor_t*)sector;
    
    if (vd->type != ISO9660_VD_PRIMARY) {
        return 0;
    }
    
    if (memcmp(vd->identifier, "CD001", 5) != 0) {
        return 0;
    }
    
    debuglog(DEBUG_INFO, "[ISO9660] Found valid ISO 9660 filesystem\n");
    return 100;
}

bool iso9660_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
                   uint64_t total_sectors, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t sector[ISO9660_SECTOR_SIZE];
    if (!read_sector(dev_data, 16, sector)) {
        debuglog(DEBUG_ERROR, "[ISO9660] Failed to read primary volume descriptor\n");
        return false;
    }
    
    iso_primary_vd_t* pvd = (iso_primary_vd_t*)sector;
    
    iso9660_fs_t* fs = (iso9660_fs_t*)enhanced_heap_alloc(sizeof(iso9660_fs_t), "iso9660_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(iso9660_fs_t));
    fs->dev_data = dev_data;
    fs->read_sector = read_sector;
    fs->total_sectors = total_sectors;
    fs->block_size = fs_read16_le((uint8_t*)&pvd->logical_block_size_lsb);
    fs->root_lba = fs_read32_le((uint8_t*)&pvd->root_dir_record[2]);
    fs->path_table_lba = fs_read32_le((uint8_t*)&pvd->path_table_lba_lsb);
    
    debuglog(DEBUG_INFO, "[ISO9660] Mounted: root LBA=%u, block_size=%u\n", 
             fs->root_lba, fs->block_size);
    
    *sb_out = fs;
    return true;
}

bool iso9660_umount(void* sb) {
    if (!sb) return false;
    iso9660_fs_t* fs = (iso9660_fs_t*)sb;
    enhanced_heap_free(fs, "iso9660_fs");
    return true;
}
