/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/fs_ext.h - Read-only ext2/ext3/ext4 filesystem driver (public API)
 * =============================================================================
 * A dependency-free (no libc, no gnu-efi) read-only driver over a raw block
 * device (EFI_BLOCK_IO_PROTOCOL, optionally EFI_DISK_IO_PROTOCOL for byte-
 * granular reads). It parses enough of the on-disk format to:
 *
 *     - identify the filesystem (superblock magic 0xEF53 @ byte 1024),
 *     - resolve an absolute path to an inode (walking directory entries),
 *     - list a directory (ext_ls, via callback),
 *     - read/cat a regular file by path (ext_read).
 *
 * Both the classic indirect BLOCK MAP (ext2/ext3) and the ext4 EXTENT TREE are
 * supported for file data. See fs_ext.c's header comment for the exact DEPTH
 * and FEATURE limits (honest boundaries).
 *
 * Intended consumer: the ForeB shell / recovery tools (read + cat + ls on an
 * ext volume the firmware cannot mount). Everything runs before or after
 * ExitBootServices only insofar as its BootServices pointer stays valid; the
 * driver itself performs no firmware calls beyond ReadBlocks/ReadDisk and
 * AllocatePool/FreePool that the caller supplies via EFI_BOOT_SERVICES.
 * ========================================================================== */

#ifndef FOREB_FS_EXT_H
#define FOREB_FS_EXT_H

#include "../efi.h"
#include "../efi_ext.h"   /* EFI_DISK_IO_PROTOCOL (optional fast path) */

/* Opaque mount context. Allocated by ext_mount(), released by ext_unmount(). */
typedef struct ext_ctx ext_ctx;

/* Directory-entry file types (ext2/3/4 dir_entry_2 file_type field). */
#define EXT_FT_UNKNOWN   0
#define EXT_FT_REG       1
#define EXT_FT_DIR       2
#define EXT_FT_CHRDEV    3
#define EXT_FT_BLKDEV    4
#define EXT_FT_FIFO      5
#define EXT_FT_SOCK      6
#define EXT_FT_SYMLINK   7

/* Callback invoked once per directory entry by ext_ls().
 *   name      - NUL-terminated entry name (not including path).
 *   inode     - inode number the entry points at.
 *   file_type - one of EXT_FT_* (may be EXT_FT_UNKNOWN on very old ext2 that
 *               did not store the type byte; caller may stat via ext_ls again).
 *   user      - opaque pointer passed through from ext_ls(). */
typedef void (*ext_dirent_cb)(const char *name, uint32_t inode,
                              uint8_t file_type, void *user);

/* -----------------------------------------------------------------------------
 * ext_probe - lightweight "is this an ext2/3/4 volume?" test.
 * Reads only the superblock. Returns TRUE if magic 0xEF53 is present.
 * -------------------------------------------------------------------------- */
BOOLEAN ext_probe(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio);

/* -----------------------------------------------------------------------------
 * ext_mount - parse the superblock + geometry and return a context, or NULL if
 * the device is not ext or a read failed. `dio` may be NULL (BlockIo is then
 * used with sector rounding). The context borrows `bs`/`bio`/`dio`; keep them
 * valid for the context's lifetime.
 * -------------------------------------------------------------------------- */
ext_ctx *ext_mount(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio,
                   EFI_DISK_IO_PROTOCOL *dio /*optional*/);

/* Release a context and all its buffers. Safe on NULL. */
void ext_unmount(ext_ctx *ctx);

/* -----------------------------------------------------------------------------
 * ext_ls - list the directory named by absolute `path` ("/", "/boot", ...).
 * Invokes `cb` for each entry (including "." and ".."). Returns 0 on success,
 * negative on error (path not found / not a directory / read error).
 * -------------------------------------------------------------------------- */
int ext_ls(ext_ctx *ctx, const char *path, ext_dirent_cb cb, void *user);

/* -----------------------------------------------------------------------------
 * ext_read - read up to `max` bytes of the regular file at absolute `path`
 * into `buf`. Returns the number of bytes read (>=0), or negative on error
 * (not found / not a regular file / read error). Sparse holes read as zeros.
 * -------------------------------------------------------------------------- */
int64_t ext_read(ext_ctx *ctx, const char *path, void *buf, uint64_t max);

/* -----------------------------------------------------------------------------
 * ext_file_size - return the size in bytes of the regular file at `path`, or
 * a negative value on error. Handy for the shell to size a buffer before
 * ext_read. (0 is a valid size for an empty file.)
 * -------------------------------------------------------------------------- */
int64_t ext_file_size(ext_ctx *ctx, const char *path);

/* Human-readable volume label (up to 16 chars, NUL-terminated) into `out`.
 * `out` must hold at least 17 bytes. Returns 0 on success. */
int ext_volume_label(ext_ctx *ctx, char out[17]);

/* Errors (negative). */
#define EXT_OK               0
#define EXT_ERR_IO          (-1)
#define EXT_ERR_NOTEXT      (-2)
#define EXT_ERR_NOTFOUND    (-3)
#define EXT_ERR_NOTDIR      (-4)
#define EXT_ERR_NOTFILE     (-5)
#define EXT_ERR_NOMEM       (-6)
#define EXT_ERR_CORRUPT     (-7)

#endif /* FOREB_FS_EXT_H */
