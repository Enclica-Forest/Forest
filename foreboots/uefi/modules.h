/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/modules.h - Multiboot1 module (initrd/ramdisk/...) loader (public API).
 * =============================================================================
 * Implemented in uefi/modules.c. Runs BEFORE ExitBootServices: it reads each of
 * a menu entry's module files off the ESP into firmware-allocated pages (which
 * survive ExitBootServices), builds a multiboot1 mb_module[] array in the fixed
 * ForeB low-RAM scratch region, and wires it into the multiboot_info the kernel
 * receives in EBX (mods_count / mods_addr, MB_FLAG_MODS). The first module is
 * also mirrored into foreboots_boot_info.initrd_addr/size (FOREB_BIF_INITRD) so
 * the richer boot-info consumer sees the primary initrd too.
 * =============================================================================
 */
#ifndef FOREB_UEFI_MODULES_H
#define FOREB_UEFI_MODULES_H

#include "efi.h"
#include "../include/forebo_cfg.h"
#include "../include/boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Physical address of the mb_module[] array built by modules_load(). Sits just
 * past the multiboot_info at 0x1800 (ends 0x1870) inside the loader-reserved
 * 0x1000..0x7FFF low region. Room for FOREB_CFG_MAX_MODULES records plus their
 * cmdline strings before the 0x2000 cmdline/VBE area. mbi->mods_addr is set to
 * this value.
 */
#define FOREB_MB_MODULE_ARRAY_ADDR   0x00001900u
/* NUL-terminated per-module name strings live right after the fixed-size array. */
#define FOREB_MB_MODULE_STR_ADDR \
    (FOREB_MB_MODULE_ARRAY_ADDR + (FOREB_CFG_MAX_MODULES * 16u))
/* Upper bound of the string pool (exclusive): the 0x2000 cmdline region. */
#define FOREB_MB_MODULE_STR_END      0x00002000u

/*
 * Load every module listed in 'entry' and register them as multiboot1 modules.
 *
 *   image      - the loader's EFI image handle (for ESP access)
 *   bs         - live BootServices (must be called before ExitBootServices)
 *   entry      - selected menu entry (its modules[]/module_count drive the load)
 *   mbi        - multiboot_info being built (mods_count/mods_addr/flags updated)
 *   boot_info  - foreboots_boot_info (initrd_addr/size/flags set for module #0)
 *
 * Behaviour:
 *   - entry with zero modules: no-op, returns EFI_SUCCESS (mbi left unchanged).
 *   - each module file is read into AllocatePages(EfiLoaderData) pages placed
 *     below 4 GiB (multiboot mod_start/mod_end are 32-bit physical addresses).
 *   - a module that fails to load (missing file, OOM) is skipped; remaining
 *     modules still load. If AT LEAST ONE module loads, MB_FLAG_MODS is set.
 *   - returns EFI_SUCCESS if all requested modules loaded, EFI_NOT_FOUND if
 *     some were skipped, or an EFI error for a hard failure (e.g. no ESP).
 */
EFI_STATUS modules_load(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                        const struct forebo_menuentry *entry,
                        struct multiboot_info *mbi,
                        struct foreboots_boot_info *boot_info);

#ifdef __cplusplus
}
#endif

#endif /* FOREB_UEFI_MODULES_H */
