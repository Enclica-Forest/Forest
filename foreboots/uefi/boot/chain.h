/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/chain.h - chainload another EFI bootloader (GRUB on USB, etc.)
 * =============================================================================
 * chainload() switches to a second EFI application: if ent->chain names a file
 * it LoadImage/StartImage's it (from ForeB's own ESP by default); if ent->chain
 * is EMPTY it auto-scans EVERY SimpleFileSystem volume (internal + USB) for a
 * standard loader (\EFI\BOOT\BOOTX64.EFI, \EFI\<distro>\grubx64.efi, ...) and
 * boots the first it finds. Builds a full device path (volume DP + file node)
 * so the chainloaded loader inherits a correct DeviceHandle. Works on all UEFI
 * arches. Runs before ExitBootServices; returns only on failure. See chain.c.
 * =============================================================================
 */
#ifndef FOREB_UEFI_CHAIN_H
#define FOREB_UEFI_CHAIN_H

#include "../efi.h"
#include "../../include/forebo_cfg.h"

EFI_STATUS chainload(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                     EFI_SYSTEM_TABLE *st, const struct forebo_menuentry *ent);

#endif /* FOREB_UEFI_CHAIN_H */
