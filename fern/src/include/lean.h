#ifndef LEAN_H
#define LEAN_H

#include "types.h"
#include "stdbool.h"

#define LEAN_BLOCK_SIZE 512

typedef struct {
    uint32_t checksum;
    uint32_t magic;
    uint16_t fs_version;
    uint8_t  prealloc_count;
    uint8_t  log_blocks_per_band;
    uint32_t state;
    uint8_t  uuid[16];
    char     volume_label[64];
    uint64_t block_count;
    uint64_t free_block_count;
    uint64_t primary_super;
    uint64_t backup_super;
    uint64_t bitmap_start;
    uint64_t root_inode;
    uint64_t bad_inode;
    uint64_t journal_inode;
    uint8_t  log_block_size;
    uint8_t  reserved[7];
    uint8_t  reserved2[344];
} lean_superblock_t;

typedef struct {
    uint32_t checksum;
    uint32_t magic;
    uint8_t  extent_count;
    uint8_t  reserved[3];
    uint32_t indirect_count;
    uint32_t link_count;
    uint32_t uid;
    uint32_t gid;
    uint32_t attributes;
    uint64_t file_size;
    uint64_t block_count;
    int64_t  access_time;
    int64_t  status_change_time;
    int64_t  modification_time;
    int64_t  creation_time;
    uint64_t first_indirect;
    uint64_t last_indirect;
    uint64_t fork;
    uint64_t extent_starts[6];
    uint32_t extent_sizes[6];
} lean_inode_t;

typedef struct lean_fs {
    void* dev_data;
    uint32_t (*read_sector)(void*, uint64_t, uint8_t*);
    uint32_t (*write_sector)(void*, uint64_t, uint8_t*);
    uint64_t total_sectors;
    uint32_t block_size;
    uint64_t root_inode_lba;
} lean_fs_t;

uint32_t lean_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors);
bool lean_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
                uint32_t (*write_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors, void** sb_out);
bool lean_umount(void* sb);

#endif
