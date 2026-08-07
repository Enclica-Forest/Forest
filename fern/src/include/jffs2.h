#ifndef JFFS2_H
#define JFFS2_H

#include "types.h"
#include "stdbool.h"

#define JFFS2_MAGIC_SHORT 0x1985
#define JFFS2_MAGIC 0x2001

typedef struct {
    uint32_t magic;
    uint32_t nodetype;
    uint32_t hdr_crc;
    uint32_t totlen;
    uint32_t node_crc;
    uint32_t data_crc;
} jffs2_unknown_node_t;

typedef struct {
    jffs2_unknown_node_t hdr;
    uint32_t ino;
    uint32_t version;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t isize;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t offset;
    uint32_t csize;
    uint32_t dsize;
    uint8_t  compr;
    uint8_t  flags;
} jffs2_inode_node_t;

typedef struct {
    jffs2_unknown_node_t hdr;
    uint32_t ino;
    uint32_t pino;
    uint32_t version;
    uint8_t  namelen;
    uint8_t  type;
    uint8_t  name[];
} jffs2_dirent_node_t;

typedef struct jffs2_fs {
    void* dev_data;
    uint64_t (*read_page)(void*, uint64_t, uint8_t*);
    uint64_t total_size;
    uint32_t page_size;
    uint32_t oob_size;
} jffs2_fs_t;

uint32_t jffs2_probe(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), uint64_t total_size);
bool jffs2_mount(void* dev_data, uint64_t (*read_page)(void*, uint64_t, uint8_t*), uint64_t total_size, void** sb_out);
bool jffs2_umount(void* sb);

#endif
