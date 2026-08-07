/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/fs_btrfs.c - Read-only btrfs detect + subvolume/snapshot listing
 * =============================================================================
 * Freestanding. Build with the standard UEFI loader recipe:
 *
 *   clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
 *         -mno-red-zone -mno-mmx -mno-sse -std=c11 -I. -Iinclude \
 *         -c uefi/fs_btrfs.c -o uefi/fs_btrfs.o
 *
 * ---------------------------------------------------------------------------
 * HOW IT WORKS (the minimum viable btrfs read path for enumerating snapshots)
 *
 *  1. Superblock (@ byte 0x10000, magic "_BHRfS_M" @ +0x40) yields: nodesize,
 *     sectorsize, the CHUNK-tree root logical addr + level, the ROOT-tree root
 *     logical addr + level, and the embedded "system chunk array".
 *
 *  2. btrfs addresses everything by *logical* address; a CHUNK maps a logical
 *     range to a physical device offset. The chunk tree itself lives at a
 *     logical address, so there is a bootstrap: the superblock carries a
 *     "system chunk array" that maps exactly the chunks needed to read the
 *     chunk tree. We seed our logical->physical table from that array, then
 *     WALK THE CHUNK TREE to discover every remaining chunk mapping.
 *
 *  3. With a complete chunk map we WALK THE ROOT TREE (objectid 1) and collect
 *     ROOT_REF items. Each ROOT_REF carries a subvolume/snapshot's NAME, its
 *     own id (key.offset) and its parent's id (key.objectid). That is exactly
 *     the "list of subvolumes and snapshots" a recovery UI wants.
 *
 * ---------------------------------------------------------------------------
 * HONEST BOUNDARIES
 *   - Read-only. No CoW, no writes, no csum verification (best-effort: we trust
 *     the on-disk metadata; a scrub is out of scope).
 *   - SINGLE DEVICE / first mirror only: for every chunk we use stripe[0]. RAID
 *     0/1/10/5/6 multi-device profiles are not reassembled; on a single-device
 *     btrfs (the common case) stripe[0] is the data. Multi-device volumes may
 *     yield BTRFS_ERR_NOMAP for chunks whose data is on another device.
 *   - Chunk map is capped at BTRFS_MAX_CHUNKS entries; enormous filesystems can
 *     overflow it (older chunks are then unmapped -> BTRFS_ERR_NOMAP).
 *   - We do not distinguish snapshots from plain subvolumes (btrfs stores both
 *     as named roots); both are reported.
 *   - b-tree recursion is depth-limited (BTRFS_MAX_LEVEL). Legal trees are
 *     shallow (<=8).
 *   - We read the PRIMARY superblock only (offset 0x10000); backup superblocks
 *     at 0x4000000 / 0x4000000000 are not consulted.
 * ========================================================================== */

#include "fs_btrfs.h"

/* =============================================================================
 * Tiny freestanding helpers.
 * ========================================================================== */
