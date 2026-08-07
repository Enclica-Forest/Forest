#include "include/udf.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static udf_fs_t g_udf_dev;

uint32_t udf_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors) {
    uint8_t sector[UDF_SECTOR_SIZE];
    
    g_udf_dev.dev_data = dev_data;
    g_udf_dev.read_sector = read_sector;
    g_udf_dev.total_sectors = total_sectors;
    
    if (!read_sector(dev_data, 16, sector)) {
        return 0;
    }
    
    udf_descriptor_t* vd = (udf_descriptor_t*)sector;
    
    if (memcmp(vd->identifier, "NSR02", 5) != 0 && 
        memcmp(vd->identifier, "NSR03", 5) != 0) {
        return 0;
    }
    
    debuglog(DEBUG_INFO, "[UDF] Found UDF filesystem\n");
    return 85;
}

bool udf_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
               uint64_t total_sectors, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t sector[UDF_SECTOR_SIZE];
    
    if (!read_sector(dev_data, 16, sector)) {
        debuglog(DEBUG_ERROR, "[UDF] Failed to read volume descriptor\n");
        return false;
    }
    
    udf_descriptor_t* vd = (udf_descriptor_t*)sector;
    
    if (memcmp(vd->identifier, "NSR02", 5) != 0 && 
        memcmp(vd->identifier, "NSR03", 5) != 0) {
        debuglog(DEBUG_ERROR, "[UDF] Invalid NSR identifier\n");
        return false;
    }
    
    udf_fs_t* fs = (udf_fs_t*)enhanced_heap_alloc(sizeof(udf_fs_t), "udf_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(udf_fs_t));
    fs->dev_data = dev_data;
    fs->read_sector = read_sector;
    fs->total_sectors = total_sectors;
    fs->block_size = UDF_SECTOR_SIZE;
    
    debuglog(DEBUG_INFO, "[UDF] Mounted (version %c%c%c)\n", 
             vd->identifier[3], vd->identifier[4], vd->identifier[5]);
    
    *sb_out = fs;
    return true;
}

bool udf_umount(void* sb) {
    if (!sb) return false;
    udf_fs_t* fs = (udf_fs_t*)sb;
    enhanced_heap_free(fs, "udf_fs");
    return true;
}
