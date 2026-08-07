#ifndef FAT_H
#define FAT_H

#include "types.h"
#include "stdbool.h"
#include "vfs.h"

#define FAT12  12
#define FAT16  16
#define FAT32  32

typedef struct fat_fs fat_fs_t;
typedef struct fat_node {
    fat_fs_t* fs;
    uint32_t start_cluster;
} fat_node_t;

typedef uint32_t (*fat_read_sector_fn)(void* dev_data, uint64_t lba, uint8_t* buffer);
typedef uint32_t (*fat_write_sector_fn)(void* dev_data, uint64_t lba, uint8_t* buffer);

struct fat_fs {
    void* dev_data;
    fat_read_sector_fn read_sector;
    fat_write_sector_fn write_sector;
    
    uint8_t fat_type;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t root_sectors;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t data_sectors;
    uint32_t total_clusters;
    
    uint32_t fat_start;
    uint32_t root_start;
    uint32_t data_start;
    
    uint8_t* fat_buffer;
    uint32_t fat_buffer_size;
    
    uint8_t media_descriptor;
    bool dirty;
};

uint32_t fat_probe(void* dev_data, fat_read_sector_fn read_sector, fat_write_sector_fn write_sector);
bool fat_mount(void* dev_data, fat_read_sector_fn read_sector, fat_write_sector_fn write_sector,
               uint64_t sectors, void** sb_out);
bool fat_umount(void* sb);
vfs_node_t* fat_get_root(void* sb);

int fat_register(void);

#endif
