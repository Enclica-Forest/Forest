#include "include/yaffs.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static yaffs_fs_t g_yaffs_dev;

uint32_t yaffs_probe(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), uint64_t total_pages) {
    uint8_t buffer[512];
    
    g_yaffs_dev.dev_data = dev_data;
    g_yaffs_dev.read_page = read_page;
    g_yaffs_dev.total_pages = total_pages;
    
    if (total_pages == 0) return 0;
    
    if (read_page(dev_data, 0, buffer)) {
        yaffs_obj_header_t* hdr = (yaffs_obj_header_t*)buffer;
        if (hdr->type >= YAFFS_OBJ_TYPE_FILE && hdr->type <= YAFFS_OBJ_TYPE_SPECIAL) {
            debuglog(DEBUG_INFO, "[YAFFS] Found YAFFS filesystem (type=%u)\n", hdr->type);
            return 70;
        }
    }
    
    return 0;
}

bool yaffs_mount(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), 
                 uint64_t (*read_oob)(void*, uint64_t, uint8_t*), uint64_t total_pages, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t buffer[512];
    if (total_pages > 0 && !read_page(dev_data, 0, buffer)) {
        debuglog(DEBUG_ERROR, "[YAFFS] Failed to read first page\n");
        return false;
    }
    
    yaffs_fs_t* fs = (yaffs_fs_t*)enhanced_heap_alloc(sizeof(yaffs_fs_t), "yaffs_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(yaffs_fs_t));
    fs->dev_data = dev_data;
    fs->read_page = read_page;
    fs->read_oob = read_oob;
    fs->total_pages = total_pages;
    fs->page_size = 512;
    fs->oob_size = 16;
    fs->inode_tree = NULL;
    
    debuglog(DEBUG_INFO, "[YAFFS] Mounted: pages=%llu\n", total_pages);
    
    *sb_out = fs;
    return true;
}

bool yaffs_umount(void* sb) {
    if (!sb) return false;
    yaffs_fs_t* fs = (yaffs_fs_t*)sb;
    if (fs->inode_tree) {
        enhanced_heap_free(fs->inode_tree, "yaffs_inode_tree");
    }
    enhanced_heap_free(fs, "yaffs_fs");
    return true;
}
