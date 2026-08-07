/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/chainload.h - Enumerate volumes + chainload another EFI bootloader.
 * =============================================================================
 * Implemented in uefi/chainload.c. This is the `type=chainload` boot method and
 * the "boot from USB / switch to GRUB" feature. It runs BEFORE ExitBootServices.
 *
 * Two responsibilities:
 *
 *   1. chain_list() - discovery. Enumerate EVERY volume that exposes a
 *      SimpleFileSystem (internal ESP, live USB stick, CD, ...) and probe each
 *      for a well-known secondary EFI bootloader:
 *          \EFI\BOOT\BOOTX64.EFI   (the removable-media default; arch-named on
 *                                   AArch64/RISC-V: BOOTAA64.EFI/BOOTRISCV64.EFI)
 *          \EFI\<vendor>\grubx64.efi   (a distro GRUB, e.g. \EFI\ubuntu\grubx64.efi)
 *          \EFI\<vendor>\shimx64.efi   (a Secure-Boot shim in front of GRUB)
 *      Each hit is returned with the owning device handle + the ESP-relative
 *      path, ready to feed straight into chain_boot(). Results are ordered so
 *      the menu can list them ("Boot GRUB on USB", ...).
 *
 *   2. chain_boot() - execution. Build a device path (volume device path + a
 *      MEDIA_FILEPATH node for the chosen loader), LoadImage() it FROM THAT
 *      volume (so a USB GRUB loads its own grub.cfg / modules), then
 *      StartImage(). This is a full handoff: control passes to GRUB/shim and
 *      normally never comes back.
 *
 * Everything is device-path + SimpleFileSystem based, so it is arch-neutral and
 * compiles/runs on x86_64, AArch64 and RISC-V UEFI unchanged.
 * =============================================================================
 */
#ifndef FOREB_UEFI_CHAINLOAD_H
#define FOREB_UEFI_CHAINLOAD_H

#include "efi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max chainload candidates chain_list() will report. */
#define FOREB_CHAIN_MAX_RESULTS   32
/* CHAR16 capacity of a stored ESP-relative path (incl. NUL). */
#define FOREB_CHAIN_PATH_LEN      128
/* Char capacity of a human-readable label (ASCII). */
#define FOREB_CHAIN_LABEL_LEN     96

/* What kind of secondary loader a candidate is (for labelling / ordering). */
enum foreb_chain_kind {
    FOREB_CHAIN_UNKNOWN   = 0,
    FOREB_CHAIN_REMOVABLE = 1,   /* \EFI\BOOT\BOOT{X64,AA64,RISCV64}.EFI */
    FOREB_CHAIN_GRUB      = 2,   /* \EFI\<vendor>\grubx64.efi            */
    FOREB_CHAIN_SHIM      = 3,   /* \EFI\<vendor>\shimx64.efi            */
    FOREB_CHAIN_WINDOWS   = 4    /* \EFI\Microsoft\Boot\bootmgfw.efi     */
};

/* Canonical ESP-relative path of the Windows Boot Manager (all UEFI arches use
 * this same fixed location; bootmgfw.efi is a normal EFI app that LoadImage/
 * StartImage boots without any special handling). Exposed so config.c can use
 * it as the default `chain=` for a `type=windows` entry. */
#define FOREB_CHAIN_WINDOWS_PATH  "/EFI/Microsoft/Boot/bootmgfw.efi"

/* One discovered chainload target. */
struct foreb_chain_entry {
    EFI_HANDLE device;                          /* volume's SFS handle       */
    CHAR16     path[FOREB_CHAIN_PATH_LEN];      /* ESP-relative, '\\'-sep     */
    char       label[FOREB_CHAIN_LABEL_LEN];    /* e.g. "USB: \\EFI\\ubuntu\\grubx64.efi" */
    int        kind;                            /* enum foreb_chain_kind      */
    int        volume_index;                    /* index in the SFS handle list */
};

/* Discovery result set (fixed-size, no allocation; caller owns the storage). */
struct foreb_chain_list {
    struct foreb_chain_entry items[FOREB_CHAIN_MAX_RESULTS];
    int count;
};

/*
 * Scan all SimpleFileSystem volumes for well-known EFI bootloaders and fill
 * *out. Non-destructive (read-only Open() probes). Returns the number of
 * candidates found (>=0), or -1 on a hard error (bad args / no BootServices).
 * *out->count mirrors the return value.
 */
int chain_list(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
               struct foreb_chain_list *out);

/*
 * Chainload a specific EFI application FROM a specific volume.
 *
 *   parent_image - ForeB's image handle (parent of the LoadImage call).
 *   st           - live system table (call BEFORE ExitBootServices).
 *   device       - the SimpleFileSystem/BlockIo handle the loader lives on
 *                  (as returned in foreb_chain_entry.device).
 *   path         - ESP-relative CHAR16 path to the loader, e.g.
 *                  L"\\EFI\\BOOT\\BOOTX64.EFI".
 *
 * Returns: does NOT return on success (the chained loader takes over). If it
 * returns, the value is the failure status (EFI_LOAD_ERROR, EFI_NOT_FOUND, ...).
 */
EFI_STATUS chain_boot(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
                      EFI_HANDLE device, const CHAR16 *path);

/*
 * Convenience: chainload the FIRST discovered candidate (chain_list order).
 * Used by a `type=chainload` entry with an empty `chain=` (auto-scan). Returns
 * an error if nothing was found or the boot attempt returned.
 */
EFI_STATUS chain_boot_first(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st);

/*
 * Scan all SimpleFileSystem volumes for a Windows Boot Manager
 * (\EFI\Microsoft\Boot\bootmgfw.efi) and append every hit to *out (kind
 * FOREB_CHAIN_WINDOWS). *out is NOT reset - existing entries are preserved, so
 * this can be layered after chain_list() or called standalone. Returns the
 * number of Windows candidates appended (>=0), or -1 on a hard error.
 */
int chain_find_windows(EFI_HANDLE parent_image, EFI_SYSTEM_TABLE *st,
                       struct foreb_chain_list *out);

#ifdef __cplusplus
}
#endif

#endif /* FOREB_UEFI_CHAINLOAD_H */
