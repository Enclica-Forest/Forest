#include "include/ffs_amiga.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static ffs_fs_t g_ffs_dev;

static uint16_t amiga_checksum(uint16_t* data, uint32_t words) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < words; i++) {
        sum += data[i];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

uint32_t ffs_amiga_probe(void* dev_data, uint32_t (*read_block)(void*, uint32_t, uint8_t*), uint32_t total_blocks) {
    uint8_t block[FFS_BLOCK_SIZE];
    
    g_ffs_dev.dev_data = dev_data;
    g_ffs_dev.read_block = read_block;
    g_ffs_dev.total_blocks = total_blocks;
    
    if (total_blocks < 2) return 0;
    
    if (!read_block(dev_data, 0, block)) {
        return 0;
    }
    
    uint16_t* p = (uint16_t*)block;
    uint16_t sum = amiga_checksum(p, 256);
    
    if (p[0] == 0x444F && sum == 0) {
        debuglog(DEBUG_INFO, "[FFS Amiga] Found Amiga FFS filesystem\n");
        return 65;
    }
    
    return 0;
}

bool ffs_amiga_mount(void* dev_data, uint32_t (*read_block)(void*, uint32_t, uint8_t*), uint32_t total_blocks, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t block[FFS_BLOCK_SIZE];
    if (!read_block(dev_data, 0, block)) {
        debuglog(DEBUG_ERROR, "[FFS Amiga] Failed to read root block\n");
        return false;
    }
    
    uint16_t* p = (uint16_t*)block;
    uint16_t sum = amiga_checksum(p, 256);
    
    if (p[0] != 0x444F || sum != 0) {
        debuglog(DEBUG_ERROR, "[FFS Amiga] Invalid root block checksum\n");
        return false;
    }
    
    ffs_fs_t* fs = (ffs_fs_t*)enhanced_heap_alloc(sizeof(ffs_fs_t), "ffs_amiga_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(ffs_fs_t));
    fs->dev_data = dev_data;
    fs->read_block = read_block;
    fs->total_blocks = total_blocks;
    fs->root_block = 0;
    
    debuglog(DEBUG_INFO, "[FFS Amiga] Mounted: blocks=%u\n", total_blocks);
    
    *sb_out = fs;
    return true;
}

bool ffs_amiga_umount(void* sb) {
    if (!sb) return false;
    ffs_fs_t* fs = (ffs_fs_t*)sb;
    enhanced_heap_free(fs, "ffs_amiga_fs");
    return true;
}
