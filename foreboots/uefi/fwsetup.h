/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/fwsetup.h - Reboot into the firmware / UEFI setup screen.
 * =============================================================================
 * Implements the standard UEFI "boot to firmware UI" flow (UEFI spec, the
 * OsIndications / OsIndicationsSupported global variables):
 *
 *   1. GetVariable("OsIndicationsSupported", EFI_GLOBAL_VARIABLE) and test the
 *      EFI_OS_INDICATIONS_BOOT_TO_FW_UI (0x1) bit. If the firmware does NOT
 *      advertise the bit it cannot honour the request -> report and stay.
 *   2. Read-modify-write "OsIndications" (NV | BS | RT access) OR-ing that bit.
 *   3. RuntimeServices->ResetSystem(EfiResetCold, ...) so the firmware, on the
 *      next boot, sees the pending indication and enters its setup UI.
 *
 * Self-contained: reaches every service through the EFI_RUNTIME_SERVICES pointer
 * the caller passes (gST->RuntimeServices in bootx64.c, sRT in shell.c), so it
 * holds no globals of its own. Freestanding, pre-ExitBootServices, no libc.
 * ==========================================================================*/
#ifndef FOREB_UEFI_FWSETUP_H
#define FOREB_UEFI_FWSETUP_H

#include "efi.h"

/* OsIndications bit that requests a reboot into the firmware/UEFI setup UI.
 * Guarded because tools.h / efi_ext.h also expose the same constant. */
#ifndef EFI_OS_INDICATIONS_BOOT_TO_FW_UI
#define EFI_OS_INDICATIONS_BOOT_TO_FW_UI  0x0000000000000001ULL
#endif

/* fw_boot_to_setup() return codes. */
#define FW_SETUP_OK           0    /* request issued; ResetSystem should fire   */
#define FW_SETUP_UNSUPPORTED (-1)  /* firmware does not advertise BOOT_TO_FW_UI  */
#define FW_SETUP_ERROR       (-2)  /* NULL services or a variable/reset error    */

/*
 * 1 if the firmware advertises EFI_OS_INDICATIONS_BOOT_TO_FW_UI in
 * OsIndicationsSupported, else 0. Lets a caller grey-out / annotate a
 * "Firmware Setup" entry honestly. `rt` may be NULL (returns 0).
 */
int fw_setup_supported(EFI_RUNTIME_SERVICES *rt);

/*
 * Request a reboot into firmware setup (see file header). On success this does
 * NOT return (the machine resets). Otherwise returns:
 *   FW_SETUP_UNSUPPORTED - firmware does not advertise the BOOT_TO_FW_UI bit,
 *   FW_SETUP_ERROR       - rt/services NULL, or SetVariable failed.
 */
int fw_boot_to_setup(EFI_RUNTIME_SERVICES *rt);

#endif /* FOREB_UEFI_FWSETUP_H */
