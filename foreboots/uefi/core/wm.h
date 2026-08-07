/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/wm.h - tiny compositor / window manager over the ui.c back buffer.
 * =============================================================================
 * Draggable, z-ordered, focusable windows with a title bar + close button, all
 * composited into the ui.c off-screen back buffer (so ui_present() flips them
 * tear-free). Each window carries a DRAW callback (paints its client area with
 * ui.c primitives) and an optional EVENT callback (keyboard + mouse in client
 * coordinates). Pointer interaction (titlebar drag, raise-on-click, close box)
 * is driven from a mouse_state (input.h); keyboard from EFI_INPUT_KEY. Colors +
 * skin come from struct forebo_theme.
 *
 * Per-frame contract (menu loop):
 *     if (wm_active_count()) wm_run_frame(&ms, keyp);   // update: drag/focus/close/events
 *     ... draw background + menu ...
 *     wm_draw();                                         // composite windows on top
 *     input_draw_cursor(&ms, col);                       // cursor above everything
 *     ui_present();
 *
 * Freestanding (no libc), pre-ExitBootServices. Fixed window pool, no heap.
 * ========================================================================== */
#ifndef FOREB_UEFI_WM_H
#define FOREB_UEFI_WM_H

#include "../efi.h"
#include "../../include/forebo_cfg.h"
#include "input.h"

#define WM_MAX_WINDOWS  8
#define WM_TITLE_LEN    48

struct wm_window;
typedef struct wm_window wm_window;

/* Event kinds delivered to a window's event callback. Mouse coordinates are
 * CLIENT-relative (0,0 = client top-left). */
enum {
    WM_EV_OPEN = 0,      /* window just opened                                */
    WM_EV_KEY,           /* keyboard: scancode + unicode                      */
    WM_EV_MOUSE_DOWN,    /* left/right press inside the client area           */
    WM_EV_MOUSE_UP,      /* button release (focused window)                   */
    WM_EV_MOUSE_MOVE,    /* pointer moved over the focused client area        */
    WM_EV_CLOSE,         /* window is about to close                          */
    WM_EV_MOUSE_WHEEL    /* wheel notch(es): `wheel` holds the signed delta   */
};

typedef struct {
    int    type;         /* one of WM_EV_*                                    */
    int    mx, my;       /* client-relative pointer (mouse events)            */
    int    button;       /* 0 = left, 1 = right (mouse events)                */
    UINT16 scancode;     /* EFI scan code (WM_EV_KEY)                         */
    CHAR16 unicode;      /* EFI unicode char (WM_EV_KEY)                      */
    int    wheel;        /* signed wheel delta (WM_EV_MOUSE_WHEEL), else 0    */
} wm_event;

/* Paint the window's client area. (cx,cy) is the client origin in SCREEN
 * coordinates; (cw,ch) the client size. Use ui.c draw primitives. */
typedef void (*wm_draw_cb)(wm_window *w, int cx, int cy, int cw, int ch);

/* Handle an event. Return WM_CLOSE_REQUEST to have the WM close this window,
 * else 0. May be NULL. */
typedef int  (*wm_event_cb)(wm_window *w, const wm_event *ev);

#define WM_CLOSE_REQUEST 1

/* Initialise the WM against a theme (colors + window skin). Closes all windows.
 * `theme` may be NULL to use the built-in default palette. */
void wm_init(const struct forebo_theme *theme);

/* Provide BootServices for the window content cache (AllocatePool/FreePool).
 * Call once from efi_main before the menu loop; NULL-safe (disables caching). */
void wm_init_cache(EFI_BOOT_SERVICES *bs);

/* Re-skin every OPEN window against a new theme WITHOUT closing anything (unlike
 * wm_init). Used by the Theme/Settings tool to apply color/skin edits live while
 * its own window stays open. NULL-safe (ignored). */
void wm_set_theme(const struct forebo_theme *theme);

/* Optional custom chrome faces (img_window / img_titlebar / img_button). Any
 * pointer may be NULL to keep the drawn look. Owned by the caller (bootx64). */
struct img_image;   /* defined in image.h */
void wm_set_images(const struct img_image *window,
                   const struct img_image *titlebar,
                   const struct img_image *button);

/*
 * Open a centered window of client-inclusive size (w,h) with the given title +
 * callbacks. Returns a handle (raised to top + focused) or NULL if the pool is
 * full. `draw` may be NULL (blank client). `user` is opaque caller data.
 */
