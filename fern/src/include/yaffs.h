#ifndef YAFFS_H
#define YAFFS_H

#include "types.h"
#include "stdbool.h"

#define YAFFS_OBJ_TYPE_FILE 1
#define YAFFS_OBJ_TYPE_DIR 2
#define YAFFS_OBJ_TYPE_SYMLINK 3
#define YAFFS_OBJ_TYPE_HARDLINK 4
#define YAFFS_OBJ_TYPE_SPECIAL 5

typedef struct {
    uint32_t type;
    uint32_t parent_id;
    uint16_t name_len;
    char     name[256];
    uint32_t file_size;
    uint32_t access_time;
    uint32_t modification_time;
    uint32_t creation_time;
    uint32_t custom_field_len;
    uint32_t equivalent_id;
    uint32_t alias;
    uint32_t uid;
    uint32_t gid;
    uint32_t check_sum;
} yaffs_obj_header_t;

typedef struct yaffs_fs {
    void* dev_data;
    uint64_t (*read_page)(void*, uint64_t, uint8_t*);
    uint64_t (*read_oob)(void*, uint64_t, uint8_t*);
    uint64_t total_pages;
    uint32_t page_size;
    uint32_t oob_size;
    void* inode_tree;
} yaffs_fs_t;

uint32_t yaffs_probe(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), uint64_t total_pages);
bool yaffs_mount(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), 
                 uint64_t (*read_oob)(void*, uint64_t, uint8_t*), uint64_t total_pages, void** sb_out);
bool yaffs_umount(void* sb);

#endif
