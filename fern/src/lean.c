#include "include/lean.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static lean_fs_t g_lean_dev;

uint32_t lean_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors) {
    uint8_t sector[LEAN_BLOCK_SIZE];
    
    g_lean_dev.dev_data = dev_data;
    g_lean_dev.read_sector = read_sector;
    g_lean_dev.total_sectors = total_sectors;
    
    for (uint32_t blk = 1; blk <= 32; blk++) {
        if (!read_sector(dev_data, blk, sector)) {
            continue;
        }
        
        lean_superblock_t* sb = (lean_superblock_t*)sector;
        if (sb->magic == 0x4E41454C) {
            debuglog(DEBUG_INFO, "[LEAN] Found LEAN filesystem (version %u.%u)\n",
                     sb->fs_version >> 8, sb->fs_version & 0xFF);
            return 80;
        }
    }
    
    return 0;
}

bool lean_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
                uint32_t (*write_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t sector[LEAN_BLOCK_SIZE];
    lean_superblock_t* sb = NULL;
    
    for (uint32_t blk = 1; blk <= 32; blk++) {
        if (!read_sector(dev_data, blk, sector)) {
            continue;
        }
        
        lean_superblock_t* candidate = (lean_superblock_t*)sector;
        if (candidate->magic == 0x4E41454C) {
            sb = candidate;
            break;
        }
    }
    
    if (!sb) {
        debuglog(DEBUG_ERROR, "[LEAN] No valid superblock found\n");
        return false;
    }
    
    lean_fs_t* fs = (lean_fs_t*)enhanced_heap_alloc(sizeof(lean_fs_t), "lean_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(lean_fs_t));
    fs->dev_data = dev_data;
    fs->read_sector = read_sector;
    fs->write_sector = write_sector;
    fs->total_sectors = total_sectors;
    fs->block_size = 1 << sb->log_block_size;
    fs->root_inode_lba = sb->root_inode;
    
    debuglog(DEBUG_INFO, "[LEAN] Mounted: blocks=%llu, block_size=%u\n",
             sb->block_count, fs->block_size);
    
    *sb_out = fs;
    return true;
}

bool lean_umount(void* sb) {
    if (!sb) return false;
    lean_fs_t* fs = (lean_fs_t*)sb;
    enhanced_heap_free(fs, "lean_fs");
    return true;
}