wm_window *wm_open(const char *title, int w, int h,
                   wm_draw_cb draw, wm_event_cb evcb, void *user);

/* Close a window (fires WM_EV_CLOSE). NULL-safe. */
void wm_close(wm_window *w);
/* Close every open window. */
void wm_close_all(void);

/* Number of open windows. */
int  wm_active_count(void);
/* The focused (top-most) window, or NULL. */
wm_window *wm_focused(void);

/*
 * Per-window animation / damage gating for the main loop. wm_window_set_animated()
 * flags a window (by handle id) as continuously animating; combined with drag and
 * event state it lets the loop skip forcing a full-scene recomposite every frame
 * while a window is open. wm_wants_repaint() returns 1 when a repaint is actually
 * needed this frame: any OPEN window is flagged animated, a titlebar/window drag is
 * in progress, or wm_run_frame() dispatched an event this frame; else 0.
 */
void wm_window_set_animated(int win_id, int on);
int  wm_wants_repaint(void);
/* Pool id of a window handle (the win_id wm_window_set_animated() expects), or
 * -1 if `w` is not a live handle. Lets a caller that only kept the wm_open()
 * pointer flag itself animated: wm_window_set_animated(wm_window_id(win), 1). */
int  wm_window_id(wm_window *w);

/*
 * Per-frame update: raise-on-click, titlebar drag, close box, and event
 * dispatch to the focused window (mouse in client coords + keyboard). `m` and
 * `key` may be NULL. Does NOT draw - call wm_draw() during compositing.
 */
void wm_run_frame(mouse_state *m, EFI_INPUT_KEY *key);

/* Composite every window back-to-front into the ui.c back buffer. */
void wm_draw(void);

/* Accessors usable from callbacks. */
void *wm_user(wm_window *w);
int   wm_client_w(wm_window *w);
int   wm_client_h(wm_window *w);
/* Non-client height (title bar + bottom border) the WM adds around the
 * client area. Add this to a desired CONTENT height when sizing a window
 * with wm_open(), whose `h` is the whole-window height. */
int   wm_chrome_h(void);

/* ------------------------------------------------------------------ */
/*  Theme exposure + reusable button widget                            */
/* ------------------------------------------------------------------ */
/* The theme colors the WM adopted in wm_init()/wm_set_theme(). */
enum {
    WM_COL_WINDOW = 0,   /* client background      */
    WM_COL_FG,           /* default text           */
    WM_COL_ACCENT,       /* highlights / focus     */
    WM_COL_SEL_BG,       /* selected-row bg        */
    WM_COL_SEL_FG,       /* selected-row fg        */
    WM_COL_TITLEBAR      /* title bar              */
};
UINT32 wm_theme_color(int which);
/* Linear blend of two 0x00RRGGBB colors, t in 0..256 (same helper the WM
 * uses internally; exposed so tools can derive hover shades on-theme). */
UINT32 wm_blend(UINT32 a, UINT32 b, int t);

/*
 * A clickable push-button, positioned in CLIENT coordinates of whichever
 * window is being drawn. Typical use from a window's draw callback:
 *     wm_button b = { x, y, w, h, ID, 1, "Open" };
 *     wm_button_draw(&b, hover_id == ID, press_id == ID);
 * and from the event callback: wm_button_hit(&b, ev->mx, ev->my).
 * Geometry is caller-owned (compute it in ONE shared helper so the draw and
 * the hit-test can never disagree). No state lives in the WM.
 */
typedef struct wm_button {
    int  x, y, w, h;     /* client-relative rect                       */
    int  id;             /* caller tag returned by hit tests           */
    int  enabled;        /* 0 = dimmed, no hover/press                 */
    char label[28];      /* centered text (clipped to the button)      */
} wm_button;

/* Paint a button with the current theme + window_skin bevel look: raised
 * when idle, brighter on hover, pressed-in when `pressed`, dimmed when
 * disabled. MUST be called from a window draw callback (it needs the client
 * origin the WM sets up while compositing); a button that does not fit fully
 * inside the client area is skipped (graceful clip). */
void wm_button_draw(const wm_button *b, int hover, int pressed);
/* Client-coordinate point-in-rect test. */
int  wm_button_hit(const wm_button *b, int mx, int my);
/* Natural pixel width for `label` (text + horizontal padding). */
int  wm_button_measure(const char *label);
/* Standard button height (glyph + vertical padding). */
int  wm_button_h(void);

#endif /* FOREB_UEFI_WM_H */