static void x_memcpy(void *d, const void *s, uint64_t n) {
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}
static void x_memset(void *d, int c, uint64_t n) {
    uint8_t *dd = (uint8_t *)d; while (n--) *dd++ = (uint8_t)c;
}
static int x_memcmp(const void *a, const void *b, uint64_t n) {
    const uint8_t *aa = a, *bb = b;
    while (n--) { if (*aa != *bb) return (int)*aa - (int)*bb; aa++; bb++; }
    return 0;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* =============================================================================
 * On-disk constants / offsets.
 * ========================================================================== */
#define BTRFS_SUPER_OFFSET      0x10000ULL
#define BTRFS_SUPER_SIZE        4096
#define BTRFS_MAGIC_STR         "_BHRfS_M"      /* 8 bytes @ sb+0x40 */

/* Superblock field offsets (relative to superblock start). */
#define SB_MAGIC                0x40
#define SB_ROOT                 0x50    /* logical addr of root-tree root  */
#define SB_CHUNK_ROOT           0x58    /* logical addr of chunk-tree root */
#define SB_TOTAL_BYTES          0x70
#define SB_ROOT_DIR_OBJECTID    0x80
#define SB_SECTORSIZE           0x90
#define SB_NODESIZE             0x94
#define SB_SYS_CHUNK_ARRAY_SIZE 0xa0
#define SB_CHUNK_ROOT_GEN       0xa4
#define SB_ROOT_LEVEL           0xc6    /* u8 */
#define SB_CHUNK_ROOT_LEVEL     0xc7    /* u8 */
#define SB_LABEL                0x12b   /* 256 bytes */
#define SB_SYS_CHUNK_ARRAY      0x32b   /* up to 2048 bytes */

/* b-tree header (btrfs_header), 101 bytes. */
#define HDR_BYTENR              0x30
#define HDR_NRITEMS             0x60    /* u32 */
#define HDR_LEVEL               0x64    /* u8  */
#define HDR_SIZE                101

/* Leaf item (btrfs_item), 25 bytes: disk_key(17) + offset(u32) + size(u32). */
#define ITEM_SIZE               25
#define ITEM_KEY_TYPE           8       /* within disk_key */
#define ITEM_KEY_OFFSET         9       /* within disk_key (u64) */
#define ITEM_DATA_OFF           17      /* u32 offset from end of header */
#define ITEM_DATA_SIZE          21      /* u32 */

/* Internal key pointer (btrfs_key_ptr), 33 bytes: disk_key(17)+blockptr(8)+gen(8). */
#define KP_SIZE                 33
#define KP_BLOCKPTR             17      /* u64 */

/* disk_key: objectid(u64)@0, type(u8)@8, offset(u64)@9. */

/* btrfs_chunk. */
#define CHUNK_LENGTH            0x00    /* u64 */
#define CHUNK_TYPE              0x18    /* u64 */
#define CHUNK_NUM_STRIPES       0x2c    /* u16 */
#define CHUNK_STRIPES           0x30
#define STRIPE_SIZE             32      /* devid(8)+offset(8)+uuid(16) */
#define STRIPE_DEVID            0x00    /* u64 */
#define STRIPE_OFFSET           0x08    /* u64 */

/* Key types. */
#define BTRFS_CHUNK_ITEM_KEY    228
#define BTRFS_ROOT_ITEM_KEY     132
#define BTRFS_ROOT_REF_KEY      156
#define BTRFS_ROOT_BACKREF_KEY  144

/* Objectids. */
#define BTRFS_ROOT_TREE_OBJECTID 1

/* btrfs_root_ref: dirid(u64)@0, sequence(u64)@8, name_len(u16)@16, name@18. */
#define ROOTREF_NAMELEN         16
#define ROOTREF_NAME            18

#define BTRFS_MAX_CHUNKS        512
#define BTRFS_MAX_LEVEL         8

/* =============================================================================
 * Context.
 * ========================================================================== */
typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t physical;   /* stripe[0] device offset */
    uint64_t devid;
} bmap_t;

typedef struct {
    EFI_BOOT_SERVICES     *bs;
    EFI_BLOCK_IO_PROTOCOL *bio;
    EFI_DISK_IO_PROTOCOL  *dio;
    uint32_t media_id;
    uint32_t dev_bsize;

    uint32_t nodesize;
    uint32_t sectorsize;
    uint64_t root_logical;
    uint64_t chunk_root_logical;
    uint8_t  root_level;
    uint8_t  chunk_root_level;

    bmap_t   maps[BTRFS_MAX_CHUNKS];
    int      nmaps;

    uint8_t *dsect;    /* one device sector scratch */

    /* Fixed per-depth scratch pool for the chunk-tree / root-tree b-tree
     * walks, one nodesize buffer per recursion level. Allocated once in
     * btrfs_open(), freed once in btrfs_close(). Both walks share this array
     * (they never run concurrently); safe to reuse per depth because each
     * walk is strictly depth-first -- a node's items are fully consumed
     * before recursing into any child, so the buffer for a given depth is
     * free for the next sibling at that depth. */
    uint8_t *nodebuf[BTRFS_MAX_LEVEL + 1];

    /* listing callback plumbing */
    btrfs_snap_cb cb;
    void         *user;
    int           count;
} bctx;

