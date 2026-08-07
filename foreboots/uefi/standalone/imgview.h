/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/imgview.h - Windowed GUI image viewer (browse + preview BMP/TGA).
 * =============================================================================
 * A "template B" wm.c tool (see tools.h): tool_imgview_open() calls wm_open()
 * and returns immediately; the bootx64.c menu loop drives input + compositing.
 *
 * The viewer browses a filesystem starting at the ESP root (the same
 * esp_open_root()/EFI_FILE_PROTOCOL directory listing the File Browser uses),
 * and when the user opens a .bmp/.tga file it decodes it with the image.c
 * decoder and displays it scaled-to-fit (letterboxed) inside the window client
 * rect, with the filename + WxH. Left/Right or PgUp/PgDn step to the prev/next
 * image in the same directory; a Fit / 1:1 toggle switches between fit-to-window
 * and actual-size; Esc leaves the preview (or closes the window from the list).
 *
 * Freestanding (no libc), pre-ExitBootServices. Fixed static pools, no heap
 * beyond BootServices AllocatePool (the file read buffer, freed after decode).
 * ========================================================================== */
#ifndef FOREB_UEFI_IMGVIEW_H
#define FOREB_UEFI_IMGVIEW_H

#include "efi.h"

/*
 * Capture the loader image handle + system table the viewer needs to open the
 * boot volume (ESP) and AllocatePool the file read buffer. Call once, the same
 * way tools_init() is called, BEFORE tool_imgview_open(). NULL-safe: if never
 * called (or passed NULL) the viewer opens but shows a "could not open volume"
 * message instead of crashing.
 */
void tool_imgview_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

/*
 * Open the Image Viewer as a wm window (template B) and return immediately.
 * Idempotent: a no-op while the window is already open.
 */
void tool_imgview_open(void);

#endif /* FOREB_UEFI_IMGVIEW_H */
