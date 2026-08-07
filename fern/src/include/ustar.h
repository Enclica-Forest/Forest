#ifndef USTAR_H
#define USTAR_H

#include "types.h"
#include "stdbool.h"

#define USTAR_BLOCK_SIZE 512
#define USTAR_MAGIC "ustar\0"

typedef struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} ustar_header_t;

typedef enum {
    USTAR_NORMAL = '0',
    USTAR_HARDLINK = '1',
    USTAR_SYMLINK = '2',
    USTAR_CHARDEV = '3',
    USTAR_BLOCKDEV = '4',
    USTAR_DIR = '5',
    USTAR_FIFO = '6'
} ustar_type_t;

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
    ustar_header_t* current_header;
    uint32_t file_count;
} ustar_fs_t;

#endif
