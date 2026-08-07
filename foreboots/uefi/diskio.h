/* =============================================================================
 * diskio.h -- corruption-tolerant raw block-device layer for ForeB.
 *
 * A thin, ddrescue-style wrapper over EFI_BLOCK_IO_PROTOCOL (and, when present,
 * EFI_DISK_IO_PROTOCOL) that NEVER aborts a read because of bad sectors. The
 * clone, undelete and hex/image tools build on top of this: they must be able
 * to pull data off failing/partially-corrupt media, tolerating unreadable
 * blocks by zero-filling them and recording where the damage is.
 *
 * Freestanding C11, no libc, no heap beyond BootServices AllocatePool. Call
 * diskio_init(gST) once before any other entry point.
 * ========================================================================== */
#ifndef FOREB_DISKIO_H
#define FOREB_DISKIO_H

#include "efi.h"
#include "efi_ext.h"

/* Maximum characters (incl. NUL) in a device's human label. */
#define DISKIO_LABEL_MAX 40

/* Largest logical block we keep scratch space for. 4 KiB covers 512e and
 * native-4K media; larger physical blocks are rejected by diskio_read_bytes'
 * scratch path (BlockIo reads must fit one scratch block). */
#define DISKIO_MAX_BLOCK 4096

/* Per-block read retries before a block is declared bad and zero-filled. */
#define DISKIO_RETRIES 3

/* One enumerated block device. Populated by diskio_enumerate(). */
struct diskio_dev {
    EFI_BLOCK_IO_PROTOCOL *bio;              /* required, never NULL on success  */
    EFI_DISK_IO_PROTOCOL  *dio;              /* optional byte-granular reads, may be NULL */
    UINT32                 media_id;         /* MediaId to pass to ReadBlocks     */
    UINT32                 block_size;       /* logical block size in bytes       */
    UINT64                 last_lba;         /* index of the last addressable LBA */
    UINT64                 total_bytes;      /* (last_lba+1) * block_size         */
    int                    removable;        /* 1 if RemovableMedia               */
    int                    logical_partition;/* 1 if a partition, 0 if whole disk */
    EFI_HANDLE             handle;           /* handle this protocol was bound to  */
    char                   label[DISKIO_LABEL_MAX]; /* short human description    */
};

/* Statistics accumulated across a read. Pass NULL if uninterested. When a
 * pointer is supplied it is fully overwritten (reset) at the start of the
 * top-level diskio_read / diskio_read_bytes call. */
struct diskio_read_stat {
    UINT64 blocks_ok;      /* blocks read successfully                     */
    UINT64 blocks_bad;     /* blocks that failed and were zero-filled      */
    UINT64 first_bad_lba;  /* LBA of the first bad block (valid iff bad>0) */
};

/* One-time init: caches gBS/gST from the system table. Safe to call again. */
void diskio_init(EFI_SYSTEM_TABLE *st);

/* Enumerate every EFI_BLOCK_IO device into out[0..max-1]. Returns the number
 * of devices filled (0..max), or 0 if none / bad args. NULL/empty-safe. */
int diskio_enumerate(struct diskio_dev *out, int max);

/* Corruption-tolerant block read. Reads 'count' logical blocks starting at
 * 'lba' into buf (must be >= count*block_size bytes). Tries the whole span in
 * one call first; on any failure it falls back to reading block-by-block,
 * retrying each failing block DISKIO_RETRIES times and zero-filling blocks that
 * stay unreadable. 'st' (optional) records ok/bad counts and the first bad LBA.
 *
 * Returns: 0  = every block read cleanly.
 *          >0 = number of bad blocks tolerated (data zero-filled).
 *          <0 = fatal setup error (bad args, no media, block too large). */
int diskio_read(struct diskio_dev *d, UINT64 lba, UINT32 count, void *buf,
                struct diskio_read_stat *st);

/* Byte-offset convenience wrapper handling sub-block alignment. Reads 'len'
 * bytes starting at absolute byte 'offset' into buf. Uses DiskIo->ReadDisk when
 * available (still corruption-tolerant: on failure it retries via BlockIo per
 * block); otherwise BlockIo + an internal scratch block for the ragged head and
 * tail. Same return convention as diskio_read (bad-block count / <0 fatal). */
int diskio_read_bytes(struct diskio_dev *d, UINT64 offset, void *buf,
                      UINTN len, struct diskio_read_stat *st);

#endif /* FOREB_DISKIO_H */
