#ifndef EXFAT_H
#define EXFAT_H

#include "types.h"
#include "stdbool.h"

#define EXFAT_SECTOR_SIZE 512

typedef struct {
    uint8_t  jump[3];
    uint8_t  fsid[8];
    uint8_t  pad[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  fat_count;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  bootcode[390];
    uint16_t boot_signature;
} exfat_bootsector_t;

typedef struct {
    uint8_t  entry_type;
    uint8_t  entry_count;
    uint8_t  flags[2];
    uint32_t creation_time;
    uint32_t modification_time;
    uint32_t access_time;
    uint8_t  reserved[12];
    uint32_t start_cluster;
    uint64_t file_size;
} exfat_file_entry_t;

typedef struct exfat_fs {
    void* dev_data;
    uint32_t (*read_sector)(void*, uint64_t, uint8_t*);
    uint32_t (*write_sector)(void*, uint64_t, uint8_t*);
    uint64_t total_sectors;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t root_dir_cluster;
} exfat_fs_t;

uint32_t exfat_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors);
bool exfat_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), 
                 uint32_t (*write_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors, void** sb_out);
bool exfat_umount(void* sb);

#endif
