/*
 * mkfs.c - Forest OS mkfs implementation
 *
 * Creates filesystems on block devices.
 * Supports FAT32 and ext2 filesystem types.
 * Usage: mkfs [-t TYPE] [-b BLOCK_SIZE] [-i INODES] [-L LABEL] [-v] [-f] [-n INODES] device
 */
#define _DEFAULT_SOURCE
#include <forest.h>

static const char *prog = "mkfs";

static int flag_verbose = 0;
static int flag_force = 0;
static int flag_inodes_set = 0;

static int fs_type = 0;       /* 0=none, 1=FAT32, 2=EXT2 */
#define FS_FAT32 1
#define FS_EXT2  2

static uint32_t block_size = 512;
static uint32_t num_inodes = 128;
static char label[12] = "NO NAME";
static int label_len = 8;

/* --- Little-endian write helpers --- */

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/* --- MBR partition type codes --- */

#define MBR_TYPE_FAT32_LBA 0x0C
#define MBR_TYPE_LINUX     0x83

/* --- Sector/block read/write --- */

static int write_sector(int fd, uint32_t offset, const void *buf, size_t len) {
    ssize_t n;
    if (lseek(fd, offset, SEEK_SET) < 0) return -1;
    n = write(fd, buf, len);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int read_sector(int fd, uint32_t offset, void *buf, size_t len) {
    ssize_t n;
    if (lseek(fd, offset, SEEK_SET) < 0) return -1;
    n = read(fd, buf, len);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* --- MBR helpers --- */

static int write_mbr_partition_type(int fd, uint32_t total_sectors, uint8_t part_type) {
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));

    /* Read existing MBR if present */
    if (read_sector(fd, 0, mbr, 512) == 0) {
        /* Check for existing signature */
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
            /* Write new MBR signature */
            mbr[510] = 0x55;
            mbr[511] = 0xAA;
        }
    } else {
        memset(mbr, 0, sizeof(mbr));
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
    }

    /* Partition table entry 1 at offset 446 */
    uint8_t *entry = &mbr[446];

    /* Boot indicator: 0x80 = active (bootable) */
    entry[0] = 0x80;

    /* CHS of first sector: LBA mode (0xFE, 0xFF, 0xFF) */
    entry[1] = 0x00; /* head */
    entry[2] = 0x01; /* sector + cylinder high */
    entry[3] = 0x00; /* cylinder low */

    /* Partition type */
    entry[4] = part_type;

    /* CHS of last sector: LBA mode */
    entry[5] = 0xFE;
    entry[6] = 0xFF;
    entry[7] = 0xFF;

    /* LBA of first sector: 1 (skip MBR) */
    put_le32(&entry[8], 1);

    /* Number of sectors */
    put_le32(&entry[12], total_sectors > 1 ? total_sectors - 1 : 0);

    return write_sector(fd, 0, mbr, 512);
}

/* --- FAT32 filesystem creation --- */

/* FAT32 BPB (BIOS Parameter Block) structure */
#define FAT32_BPB_SIZE 90
#define FAT32_RESERVED_SECTORS 32
#define FAT32_NUM_FATS 2
#define FAT32_ROOT_CLUSTER 2
#define FAT32_SECTOR_SIZE 512

