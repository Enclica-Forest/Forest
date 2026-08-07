/*
 * ui.h - Self-contained GOP framebuffer UI for the ForeB UEFI loader.
 *
 * Draws DIRECTLY to the GOP linear framebuffer (no firmware text console,
 * no Blt) so every routine is valid both before and after ExitBootServices.
 * Freestanding: no libc is used or required.
 *
 * Coordinate system: pixels, top-left origin. Colors are packed 0x00RRGGBB
 * (see forebo_theme.h); the pixel-format handling (BGRX vs RGBX) is done
 * internally based on the format passed to ui_init().
 *
 * Double buffering: since this upgrade every drawing primitive writes into an
 * off-screen RAM back buffer allocated at ui_init(); ui_present() flips it to
 * the GOP VRAM front buffer in one fast blit (tear-free, flicker-free). The
 * public draw API is unchanged - callers just add a ui_present() once per frame
 * (and after each in-place progress/status update). If the back-buffer
 * allocation fails ui.c transparently falls back to drawing straight to VRAM
 * and ui_present() becomes a no-op, so old behavior is preserved.
 *
 * Typical usage:
 *     ui_init(gBS, fb_base, fb_pitch, fb_w, fb_h, mi->PixelFormat);
 *     ui_background();
 *     ui_menu(labels, count, selected, seconds_left);   // pure draw
 *     ui_present();                                      // flip to screen
 *     ...
 *     ui_progress("Loading kernel", read, total);       // per chunk, in place
 *     ui_present();
 *     ui_status("Staging segments...");                 // in place
 *     ui_present();
 */
#ifndef FOREB_UEFI_UI_H
#define FOREB_UEFI_UI_H

#include "efi.h"
#include "../include/forebo_theme.h"

/* -------- initialization / state -------------------------------------- */
/*
 * bs      : BootServices table (used once to AllocatePool the RAM back buffer).
 *           May be NULL, in which case drawing falls back to writing straight
 *           to VRAM (ui_present() then becomes a no-op).
 * fb_base : linear framebuffer physical/MMIO base address (the VRAM front buffer
 *           ui_present() flips to).
 * pitch   : bytes per scanline (PixelsPerScanLine * 4 for 32bpp).
 * width   : visible width  in pixels (HorizontalResolution).
 * height  : visible height in pixels (VerticalResolution).
 * pixfmt  : EFI_GRAPHICS_PIXEL_FORMAT value from mi->PixelFormat. Only
 *           PixelRedGreenBlueReserved8BitPerColor triggers an R<->B swap;
 *           every other format is treated as x86-default BGRX.
 */
void ui_init(EFI_BOOT_SERVICES *bs, UINT64 fb_base, UINT32 pitch,
             UINT32 width, UINT32 height, UINT32 pixfmt);

UINT32 ui_width(void);
UINT32 ui_height(void);
/* Auto-selected integer magnification (1x, or 2x on 1080p+ panels). Applied on
 * top of the per-call `scale` by every text routine below. */
int    ui_scale(void);

/* -------- double buffering -------------------------------------------- */
/*
 * Base address + stride of the buffer that every draw primitive writes to (the
 * off-screen RAM back buffer when one was allocated, else VRAM). Sibling draw
 * modules (image.c, anim.c) init themselves against these so they composite
 * into the SAME buffer as ui.c. Call after ui_init().
 */
UINT64 ui_backbuffer_base(void);
UINT32 ui_draw_pitch(void);
/* 1 when a real off-screen back buffer is active, 0 when drawing straight to VRAM. */
int    ui_double_buffered(void);

/*
 * Flip the back buffer to the GOP VRAM front buffer (one fast per-scanline
 * copy; a single block copy when strides match). No-op when there is no back
 * buffer (draws already landed on VRAM). Pure memory/MMIO: valid both before
 * and after ExitBootServices. Call once per composed frame.
 */
void ui_present(void);

/*
 * Dirty-rectangle presentation. ui_present() copies ONLY the scanline spans that
 * changed since the last flip (huge win on real hardware, where VRAM is uncached
 * write-combining MMIO and a full-frame blit at 60 fps is the dominant cost).
 * The ui.c primitives mark themselves automatically; call ui_mark_dirty() from
 * sibling writers that touch the back buffer directly (e.g. anim.c particles),
 * and ui_mark_all() after a full-screen repaint that bypassed the primitives
 * (image blit, fade) to force one whole-screen flip.
 */
void ui_mark_dirty(int x, int y, int w, int h);
void ui_mark_all(void);

/*
 * Last-frame dirty span (screen px) that ui_present() actually flipped: lets a
 * caller (bootx64.c) restore only the rows damaged last frame instead of the
 * whole back buffer. Reads the previous-frame span arrays (g_py0/g_py1/g_pmin/
 * g_pmax) recorded by the prior ui_present(); returns the full screen rect when
 * the last flip was a whole-screen (ui_mark_all) flip. Any out-pointer may be
 * NULL.
 */
void ui_prev_dirty_bbox(int *x, int *y, int *w, int *h);

