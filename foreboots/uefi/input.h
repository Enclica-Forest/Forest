/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/input.h - Pointer (mouse / touch) input + software cursor.
 * =============================================================================
 * A tiny freestanding (no libc) pointer layer for the UEFI loader. It locates
 * EFI_SIMPLE_POINTER_PROTOCOL (relative PS/2-style mice) and/or
 * EFI_ABSOLUTE_POINTER_PROTOCOL (tablet/touch - the QEMU `-device usb-tablet`
 * default), then folds their motion + buttons into a single `mouse_state`
 * each poll. Because OVMF connects USB devices ASYNCHRONOUSLY, the menu loop
 * re-scans via input_rescan() until (and after) a device binds; re-scans never
 * double-bind or re-reset a live device. Absolute coordinates take priority
 * (they map linearly onto the GOP resolution); relative motion is accumulated.
 * The one logical cursor is clamped to the screen. Its arrow sprite is
 * composited onto the ui.c BACK BUFFER (draw it LAST, right before
 * ui_present()) so the double buffer erases + redraws it every frame with no
 * manual restore.
 *
 * Per-frame use:
 *     input_poll(&ms);              // update from firmware pointer devices
 *     ... draw scene / windows ...
 *     input_draw_cursor(&ms, col);  // sprite on top of the back buffer
 *     ui_present();                 // flip
 *
 * All device state lives inside the caller-owned `mouse_state` (no module
 * globals), so it is trivially re-entrant. Pointer protocols are BootServices-
 * provided: poll only BEFORE ExitBootServices.
 * ========================================================================== */
#ifndef FOREB_UEFI_INPUT_H
#define FOREB_UEFI_INPUT_H

#include "efi.h"

/* Maximum pointer instances of each class we bind. Under OVMF the same
 * protocol GUID is typically installed on several handles (the real USB device
 * AND the ConSplitter aggregate); we bind and poll ALL of them, so this must be
 * comfortably larger than 1. */
#define INPUT_MAX_PTR 8

typedef struct {
    /* --- public per-poll snapshot (pixels, top-left origin) --- */
    int x, y;                 /* cursor position (clamped to screen)          */
    int dx, dy;               /* movement since the previous poll             */
    int wheel;                /* wheel delta this poll (device units)         */
    int left, right;          /* CURRENT button state (1 = pressed)           */
    int left_pressed;         /* rising edge: press began this poll           */
    int left_released;        /* falling edge: press ended this poll          */
    int right_pressed;
    int right_released;
    int moved;                /* dx || dy this poll                           */
    int present;              /* # of pointer devices bound (0 = none)        */

    /* --- private device/handle state (do not touch) --- *
     * We locate EVERY handle exposing each protocol (LocateHandleBuffer, not a
     * single LocateProtocol) and poll them all each frame, merging the result.
     * Binding only the first arbitrary instance is the classic reason an OVMF
     * cursor draws but never tracks. Bound EFI_HANDLEs are remembered so a
     * re-scan (OVMF enumerates USB asynchronously - the physical pointer often
     * appears seconds AFTER the menu started) never double-binds or re-resets
     * a live device. */
    int    n_abs;                              /* # absolute devices bound     */
    int    n_simple;                           /* # simple/relative bound      */
    void  *abs_dev[INPUT_MAX_PTR];             /* EFI_ABSOLUTE_POINTER_PROTOCOL* */
    void  *abs_hnd[INPUT_MAX_PTR];             /* EFI_HANDLE it was bound from */
    UINT64 amin_x[INPUT_MAX_PTR], amax_x[INPUT_MAX_PTR];
    UINT64 amin_y[INPUT_MAX_PTR], amax_y[INPUT_MAX_PTR];
    UINT8  abs_live[INPUT_MAX_PTR];            /* ever reported a non-origin   */
    void  *simple_dev[INPUT_MAX_PTR];          /* EFI_SIMPLE_POINTER_PROTOCOL* */
    void  *simple_hnd[INPUT_MAX_PTR];          /* EFI_HANDLE it was bound from */
    void  *bs;                                 /* BootServices for re-scan     */
    int    screen_w, screen_h;
    int    prev_left, prev_right;

    /* --- direct i8042 PS/2 mouse fallback (x86 only) ------------------------ *
     * Many firmwares (notably the OVMF build shipped here) expose NO working EFI
     * pointer - the only handle carrying the pointer GUIDs is the edk2
     * ConSplitter aggregate, whose GetState never delivers motion, so the cursor
     * draws but is frozen. When the firmware keyboard is USB (or absent from the
     * i8042), we own the i8042 aux port and read the PS/2 mouse ourselves. This
     * also covers real laptop trackpads, which are PS/2 behind the i8042. */
    int    ps2_ok;                             /* aux stream initialised        */
    UINT8  ps2_pkt[4];                         /* 3/4-byte packet accumulator   */
    int    ps2_idx;                            /* bytes collected so far        */
    int    ps2_bytes;                          /* 3 (std) or 4 (IntelliMouse)   */
} mouse_state;

/*
 * Locate the pointer protocols via BootServices and center the cursor on the
 * given GOP resolution (pass ui_width()/ui_height()). Zeroes *m first, then
 * performs an initial input_rescan(). `bs` may be NULL (no device bound:
 * present==0, keyboard-only via input_nudge). Safe to call more than once.
 */
void input_init(EFI_BOOT_SERVICES *bs, mouse_state *m, int screen_w, int screen_h);

/*
 * Re-enumerate the pointer protocols and bind only handles that are NEW since
 * the last scan (OVMF connects USB devices asynchronously; the physical
 * tablet/mouse commonly shows up after the menu is already running). Already-
 * bound devices are left untouched - in particular they are NOT Reset again
 * (resetting a live PS/2-style stream loses packets). `bs` may be NULL, in
 * which case the BootServices pointer captured by input_init() is reused.
 * Updates m->present; safe to call at any cadence (it is cheap and idempotent).
 */
void input_rescan(EFI_BOOT_SERVICES *bs, mouse_state *m);

/* 1 if at least one pointer device was bound. */
int  input_available(const mouse_state *m);

/*
 * Poll the bound device(s) and update *m (position, deltas, buttons + rising/
 * falling edges). Returns 1 if anything changed (movement or a button edge).
 * A poll with no new firmware data yields dx==dy==0 and keeps the last position.
 */
int  input_poll(mouse_state *m);

/* Move the cursor by (dx,dy) pixels from the keyboard; clamps to the screen. */
void input_nudge(mouse_state *m, int dx, int dy);

/*
 * Composite the arrow cursor onto the ui.c back buffer at (m->x,m->y) with body
 * color `fill` (0x00RRGGBB) and a dark 1px outline. Draw LAST (after menu +
 * windows), before ui_present(). Hot-spot is the sprite's top-left pixel.
 */
void input_draw_cursor(const mouse_state *m, UINT32 fill);

#endif /* FOREB_UEFI_INPUT_H */
