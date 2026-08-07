#ifndef _LINUX_LOOP_H
#define _LINUX_LOOP_H

#include <sys/ioctl.h>
#include <stdint.h>

#define LOOP_SET_FD         0x4C00
#define LOOP_CLR_FD         0x4C01
#define LOOP_SET_STATUS     0x4C02
#define LOOP_GET_STATUS     0x4C03
#define LOOP_SET_STATUS64   0x4C04
#define LOOP_GET_STATUS64   0x4C05
#define LOOP_CTL_GET_FREE   0x4C82

#define LOOP_DEV_FMT        "/dev/loop%llu"

#define LO_FLAGS_READ_ONLY  1
#define LO_FLAGS_AUTOCLEAR  4

struct loop_info64 {
    uint64_t lo_device;
    uint64_t lo_inode;
    uint64_t lo_rdevice;
    uint64_t lo_offset;
    uint64_t lo_sizelimit;
    uint64_t lo_number;
    uint32_t lo_encrypt_type;
    uint32_t lo_encrypt_key_size;
    uint32_t lo_flags;
    uint8_t  lo_file_name[64];
    uint8_t  lo_crypt_name[64];
    uint8_t  lo_encrypt_key[32];
    uint64_t lo_init[2];
};

struct loop_config {
    uint32_t fd;
    uint32_t block_size;
    struct loop_info64 info;
    uint64_t __reserved[8];
};

#endif
