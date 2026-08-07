#include "include/fs.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"

static fs_filesystem_t* g_fs_list = NULL;
static fs_mountpoint_t* g_mountpoints = NULL;

void fs_init(void) {
    debuglog(DEBUG_INFO, "[FS] Initializing filesystem layer\n");
}

bool fs_register(fs_filesystem_t* fs) {
    if (!fs) return false;
    
    fs->next = g_fs_list;
    g_fs_list = fs;
    
    debuglog(DEBUG_INFO, "[FS] Registered filesystem: %s\n", fs->name);
    return true;
}

bool fs_unregister(fs_filesystem_t* fs) {
    if (!fs) return false;
    
    fs_filesystem_t** prev = &g_fs_list;
    for (fs_filesystem_t* f = g_fs_list; f; f = f->next) {
        if (f == fs) {
            *prev = f->next;
            return true;
        }
        prev = &f->next;
    }
    return false;
}

fs_filesystem_t* fs_get_filesystem(const char* fstype) {
    if (!fstype) return NULL;
    
    for (fs_filesystem_t* f = g_fs_list; f; f = f->next) {
        if (strcmp(f->name, fstype) == 0) {
            return f;
        }
    }
    return NULL;
}

fs_filesystem_t* fs_probe(const char* device) {
    (void)device;
    return NULL;
}

fs_superblock_t* fs_mount(const char* device, const char* mountpoint, const char* fstype) {
    (void)device;
    (void)mountpoint;
    (void)fstype;
    return NULL;
}

bool fs_umount(const char* mountpoint) {
    (void)mountpoint;
    return false;
}

fs_mountpoint_t* fs_get_mountpoints(void) {
    return g_mountpoints;
}