/* =============================================================================
 * Raw device read (byte offset/length) via DiskIo or sector-rounded BlockIo.
 * ========================================================================== */
static int dev_read(bctx *c, uint64_t off, void *buf, uint64_t len) {
    if (c->dio) {
        EFI_STATUS st = c->dio->ReadDisk(c->dio, c->media_id, off, len, buf);
        return EFI_ERROR(st) ? BTRFS_ERR_IO : BTRFS_OK;
    }
    uint32_t bs = c->dev_bsize;
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint64_t lba    = off / bs;
        uint32_t within = (uint32_t)(off % bs);
        if (within == 0 && len >= bs) {
            uint64_t nby = (len / bs) * bs;
            EFI_STATUS st = c->bio->ReadBlocks(c->bio, c->media_id, lba, nby, out);
            if (EFI_ERROR(st)) return BTRFS_ERR_IO;
            out += nby; off += nby; len -= nby;
            continue;
        }
        EFI_STATUS st = c->bio->ReadBlocks(c->bio, c->media_id, lba, bs, c->dsect);
        if (EFI_ERROR(st)) return BTRFS_ERR_IO;
        uint32_t chunk = bs - within;
        if (chunk > len) chunk = (uint32_t)len;
        x_memcpy(out, c->dsect + within, chunk);
        out += chunk; off += chunk; len -= chunk;
    }
    return BTRFS_OK;
}

/* Translate a logical address to a physical device offset via the chunk map. */
static int translate(bctx *c, uint64_t logical, uint64_t *phys) {
    for (int i = 0; i < c->nmaps; i++) {
        bmap_t *m = &c->maps[i];
        if (logical >= m->logical && logical < m->logical + m->length) {
            *phys = m->physical + (logical - m->logical);
            return BTRFS_OK;
        }
    }
    return BTRFS_ERR_NOMAP;
}

/* Read `len` bytes starting at a logical address into `buf` (must lie within a
 * single chunk, which is always true for a single tree node). */
static int read_logical(bctx *c, uint64_t logical, void *buf, uint64_t len) {
    uint64_t phys;
    int r = translate(c, logical, &phys);
    if (r != BTRFS_OK) return r;
    return dev_read(c, phys, buf, len);
}

/* Append one chunk mapping (deduped, bounded). */
static void add_map(bctx *c, uint64_t logical, uint64_t length,
                    uint64_t physical, uint64_t devid) {
    for (int i = 0; i < c->nmaps; i++)
        if (c->maps[i].logical == logical) return; /* already known */
    if (c->nmaps >= BTRFS_MAX_CHUNKS) return;       /* honest cap */
    c->maps[c->nmaps].logical  = logical;
    c->maps[c->nmaps].length   = length;
    c->maps[c->nmaps].physical = physical;
    c->maps[c->nmaps].devid    = devid;
    c->nmaps++;
}

/* Parse one on-disk btrfs_chunk (at `chunk`, `avail` bytes) into a map, using
 * stripe[0]. `logical` comes from the item/array key. */
static void parse_chunk(bctx *c, uint64_t logical, const uint8_t *chunk, uint32_t avail) {
    if (avail < CHUNK_STRIPES + STRIPE_SIZE) return;
    uint64_t length     = le64(chunk + CHUNK_LENGTH);
    uint16_t num_str    = le16(chunk + CHUNK_NUM_STRIPES);
    if (num_str == 0) return;
    const uint8_t *s0   = chunk + CHUNK_STRIPES;
    uint64_t devid      = le64(s0 + STRIPE_DEVID);
    uint64_t phys       = le64(s0 + STRIPE_OFFSET);
    add_map(c, logical, length, phys, devid);
}

