#ifndef ISO9660_H
#define ISO9660_H

#include "types.h"
#include "stdbool.h"

#define ISO9660_SECTOR_SIZE 2048
#define ISO9660_VD_PRIMARY 1
#define ISO9660_VD_TERMINATOR 255

typedef struct {
    uint8_t type;
    uint8_t identifier[5];
    uint8_t version;
    uint8_t data[2041];
} iso_volume_descriptor_t;

typedef struct {
    uint8_t  type;
    uint8_t  identifier[5];
    uint8_t  version;
    uint8_t  unused;
    uint8_t  system_id[32];
    uint8_t  volume_id[32];
    uint8_t  reserved[8];
    uint32_t volume_space_size_lsb;
    uint32_t volume_space_size_msb;
    uint8_t  reserved2[32];
    uint16_t volume_set_size_lsb;
    uint16_t volume_set_size_msb;
    uint16_t volume_seq_num_lsb;
    uint16_t volume_seq_num_msb;
    uint16_t logical_block_size_lsb;
    uint16_t logical_block_size_msb;
    uint32_t path_table_size_lsb;
    uint32_t path_table_size_msb;
    uint32_t path_table_lba_lsb;
    uint32_t opt_path_table_lba_lsb;
    uint32_t path_table_lba_msb;
    uint32_t opt_path_table_lba_msb;
    uint8_t  root_dir_record[34];
    uint8_t  volume_set_id[128];
    uint8_t  publisher_id[128];
    uint8_t  preparer_id[128];
    uint8_t  application_id[128];
    uint8_t  copyright_file[37];
    uint8_t  abstract_file[37];
    uint8_t  bibliographic_file[37];
    uint8_t  creation_date[17];
    uint8_t  modification_date[17];
    uint8_t  expiration_date[17];
    uint8_t  effective_date[17];
    uint8_t  file_struct_version;
    uint8_t  reserved3;
    uint8_t  application_use[512];
    uint8_t  reserved4[653];
} iso_primary_vd_t;

typedef struct {
    uint8_t  length;
    uint8_t  attr_len;
    uint32_t extent_lba_lsb;
    uint32_t extent_lba_msb;
    uint32_t data_len_lsb;
    uint32_t data_len_msb;
    uint8_t  recording_date[7];
    uint8_t  file_flags;
    uint8_t  file_unit_size;
    uint8_t  interleave_gap;
    uint16_t volume_seq_num_lsb;
    uint16_t volume_seq_num_msb;
    uint8_t  name_len;
    uint8_t  name[];
} iso_dir_record_t;

typedef struct {
    uint8_t  name_len;
    uint8_t  attr_len;
    uint32_t extent_lba;
    uint16_t parent_dir_num;
    uint8_t  name[];
} iso_path_table_entry_t;

typedef struct {
    uint32_t lba;
    uint32_t size;
    char name[256];
    bool is_dir;
    uint32_t parent_lba;
} iso_file_t;

typedef struct {
    void* dev_data;
    uint32_t (*read_sector)(void*, uint64_t, uint8_t*);
    uint64_t total_sectors;
    uint32_t root_lba;
    uint32_t path_table_lba;
    uint32_t block_size;
} iso9660_fs_t;

uint32_t iso9660_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors);
bool iso9660_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors, void** sb_out);
bool iso9660_umount(void* sb);

#endif
