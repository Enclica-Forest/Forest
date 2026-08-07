/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/fs_ext.c - Read-only ext2/ext3/ext4 driver implementation
 * =============================================================================
 * Freestanding. Build with the same recipe as the rest of the UEFI loader:
 *
 *   clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
 *         -mno-red-zone -mno-mmx -mno-sse -std=c11 -I. -Iinclude \
 *         -c uefi/fs_ext.c -o uefi/fs_ext.o
 *   (aarch64: -target aarch64-unknown-windows -mgeneral-regs-only, drop -mno-mmx/-sse)
 *
 * ---------------------------------------------------------------------------
 * WHAT IS IMPLEMENTED
 *   - Superblock parse (magic 0xEF53), dynamic block size, 64-bit feature
 *     (INCOMPAT_64BIT) block-group-descriptor sizing, dynamic inode size.
 *   - Block group descriptor -> inode table location (32B or 64B descriptors).
 *   - Inode read (mode / size(64-bit) / flags / i_block[15]).
 *   - Directory walk over ext4_dir_entry_2 records; absolute path resolution.
 *   - File data mapping via BOTH:
 *        * classic indirect BLOCK MAP (direct / single / double / triple),
 *        * ext4 EXTENT TREE (ext4_extent_header/idx/extent), incl. index nodes.
 *
 * HONEST BOUNDARIES (read-only, best-effort recovery driver):
 *   - No journal replay: a dirty ext3/4 volume is read as-is (last committed
 *     metadata on disk). Fine for recovery/cat; not a substitute for fsck.
 *   - No htree hashed-directory acceleration: directories are scanned
 *     linearly (correct, just O(n)). The dir_index feature only speeds lookup;
 *     the linear blocks are always valid, so this is correct on all volumes.
 *   - No inline_data (EXT4_INLINE_DATA_FL) files: an inode with data stored in
 *     the i_block area returns EXT_ERR_CORRUPT rather than garbage. Rare.
 *   - No symlink following (fast or slow): symlink targets are not resolved;
 *     a path component that is a symlink fails with EXT_ERR_NOTDIR/NOTFOUND.
 *   - Extent/indirect recursion is depth-limited (EXT_MAX_DEPTH) to guard
 *     against corrupt trees; legal ext4 trees never exceed depth 5.
 *   - Bigalloc (clustered allocation) is not handled; standard 1:1 block:cluster
 *     volumes (the overwhelming default) work.
 * ========================================================================== */

#include "fs_ext.h"

/* =============================================================================
 * Tiny freestanding mem/str helpers (no libc).
 * ========================================================================== */
static void *x_memcpy(void *d, const void *s, uint64_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
    return d;
}
static void x_memset(void *d, int c, uint64_t n) {
    uint8_t *dd = (uint8_t *)d;
    while (n--) *dd++ = (uint8_t)c;
}
static int x_memcmp(const void *a, const void *b, uint64_t n) {
    const uint8_t *aa = (const uint8_t *)a, *bb = (const uint8_t *)b;
    while (n--) { if (*aa != *bb) return (int)*aa - (int)*bb; aa++; bb++; }
    return 0;
}
static uint64_t x_strlen(const char *s) { uint64_t n = 0; while (s[n]) n++; return n; }

/* Little-endian scalar reads from an unaligned byte pointer. */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* =============================================================================
 * On-disk constants.
 * ========================================================================== */
#define EXT_SUPER_OFFSET        1024ULL
#define EXT_SUPER_MAGIC         0xEF53
#define EXT_ROOT_INO            2

#define EXT_INCOMPAT_64BIT      0x0080  /* s_feature_incompat: 64-bit descriptors */
#define EXT_INODE_EXTENTS_FL    0x80000 /* i_flags: inode uses an extent tree     */
#define EXT_INODE_INLINE_FL     0x10000000 /* i_flags: inline data (unsupported)  */

#define EXT_EXTENT_MAGIC        0xF30A  /* ext4_extent_header.eh_magic            */

#define EXT_MAX_DEPTH           6       /* extent / indirect recursion guard      */