/* Seed the chunk map from the superblock's system chunk array. */
static void seed_sys_chunks(bctx *c, const uint8_t *sb) {
    uint32_t arr_size = le32(sb + SB_SYS_CHUNK_ARRAY_SIZE);
    if (arr_size > 2048) arr_size = 2048;
    const uint8_t *p   = sb + SB_SYS_CHUNK_ARRAY;
    const uint8_t *end = p + arr_size;
    while (p + 17 + CHUNK_STRIPES + STRIPE_SIZE <= end) {
        /* disk_key: objectid(8), type(8..), offset(9) = chunk logical. */
        uint64_t logical = le64(p + ITEM_KEY_OFFSET);
        const uint8_t *chunk = p + 17;
        uint16_t num_str = le16(chunk + CHUNK_NUM_STRIPES);
        if (num_str == 0 || num_str > 128) break;
        uint32_t clen = CHUNK_STRIPES + (uint32_t)num_str * STRIPE_SIZE;
        if (chunk + clen > end) break;
        parse_chunk(c, logical, chunk, clen);
        p = chunk + clen;
    }
}

/* =============================================================================
 * Chunk-tree walk: complete the logical->physical map.
 * ========================================================================== */
static int walk_chunk_tree(bctx *c, uint64_t logical, int level) {
    if (level < 0 || level > BTRFS_MAX_LEVEL) return BTRFS_ERR_CORRUPT;

    uint8_t *node = c->nodebuf[level];
    if (!node) return BTRFS_ERR_NOMEM;
    int r = read_logical(c, logical, node, c->nodesize);
    if (r != BTRFS_OK) return r;

    uint32_t nritems = le32(node + HDR_NRITEMS);
    uint8_t  nlevel  = node[HDR_LEVEL];

    /* The on-disk header level is untrusted. If it disagrees with the depth we
     * expected (and therefore the buffer we read into, nodebuf[level]), a
     * corrupt header could steer the child recursion into nodebuf[nlevel-1] ==
     * nodebuf[level], clobbering this node's buffer while we still iterate it.
     * Reject the mismatch and recurse off the validated `level`. */
    if (nlevel != level) return BTRFS_ERR_CORRUPT;

    if (level == 0) {
        uint32_t maxit = (c->nodesize - HDR_SIZE) / ITEM_SIZE;
        if (nritems > maxit) nritems = maxit;
        for (uint32_t i = 0; i < nritems; i++) {
            const uint8_t *it = node + HDR_SIZE + i * ITEM_SIZE;
            uint8_t  ktype   = it[ITEM_KEY_TYPE];
            uint64_t klog    = le64(it + ITEM_KEY_OFFSET);
            uint32_t doff    = le32(it + ITEM_DATA_OFF);
            uint32_t dsize   = le32(it + ITEM_DATA_SIZE);
            if (ktype != BTRFS_CHUNK_ITEM_KEY) continue;
            if ((uint64_t)HDR_SIZE + doff + dsize > c->nodesize) continue;
            parse_chunk(c, klog, node + HDR_SIZE + doff, dsize);
        }
    } else {
        uint32_t maxit = (c->nodesize - HDR_SIZE) / KP_SIZE;
        if (nritems > maxit) nritems = maxit;
        for (uint32_t i = 0; i < nritems; i++) {
            const uint8_t *kp = node + HDR_SIZE + i * KP_SIZE;
            uint64_t child = le64(kp + KP_BLOCKPTR);
            walk_chunk_tree(c, child, level - 1);   /* best-effort; ignore per-child errs */
        }
    }
    return BTRFS_OK;
}

/* =============================================================================
 * Root-tree walk: collect ROOT_REF items -> subvolume/snapshot names.
 * ========================================================================== */
