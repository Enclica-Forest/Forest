/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools.h - Windowed GUI tool registry + launcher.
 * =============================================================================
 * A small table-driven registry of self-contained GUI tools, each rendered as
 * a wm.c window (title bar, drag, close box) that composites into the ui.c back
 * buffer and is driven by the shared mouse_state (input.h) + EFI_INPUT_KEY.
 *
 * DESIGN CONTRACT (how a tool lives in the frame loop)
 * ----------------------------------------------------
 * Every tool here follows "template B" from bootx64.c (the lightweight in-menu
 * window pattern used by open_about_window()/open_recovery_window()): a tool's
 * open() function just calls wm_open(title, w, h, draw_cb, evcb, user) and
 * returns immediately. It does NOT run its own nested frame loop. The existing
 * run_menu_animated() loop in bootx64.c already:
 *     - routes all pointer + keyboard input to the compositor while
 *       wm_active_count() > 0 (wm_run_frame),
 *     - composites every open window via wm_draw(),
 *     - draws the cursor and flips with ui_present() once per frame.
 * This keeps every tool non-modal and mutually stackable (up to WM_MAX_WINDOWS
 * = 8 windows at once, so the launcher + a few tools can coexist).
 *
 * Tools that need to perform an action which itself spawns a nested loop (e.g.
 * chainloading, opening the full shell) must defer it the way recovery.c does:
 * stash a "pending action" in the tool's user-data struct from the event
 * callback and let bootx64.c execute it OUTSIDE the wm frame. Pure read-only
 * inspectors (the majority below) need none of that.
 *
 * SHARED SERVICES each tool draws on
 * ----------------------------------
 *   wm.c    : wm_open/wm_close, wm_user(), wm_client_w/h(), wm_event routing.
 *   ui.c    : fill_rect, draw_rect_outline, draw_string[_center], draw_char,
 *             draw_hline/vline  (client origin is in SCREEN coords, per wm.h).
 *   input.h : mouse_state (position, buttons, wheel) via the wm_event the WM
 *             synthesizes; wheel deltas for scrolling lists.
 *   fs      : config.c esp_open_root()/esp helpers + fs_ext.c/fs_btrfs.c for the
 *             File Browser and Partition Browser.
 *   blockio : EFI_BLOCK_IO_PROTOCOL / EFI_DISK_IO_PROTOCOL over
 *             LocateHandleBuffer for Disk Info, GPT Viewer, Hex Viewer (raw
 *             sector reads), Partition Browser.
 *   RT/BS   : gST->RuntimeServices GetVariable/SetVariable/GetNextVariableName
 *             for EFI Variables + Boot Manager; gBS->GetMemoryMap for Memory
 *             Map; gST->FirmwareVendor/Revision + gBS/CPU for System Info.
 *
 * Freestanding (no libc), pre-ExitBootServices. Fixed pools, no heap.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_H
#define FOREB_UEFI_TOOLS_H

#include "efi.h"
#include "wm.h"
#include "ui.h"
#include "input.h"
#include "../include/forebo_cfg.h"

/* ------------------------------------------------------------------ */
/*  One registered GUI tool                                           */
/* ------------------------------------------------------------------ */
/*
 * name : short display label shown in the launcher list + used as the wm
 *        window title (kept < WM_TITLE_LEN).
 * desc : one-line description shown under the selection in the launcher.
 * icon : short icon name resolved to /forebo/icons/<icon>.tga by the same
 *        name->path map used for menu entries (see tools_icon_path()); may be
 *        "" for no icon.
 * open : opens the tool as a wm window (template B) and returns immediately.
 *        Must be safe to call when the tool is already open (either raise the
 *        existing window or open a second instance up to WM_MAX_WINDOWS).
 */
struct forebo_tool {
    const char *name;
    const char *desc;
    const char *icon;
    void      (*open)(void);
};

/* ------------------------------------------------------------------ */
/*  Registry (defined in tools.c)                                     */
/* ------------------------------------------------------------------ */
extern const struct forebo_tool forebo_tools[];
extern const int                forebo_tools_count;

/*
 * Initialise the tools layer. Call once after wm_init()/input_init(), before any
 * tool is opened.
 *
 *   image : the loader's EFI_HANDLE (its DeviceHandle == the ESP) - used by the
 *           File Browser + Hex Viewer to open the boot volume.
 *   st    : the EFI system table. BootServices (LocateHandleBuffer for block IO,
 *           GetMemoryMap, GOP) AND RuntimeServices (GetVariable/SetVariable/
 *           GetNextVariableName/ResetSystem for EFI Variables, Boot Manager,
 *           Firmware Setup) AND FirmwareVendor/Revision (System Info) are all
 *           reached through it - `bs` alone cannot provide RuntimeServices or the
 *           firmware vendor string, so the whole table is threaded in.
 *   cfg   : the live boot config. cfg->theme provides tool window colors and is
 *           MUTATED live by the Theme/Settings tool. May be NULL (built-in
 *           palette + no config-dependent features).
 *
 * NULL-safe throughout (tools then show "N/A" / "no config").
 */