/* Superblock field byte offsets (relative to the 1024-byte superblock start). */
#define SB_INODES_COUNT         0
#define SB_BLOCKS_COUNT_LO      4
#define SB_FIRST_DATA_BLOCK     20
#define SB_LOG_BLOCK_SIZE       24
#define SB_BLOCKS_PER_GROUP     32
#define SB_INODES_PER_GROUP     40
#define SB_MAGIC                56
#define SB_REV_LEVEL            76
#define SB_FIRST_INO            84
#define SB_INODE_SIZE           88
#define SB_FEATURE_INCOMPAT     96
#define SB_VOLUME_NAME          120     /* 16 bytes */
#define SB_DESC_SIZE            254     /* u16, valid only when 64BIT feature set */

/* Inode field byte offsets (within the inode structure). */
#define INO_MODE                0
#define INO_SIZE_LO             4
#define INO_FLAGS               32
#define INO_BLOCK               40      /* 15 * 4 = 60 bytes (i_block[15])        */
#define INO_SIZE_HIGH           108     /* i_size_high / i_dir_acl               */

/* =============================================================================
 * Mount context.
 * ========================================================================== */
struct ext_ctx {
    EFI_BOOT_SERVICES     *bs;
    EFI_BLOCK_IO_PROTOCOL *bio;
    EFI_DISK_IO_PROTOCOL  *dio;    /* optional; NULL => BlockIo only */
    UINT32                 media_id;
    UINT32                 dev_bsize;   /* device sector size (e.g. 512)         */

    /* Geometry from the superblock. */
    uint32_t block_size;               /* fs block size (1024 << s_log_block_size) */
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t inodes_count;
    uint16_t desc_size;                /* 32 or 64                               */
    uint32_t feature_incompat;
    char     label[17];

    uint8_t *dsect;                    /* one device-sector scratch              */
    uint8_t *blkbuf;                   /* one fs-block scratch (path/dir walk)    */
    uint8_t *auxbuf;                   /* second fs-block scratch (extent/indir)  */

    /* Fixed per-depth scratch pool for extent_lookup()'s index-node descent,
     * one fs-block buffer per recursion level. Allocated once in ext_mount(),
     * freed once in ext_unmount(). Safe to reuse per depth because the walk is
     * strictly depth-first: a node's matching child is chosen (and the node's
     * bytes are done being read) before recursing into that child. */
    uint8_t *extbuf[EXT_MAX_DEPTH + 1];

    /* Single-entry cache for indirect/extent-index block reads (sequential IO). */
    uint64_t cache_block;              /* fs block number cached in auxbuf, ~0=none */

    /* Group-descriptor cache: the last block group read in read_inode plus its
     * descriptor bytes. Consecutive inodes in the same group (the common case
     * for files listed/opened within one directory) reuse this without IO. */
    uint32_t last_group;               /* ~0 = none cached                        */
    uint8_t  last_gd[64];              /* descriptor bytes (32 or 64)             */

    /* Last extent matched by extent_lookup(): lets map_block() answer the next
     * (usually sequential) logical block without re-walking the extent tree.
     * Invalidated whenever a new inode is loaded (read_inode). */
    int      ext_valid;                /* 0 = nothing cached                      */
    uint32_t ext_lo;                   /* first logical block covered             */
    uint32_t ext_hi;                   /* one past last logical block covered     */
    uint64_t ext_phys;                 /* physical block backing ext_lo           */
};

/* =============================================================================
 * Raw device read: arbitrary byte offset/length via ReadBlocks (or ReadDisk).
 * Sector-at-a-time through the ctx scratch sector; correctness over speed.
 * ========================================================================== */
static int dev_read(ext_ctx *c, uint64_t off, void *buf, uint64_t len) {
    /* Fast path: DiskIo does the byte math for us. */
    if (c->dio) {
        EFI_STATUS st = c->dio->ReadDisk(c->dio, c->media_id, off, len, buf);
        return EFI_ERROR(st) ? EXT_ERR_IO : EXT_OK;
    }
    uint32_t bs = c->dev_bsize;
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint64_t lba    = off / bs;
        uint32_t within = (uint32_t)(off % bs);
        /* Aligned bulk read when we can: whole sectors straight into `out`. */
        if (within == 0 && len >= bs) {
            uint64_t nsec = len / bs;
            uint64_t nby  = nsec * bs;
            EFI_STATUS st = c->bio->ReadBlocks(c->bio, c->media_id, lba, nby, out);
            if (EFI_ERROR(st)) return EXT_ERR_IO;
            out += nby; off += nby; len -= nby;
            continue;
        }
        EFI_STATUS st = c->bio->ReadBlocks(c->bio, c->media_id, lba, bs, c->dsect);
        if (EFI_ERROR(st)) return EXT_ERR_IO;
        uint32_t chunk = bs - within;
        if (chunk > len) chunk = (uint32_t)len;
        x_memcpy(out, c->dsect + within, chunk);
        out += chunk; off += chunk; len -= chunk;
    }
    return EXT_OK;
}