static int walk_root_tree(bctx *c, uint64_t logical, int level) {
    if (level < 0 || level > BTRFS_MAX_LEVEL) return BTRFS_ERR_CORRUPT;

    uint8_t *node = c->nodebuf[level];
    if (!node) return BTRFS_ERR_NOMEM;
    int r = read_logical(c, logical, node, c->nodesize);
    if (r != BTRFS_OK) return r;

    uint32_t nritems = le32(node + HDR_NRITEMS);
    uint8_t  nlevel  = node[HDR_LEVEL];

    /* Same guard as walk_chunk_tree: a corrupt header level must not redirect
     * the child recursion into a shallower buffer and clobber this node's
     * nodebuf[level] mid-iteration. Recurse off the validated `level`. */
    if (nlevel != level) return BTRFS_ERR_CORRUPT;

    if (level == 0) {
        uint32_t maxit = (c->nodesize - HDR_SIZE) / ITEM_SIZE;
        if (nritems > maxit) nritems = maxit;
        for (uint32_t i = 0; i < nritems; i++) {
            const uint8_t *it = node + HDR_SIZE + i * ITEM_SIZE;
            uint64_t kobj  = le64(it + 0);            /* key.objectid = parent id */
            uint8_t  ktype = it[ITEM_KEY_TYPE];
            uint64_t koff  = le64(it + ITEM_KEY_OFFSET); /* key.offset = child id */
            uint32_t doff  = le32(it + ITEM_DATA_OFF);
            uint32_t dsize = le32(it + ITEM_DATA_SIZE);

            if (ktype != BTRFS_ROOT_REF_KEY) continue;
            if ((uint64_t)HDR_SIZE + doff + dsize > c->nodesize) continue;
            if (dsize < ROOTREF_NAME) continue;

            const uint8_t *data = node + HDR_SIZE + doff;
            uint16_t namelen = le16(data + ROOTREF_NAMELEN);
            if ((uint32_t)ROOTREF_NAME + namelen > dsize) continue;
            if (namelen > 255) namelen = 255;

            char name[256];
            x_memcpy(name, data + ROOTREF_NAME, namelen);
            name[namelen] = 0;

            if (c->cb) c->cb(name, koff, kobj, c->user);
            c->count++;
        }
    } else {
        uint32_t maxit = (c->nodesize - HDR_SIZE) / KP_SIZE;
        if (nritems > maxit) nritems = maxit;
        for (uint32_t i = 0; i < nritems; i++) {
            const uint8_t *kp = node + HDR_SIZE + i * KP_SIZE;
            uint64_t child = le64(kp + KP_BLOCKPTR);
            walk_root_tree(c, child, level - 1);
        }
    }
    return BTRFS_OK;
}

/* =============================================================================
 * Superblock read + context open.
 * ========================================================================== */
static int read_superblock(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio,
                           EFI_DISK_IO_PROTOCOL *dio, uint8_t sb[BTRFS_SUPER_SIZE]) {
    if (!bs || !bio || !bio->Media || !bio->Media->MediaPresent) return BTRFS_ERR_IO;
    uint32_t dev_bsize = bio->Media->BlockSize ? bio->Media->BlockSize : 512;
    uint32_t media_id  = bio->Media->MediaId;

    if (dio) {
        EFI_STATUS st = dio->ReadDisk(dio, media_id, BTRFS_SUPER_OFFSET,
                                      BTRFS_SUPER_SIZE, sb);
        return EFI_ERROR(st) ? BTRFS_ERR_IO : BTRFS_OK;
    }
    /* BlockIo: read the aligned window covering [0x10000, 0x10000+4096). */
    uint64_t lba = BTRFS_SUPER_OFFSET / dev_bsize;
    uint32_t span = BTRFS_SUPER_SIZE;
    /* superblock is 4096-aligned; dev_bsize (<=4096) divides the offset evenly */
    void *tmp = NULL;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, span, &tmp)) || !tmp) return BTRFS_ERR_NOMEM;
    EFI_STATUS st = bio->ReadBlocks(bio, media_id, lba, span, tmp);
    if (EFI_ERROR(st)) { bs->FreePool(tmp); return BTRFS_ERR_IO; }
    x_memcpy(sb, tmp, BTRFS_SUPER_SIZE);
    bs->FreePool(tmp);
    return BTRFS_OK;
}

static void btrfs_close(bctx *c);