static int create_fat32(int fd, uint32_t total_sectors) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t sectors_per_cluster = 1;
    uint32_t total_clusters;
    uint32_t fat_size_sectors;
    uint32_t data_sectors;

    if (flag_verbose) {
        eprint2(prog, "creating FAT32 filesystem\n");
    }

    /* Calculate geometry */
    if (total_sectors < FAT32_RESERVED_SECTORS + 2) {
        eprint2(prog, "device too small for FAT32\n");
        return -1;
    }

    data_sectors = total_sectors - FAT32_RESERVED_SECTORS;

    /* Determine sectors per cluster based on volume size */
    if (data_sectors > 67108864) /* > 32GB */
        sectors_per_cluster = 64;
    else if (data_sectors > 33554432) /* > 16GB */
        sectors_per_cluster = 32;
    else if (data_sectors > 16777216) /* > 8GB */
        sectors_per_cluster = 16;
    else if (data_sectors > 8388608) /* > 4GB */
        sectors_per_cluster = 8;
    else if (data_sectors > 2097152) /* > 1GB */
        sectors_per_cluster = 4;
    else
        sectors_per_cluster = 1;

    total_clusters = data_sectors / sectors_per_cluster;
    if (total_clusters < 65525) {
        eprint2(prog, "device too small for FAT32 (need 65525 clusters)\n");
        return -1;
    }

    /* Each FAT entry is 4 bytes, 128 entries per sector */
    fat_size_sectors = (total_clusters + 127) / 128;
    if (fat_size_sectors < 1) fat_size_sectors = 1;

    /* Build Volume ID from time */
    uint32_t volume_id = 0x12345678;

    /* Write boot sector (VBR) at sector 0 */
    memset(sector, 0, FAT32_SECTOR_SIZE);

    /* Jump instruction */
    sector[0] = 0xEB;
    sector[1] = 0x58; /* jump to BPB */
    sector[2] = 0x90; /* NOP */

    /* OEM name */
    memcpy(&sector[3], "FOREST  ", 8);

    /* BPB starts at offset 11 */
    uint8_t *bpb = &sector[11];

    /* Bytes per sector */
    put_le16(&bpb[0], FAT32_SECTOR_SIZE);
    /* Sectors per cluster */
    bpb[2] = (uint8_t)sectors_per_cluster;
    /* Reserved sectors */
    put_le16(&bpb[3], FAT32_RESERVED_SECTORS);
    /* Number of FATs */
    bpb[5] = FAT32_NUM_FATS;
    /* Root entry count (0 for FAT32) */
    put_le16(&bpb[6], 0);
    /* Total sectors 16 (0 if > 65535) */
    put_le16(&bpb[8], 0);
    /* Media type: 0xF8 = fixed disk */
    bpb[10] = 0xF8;
    /* FAT size 16 (0 for FAT32) */
    put_le16(&bpb[11], 0);
    /* Sectors per track */
    put_le16(&bpb[13], FAT32_SECTOR_SIZE);
    /* Number of heads */
    put_le16(&bpb[15], 2);
    /* Hidden sectors */
    put_le32(&bpb[17], 0);
    /* Total sectors 32 */
    put_le32(&bpb[21], total_sectors);

    /* FAT32 extended BPB (offset 32 from sector start, relative to bpb = offset 24) */
    put_le32(&bpb[25], fat_size_sectors);   /* FAT size 32 */
    put_le16(&bpb[29], 0);                  /* Extended flags */
    put_le16(&bpb[31], 0);                  /* FS version */
    put_le32(&bpb[33], FAT32_ROOT_CLUSTER); /* Root cluster */
    put_le16(&bpb[37], 1);                  /* FSInfo sector */
    put_le16(&bpb[39], 0);                  /* Backup boot sector */

    /* Drive number */
    bpb[40] = 0x80; /* Hard disk */
    /* Reserved */
    bpb[41] = 0;
    /* Boot signature */
    bpb[42] = 0x29;
    /* Volume ID */
    put_le32(&bpb[43], volume_id);
    /* Volume label */
    memcpy(&bpb[47], label, 11);
    /* File system type */
    memcpy(&bpb[58], "FAT32   ", 8);

    /* Boot signature at 510-511 */
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (write_sector(fd, 0, sector, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write FAT32 boot sector\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "wrote FAT32 boot sector\n");
    }

    /* Write FSInfo sector at sector 1 */
    memset(sector, 0, FAT32_SECTOR_SIZE);
    put_le32(&sector[0], 0x41615252);  /* Lead signature */
    put_le32(&sector[484], 0x61417272); /* Structure signature */
    put_le32(&sector[488], 0xFFFFFFFF); /* Free cluster count */
    put_le32(&sector[492], FAT32_ROOT_CLUSTER); /* Next free cluster hint */
    put_le16(&sector[508], 0xAA55);    /* End signature */

    if (write_sector(fd, FAT32_SECTOR_SIZE, sector, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write FAT32 FSInfo sector\n");
        return -1;
    }

    /* Initialize FAT tables */
    uint8_t fat_cluster[FAT32_SECTOR_SIZE];
    memset(fat_cluster, 0, FAT32_SECTOR_SIZE);

    /* FAT entry 0: media type */
    put_le32(&fat_cluster[0], 0x0FFFFFF8);
    /* FAT entry 1: end of chain marker */
    put_le32(&fat_cluster[4], 0x0FFFFFFF);
    /* FAT entry 2: root directory (end of chain) */
    put_le32(&fat_cluster[8], 0x0FFFFFFF);

    uint32_t fat_offset = FAT32_RESERVED_SECTORS;
    for (int fat = 0; fat < FAT32_NUM_FATS; fat++) {
        for (uint32_t s = 0; s < fat_size_sectors; s++) {
            if (s == 0) {
                if (write_sector(fd, (fat_offset + s) * FAT32_SECTOR_SIZE,
                                 fat_cluster, FAT32_SECTOR_SIZE) < 0) {
                    eprint2(prog, "failed to write FAT\n");
                    return -1;
                }
            } else {
                /* Zero remaining FAT sectors */
                memset(fat_cluster, 0, FAT32_SECTOR_SIZE);
                if (write_sector(fd, (fat_offset + s) * FAT32_SECTOR_SIZE,
                                 fat_cluster, FAT32_SECTOR_SIZE) < 0) {
                    eprint2(prog, "failed to write FAT\n");
                    return -1;
                }
            }
        }
        fat_offset += fat_size_sectors;
    }

    if (flag_verbose) {
        eprint2(prog, "initialized FAT32 FAT tables\n");
    }

    /* Write root directory (cluster 2) - zeroed out */
    memset(sector, 0, FAT32_SECTOR_SIZE);
    uint32_t root_offset = (FAT32_RESERVED_SECTORS + FAT32_NUM_FATS * fat_size_sectors);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (write_sector(fd, (root_offset + s) * FAT32_SECTOR_SIZE,
                         sector, FAT32_SECTOR_SIZE) < 0) {
            eprint2(prog, "failed to write FAT32 root directory\n");
            return -1;
        }
    }

    if (flag_verbose) {
        eprint2(prog, "initialized FAT32 root directory\n");
    }

    /* Write MBR partition type */
    if (write_mbr_partition_type(fd, total_sectors, MBR_TYPE_FAT32_LBA) < 0) {
        eprint2(prog, "failed to write MBR partition type\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "FAT32 filesystem created successfully\n");
    }

    return 0;
}