/* Read a whole fs block into `buf` (must be >= block_size). */
static int read_fsblock(ext_ctx *c, uint64_t block, void *buf) {
    return dev_read(c, block * (uint64_t)c->block_size, buf, c->block_size);
}

/* Cached read of one fs block into c->auxbuf (for indirect/index scans). */
static int read_fsblock_cached(ext_ctx *c, uint64_t block) {
    if (c->cache_block == block) return EXT_OK;
    int r = read_fsblock(c, block, c->auxbuf);
    if (r != EXT_OK) { c->cache_block = ~0ULL; return r; }
    c->cache_block = block;
    return EXT_OK;
}

/* =============================================================================
 * Inode read.
 * ========================================================================== */
typedef struct {
    uint16_t mode;
    uint32_t flags;
    uint64_t size;
    uint8_t  block[60];   /* raw i_block[15] */
} ext_inode;

#define EXT_S_IFMT   0xF000
#define EXT_S_IFDIR  0x4000
#define EXT_S_IFREG  0x8000
#define EXT_S_IFLNK  0xA000

static int read_inode(ext_ctx *c, uint32_t ino, ext_inode *out) {
    if (ino == 0 || ino > c->inodes_count) return EXT_ERR_CORRUPT;

    /* Loading a new inode retires the previous inode's extent-range cache. */
    c->ext_valid = 0;

    uint32_t group = (ino - 1) / c->inodes_per_group;
    uint32_t index = (ino - 1) % c->inodes_per_group;

    /* Group descriptor table starts at the block after the first data block. */
    uint64_t gdt_block = (uint64_t)c->first_data_block + 1;
    uint64_t gd_off = gdt_block * (uint64_t)c->block_size +
                      (uint64_t)group * c->desc_size;

    /* Reuse the cached descriptor when we're still in the same block group. */
    if (group != c->last_group) {
        uint32_t gdlen = c->desc_size < sizeof(c->last_gd)
                             ? c->desc_size : (uint32_t)sizeof(c->last_gd);
        if (dev_read(c, gd_off, c->last_gd, gdlen) != EXT_OK) return EXT_ERR_IO;
        c->last_group = group;
    }
    uint8_t *gd = c->last_gd;

    /* bg_inode_table_lo @ +8; hi @ +40 when 64-bit descriptors in use. */
    uint64_t itable = le32(gd + 8);
    if (c->desc_size > 32 && (c->feature_incompat & EXT_INCOMPAT_64BIT))
        itable |= (uint64_t)le32(gd + 40) << 32;

    uint64_t ino_off = itable * (uint64_t)c->block_size +
                       (uint64_t)index * c->inode_size;

    uint8_t raw[256];
    uint32_t want = c->inode_size < sizeof(raw) ? c->inode_size : sizeof(raw);
    if (want < 128) want = 128;
    if (dev_read(c, ino_off, raw, want) != EXT_OK) return EXT_ERR_IO;

    out->mode  = le16(raw + INO_MODE);
    out->flags = le32(raw + INO_FLAGS);
    out->size  = (uint64_t)le32(raw + INO_SIZE_LO);
    /* i_size_high applies to regular files (dir sizes stay in the low word). */
    if ((out->mode & EXT_S_IFMT) == EXT_S_IFREG)
        out->size |= (uint64_t)le32(raw + INO_SIZE_HIGH) << 32;
    x_memcpy(out->block, raw + INO_BLOCK, 60);
    return EXT_OK;
}

/* =============================================================================
 * Logical-block -> physical-block mapping.
 *
 * Returns the physical fs block for `lblk` (0 == sparse hole / none), or a
 * negative EXT_ERR_* on IO/corruption. Two paths:
 *   (a) extent tree  (EXT4_EXTENTS_FL): descend eh_depth index levels.
 *   (b) block map    (ext2/3):          direct/single/double/triple indirect.
 * ========================================================================== */