static bctx *btrfs_open(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio,
                        EFI_DISK_IO_PROTOCOL *dio) {
    uint8_t sb[BTRFS_SUPER_SIZE];
    if (read_superblock(bs, bio, dio, sb) != BTRFS_OK) return NULL;
    if (x_memcmp(sb + SB_MAGIC, BTRFS_MAGIC_STR, 8) != 0) return NULL;

    bctx *c = NULL;
    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, sizeof(*c), (void **)&c)) || !c) return NULL;
    x_memset(c, 0, sizeof(*c));
    c->bs = bs; c->bio = bio; c->dio = dio;
    c->media_id  = bio->Media->MediaId;
    c->dev_bsize = bio->Media->BlockSize ? bio->Media->BlockSize : 512;

    c->nodesize           = le32(sb + SB_NODESIZE);
    c->sectorsize         = le32(sb + SB_SECTORSIZE);
    c->root_logical       = le64(sb + SB_ROOT);
    c->chunk_root_logical = le64(sb + SB_CHUNK_ROOT);
    c->root_level         = sb[SB_ROOT_LEVEL];
    c->chunk_root_level   = sb[SB_CHUNK_ROOT_LEVEL];

    if (c->nodesize < 512 || c->nodesize > 65536) { bs->FreePool(c); return NULL; }

    if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, c->dev_bsize, (void **)&c->dsect)) || !c->dsect) {
        bs->FreePool(c); return NULL;
    }

    /* Fixed per-depth scratch pool for the chunk-tree / root-tree walks. */
    for (int i = 0; i <= BTRFS_MAX_LEVEL; i++) {
        if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, c->nodesize, (void **)&c->nodebuf[i])) || !c->nodebuf[i]) {
            btrfs_close(c); return NULL;
        }
    }

    /* 1. seed system chunks, 2. complete the map from the chunk tree. */
    seed_sys_chunks(c, sb);
    walk_chunk_tree(c, c->chunk_root_logical, c->chunk_root_level);
    return c;
}

static void btrfs_close(bctx *c) {
    if (!c) return;
    EFI_BOOT_SERVICES *bs = c->bs;
    if (c->dsect) bs->FreePool(c->dsect);
    for (int i = 0; i <= BTRFS_MAX_LEVEL; i++)
        if (c->nodebuf[i]) bs->FreePool(c->nodebuf[i]);
    bs->FreePool(c);
}

/* =============================================================================
 * Public API.
 * ========================================================================== */
BOOLEAN btrfs_probe(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio) {
    if (!bs || !bio || !bio->Media || !bio->Media->MediaPresent) return FALSE;
    uint8_t sb[BTRFS_SUPER_SIZE];
    if (read_superblock(bs, bio, NULL, sb) != BTRFS_OK) return FALSE;
    return (x_memcmp(sb + SB_MAGIC, BTRFS_MAGIC_STR, 8) == 0) ? TRUE : FALSE;
}

int btrfs_label(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio, char out[256]) {
    if (!bs || !bio || !out) return BTRFS_ERR_IO;
    uint8_t sb[BTRFS_SUPER_SIZE];
    if (read_superblock(bs, bio, NULL, sb) != BTRFS_OK) return BTRFS_ERR_IO;
    if (x_memcmp(sb + SB_MAGIC, BTRFS_MAGIC_STR, 8) != 0) return BTRFS_ERR_NOTBTRFS;
    x_memcpy(out, sb + SB_LABEL, 255);
    out[255] = 0;
    return BTRFS_OK;
}

int btrfs_list_snapshots(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio,
                         EFI_DISK_IO_PROTOCOL *dio,
                         btrfs_snap_cb cb, void *user) {
    bctx *c = btrfs_open(bs, bio, dio);
    if (!c) return BTRFS_ERR_NOTBTRFS;

    c->cb = cb; c->user = user; c->count = 0;
    int r = walk_root_tree(c, c->root_logical, c->root_level);
    int n = c->count;
    btrfs_close(c);
    if (r != BTRFS_OK && n == 0) return r;
    return n;
}