/* Restore only the previous frame's per-row dirty spans from `src` (a snapshot
 * with the back buffer's layout, e.g. the static-scene cache) into the back
 * buffer - erases last frame's sprites for a few KB instead of a full-buffer
 * copy. Returns 1 on success, 0 if the caller must do a full restore (dirty
 * tracking off, or the last flip was whole-screen). */
int ui_restore_prev_spans(const void *src);

/* VSync: gate whole-screen flips on the VGA vertical-retrace (x86, where the GPU
 * keeps VGA I/O decode) so full-frame swaps land during blanking - no visible
 * tear. Auto-detected and bounded; a no-op where unavailable. Default on. */
void ui_set_vsync(int on);

/* -------- clip rectangle (compositor occlusion culling) ----------------- *
 * Every draw primitive intersects its output with the active clip rect; the
 * default is the whole screen. The window manager (wm.c) pushes each panel's
 * visible region before painting it so fully/partially covered panels skip
 * invisible work. push() intersects with the current clip; pop() restores.
 * Sibling writers that clip themselves (image.c blitters) read it via
 * ui_clip_get(). Balanced push/pop only; ui_clip_reset() forces full-screen. */
void ui_clip_reset(void);
void ui_clip_push(int x, int y, int w, int h);
void ui_clip_pop(void);
void ui_clip_get(int *x, int *y, int *w, int *h);

/* Fill the whole back buffer with a solid 0x00RRGGBB color / clear to theme BG. */
void ui_fill(UINT32 color);
void ui_clear(void);

/* -------- runtime theming --------------------------------------------- *
 * The whole menu/background palette is swappable at runtime. Select a named
 * preset ("forest" default, "midnight", "nord", "dracula", "gruvbox",
 * "solarized", "amber", "matrix", "rose", "ocean", "mono"); returns 1 if the
 * name matched. ui_theme_override() then lets forebo.cfg's individual color_*
 * keys tweak single entries on top (pass 0 / 0xFFFFFFFF to leave one alone).
 * ui_theme_count()/ui_theme_name() enumerate the presets (e.g. for a picker). */
int         ui_set_theme_by_name(const char *name);
void        ui_theme_override(UINT32 bg, UINT32 fg, UINT32 accent,
                              UINT32 sel_bg, UINT32 sel_fg);
int         ui_theme_count(void);
const char *ui_theme_name(int index);
UINT32      ui_theme_accent(void);
UINT32      ui_theme_title(void);

/* -------- menu layout / style ----------------------------------------- *
 * The whole boot-menu layout (panel position/size, selection style, borders,
 * alignment, icons, title/footer/timer visibility, ...) is data-driven. Pass a
 * struct forebo_style (from the parsed config); a NULL/empty preset resolves to
 * the built-in "classic" look. ui_style_count()/ui_style_name() enumerate the
 * 30 named presets. The icon compositor honors ui_style_show_icons()/right(). */
struct forebo_style;   /* defined in forebo_cfg.h */
void        ui_apply_style(const struct forebo_style *style);
int         ui_style_show_icons(void);
int         ui_style_icon_right(void);
int         ui_style_count(void);
const char *ui_style_name(int index);

/* -------- visual effects (blur / frosted glass / vignette / scanlines) ---- *
 * Operate on the back buffer before ui_present(); integer + channel-agnostic.
 * ui_fx_config() is fed from forebo.cfg; ui_backdrop() frosts what is behind a
 * window/panel (the "glass" skin). Vignette/scanlines force a full flip, so the
 * caller gates them to redraw frames. */
void ui_fx_config(int glass, int blur, int opacity, int vignette, int scanlines);
int  ui_fx_enabled(void);
int  ui_fx_vignette_amt(void);
int  ui_fx_scanline_amt(void);
void ui_blur_rect(int x, int y, int w, int h, int radius);
void ui_blend_rect(int x, int y, int w, int h, UINT32 color, int alpha);
void ui_backdrop(int x, int y, int w, int h);
void ui_vignette(int strength);
void ui_scanlines(int strength);

/* -------- configurable widgets (buttons / checkbox / slider) -------------- *
 * ui_apply_widgets() folds a struct forebo_widget (from forebo.cfg) onto the
 * built-in look. States: FBTN_NORMAL/HOVER/ACTIVE/FOCUSED/DISABLED. The Settings
 * dialog and window chrome draw through these so every control obeys the config. */
struct forebo_widget;  /* defined in forebo_cfg.h */
void   ui_apply_widgets(const struct forebo_widget *cfg);
void   ui_button(int x, int y, int w, int h, const char *label, int state);
int    ui_button_state(int x, int y, int w, int h, int mx, int my, int down);
int    ui_hit(int x, int y, int w, int h, int mx, int my);
void   ui_checkbox(int x, int y, int size, int checked, const char *label, int state);
void   ui_slider(int x, int y, int w, int h, int val, int max, int state);
int    ui_slider_value_at(int x, int w, int max, int mx);
UINT32 ui_wid_separator(void);
UINT32 ui_wid_scrollbar(void);
int    ui_wid_scrollbar_w(void);
UINT32 ui_wid_focus(void);
int    ui_wid_window_corner(void);

