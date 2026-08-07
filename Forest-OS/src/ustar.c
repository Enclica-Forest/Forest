#include "include/ustar.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

typedef struct {
    void* dev_data;
    uint32_t (*read_sector)(void*, uint64_t, uint8_t*);
    uint64_t total_sectors;
} ustar_blockdev_t;

static ustar_blockdev_t g_ustar_dev;

uint32_t ustar_read_sector(uint64_t lba, uint8_t* buffer) {
    if (!buffer || lba >= g_ustar_dev.total_sectors) return 0;
    return g_ustar_dev.read_sector(g_ustar_dev.dev_data, lba, buffer);
}

uint32_t ustar_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors) {
    ustar_header_t hdr;
    
    g_ustar_dev.dev_data = dev_data;
    g_ustar_dev.read_sector = read_sector;
    g_ustar_dev.total_sectors = total_sectors;
    
    uint8_t buffer[USTAR_BLOCK_SIZE];
    if (!read_sector(dev_data, 0, buffer)) {
        return 0;
    }
    
    memcpy(&hdr, buffer, sizeof(ustar_header_t));
    
    if (memcmp(hdr.magic, USTAR_MAGIC, 6) != 0) {
        return 0;
    }
    
    debuglog(DEBUG_INFO, "[USTAR] Found valid USTAR archive\n");
    return 100;
}

bool ustar_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
                 uint64_t total_sectors, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    ustar_fs_t* fs = (ustar_fs_t*)enhanced_heap_alloc(sizeof(ustar_fs_t), "ustar_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(ustar_fs_t));
    
    g_ustar_dev.dev_data = dev_data;
    g_ustar_dev.read_sector = read_sector;
    g_ustar_dev.total_sectors = total_sectors;
    
    ustar_header_t hdr;
    uint8_t buffer[USTAR_BLOCK_SIZE];
    if (!read_sector(dev_data, 0, buffer)) {
        enhanced_heap_free(fs, "ustar_fs");
        return false;
    }
    
    memcpy(&hdr, buffer, sizeof(ustar_header_t));
    
    if (memcmp(hdr.magic, USTAR_MAGIC, 6) != 0) {
        debuglog(DEBUG_ERROR, "[USTAR] Invalid magic\n");
        enhanced_heap_free(fs, "ustar_fs");
        return false;
    }
    
    uint32_t file_size = fs_octal_to_uint32((uint8_t*)hdr.size, 11);
    fs->file_count = 1;
    
    debuglog(DEBUG_INFO, "[USTAR] Mounted (file_count=1, first_file_size=%u)\n", file_size);
    
    *sb_out = fs;
    return true;
}

bool ustar_umount(void* sb) {
    if (!sb) return false;
    ustar_fs_t* fs = (ustar_fs_t*)sb;
    if (fs->data) {
        enhanced_heap_free(fs->data, "ustar_data");
    }
    enhanced_heap_free(fs, "ustar_fs");
    return true;
}
