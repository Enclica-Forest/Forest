/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/linux.h - boot a Linux distro via the modern EFI-stub path (public API)
 * =============================================================================
 * boot_linux() treats vmlinuz as an EFI-stub PE application: LoadImage the
 * kernel (from a source buffer), set the kernel command line as its LoadOptions,
 * and - when an initrd is configured - publish it on a throw-away handle via the
 * Linux initrd media protocol (LINUX_EFI_INITRD_MEDIA_GUID + LoadFile2) so the
 * stub fetches it. Then StartImage(). Works on x86_64 / aarch64 / riscv64 UEFI.
 *
 * Runs BEFORE ExitBootServices (the stub itself calls ExitBootServices). On a
 * successful boot control never returns; a returned EFI_STATUS means the launch
 * failed (bad path, not an EFI app, stub returned) and the caller repaints the
 * menu. See linux.c.
 * =============================================================================
 */
#ifndef FOREB_UEFI_LINUX_H
#define FOREB_UEFI_LINUX_H

#include "efi.h"
#include "../include/forebo_cfg.h"

EFI_STATUS boot_linux(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                      EFI_SYSTEM_TABLE *st, const struct forebo_menuentry *ent);

#endif /* FOREB_UEFI_LINUX_H */
