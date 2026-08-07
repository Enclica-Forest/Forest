#include "include/jffs2.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"

static jffs2_fs_t g_jffs2_dev;

uint32_t jffs2_probe(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), uint64_t total_size) {
    uint8_t buffer[256];
    
    g_jffs2_dev.dev_data = dev_data;
    g_jffs2_dev.read_page = read_page;
    g_jffs2_dev.total_size = total_size;
    
    if (!read_page(dev_data, 0, buffer)) {
        return 0;
    }
    
    jffs2_unknown_node_t* node = (jffs2_unknown_node_t*)buffer;
    if (node->magic == JFFS2_MAGIC || node->magic == JFFS2_MAGIC_SHORT) {
        debuglog(DEBUG_INFO, "[JFFS2] Found JFFS2 filesystem\n");
        return 75;
    }
    
    return 0;
}

bool jffs2_mount(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), uint64_t total_size, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    uint8_t buffer[256];
    if (!read_page(dev_data, 0, buffer)) {
        debuglog(DEBUG_ERROR, "[JFFS2] Failed to read first node\n");
        return false;
    }
    
    jffs2_unknown_node_t* node = (jffs2_unknown_node_t*)buffer;
    if (node->magic != JFFS2_MAGIC && node->magic != JFFS2_MAGIC_SHORT) {
        debuglog(DEBUG_ERROR, "[JFFS2] Invalid magic\n");
        return false;
    }
    
    jffs2_fs_t* fs = (jffs2_fs_t*)enhanced_heap_alloc(sizeof(jffs2_fs_t), "jffs2_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(jffs2_fs_t));
    fs->dev_data = dev_data;
    fs->read_page = read_page;
    fs->total_size = total_size;
    fs->page_size = 256;
    fs->oob_size = 16;
    
    debuglog(DEBUG_INFO, "[JFFS2] Mounted: size=%llu\n", total_size);
    
    *sb_out = fs;
    return true;
}

bool jffs2_umount(void* sb) {
    if (!sb) return false;
    jffs2_fs_t* fs = (jffs2_fs_t*)sb;
    enhanced_heap_free(fs, "jffs2_fs");
    return true;
}
