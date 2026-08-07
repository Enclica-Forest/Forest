#ifndef FS_INTERNAL_H
#define FS_INTERNAL_H

#include "types.h"
#include "stdbool.h"

#define FS_NAME_MAX 64
#define FS_PATH_MAX 256

typedef struct {
    uint64_t lba;
    uint32_t size;
} fs_extent_t;

#define FS_MAX_EXTENTS 8

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t nlink;
    uint32_t blocks;
    uint64_t block_size;
} fs_stat_t;

static inline uint16_t fs_read16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint16_t fs_read16_be(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t fs_read32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t fs_read32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t fs_read64_le(const uint8_t* p) {
    return (uint64_t)fs_read32_le(p) | ((uint64_t)fs_read32_le(p + 4) << 32);
}

static inline uint64_t fs_read64_be(const uint8_t* p) {
    return ((uint64_t)fs_read32_be(p) << 32) | (uint64_t)fs_read32_be(p + 4);
}

static inline void fs_write16_le(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static inline void fs_write16_be(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

static inline void fs_write32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static inline void fs_write32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

static inline void fs_write64_le(uint8_t* p, uint64_t v) {
    fs_write32_le(p, v & 0xFFFFFFFF);
    fs_write32_le(p + 4, (v >> 32) & 0xFFFFFFFF);
}

static inline void fs_write64_be(uint8_t* p, uint64_t v) {
    fs_write32_be(p, (v >> 32) & 0xFFFFFFFF);
    fs_write32_be(p + 4, v & 0xFFFFFFFF);
}

uint32_t fs_octal_to_uint32(const uint8_t* str, int len);
uint64_t fs_octal_to_uint64(const uint8_t* str, int len);
void fs_uint32_to_octal(uint32_t v, uint8_t* out, int len);

#endif