/* Extent-tree lookup. `node` points at an ext4_extent_header; `avail` is the
 * bytes available in that node buffer. Recurses through index nodes. */
static int64_t extent_lookup(ext_ctx *c, const uint8_t *node, uint32_t avail,
                             uint32_t lblk, int depth) {
    if (depth > EXT_MAX_DEPTH) return EXT_ERR_CORRUPT;
    if (avail < 12 || le16(node) != EXT_EXTENT_MAGIC) return EXT_ERR_CORRUPT;

    uint16_t entries = le16(node + 2);
    uint16_t edepth  = le16(node + 6);

    /* Bound entries by the buffer we actually have (12-byte header + entries). */
    uint32_t max_by_buf = (avail - 12) / 12;
    if (entries > max_by_buf) entries = (uint16_t)max_by_buf;

    if (edepth == 0) {
        /* Leaf: ext4_extent { ee_block(4), ee_len(2), ee_start_hi(2), ee_start_lo(4) } */
        for (uint16_t i = 0; i < entries; i++) {
            const uint8_t *e = node + 12 + i * 12;
            uint32_t ee_block = le32(e);
            uint16_t ee_len   = le16(e + 4);
            uint16_t hi       = le16(e + 6);
            uint32_t lo       = le32(e + 8);
            uint32_t len = ee_len > 32768 ? (uint32_t)(ee_len - 32768) : ee_len; /* uninit */
            if (lblk >= ee_block && lblk < ee_block + len) {
                uint64_t start = ((uint64_t)hi << 32) | lo;
                /* Cache the covering extent so map_block() can answer the next
                 * sequential blocks of this inode without re-walking the tree. */
                c->ext_lo    = ee_block;
                c->ext_hi    = ee_block + len;
                c->ext_phys  = start;
                c->ext_valid = 1;
                return (int64_t)(start + (lblk - ee_block));
            }
        }
        return 0; /* not covered => hole */
    }

    /* Index node: ext4_extent_idx { ei_block(4), ei_leaf_lo(4), ei_leaf_hi(2) }.
     * Pick the last child whose ei_block <= lblk. */
    uint64_t child = 0; int found = 0;
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *x = node + 12 + i * 12;
        uint32_t ei_block = le32(x);
        if (ei_block <= lblk) {
            child = (uint64_t)le32(x + 4) | ((uint64_t)le16(x + 8) << 32);
            found = 1;
        } else break;
    }
    if (!found) return 0;

    /* Read the child node into this depth's fixed scratch buffer (one fs
     * block). The walk is depth-first, so the buffer for `depth` is free to
     * reuse for the next sibling at that depth once this call returns; no
     * per-call AllocatePool/FreePool needed. Legal depth is <=5. */
    uint8_t *cbuf = c->extbuf[depth];
    if (!cbuf) return EXT_ERR_NOMEM;
    int r = read_fsblock(c, child, cbuf);
    if (r != EXT_OK) return r;
    return extent_lookup(c, cbuf, c->block_size, lblk, depth + 1);
}

/* Classic block-map lookup for one logical block. */
static int64_t blockmap_lookup(ext_ctx *c, const ext_inode *ino, uint32_t lblk) {
    const uint32_t per = c->block_size / 4;   /* entries per indirect block */

    /* 12 direct entries. */
    if (lblk < 12)
        return (int64_t)le32(ino->block + lblk * 4);

    lblk -= 12;

    /* Single indirect. */
    if (lblk < per) {
        uint32_t ind = le32(ino->block + 12 * 4);
        if (!ind) return 0;
        if (read_fsblock_cached(c, ind) != EXT_OK) return EXT_ERR_IO;
        return (int64_t)le32(c->auxbuf + lblk * 4);
    }
    lblk -= per;

    /* Double indirect. */
    if (lblk < per * per) {
        uint32_t dind = le32(ino->block + 13 * 4);
        if (!dind) return 0;
        uint32_t l1 = lblk / per, l2 = lblk % per;
        if (read_fsblock_cached(c, dind) != EXT_OK) return EXT_ERR_IO;
        uint32_t ind = le32(c->auxbuf + l1 * 4);
        if (!ind) return 0;
        if (read_fsblock_cached(c, ind) != EXT_OK) return EXT_ERR_IO;
        return (int64_t)le32(c->auxbuf + l2 * 4);
    }
    lblk -= per * per;

    /* Triple indirect. */
    if (lblk < per * per * per) {
        uint32_t tind = le32(ino->block + 14 * 4);
        if (!tind) return 0;
        uint32_t l1 = lblk / (per * per);
        uint32_t rem = lblk % (per * per);
        uint32_t l2 = rem / per, l3 = rem % per;
        if (read_fsblock_cached(c, tind) != EXT_OK) return EXT_ERR_IO;
        uint32_t dind = le32(c->auxbuf + l1 * 4);
        if (!dind) return 0;
        if (read_fsblock_cached(c, dind) != EXT_OK) return EXT_ERR_IO;
        uint32_t ind = le32(c->auxbuf + l2 * 4);
        if (!ind) return 0;
        if (read_fsblock_cached(c, ind) != EXT_OK) return EXT_ERR_IO;
        return (int64_t)le32(c->auxbuf + l3 * 4);
    }
    return EXT_ERR_CORRUPT; /* beyond triple-indirect reach */
}

