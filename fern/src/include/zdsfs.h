#ifndef ZDSFS_H
#define ZDSFS_H

#include "types.h"
#include "stdbool.h"

#define ZDSFS_SECTOR_SIZE 512

typedef struct {
    uint8_t  cchhr[5];
    uint8_t  format;
    uint8_t  dsname[44];
    uint8_t  ds1ssrsq;
    uint8_t  ds1scall[4];
    uint8_t  ds1credt;
    uint8_t  ds1expdt;
    uint8_t  ds1noepv;
    uint8_t  ds1format;
    uint8_t  ds1refd[4];
    uint8_t  ds1sysp1[2];
    uint8_t  ds1sysp2[2];
    uint8_t  ds1recsz[2];
    uint8_t  ds1recfm;
    uint8_t  ds1optcd;
    uint8_t  ds1blksz[3];
    uint8_t  ds1lrecl[3];
    uint8_t  ds1keyl;
    uint8_t  ds1rkp;
    uint8_t  ds1dsg;
    uint8_t  ds1sms;
    uint8_t  ds1compr[35];
    uint8_t  ds1codf[2];
    uint8_t  ds1ext1[13];
    uint8_t  ds1ext2[13];
    uint8_t  ds1ext3[13];
    uint8_t  ds1dstag[13];
    uint8_t  ds1usrdi[6];
    uint8_t  ds1trbal[6];
} dscb1_t;

typedef struct zdsfs_fs {
    void* dev_data;
    uint32_t (*read_record)(void*, int*, int*, int*, void*, uint32_t);
    uint64_t total_cylinders;
    uint32_t total_heads;
    uint32_t total_records;
} zdsfs_fs_t;

uint32_t zdsfs_probe(void* dev_data, uint32_t (*read_record)(void*, int*, int*, int*, void*, uint32_t), uint64_t cylinders);
bool zdsfs_mount(void* dev_data, uint32_t (*read_record)(void*, int*, int*, int*, void*, uint32_t), uint64_t cylinders, void** sb_out);
bool zdsfs_umount(void* sb);

#endif
