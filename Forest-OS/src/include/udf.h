#ifndef UDF_H
#define UDF_H

#include "types.h"
#include "stdbool.h"

#define UDF_SECTOR_SIZE 2048

typedef struct {
    uint16_t id;
    uint16_t version;
    uint8_t  checksum;
    uint8_t  reserved;
    uint16_t serial_num;
    uint16_t crc;
    uint16_t crc_len;
    uint32_t location;
} udf_tag_t;

typedef struct {
    uint8_t  type;
    uint8_t  identifier[5];
    uint8_t  version;
    uint8_t  data[2041];
} udf_descriptor_t;

typedef struct {
    uint32_t length;
    uint32_t location;
} udf_extent_t;

typedef struct {
    udf_tag_t tag;
    uint32_t main_vds_extent_loc;
    uint32_t main_vds_extent_len;
    uint32_t reserve_vds_extent_loc;
    uint32_t reserve_vds_extent_len;
    uint8_t  reserved[480];
} udf_anchor_vdp_t;

typedef struct {
    void* dev_data;
    uint32_t (*read_sector)(void*, uint64_t, uint8_t*);
    uint64_t total_sectors;
    uint32_t block_size;
    uint32_t root_dir_lba;
} udf_fs_t;

uint32_t udf_probe(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors);
bool udf_mount(void* dev_data, uint32_t (*read_sector)(void*, uint64_t, uint8_t*), uint64_t total_sectors, void** sb_out);
bool udf_umount(void* sb);

#endif
