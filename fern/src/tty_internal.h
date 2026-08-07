#ifndef TTY_INTERNAL_H
#define TTY_INTERNAL_H

// Kernel-internal interface shared between tty.c and tty_render.c ONLY.
//
// This header is NOT part of the public TTY API (see include/tty.h for
// that) and must not be included by any translation unit other than
// tty.c/tty_render.c. It exists purely to let the status-bar/CPU-dot/
// VT-transition rendering code that lives in tty_render.c interoperate
// with the cursor/mode state that stays file-local (static) to tty.c,
// without widening include/tty.h's public surface.

#include <stdbool.h>
#include <stdint.h>

// TTY status bar height, shared by tty.c (row-count/framebuffer-Y-offset/
// scroll-start-row math) and tty_render.c (status-bar gradient/CPU-dots/
// scroll-indicator positioning). Gradient/accent colors remain decoration
// owned by tty_render.c's tty_draw_status_bar().
#define TTY_STATUS_BAR_HEIGHT 24

// -----------------------------------------------------------------------
// tty.c -> tty_render.c
//
// Narrow accessor into tty.c's file-local tty_state, used by
// tty_draw_scroll_indicator() so the scroll thumb can be positioned
// without exposing the full tty_state struct across the translation
// unit boundary. Returns false (outputs left untouched) when there is
// no active cell buffer, mirroring the original in-tty.c early return.
// -----------------------------------------------------------------------
bool tty_internal_get_scroll_state(uint16_t* rows, uint16_t* char_height, uint16_t* cursor_y);

// Login-status / current-user text buffers backing the status bar.
// Storage lives in tty.c (owned by tty_set_login_status_text() /
// tty_set_current_user_text(), which stay in tty.c since they are not
// part of the rendering cluster), but tty_render.c's status-bar drawing
// and login-state update paths also read them, and the logged-out path
// clears g_current_user directly.
#define TTY_INTERNAL_LOGIN_STATUS_LEN 64
#define TTY_INTERNAL_CURRENT_USER_LEN 32
extern char g_login_status[TTY_INTERNAL_LOGIN_STATUS_LEN];
extern char g_current_user[TTY_INTERNAL_CURRENT_USER_LEN];

// -----------------------------------------------------------------------
// tty_render.c -> tty.c
//
// Rendering entry points that used to be `static` in tty.c and now live
// in tty_render.c, but are still invoked directly by tty.c's boot-mode/
// VT-switch/screen-flush logic.
// -----------------------------------------------------------------------
void tty_display_cpu_dots(void);
void tty_draw_fade_overlay(uint8_t opacity);
void tty_draw_scroll_indicator(void);
void tty_draw_transition_screen(const char* label);

#endif // TTY_INTERNAL_H
