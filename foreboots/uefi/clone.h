/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/clone.h - drive CLONE tool (a wm.c "template B" GUI window).
 * =============================================================================
 * A windowed tool that clones one block device onto another (or onto an image
 * file on the ESP) using the corruption-tolerant diskio layer, so a failing /
 * partially-corrupt source is copied ddrescue-style (unreadable sectors are
 * zero-filled and counted rather than aborting the copy). The source is only
 * ever READ; the destructive destination write requires an explicit on-screen
 * confirmation before it starts (never auto-runs).
 *
 * Follows the tools.h "template B" contract: tool_clone_open() calls wm_open()
 * and returns immediately; the bootx64.c menu loop drives input, compositing and
 * ui_present(). The one exception is the copy itself: once the user confirms, the
 * clone runs SYNCHRONOUSLY from inside the window's event callback (acceptable
 * pre-ExitBootServices) and pumps ui_progress()/ui_status()/ui_present() every
 * chunk so the progress bar animates. ESC during the copy aborts it.
 *
 * Freestanding (no libc), pre-ExitBootServices. Fixed pools; the only heap use is
 * one BootServices AllocatePool chunk buffer, freed when the copy finishes.
 * ========================================================================== */
#ifndef FOREB_UEFI_CLONE_H
#define FOREB_UEFI_CLONE_H

#include "efi.h"

/*
 * One-time init: captures the loader image handle + system table (needed to open
 * the ESP for image-file clones and to poll ConIn for an abort key) and calls
 * diskio_init(). Call once after tools_init(), before tool_clone_open().
 * NULL-safe (the tool then reports "not initialised").
 */
void tool_clone_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

/*
 * Open the Clone tool as a wm window (template B: returns immediately). The menu
 * loop drives it. Idempotent: raises the existing window if already open.
 */
void tool_clone_open(void);

#endif /* FOREB_UEFI_CLONE_H */
