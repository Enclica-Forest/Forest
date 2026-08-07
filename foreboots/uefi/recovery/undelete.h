/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/undelete.h - GUI file-recovery tool (Undelete / Carve).
 * =============================================================================
 * A "template B" wm.c window (see tools.h). This is an advanced recovery
 * browser with a SPLIT layout: a LEFT navigation pane and a RIGHT preview pane.
 *
 * Two modes, toggled with Tab:
 *
 *   BROWSE (real filesystem hierarchy of EXISTING files)
 *     - Enumerate block devices with diskio_enumerate().
 *     - Detect the filesystem on a chosen device and walk its DIRECTORY TREE:
 *         * ext2/3/4 -> ext_probe/ext_mount + ext_ls (descend into subdirs),
 *         * FAT/ESP   -> EFI_SIMPLE_FILE_SYSTEM OpenVolume + EFI_FILE dir walk,
 *         * btrfs     -> btrfs_probe + btrfs_list_snapshots (subvols, flat).
 *       Unrecognised filesystems are reported and skipped, never fatal.
 *
 *   CARVE (deleted / unallocated recovery)
 *     - Signature-carve the selected raw device through the corruption-tolerant
 *       diskio layer (JPEG/PNG/PDF/ZIP/GIF/BMP/TGA), tolerating bad sectors.
 *
 * The PREVIEW pane decodes a selected BMP/TGA (image.c) into a fit-to-pane
 * thumbnail, or otherwise shows a bounded hex + ASCII dump of the first bytes.
 *
 * RECOVER copies the selected real file OR carved extent to
 * \forebo\recovered\NNNN.<ext> on the ESP via EFI_FILE writes. READ-ONLY on the
 * scanned/source device. Freestanding C11, no libc; all scan/preview buffers are
 * fixed-size statics, and file recovery uses a single bounded BootServices
 * AllocatePool paired with FreePool.
 * ========================================================================== */
#ifndef FOREB_UEFI_UNDELETE_H
#define FOREB_UEFI_UNDELETE_H

#include "../efi.h"

/*
 * One-time init: caches the loader image handle (for opening the ESP) and the
 * system table (BootServices for AllocatePool/FreePool + the ESP file writes),
 * and initialises the diskio layer (diskio_init). Call once early in efi_main,
 * next to the other module inits (tools_init / img_init / ...). NULL-safe: if
 * never called, tool_undelete_open() shows a graceful "unavailable" window.
 */
void tool_undelete_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

/*
 * Open the Undelete / Carve tool as a wm window (template B): builds the device
 * list and returns immediately; the bootx64.c menu loop drives input +
 * compositing + present. Idempotent (no-op if already open).
 */
void tool_undelete_open(void);

#endif /* FOREB_UEFI_UNDELETE_H */