/* Optional custom image painted as the menu-panel face (img_panel=). NULL
 * restores the drawn (flat/gradient) fill. Owned by the caller (bootx64). */
struct img_image;   /* defined in image.h */
void   ui_set_panel_image(const struct img_image *img);

/* -------- primitives -------------------------------------------------- */
/* All are bounds-clipped; out-of-range pixels are silently dropped. */
void put_pixel(int x, int y, UINT32 color /* 0x00RRGGBB */);
/* Like put_pixel but does NOT mark the dirty rectangle; for bulk plotters that
 * mark their own aggregate bounding box once (e.g. particle scatter). */
void put_pixel_nomark(int x, int y, UINT32 color /* 0x00RRGGBB */);
void fill_rect(int x, int y, int w, int h, UINT32 color);
void draw_hline(int x, int y, int len, UINT32 color);
void draw_vline(int x, int y, int len, UINT32 color);
void draw_rect_outline(int x, int y, int w, int h, int thickness, UINT32 color);

/* -------- text (font8x16, integer-scaled) ----------------------------- *
 * Renders the crisp 8x16 CP437 cell (include/font8x16.h). The effective
 * magnification is `scale` * ui_scale(), so scale >= 1 magnifies each 8x16
 * glyph by that factor times the auto hi-res factor (advance is
 * 8*scale*ui_scale() px/char). If transparent != 0 the glyph background is
 * not drawn (only the ink pixels), otherwise every cell pixel is painted
 * fg/bg.
 */
void draw_char(int x, int y, char c, UINT32 fg, UINT32 bg,
               int transparent, int scale);
void draw_string(int x, int y, const char *s, UINT32 fg, UINT32 bg,
                 int transparent, int scale);
/* Horizontally centers the string on column cx. */
void draw_string_center(int cx, int y, const char *s, UINT32 fg, UINT32 bg,
                        int transparent, int scale);
/* Like draw_string but never paints past x+maxw px. If the full string would
 * exceed maxw it is truncated and a ".." ellipsis is appended so the visible
 * text plus ".." stays within maxw. maxw <= 0 (or too small for one glyph)
 * draws nothing. Use for any label that could overrun its window width. */
void draw_string_clip(int x, int y, int maxw, const char *s, UINT32 fg,
                      UINT32 bg, int transparent, int scale);

/* -------- high-level screens ------------------------------------------ */
/* Forest gradient background + tree logo. Clears the whole screen. */
void ui_background(void);

/*
 * Titled boot menu (pure draw, no input handling):
 *   entries[]     : NUL-terminated label strings.
 *   count         : number of entries.
 *   selected      : highlighted index (0..count-1).
 *   seconds_left  : countdown value; <0 hides the timer.
 * Caller redraws (background + this) on selection change / each tick.
 */
void ui_menu(const char *const entries[], int count, int selected,
             int seconds_left);

/* -------- boot-menu viewport / scrollbar / slide animation ------------ *
 * ui_menu() draws only the entries that fit inside the panel (a viewport)
 * and, when count exceeds the visible rows, a scrollbar on the panel's
 * right edge. The caller owns the scroll offset (first visible entry) and
 * pushes it in with ui_menu_set_scroll(); ui_menu() clamps it to a legal
 * range and the clamped value is read back with ui_menu_get_scroll(). */

/* Set / read the first-visible-entry scroll offset used by ui_menu(). */
void ui_menu_set_scroll(int first);
int  ui_menu_get_scroll(void);

/* Override the selected-row highlight bar's Y (screen px) for the slide
 * animation; pass a value < 0 to restore the natural (per-selection) Y. */
void ui_menu_set_highlight_y(int y);

/* Compute the boot-menu panel geometry so callers (icon blit, mouse hit
 * test, scrollbar drag) share ui_menu()'s exact layout. Any out-pointer may
 * be NULL. `vis` is the number of entry rows that fit in the viewport. */
void ui_menu_layout(int count, int *px, int *py, int *pw, int *ph,
                    int *eh, int *entries_top, int *vis);

/* Scrollbar track + thumb geometry (screen px). Returns 1 and fills the
 * out-params when a scrollbar is shown (count > visible rows), else 0. */
int  ui_menu_scrollbar(int count, int *track_x, int *track_y,
                       int *track_w, int *track_h,
                       int *thumb_y, int *thumb_h);

/*
 * In-place load progress bar drawn at the theme PROGRESS rect. Overwrites
 * the same region every call (never scrolls) so it is cheap enough to call
 * per read chunk. Draws: track, proportional fill (cur/total), a centered
 * percentage, and `label` above the bar. total == 0 renders as 100%/full.
 */
void ui_progress(const char *label, UINT64 cur, UINT64 total);

/* One in-place status line drawn just below the progress bar. */
void ui_status(const char *line);

#endif /* FOREB_UEFI_UI_H */
