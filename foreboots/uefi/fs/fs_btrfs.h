/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/fs_btrfs.h - Read-only btrfs detection + subvolume/snapshot listing
 * =============================================================================
 * A dependency-free, best-effort, READ-ONLY btrfs inspector over a raw block
 * device. It is NOT a general btrfs filesystem driver: its job is to
 *
 *     - DETECT a btrfs volume (superblock magic "_BHRfS_M" @ byte 0x10000), and
 *     - ENUMERATE its subvolumes and snapshots by NAME + id.
 *
 * To do that it does just enough of the real machinery:
 *     - parse the superblock,
 *     - seed a logical->physical chunk map from the superblock's system chunk
 *       array,
 *     - walk the CHUNK TREE to complete that map,
 *     - walk the ROOT TREE and collect ROOT_REF items (which carry a
 *       subvolume/snapshot's name, its id, and its parent id).
 *
 * See fs_btrfs.c for the honest DEPTH/limit notes. This is enough for a
 * recovery UI to show "here are the snapshots on this disk"; it does not read
 * file contents.
 * ========================================================================== */

#ifndef FOREB_FS_BTRFS_H
#define FOREB_FS_BTRFS_H

#include "efi.h"
#include "efi_ext.h"   /* EFI_DISK_IO_PROTOCOL (optional fast path) */

/* -----------------------------------------------------------------------------
 * btrfs_probe - TRUE if `bio` holds a btrfs volume (primary superblock magic).
 * Reads only the superblock region. `bs` is used for a scratch allocation.
 * -------------------------------------------------------------------------- */
BOOLEAN btrfs_probe(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio);

/* Optional: fetch the volume label (up to 255 chars) into `out` (>=256 bytes).
 * Returns 0 on success, negative on error. */
int btrfs_label(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio, char out[256]);

/* -----------------------------------------------------------------------------
 * Callback invoked once per discovered subvolume/snapshot.
 *   name       - NUL-terminated subvolume/snapshot name (the ROOT_REF name).
 *   subvol_id  - the subvolume's root objectid (its tree id).
 *   parent_id  - the id of the subvolume it lives under (ROOT_REF key.objectid).
 *   user       - opaque pointer forwarded from btrfs_list_snapshots().
 * btrfs cannot, from ROOT_REF alone, tell a snapshot from an ordinary
 * subvolume (both are just roots with a name); callers should present them as
 * "subvolumes / snapshots" collectively.
 * -------------------------------------------------------------------------- */
typedef void (*btrfs_snap_cb)(const char *name, uint64_t subvol_id,
                              uint64_t parent_id, void *user);

/* -----------------------------------------------------------------------------
 * btrfs_list_snapshots - enumerate subvolumes/snapshots via the root tree's
 * ROOT_REF items, invoking `cb` for each. Returns the number of entries
 * reported (>=0) or a negative error code. `dio` may be NULL.
 * -------------------------------------------------------------------------- */
int btrfs_list_snapshots(EFI_BOOT_SERVICES *bs, EFI_BLOCK_IO_PROTOCOL *bio,
                         EFI_DISK_IO_PROTOCOL *dio,
                         btrfs_snap_cb cb, void *user);

/* Errors (negative). */
#define BTRFS_OK             0
#define BTRFS_ERR_IO        (-1)
#define BTRFS_ERR_NOTBTRFS  (-2)
#define BTRFS_ERR_NOMEM     (-3)
#define BTRFS_ERR_CORRUPT   (-4)
#define BTRFS_ERR_NOMAP     (-5)   /* a logical addr could not be translated  */

#endif /* FOREB_FS_BTRFS_H */
