/*
 * shell.h - Interactive framebuffer shell interface (UEFI).
 *
 * INTEGRATION CONTRACT (used by bootx64.c). Implemented in uefi/shell.c.
 * The shell is entered by pressing 'c' at the boot menu; it renders on the
 * GOP framebuffer (ui.c primitives) and reads keys via ConIn. It runs in the
 * pre-ExitBootServices window so Boot Services, Simple File System, Block I/O
 * and Runtime variable services are all live (see SHELL.md for commands).
 *
 * The shell may mutate the in-memory `struct forebo_config` (e.g. `modules
 * add`, `background <file>`); those edits take effect on the next boot / menu
 * repaint. The caller repaints the menu after the shell returns.
 *
 * Recovery / disk-fix commands (implemented in shell.c, gated behind a typed
 * 'yes' where destructive): gpt <dev>, parts, fsprobe <dev>, scan <dev>,
 * rescue <srcdev> <dstfile|dstdev> [skip-bad], fatfix <dev>, ext-ls <dev>
 * [path], ext-cat <dev> <path>, btrfs-snaps <dev>. The ext and btrfs commands
 * wire the optional fs_ext.c / fs_btrfs.c back-ends (read-only) when built.
 * These are the tools the menu Recovery window / entry delegates to.
 */
#ifndef FOREB_UEFI_SHELL_H
#define FOREB_UEFI_SHELL_H

#include "../efi.h"
#include "../../include/forebo_cfg.h"

/* shell_run() return values. A value >= 0 is a menu-entry index to boot. */
#define FOREB_SHELL_BACK    (-1)   /* leave shell, return to the menu       */
#define FOREB_SHELL_REBOOT  (-2)   /* user asked to reboot                  */

/*
 * Run the interactive shell.
 *
 *   image    : EFI ImageHandle (for file / block IO; ESP is its DeviceHandle).
 *   st       : the EFI system table efi_main received (ConIn / BootServices /
 *              RuntimeServices). Required - the shell holds no globals of its
 *              own and reaches every service through this table.
 *   cfg      : live boot configuration (may be mutated by the shell; may be
 *              NULL, in which case config-dependent commands report "no config").
 *   cur_sel  : the menu entry currently selected (default target for `boot`
 *              with no argument and for `modules add`).
 *
 * ui_init() must already have been called (the shell draws with ui.c). The
 * shell does NOT call ExitBootServices; all work is pre-exit.
 *
 * Returns:
 *   >= 0                : boot this entry index now.
 *   FOREB_SHELL_BACK    : return to the boot menu (no boot; also Esc/'exit').
 *   FOREB_SHELL_REBOOT  : reboot the machine.
 *
 * Typical bootx64.c call site (inside run_boot_menu, on the 'c' key):
 *     int a = shell_run(ImageHandle, gST, &g_cfg, sel);
 *     if (a == FOREB_SHELL_REBOOT) do_reset();
 *     else if (a >= 0) return a;            // boot that entry
 *     // else FOREB_SHELL_BACK: fall through, repaint the menu
 */
int shell_run(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
              struct forebo_config *cfg, int cur_sel);

#endif /* FOREB_UEFI_SHELL_H */