void tools_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, struct forebo_config *cfg);

/*
 * Open the Tools launcher: a wm window listing forebo_tools[] with icons +
 * descriptions, keyboard Up/Down + Enter and mouse hover/click/wheel. Selecting
 * an entry calls that tool's open(). Returns immediately (template B); the menu
 * loop drives it. Idempotent (raises the existing launcher if already open).
 * This is what FOREB_ENTRY_TOOLS dispatches to from menu_activate().
 */
void tools_launcher_open(void);

/* ------------------------------------------------------------------ */
/*  Icon-name resolution (shared with menu entries)                   */
/* ------------------------------------------------------------------ */
/*
 * Resolve a short icon name to an ESP-absolute TGA path.
 *   - If `name` already contains a path separator ('/' or '\\') or a known
 *     image extension (".tga"/".bmp"), it is returned unchanged (raw path).
 *   - Otherwise it is rewritten to "/forebo/icons/<name>.tga".
 * Writes into caller buffer `out` (>= FOREB_CFG_PATH_LEN). Returns `out`.
 * This is the single source of truth for both forebo.cfg 'icon=' shorthand and
 * the tool registry's `icon` field.
 */
char *tools_icon_path(const char *name, char *out, unsigned long out_len);

/* ------------------------------------------------------------------ */
/*  Firmware setup (FOREB_ENTRY_FWSETUP)                              */
/* ------------------------------------------------------------------ */
/*
 * Request a reboot into the firmware/UEFI setup screen:
 *   1. GetVariable("OsIndicationsSupported", &EFI_GLOBAL_VARIABLE) and test the
 *      EFI_OS_INDICATIONS_BOOT_TO_FW_UI (0x1) bit.
 *   2. If supported: read-modify-write "OsIndications" OR-ing that bit (NV +
 *      BS + RT access), then ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL).
 *   3. If NOT supported: return a negative status and do nothing (caller shows
 *      a graceful "firmware setup not supported" message and stays in the menu).
 * Returns 0 on a successful request (does not return if the reset fires),
 * <0 if the firmware does not advertise support, or a positive EFI_STATUS-ish
 * code on a variable-service error. `bs`/RT reached via gST.
 */
int tools_enter_firmware_setup(void);

/* 1 if the firmware advertises EFI_OS_INDICATIONS_BOOT_TO_FW_UI, else 0. Lets
 * the menu grey-out / annotate the "Firmware Setup" entry honestly. */
int tools_firmware_setup_supported(void);

/* OsIndications bit + global-variable GUID (mirrors the UEFI spec). */
#define EFI_OS_INDICATIONS_BOOT_TO_FW_UI  0x0000000000000001ULL

/* ------------------------------------------------------------------ */
/*  The individual tool open() functions (defined in tools.c)         */
/* ------------------------------------------------------------------ */
/* Each opens one wm window; see GUI_TOOLS.md for the full per-tool spec. */
void tool_diskinfo_open(void);   /* Disk Info: block-IO devices, size, LBA sz */
void tool_gptview_open(void);    /* GPT Viewer: GPT header + partition table   */
void tool_partbrowse_open(void); /* Partition Browser: FS type/label/free per  */
                                 /*   partition (ext4/btrfs/FAT via fs_*.c)    */
void tool_filebrowse_open(void); /* File Browser: navigate the ESP tree        */
void tool_hexview_open(void);    /* Hex Viewer: hexdump a sector/file, scroll  */
void tool_memmap_open(void);     /* Memory Map: GetMemoryMap descriptors       */
void tool_efivars_open(void);    /* EFI Variables: enumerate + inspect vars     */
void tool_bootmgr_open(void);    /* Boot Manager: BootOrder/Boot#### entries    */
void tool_sysinfo_open(void);    /* System/Firmware Info: vendor, rev, GOP, RAM */
void tool_settings_open(void);   /* Theme/Settings: live theme + toggles        */
void tool_colorpicker_open(unsigned int *target, void (*apply)(void));
                                 /* RGB colour picker -> writes *target live    */
void tool_keytest_open(void);    /* Key Tester: show scancode/unicode of keys   */
void tool_imgview_open(void);    /* Image Viewer: browse + preview BMP/TGA      */
void tool_clone_open(void);      /* Clone Drive: ddrescue-style disk/image clone*/
void tool_undelete_open(void);   /* Undelete / Carve: signature file recovery   */
void tool_calc_open(void);       /* Calculator: 64-bit integer + - * / %         */
void tool_clock_open(void);      /* Clock: live firmware RTC date + time         */
void tool_sysmon_open(void);     /* System Monitor: RAM/GOP/firmware/uptime      */

#endif /* FOREB_UEFI_TOOLS_H */