static int64_t map_block(ext_ctx *c, const ext_inode *ino, uint32_t lblk) {
    if (ino->flags & EXT_INODE_EXTENTS_FL) {
        /* Fast path: the requested block falls inside the last matched extent
         * (invalidated by read_inode, so this can only refer to `ino`). */
        if (c->ext_valid && lblk >= c->ext_lo && lblk < c->ext_hi)
            return (int64_t)(c->ext_phys + (lblk - c->ext_lo));
        return extent_lookup(c, ino->block, 60, lblk, 0);
    }
    return blockmap_lookup(c, ino, lblk);
}

/* =============================================================================
 * Directory walk + path resolution.
 * ========================================================================== */

/* Look up `name` (length nlen) in directory inode `dir`; return child inode
 * number in *out_ino (0 if not found). */
static int dir_lookup(ext_ctx *c, const ext_inode *dir,
                      const char *name, uint32_t nlen, uint32_t *out_ino) {
    *out_ino = 0;
    if ((dir->mode & EXT_S_IFMT) != EXT_S_IFDIR) return EXT_ERR_NOTDIR;

    uint64_t nblocks = (dir->size + c->block_size - 1) / c->block_size;
    for (uint64_t b = 0; b < nblocks; b++) {
        int64_t phys = map_block(c, dir, (uint32_t)b);
        if (phys < 0) return (int)phys;
        if (phys == 0) continue; /* hole */
        if (read_fsblock(c, (uint64_t)phys, c->blkbuf) != EXT_OK) return EXT_ERR_IO;

        uint32_t off = 0;
        while (off + 8 <= c->block_size) {
            uint8_t *de = c->blkbuf + off;
            uint32_t e_ino = le32(de);
            uint16_t rec   = le16(de + 4);
            uint8_t  namel = de[6];
            if (rec < 8) break; /* corrupt / end */
            if (e_ino != 0 && namel == nlen &&
                x_memcmp(de + 8, name, nlen) == 0) {
                *out_ino = e_ino;
                return EXT_OK;
            }
            off += rec;
        }
    }
    return EXT_OK; /* not found, *out_ino stays 0 */
}

/* Resolve an absolute path to an inode number (+ optionally its inode). */
static int resolve_path(ext_ctx *c, const char *path, uint32_t *out_ino,
                        ext_inode *out_inode) {
    ext_inode cur;
    if (read_inode(c, EXT_ROOT_INO, &cur) != EXT_OK) return EXT_ERR_IO;
    uint32_t cur_ino = EXT_ROOT_INO;

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t nlen = (uint32_t)(p - start);
        if (nlen == 1 && start[0] == '.') continue;

        uint32_t child;
        int r = dir_lookup(c, &cur, start, nlen, &child);
        if (r != EXT_OK) return r;
        if (child == 0) return EXT_ERR_NOTFOUND;
        if (read_inode(c, child, &cur) != EXT_OK) return EXT_ERR_IO;
        cur_ino = child;
    }

    if (out_ino) *out_ino = cur_ino;
    if (out_inode) *out_inode = cur;
    return EXT_OK;
}

/* =============================================================================
 * Public API.
 * ========================================================================== */
