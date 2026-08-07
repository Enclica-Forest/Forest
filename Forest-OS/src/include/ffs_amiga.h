#ifndef FFS_AMIGA_H
#define FFS_AMIGA_H

#include "types.h"
#include "stdbool.h"

#define FFS_BLOCK_SIZE 512

typedef struct {
    uint16_t type;
    uint16_t header_key;
    uint16_t high_seq;
    uint16_t hash_size;
    uint16_t checksum;
    uint8_t  hash_table[];
} ffs_root_block_t;

typedef struct {
    uint16_t type;
    uint16_t own_key;
    uint16_t high_seq;
    uint16_t hash_size;
    uint16_t checksum;
    uint8_t  hash_table[];
} ffs_dir_block_t;

typedef struct {
    uint16_t type;
    uint16_t own_key;
    uint16_t high_seq;
    uint16_t data_size;
    uint16_t first_data;
    uint16_t checksum;
    uint16_t data_blocks[];
} ffs_file_header_t;

typedef struct ffs_fs {
    void* dev_data;
    uint32_t (*read_block)(void*, uint32_t, uint8_t*);
    uint32_t total_blocks;
    uint32_t root_block;
} ffs_fs_t;

uint32_t ffs_amiga_probe(void* dev_data, uint32_t (*read_block)(void*, uint32_t, uint8_t*), uint32_t total_blocks);
bool ffs_amiga_mount(void* dev_data, uint32_t (*read_block)(void*, uint32_t, uint8_t*), uint32_t total_blocks, void** sb_out);
bool ffs_amiga_umount(void* sb);

#endif
