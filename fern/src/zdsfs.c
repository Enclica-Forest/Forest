#include "include/zdsfs.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static zdsfs_fs_t g_zdsfs_dev;

uint32_t zdsfs_probe(void* dev_data, uint32_t (*read_record)(void*, int*, int*, int*, void*, uint32_t), uint64_t cylinders) {
    g_zdsfs_dev.dev_data = dev_data;
    g_zdsfs_dev.read_record = read_record;
    g_zdsfs_dev.total_cylinders = cylinders;
    
    int cyl = 0, head = 0, rec = 1;
    dscb1_t dscb;
    uint32_t r = read_record(dev_data, &cyl, &head, &rec, &dscb, sizeof(dscb));
    
    if (r > 0 && dscb.format == 0xF1) {
        debuglog(DEBUG_INFO, "[ZDSFS] Found z/OS dataset filesystem\n");
        return 60;
    }
    
    return 0;
}

bool zdsfs_mount(void* dev_data, uint32_t (*read_record)(void*, int*, int*, int*, void*, uint32_t), uint64_t cylinders, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    zdsfs_fs_t* fs = (zdsfs_fs_t*)enhanced_heap_alloc(sizeof(zdsfs_fs_t), "zdsfs_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(zdsfs_fs_t));
    fs->dev_data = dev_data;
    fs->read_record = read_record;
    fs->total_cylinders = cylinders;
    fs->total_heads = 15;
    fs->total_records = 0;
    
    debuglog(DEBUG_INFO, "[ZDSFS] Mounted: cylinders=%llu\n", cylinders);
    
    *sb_out = fs;
    return true;
}

bool zdsfs_umount(void* sb) {
    if (!sb) return false;
    zdsfs_fs_t* fs = (zdsfs_fs_t*)sb;
    enhanced_heap_free(fs, "zdsfs_fs");
    return true;
}