BOOLEAN ext_probe(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio) {
    if (!bs || !bio || !bio->Media || !bio->Media->MediaPresent) return FALSE;
    uint32_t dev_bsize = bio->Media->BlockSize ? bio->Media->BlockSize : 512;

    /* Read the two sectors covering byte offset 1024..2048. */
    uint32_t need = (uint32_t)(EXT_SUPER_OFFSET + 1024);
    uint32_t rd = ((need + dev_bsize - 1) / dev_bsize) * dev_bsize;
    void *buf = NULL;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, rd, &buf)) || !buf) return FALSE;
    EFI_STATUS st = bio->ReadBlocks(bio, bio->Media->MediaId, 0, rd, buf);
    BOOLEAN ok = FALSE;
    if (!EFI_ERROR(st)) {
        uint8_t *sb = (uint8_t *)buf + EXT_SUPER_OFFSET;
        ok = (le16(sb + SB_MAGIC) == EXT_SUPER_MAGIC);
    }
    bs->FreePool(buf);
    return ok;
}

ext_ctx *ext_mount(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio,
                   EFI_DISK_IO_PROTOCOL *dio) {
    if (!bs || !bio || !bio->Media || !bio->Media->MediaPresent) return NULL;

    ext_ctx *c = NULL;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, sizeof(*c), (void **)&c)) || !c)
        return NULL;
    x_memset(c, 0, sizeof(*c));
    c->bs = bs; c->bio = bio; c->dio = dio;
    c->media_id  = bio->Media->MediaId;
    c->dev_bsize = bio->Media->BlockSize ? bio->Media->BlockSize : 512;
    c->cache_block = ~0ULL;
    c->last_group  = ~0u;   /* no group descriptor cached yet */

    /* Scratch sector for dev_read's partial path. */
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, c->dev_bsize, (void **)&c->dsect)) || !c->dsect)
        goto fail;

    /* Read the superblock (1024 bytes @ offset 1024). */
    uint8_t sb[1024];
    if (dev_read(c, EXT_SUPER_OFFSET, sb, sizeof(sb)) != EXT_OK) goto fail;
    if (le16(sb + SB_MAGIC) != EXT_SUPER_MAGIC) goto fail;

    uint32_t log_bs = le32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 6) goto fail; /* block size 1KiB..64KiB sane bound */
    c->block_size       = 1024u << log_bs;
    c->blocks_per_group = le32(sb + SB_BLOCKS_PER_GROUP);
    c->inodes_per_group = le32(sb + SB_INODES_PER_GROUP);
    c->inodes_count     = le32(sb + SB_INODES_COUNT);
    c->first_data_block = le32(sb + SB_FIRST_DATA_BLOCK);
    c->feature_incompat = le32(sb + SB_FEATURE_INCOMPAT);

    uint32_t rev = le32(sb + SB_REV_LEVEL);
    c->inode_size = (rev == 0) ? 128 : le16(sb + SB_INODE_SIZE);
    if (c->inode_size < 128 || c->inode_size > c->block_size) c->inode_size = 128;

    c->desc_size = 32;
    if (c->feature_incompat & EXT_INCOMPAT_64BIT) {
        uint16_t ds = le16(sb + SB_DESC_SIZE);
        if (ds >= 32 && ds <= 1024) c->desc_size = ds;
        else c->desc_size = 64;
    }

    if (c->inodes_per_group == 0 || c->block_size == 0) goto fail;

    /* Volume label. */
    x_memcpy(c->label, sb + SB_VOLUME_NAME, 16);
    c->label[16] = 0;

    /* fs-block scratch buffers. */
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, c->block_size, (void **)&c->blkbuf)) || !c->blkbuf)
        goto fail;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, c->block_size, (void **)&c->auxbuf)) || !c->auxbuf)
        goto fail;

    /* Fixed per-depth scratch pool for extent_lookup() index-node descent. */
    for (int i = 0; i <= EXT_MAX_DEPTH; i++) {
        if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, c->block_size, (void **)&c->extbuf[i])) || !c->extbuf[i])
            goto fail;
    }

    return c;

fail:
    ext_unmount(c);
    return NULL;
}

void ext_unmount(ext_ctx *c) {
    if (!c) return;
    EFI_BOOT_SERVICES *bs = c->bs;
    if (c->dsect)  bs->FreePool(c->dsect);
    if (c->blkbuf) bs->FreePool(c->blkbuf);
    if (c->auxbuf) bs->FreePool(c->auxbuf);
    for (int i = 0; i <= EXT_MAX_DEPTH; i++)
        if (c->extbuf[i]) bs->FreePool(c->extbuf[i]);
    bs->FreePool(c);
}

