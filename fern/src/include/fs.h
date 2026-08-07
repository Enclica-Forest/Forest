#ifndef FS_H
#define FS_H

#include "types.h"
#include "stdbool.h"
#include "vfs.h"

#define FS_SECTOR_SIZE 512

typedef uint32_t (*fs_readSector_fn)(void* dev_data, uint64_t lba, uint8_t* buffer);
typedef uint32_t (*fs_writeSector_fn)(void* dev_data, uint64_t lba, uint8_t* buffer);

typedef struct fs_superblock {
    void* dev_data;
    fs_readSector_fn read_sector;
    fs_writeSector_fn write_sector;
    uint64_t total_sectors;
    uint32_t sector_size;
    const char* fs_type;
    void* fs_data;
} fs_superblock_t;

typedef struct fs_inode {
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t nlink;
    void* fs_data;
} fs_inode_t;

typedef struct fs_dirent {
    char name[256];
    uint64_t ino;
} fs_dirent_t;

typedef struct fs_filesystem fs_filesystem_t;
typedef struct fs_mountpoint fs_mountpoint_t;

typedef struct {
    void* dev_data;
    fs_readSector_fn read_sector;
    fs_writeSector_fn write_sector;
    uint64_t total_sectors;
    uint32_t sector_size;
} fs_blockdev_t;

typedef struct {
    uint32_t (*probe)(fs_blockdev_t* dev);
    bool (*mount)(fs_blockdev_t* dev, fs_superblock_t** sb_out);
    bool (*umount)(fs_superblock_t* sb);
    vfs_node_t* (*get_root)(fs_superblock_t* sb);
    fs_inode_t* (*iget)(fs_superblock_t* sb, uint64_t ino);
    void (*iput)(fs_inode_t* inode);
    uint32_t (*iread)(fs_inode_t* inode, uint64_t offset, uint32_t size, uint8_t* buf);
    bool (*readdir)(fs_inode_t* dir, uint32_t index, fs_dirent_t* entry);
    fs_inode_t* (*finddir)(fs_inode_t* dir, const char* name);
} fs_ops_t;

struct fs_filesystem {
    const char* name;
    fs_ops_t ops;
    fs_filesystem_t* next;
};

struct fs_mountpoint {
    char device[256];
    char mountpoint[256];
    fs_superblock_t* sb;
    vfs_node_t* mount_point;
    fs_mountpoint_t* next;
};

void fs_init(void);
bool fs_register(fs_filesystem_t* fs);
bool fs_unregister(fs_filesystem_t* fs);
fs_superblock_t* fs_mount(const char* device, const char* mountpoint, const char* fstype);
bool fs_umount(const char* mountpoint);
fs_filesystem_t* fs_get_filesystem(const char* fstype);
fs_filesystem_t* fs_probe(const char* device);
fs_mountpoint_t* fs_get_mountpoints(void);

#endif
