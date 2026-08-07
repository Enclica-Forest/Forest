#ifndef TTY_RENDER_H
#define TTY_RENDER_H

// Internal-only interface between tty.c and tty_render.c.
//
// This header is NOT part of the public TTY API (see include/tty.h for
// that) and must not be included by any consumer outside tty.c/tty_render.c.
// It exists solely to let the two translation units call into each other's
// file-local rendering entry points after tty_render.c was split out of
// tty.c to isolate the framebuffer/status-bar drawing code.

#include <stdbool.h>
#include <stdint.h>

// -----------------------------------------------------------------------
// tty_render.c -> tty.c
//
// Rendering entry points that used to be `static` in tty.c and are now
// defined in tty_render.c, but are still invoked directly by tty.c's
// session/VT-switch logic (tty_exit_boot_mode, tty_switch_vt, tty_flush_screen,
// tty_flush_screen_full).
// -----------------------------------------------------------------------
void tty_display_cpu_dots(void);
void tty_draw_transition_screen(const char* label);
void tty_draw_fade_overlay(uint8_t opacity);
void tty_draw_scroll_indicator(void);

// -----------------------------------------------------------------------
// tty.c -> tty_render.c
//
// Narrow accessor into tty.c's file-local tty_state, used by
// tty_draw_scroll_indicator() so the scroll thumb can be positioned without
// exposing the full tty_state struct across the translation-unit boundary.
// Returns false (and leaves the outputs untouched) if there is no active
// cell buffer to scroll, mirroring the original in-tty.c early-return.
// -----------------------------------------------------------------------
bool tty_render_get_scroll_state(uint16_t* rows, uint16_t* char_height, uint16_t* cursor_y);

#endif // TTY_RENDER_H