int ext_ls(ext_ctx *c, const char *path, ext_dirent_cb cb, void *user) {
    if (!c || !path || !cb) return EXT_ERR_IO;
    ext_inode dir;
    int r = resolve_path(c, path, NULL, &dir);
    if (r != EXT_OK) return r;
    if ((dir.mode & EXT_S_IFMT) != EXT_S_IFDIR) return EXT_ERR_NOTDIR;

    uint64_t nblocks = (dir.size + c->block_size - 1) / c->block_size;
    for (uint64_t b = 0; b < nblocks; b++) {
        int64_t phys = map_block(c, &dir, (uint32_t)b);
        if (phys < 0) return (int)phys;
        if (phys == 0) continue;
        if (read_fsblock(c, (uint64_t)phys, c->blkbuf) != EXT_OK) return EXT_ERR_IO;

        uint32_t off = 0;
        while (off + 8 <= c->block_size) {
            uint8_t *de = c->blkbuf + off;
            uint32_t e_ino = le32(de);
            uint16_t rec   = le16(de + 4);
            uint8_t  namel = de[6];
            uint8_t  ftype = de[7];
            if (rec < 8) break;
            if (e_ino != 0 && namel > 0 &&
                (uint32_t)off + 8 + namel <= c->block_size) {
                char nm[256];
                x_memcpy(nm, de + 8, namel);
                nm[namel] = 0;
                cb(nm, e_ino, ftype, user);
            }
            off += rec;
        }
    }
    return EXT_OK;
}

int64_t ext_file_size(ext_ctx *c, const char *path) {
    if (!c || !path) return EXT_ERR_IO;
    ext_inode ino;
    int r = resolve_path(c, path, NULL, &ino);
    if (r != EXT_OK) return r;
    if ((ino.mode & EXT_S_IFMT) != EXT_S_IFREG) return EXT_ERR_NOTFILE;
    return (int64_t)ino.size;
}

int64_t ext_read(ext_ctx *c, const char *path, void *buf, uint64_t max) {
    if (!c || !path || !buf) return EXT_ERR_IO;
    ext_inode ino;
    int r = resolve_path(c, path, NULL, &ino);
    if (r != EXT_OK) return r;
    if ((ino.mode & EXT_S_IFMT) != EXT_S_IFREG) return EXT_ERR_NOTFILE;
    if (ino.flags & EXT_INODE_INLINE_FL) return EXT_ERR_CORRUPT; /* inline_data unsupported */

    uint64_t want = ino.size < max ? ino.size : max;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;

    while (done < want) {
        uint32_t lblk = (uint32_t)(done / c->block_size);
        uint32_t within = (uint32_t)(done % c->block_size);
        uint32_t chunk = c->block_size - within;
        if (chunk > want - done) chunk = (uint32_t)(want - done);

        int64_t phys = map_block(c, &ino, lblk);
        if (phys < 0) return phys;
        if (phys == 0) {
            x_memset(out + done, 0, chunk);          /* sparse hole */
        } else if (within == 0 && chunk == c->block_size) {
            /* Whole, block-aligned chunk. Coalesce as many physically
             * contiguous following blocks as remain in `want` into one
             * dev_read, straight into the caller's buffer (bypassing c->blkbuf
             * and its extra memcpy). This turns per-block IO round-trips into a
             * single multi-block read for the common contiguous file. */
            uint32_t run = 1;
            while (want - done >= (uint64_t)(run + 1) * c->block_size &&
                   map_block(c, &ino, lblk + run) == phys + (int64_t)run)
                run++;
            if (dev_read(c, (uint64_t)phys * c->block_size, out + done,
                         (uint64_t)run * c->block_size) != EXT_OK)
                return EXT_ERR_IO;
            done += (uint64_t)run * c->block_size;
            continue;
        } else {
            if (read_fsblock(c, (uint64_t)phys, c->blkbuf) != EXT_OK) return EXT_ERR_IO;
            x_memcpy(out + done, c->blkbuf + within, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

int ext_volume_label(ext_ctx *c, char out[17]) {
    if (!c || !out) return EXT_ERR_IO;
    for (int i = 0; i < 17; i++) out[i] = c->label[i];
    (void)x_strlen; /* silence unused in trimmed builds */
    return EXT_OK;
}
