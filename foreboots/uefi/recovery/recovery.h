/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/recovery.h - Windowed Recovery / disk-tools panel (public API).
 * =============================================================================
 * A mouse-driven Recovery window composited over the ui.c double buffer with the
 * tiny window manager (wm.c) and pointer layer (input.c). It presents buttons +
 * an output log for the read-only recovery tools (list disks, GPT view, FS
 * probe, carve-scan) plus launchers for the interactive shell (rescue / free-
 * form commands) and chainload-to-USB. Destructive operations are only reachable
 * through the shell, which gates them behind a typed 'yes'.
 *
 * INTEGRATION CONTRACT (bootx64.c):
 *   - A menu entry of type FOREB_ENTRY_RECOVERY (see forebo_cfg.h) opens this
 *     window; a menu entry of type FOREB_ENTRY_SHELL opens shell_run(). Wire
 *     both at the entry-dispatch site (where `ent` is resolved after the menu
 *     returns), mirroring the existing 'c' -> shell_run() handler:
 *
 *         struct forebo_menuentry *ent = &g_cfg.entries[sel_entry];
 *         if (ent->type == FOREB_ENTRY_RECOVERY) {
 *             int a = recovery_run(ImageHandle, gST, &g_cfg, sel_entry);
 *             if (a == FOREB_RECOVERY_REBOOT) do_reset();
 *             if (a >= 0) { sel_entry = a; ...boot it... }
 *             else { repaint menu; continue; }   // FOREB_RECOVERY_BACK
 *         } else if (ent->type == FOREB_ENTRY_SHELL) {
 *             int a = shell_run(ImageHandle, gST, &g_cfg, sel_entry);
 *             ...same return handling...
 *         }
 *
 *   - config.c should ship two built-in default entries so the tools are one
 *     click away even with no forebo.cfg:
 *         menuentry "ForeB Shell" { type=shell }
 *         menuentry "Recovery"    { type=recovery }
 *
 * ui_init() (double buffer) must already be up. recovery_run() runs entirely
 * BEFORE ExitBootServices; it does not itself call ExitBootServices.
 * ========================================================================== */
#ifndef FOREB_UEFI_RECOVERY_H
#define FOREB_UEFI_RECOVERY_H

#include "../efi.h"
#include "../../include/forebo_cfg.h"

/* recovery_run() return values (mirror shell.h so bootx64 can treat them alike).
 * A value >= 0 is a menu-entry index to boot. */
#define FOREB_RECOVERY_BACK    (-1)   /* close, return to the menu (no boot)     */
#define FOREB_RECOVERY_REBOOT  (-2)   /* user asked to reboot                    */

/*
 * Open the Recovery Tools window and drive it modally until the user closes it
 * (close box / "Close" button / Esc) or triggers a boot/reboot.
 *
 *   image   : EFI ImageHandle (LoadedImage -> ESP; parent for chainload).
 *   st      : the EFI system table (ConIn / BootServices / RuntimeServices).
 *   cfg     : live boot configuration (theme colors + entries; may be NULL).
 *   cur_sel : the menu entry selected when Recovery was opened (default target
 *             for the shell's `boot` with no argument).
 *
 * Returns:
 *   >= 0                  : boot this entry index now.
 *   FOREB_RECOVERY_BACK   : return to the boot menu.
 *   FOREB_RECOVERY_REBOOT : reboot the machine.
 */
int recovery_run(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                 struct forebo_config *cfg, int cur_sel);

#endif /* FOREB_UEFI_RECOVERY_H */