/* --- Ext2 filesystem creation --- */

#define EXT2_SUPER_OFFSET 1024
#define EXT2_BLOCK_SIZE_DEFAULT 1024
#define EXT2_INODE_SIZE_DEFAULT 128
#define EXT2_ROOT_INO 2
#define EXT2_GOOD_OLD_INODE_SIZE 128

/* Ext2 superblock magic */
#define EXT2_SUPER_MAGIC 0xEF53

/* Ext2 inode mode values */
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IRWXU  0x01C0
#define EXT2_S_IRWXG  0x0038
#define EXT2_S_IRWXO  0x0007

/* Ext2 directory entry file types */
#define EXT2_FT_DIR 2

static int create_ext2(int fd, uint32_t total_sectors) {
    uint32_t total_blocks = total_sectors; /* assuming 512-byte sectors, blocks = sectors for simplicity */
    uint32_t block_size = EXT2_BLOCK_SIZE_DEFAULT;
    uint32_t blocks_per_group = 8192;
    uint32_t inodes_per_group = 1024;
    uint32_t inode_size = EXT2_INODE_SIZE_DEFAULT;
    uint32_t num_groups;
    uint32_t group_desc_blocks;
    uint32_t inode_table_blocks;
    uint32_t block_bitmap_blocks;
    uint32_t inode_bitmap_blocks;
    uint32_t overhead;
    uint32_t first_data_block;
    uint8_t superblock_buf[1024];
    uint8_t group_desc_buf[1024];
    uint8_t bitmap_buf[1024];
    uint8_t inode_buf[128];
    uint8_t dir_buf[1024];

    if (flag_verbose) {
        eprint2(prog, "creating ext2 filesystem\n");
    }

    if (total_blocks < 16) {
        eprint2(prog, "device too small for ext2\n");
        return -1;
    }

    /* Calculate number of groups */
    num_groups = (total_blocks + blocks_per_group - 1) / blocks_per_group;
    if (num_groups < 1) num_groups = 1;

    /* Overhead per group: 1 block bitmap + 1 inode bitmap + inode table blocks */
    block_bitmap_blocks = 1;
    inode_bitmap_blocks = 1;
    inode_table_blocks = (inodes_per_group * inode_size + block_size - 1) / block_size;

    overhead = 2; /* superblock + group descriptors */
    first_data_block = 1; /* for 1K block size, superblock is at block 1 */

    group_desc_blocks = (sizeof(void *) * num_groups + block_size - 1) / block_size;
    if (group_desc_blocks < 1) group_desc_blocks = 1;

    /* --- Write superblock --- */
    memset(superblock_buf, 0, sizeof(superblock_buf));
    uint8_t *sb = superblock_buf;

    put_le32(&sb[0], total_blocks);           /* s_inodes_count */
    put_le32(&sb[4], total_blocks);           /* s_blocks_count */
    put_le32(&sb[8], 0);                      /* s_r_blocks_count */
    put_le32(&sb[12], total_blocks - overhead -
             num_groups * (block_bitmap_blocks + inode_bitmap_blocks +
                           inode_table_blocks)); /* s_free_blocks_count */
    put_le32(&sb[16], inodes_per_group * num_groups - 10); /* s_free_inodes_count (reserve 10) */
    put_le32(&sb[20], first_data_block);      /* s_first_data_block */
    put_le32(&sb[24], block_size);            /* s_log_block_size (shift) */
    put_le32(&sb[28], 0);                     /* s_log_frag_size */
    put_le32(&sb[32], blocks_per_group);      /* s_blocks_per_group */
    put_le32(&sb[36], 0);                     /* s_frags_per_group */
    put_le32(&sb[40], inodes_per_group);      /* s_inodes_per_group */
    put_le32(&sb[44], 0);                     /* s_mtime */
    put_le32(&sb[48], 0);                     /* s_wtime */
    put_le16(&sb[52], 1);                     /* s_mnt_count */
    put_le16(&sb[54], 0xFFFF);                   /* s_max_mnt_count */
    put_le16(&sb[56], EXT2_SUPER_MAGIC);      /* s_magic */
    put_le16(&sb[58], 1);                     /* s_state */
    put_le16(&sb[60], 1);                     /* s_errors */
    put_le16(&sb[62], 0);                     /* s_minor_rev_level */
    put_le32(&sb[64], 0);                     /* s_lastcheck */
    put_le32(&sb[68], 0);                     /* s_checkinterval */
    put_le32(&sb[72], 0);                     /* s_creator_os */
    put_le32(&sb[76], 0);                     /* s_rev_level */
    put_le16(&sb[80], 0);                     /* s_def_resuid */
    put_le16(&sb[82], 0);                     /* s_def_resgid */

    /* Extended superblock fields */
    put_le32(&sb[84], 11);                    /* s_first_ino */
    put_le32(&sb[88], inode_size);            /* s_inode_size */
    put_le16(&sb[92], 0);                     /* s_block_group_nr */
    put_le32(&sb[96], 0);                     /* s_feature_compat */
    put_le32(&sb[100], 0);                    /* s_feature_incompat */
    put_le32(&sb[104], 0);                    /* s_feature_ro_compat */

    memcpy(&sb[108], "forest", 6);           /* s_uuid */

    memcpy(&sb[120], label, label_len);      /* s_volume_name */
    sb[131] = '\0';                           /* ensure null termination */

    put_le32(&sb[132], 0);                    /* s_last_mounted */
    put_le32(&sb[136], 0);                    /* s_algo_bitmap */

    if (write_sector(fd, EXT2_SUPER_OFFSET, superblock_buf, 1024) < 0) {
        eprint2(prog, "failed to write ext2 superblock\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "wrote ext2 superblock\n");
    }

    /* --- Write group descriptors --- */
    for (uint32_t g = 0; g < num_groups; g++) {
        memset(group_desc_buf, 0, sizeof(group_desc_buf));
        uint32_t gd_offset = g * 32;

        /* Calculate free blocks in this group */
        uint32_t group_blocks = blocks_per_group;
        if (g == num_groups - 1)
            group_blocks = total_blocks - g * blocks_per_group;
        uint32_t used = overhead + block_bitmap_blocks + inode_bitmap_blocks + inode_table_blocks;
        if (used >= group_blocks) used = group_blocks;
        uint32_t free_blks = group_blocks - used;

        put_le32(&group_desc_buf[gd_offset + 0], g * blocks_per_group + overhead +
                  block_bitmap_blocks + inode_bitmap_blocks + inode_table_blocks);
        put_le32(&group_desc_buf[gd_offset + 4], overhead + g * blocks_per_group);
        put_le32(&group_desc_buf[gd_offset + 8], overhead + g * blocks_per_group + 1);
        put_le32(&group_desc_buf[gd_offset + 12],
                  overhead + g * blocks_per_group + 2);
        put_le32(&group_desc_buf[gd_offset + 16], free_blks);
        put_le32(&group_desc_buf[gd_offset + 20], inodes_per_group);
        put_le32(&group_desc_buf[gd_offset + 24], 0); /* used_dirs_count */
    }

    uint32_t gd_sector = EXT2_SUPER_OFFSET / FAT32_SECTOR_SIZE + 1;
    if (write_sector(fd, gd_sector * FAT32_SECTOR_SIZE,
                     group_desc_buf, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write ext2 group descriptors\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "wrote ext2 group descriptors\n");
    }

    /* --- Write block bitmap for group 0 --- */
    memset(bitmap_buf, 0, sizeof(bitmap_buf));
    /* Mark blocks 0 (MBR), 1 (superblock), 2 (group desc) as used */
    bitmap_buf[0] = 0x07; /* bits 0,1,2 set */
    uint32_t block_bitmap_sec = gd_sector + 1;
    if (write_sector(fd, block_bitmap_sec * FAT32_SECTOR_SIZE,
                     bitmap_buf, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write ext2 block bitmap\n");
        return -1;
    }

    /* --- Write inode bitmap for group 0 --- */
    memset(bitmap_buf, 0, sizeof(bitmap_buf));
    /* Inodes 1 and 2 are used (reserved + root) */
    bitmap_buf[0] = 0x03;
    uint32_t inode_bitmap_sec = block_bitmap_sec + 1;
    if (write_sector(fd, inode_bitmap_sec * FAT32_SECTOR_SIZE,
                     bitmap_buf, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write ext2 inode bitmap\n");
        return -1;
    }

    /* --- Write inode table for group 0 (root inode) --- */
    uint32_t inode_table_sec = inode_bitmap_sec + 1;
    memset(inode_buf, 0, sizeof(inode_buf));

    /* Root inode (inode 2) */
    put_le16(&inode_buf[0], EXT2_S_IFDIR | EXT2_S_IRWXU | EXT2_S_IRWXG | EXT2_S_IRWXO); /* mode */
    put_le16(&inode_buf[2], 0);           /* uid */
    put_le32(&inode_buf[4], 0);           /* size_lo */
    put_le32(&inode_buf[8], 0);           /* atime */
    put_le32(&inode_buf[12], 0);          /* ctime */
    put_le32(&inode_buf[16], 0);          /* mtime */
    put_le32(&inode_buf[20], 0);          /* dtime */
    put_le16(&inode_buf[24], 0);          /* gid */
    put_le16(&inode_buf[26], 0);          /* links_count */
    put_le32(&inode_buf[28], 1024);       /* blocks */
    put_le32(&inode_buf[32], 0);          /* flags */

    /* Block pointers: block 0 contains root directory entries */
    put_le32(&inode_buf[40], overhead + block_bitmap_blocks + inode_bitmap_blocks + inode_table_blocks);

    if (write_sector(fd, inode_table_sec * FAT32_SECTOR_SIZE,
                     inode_buf, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write ext2 inode table\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "wrote ext2 inode table\n");
    }

    /* --- Write root directory --- */
    memset(dir_buf, 0, sizeof(dir_buf));
    uint32_t dir_offset = 0;

    /* Entry "." (inode 2) */
    put_le32(&dir_buf[dir_offset + 0], EXT2_ROOT_INO);
    put_le16(&dir_buf[dir_offset + 4], 12);  /* rec_len */
    dir_buf[dir_offset + 6] = 1;            /* name_len */
    dir_buf[dir_offset + 7] = EXT2_FT_DIR;  /* file_type */
    dir_buf[dir_offset + 8] = '.';           /* name */
    dir_offset += 12;

    /* Entry ".." (inode 2, root) */
    put_le32(&dir_buf[dir_offset + 0], EXT2_ROOT_INO);
    put_le16(&dir_buf[dir_offset + 4], 1012); /* rec_len (rest of block) */
    dir_buf[dir_offset + 6] = 2;            /* name_len */
    dir_buf[dir_offset + 7] = EXT2_FT_DIR;  /* file_type */
    dir_buf[dir_offset + 8] = '.';
    dir_buf[dir_offset + 9] = '.';

    uint32_t root_dir_block = overhead + block_bitmap_blocks + inode_bitmap_blocks + inode_table_blocks;
    uint32_t root_dir_sec = root_dir_block; /* assuming 512-byte sectors for simplicity */
    if (write_sector(fd, root_dir_sec * FAT32_SECTOR_SIZE,
                     dir_buf, FAT32_SECTOR_SIZE) < 0) {
        eprint2(prog, "failed to write ext2 root directory\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "initialized ext2 root directory\n");
    }

    /* Write MBR partition type */
    if (write_mbr_partition_type(fd, total_sectors, MBR_TYPE_LINUX) < 0) {
        eprint2(prog, "failed to write MBR partition type\n");
        return -1;
    }

    if (flag_verbose) {
        eprint2(prog, "ext2 filesystem created successfully\n");
    }

    return 0;
}

/* --- Usage and argument parsing --- */

static void usage(void) {
    eprint2(prog, "Usage: mkfs [-t fat32|ext2] [-b block_size] [-i inodes] [-L label] [-v] [-f] [-n inodes] device\n");
    _exit(EXIT_USAGE);
}

static int parse_type(const char *s) {
    if (strcmp(s, "fat32") == 0 || strcmp(s, "vfat") == 0 || strcmp(s, "FAT32") == 0)
        return FS_FAT32;
    if (strcmp(s, "ext2") == 0 || strcmp(s, "EXT2") == 0)
        return FS_EXT2;
    return 0;
}

int main(int argc, char *argv[]) {
    int opt;
    const char *device = NULL;

    while ((opt = getopt(argc, argv, "t:b:i:L:vf n:")) != -1) {
        switch (opt) {
            case 't':
                fs_type = parse_type(optarg);
                if (!fs_type) {
                    eprint2(prog, "unknown filesystem type '");
                    eprint(optarg);
                    eprint("'\n");
                    _exit(EXIT_USAGE);
                }
                break;
            case 'b':
                block_size = 0;
                {
                    const char *p = optarg;
                    while (*p) {
                        if (*p >= '0' && *p <= '9')
                            block_size = block_size * 10 + (*p - '0');
                        p++;
                    }
                }
                if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
                    eprint2(prog, "invalid block size\n");
                    _exit(EXIT_USAGE);
                }
                break;
            case 'i':
            case 'n':
                flag_inodes_set = 1;
                num_inodes = 0;
                {
                    const char *p = optarg;
                    while (*p) {
                        if (*p >= '0' && *p <= '9')
                            num_inodes = num_inodes * 10 + (*p - '0');
                        p++;
                    }
                }
                if (num_inodes == 0) {
                    eprint2(prog, "invalid number of inodes\n");
                    _exit(EXIT_USAGE);
                }
                break;
            case 'L':
                memset(label, ' ', sizeof(label));
                label_len = 0;
                {
                    const char *p = optarg;
                    while (*p && label_len < 11) {
                        label[label_len++] = *p++;
                    }
                }
                break;
            case 'v':
                flag_verbose = 1;
                break;
            case 'f':
                flag_force = 1;
                break;
            default:
                usage();
        }
    }

    if (optind >= argc)
        usage();
    device = argv[optind];

    if (!fs_type) {
        eprint2(prog, "filesystem type not specified (use -t)\n");
        _exit(EXIT_USAGE);
    }

    /* Get device size */
    struct stat st;
    if (stat(device, &st) < 0) {
        eprint2(prog, "cannot stat '");
        eprint(device);
        eprint("': ");
        eprint(strerror(errno));
        eprint("\n");
        _exit(EXIT_FAIL);
    }

    int is_block = S_ISBLK(st.st_mode);
    int is_reg = S_ISREG(st.st_mode);

    if (!is_block && !is_reg) {
        eprint2(prog, "'");
        eprint(device);
        eprint("' is not a block device or regular file\n");
        _exit(EXIT_FAIL);
    }

    uint32_t total_sectors = 0;
    if (is_reg) {
        total_sectors = (uint32_t)(st.st_size / 512);
    }

    int fd;
    if (flag_force)
        fd = open(device, O_RDWR);
    else
        fd = open(device, O_WRONLY);

    if (fd < 0) {
        eprint2(prog, "cannot open '");
        eprint(device);
        eprint("': ");
        eprint(strerror(errno));
        eprint("\n");
        _exit(EXIT_FAIL);
    }

    /* For block devices, get size after opening */
    if (is_block && total_sectors == 0) {
        off_t size = lseek(fd, 0, SEEK_END);
        if (size > 0) {
            total_sectors = (uint32_t)(size / 512);
        } else {
            total_sectors = 2048; /* default 1MB */
        }
        lseek(fd, 0, SEEK_SET);
    }

    if (total_sectors == 0) {
        eprint2(prog, "device size is zero\n");
        close(fd);
        _exit(EXIT_FAIL);
    }

    if (!flag_force) {
        /* Check if device already has a filesystem */
        uint8_t check[2];
        if (read_sector(fd, 0, check, 2) == 0) {
            if (check[0] == 0x55 && check[1] == 0xAA) {
                eprint2(prog, "'");
                eprint(device);
                eprint("' appears to contain a partition table\n");
                eprint2(prog, "use -f to force\n");
                close(fd);
                _exit(EXIT_FAIL);
            }
        }
    }

    int result;
    switch (fs_type) {
        case FS_FAT32:
            result = create_fat32(fd, total_sectors);
            break;
        case FS_EXT2:
            result = create_ext2(fd, total_sectors);
            break;
        default:
            eprint2(prog, "unsupported filesystem type\n");
            result = -1;
    }

    close(fd);

    if (result < 0) {
        eprint2(prog, "failed to create filesystem\n");
        _exit(EXIT_FAIL);
    }

    if (flag_verbose) {
        eprint2(prog, "filesystem creation complete\n");
    }

    _exit(EXIT_OK);
}
