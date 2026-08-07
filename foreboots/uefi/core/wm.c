/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/wm.c - tiny compositor / window manager. See wm.h.
 * =============================================================================
 * Fixed pool of windows, painted back-to-front (g_order[] is the z-order, last
 * entry = top/focused). Every draw goes through ui.c primitives into the shared
 * off-screen back buffer; the caller flips with ui_present().
 * ========================================================================== */
#include "wm.h"
#include "../ui.h"
#include "image.h"
#include "../../include/forebo_theme.h"

struct wm_window {
    int         used;
    int         x, y, w, h;               /* whole-window rect (incl titlebar) */
    char        title[WM_TITLE_LEN];
    int         tlen;                     /* cached strlen(title), set at open  */
    wm_draw_cb  draw;
    wm_event_cb evcb;
    void       *user;
    int         dragging;
    int         drag_off_x, drag_off_y;
    int         wants_close;
    int         animated;                 /* continuously animates -> force repaint */
};

static struct wm_window g_win[WM_MAX_WINDOWS];
static int  g_order[WM_MAX_WINDOWS];       /* ids, back(0)-to-front(n-1)        */
static int  g_norder;
static struct forebo_theme g_theme;

/* Fast-path bookkeeping (avoid per-frame O(n) scans). */
static int  g_dragid = -1;                 /* window id mid-titlebar-drag, else -1 */
static int  g_any_wants_close = 0;         /* set when any window requests close   */
static int  g_event_frame = 0;             /* an event was dispatched this frame   */

/* Optional custom chrome faces (NULL = drawn look). Owned by the caller. */
static const struct img_image *g_win_img = 0;   /* window client face   */
static const struct img_image *g_tb_img  = 0;   /* title-bar face        */
static const struct img_image *g_btn_img = 0;   /* button face           */

void wm_set_images(const struct img_image *window,
                   const struct img_image *titlebar,
                   const struct img_image *button)
{
    g_win_img = window;
    g_tb_img  = titlebar;
    g_btn_img = button;
}

#define TITLE_H_BASE  22   /* minimum title-bar height (unscaled px)           */
#define BORDER_PX      1

/* ---- window-skin (win_*) resolvers: fall back to the built-in/theme look. */
static UINT32 ws_or(UINT32 v, UINT32 fb) { return v == FOREB_COLOR_UNSET ? fb : v; }
static int    ws_border_w(void) { int b = g_theme.winskin.border_w; return b >= 0 ? b : BORDER_PX; }
static int    ws_corner(void)   { int c = g_theme.winskin.corner;   return c >= 0 ? c : FCN_SQUARE; }

/* ---- small helpers ---- */
static void sstrcpy(char *d, const char *s, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = '\0';
}

static int titlebar_h(void)
{
    if (g_theme.winskin.title_h >= 0) return g_theme.winskin.title_h;
    int gh = FOREB_GLYPH_H * ui_scale();
    int h = gh + 8;
    return h < TITLE_H_BASE ? TITLE_H_BASE : h;
}

static int pt_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int id_of(const wm_window *w)
{
    return (w && w >= g_win && w < g_win + WM_MAX_WINDOWS) ? (int)(w - g_win) : -1;
}

/* Client-area rect (below the title bar, inside the border). */
static void client_rect(const wm_window *w, int *cx, int *cy, int *cw, int *ch)
{
    int th = titlebar_h();
    int bw = ws_border_w();
    *cx = w->x + bw;
    *cy = w->y + th;
    *cw = w->w - 2 * bw;
    *ch = w->h - th - bw;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

/* Close-box rect (top-right of the title bar). */
static void closebox_rect(const wm_window *w, int *cx, int *cy, int *cs)
{
    int th = titlebar_h();
    int s = th - 8; if (s < 10) s = 10;
    *cs = s;
    *cx = w->x + w->w - s - 4;
    *cy = w->y + (th - s) / 2;
}

/* ---- z-order bookkeeping ---- */
static void order_remove(int id)
{
    int j = 0;
    for (int i = 0; i < g_norder; i++)
        if (g_order[i] != id) g_order[j++] = g_order[i];
    g_norder = j;
}

static void raise_id(int id)
{
    order_remove(id);
    if (g_norder < WM_MAX_WINDOWS) g_order[g_norder++] = id;
}

/* ---- lifecycle ---- */
void wm_init(const struct forebo_theme *theme)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) g_win[i].used = 0;
    g_norder = 0;
    g_dragid = -1;
    g_any_wants_close = 0;
    if (theme) g_theme = *theme;
    else       forebo_theme_default(&g_theme);
}

void wm_set_theme(const struct forebo_theme *theme)
{
    if (theme) g_theme = *theme;
}

wm_window *wm_open(const char *title, int w, int h,
                   wm_draw_cb draw, wm_event_cb evcb, void *user)
{
    int id = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!g_win[i].used) { id = i; break; }
    if (id < 0) return NULL;

    struct wm_window *win = &g_win[id];
    for (unsigned k = 0; k < sizeof(*win); k++) ((unsigned char *)win)[k] = 0;

    int SW = (int)ui_width(), SH = (int)ui_height();
    if (w < 80)  w = 80;
    if (h < 60)  h = 60;
    if (w > SW)  w = SW;
    if (h > SH)  h = SH;

    win->used  = 1;
    win->w = w; win->h = h;
    win->x = (SW - w) / 2; if (win->x < 0) win->x = 0;
    win->y = (SH - h) / 2; if (win->y < 0) win->y = 0;
    win->draw = draw;
    win->evcb = evcb;
    win->user = user;
    sstrcpy(win->title, title ? title : "Window", (int)sizeof(win->title));
    win->tlen = 0; while (win->title[win->tlen]) win->tlen++;

    raise_id(id);

    if (win->evcb) {
        wm_event ev; ev.type = WM_EV_OPEN; ev.mx = ev.my = 0;
        ev.button = 0; ev.scancode = 0; ev.unicode = 0; ev.wheel = 0;
        win->evcb(win, &ev);
    }
    return win;
}

void wm_close(wm_window *w)
{
    int id = id_of(w);
    if (id < 0 || !g_win[id].used) return;
    if (w->evcb) {
        wm_event ev; ev.type = WM_EV_CLOSE; ev.mx = ev.my = 0;
        ev.button = 0; ev.scancode = 0; ev.unicode = 0; ev.wheel = 0;
        w->evcb(w, &ev);
    }
    g_win[id].used = 0;
    if (g_dragid == id) g_dragid = -1;
    order_remove(id);
}

void wm_close_all(void)
{
    for (int i = g_norder - 1; i >= 0; i--) wm_close(&g_win[g_order[i]]);
    g_norder = 0;
}

int wm_active_count(void)
{
    /* g_norder is maintained as the exact count of used windows (raise_id on
     * open, order_remove on close), so the O(WM_MAX_WINDOWS) scan is redundant. */
    return g_norder;
}

wm_window *wm_focused(void)
{
    return g_norder > 0 ? &g_win[g_order[g_norder - 1]] : NULL;
}

/* ---- damage / animation gating (see wm.h) ---- */
void wm_window_set_animated(int win_id, int on)
{
    if (win_id < 0 || win_id >= WM_MAX_WINDOWS) return;
    g_win[win_id].animated = on ? 1 : 0;
}

int wm_wants_repaint(void)
{
    if (g_dragid >= 0) return 1;            /* a drag is in progress             */
    if (g_event_frame) return 1;            /* wm_run_frame dispatched an event  */
    for (int i = 0; i < g_norder; i++) {    /* any OPEN window animating          */
        int id = g_order[i];
        if (g_win[id].used && g_win[id].animated) return 1;
    }
    return 0;
}

/* Translate a window handle to its pool id (for wm_window_set_animated). */
int wm_window_id(wm_window *w)
{
    return id_of(w);
}

void *wm_user(wm_window *w)      { return w ? w->user : NULL; }
int   wm_client_w(wm_window *w)  { int a,b,c,d; if(!w)return 0; client_rect(w,&a,&b,&c,&d); return c; }
int   wm_client_h(wm_window *w)  { int a,b,c,d; if(!w)return 0; client_rect(w,&a,&b,&c,&d); return d; }
int   wm_chrome_h(void)          { return titlebar_h() + ws_border_w(); }

/* ---- per-frame update (input) ---- */
static void dispatch_mouse(wm_window *w, int type, int mx, int my, int button,
                           int wheel)
{
    if (!w->evcb) return;
    g_event_frame = 1;                 /* a real event reaches a callback */
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    wm_event ev;
    ev.type = type;
    ev.mx = mx - cx;
    ev.my = my - cy;
    ev.button = button;
    ev.scancode = 0; ev.unicode = 0;
    ev.wheel = wheel;
    if (w->evcb(w, &ev) == WM_CLOSE_REQUEST) { w->wants_close = 1; g_any_wants_close = 1; }
}

void wm_run_frame(mouse_state *m, EFI_INPUT_KEY *key)
{
    g_event_frame = 0;                 /* cleared each frame; set on dispatch */

    /* --- pointer --- */
    if (m) {
        /* Continue an in-progress titlebar drag first (regardless of hover).
         * At most one window drags at a time; track it by id (g_dragid) so the
         * common no-drag frame skips the z-order scan. */
        wm_window *dragw = NULL;
        if (g_dragid >= 0 && g_win[g_dragid].used && g_win[g_dragid].dragging)
            dragw = &g_win[g_dragid];
        if (dragw) {
            if (m->left) {
                int SW = (int)ui_width(), SH = (int)ui_height();
                dragw->x = m->x - dragw->drag_off_x;
                dragw->y = m->y - dragw->drag_off_y;
                if (dragw->x < -(dragw->w - 40)) dragw->x = -(dragw->w - 40);
                if (dragw->y < 0) dragw->y = 0;
                if (dragw->x > SW - 40) dragw->x = SW - 40;
                if (dragw->y > SH - 10) dragw->y = SH - 10;
            } else {
                dragw->dragging = 0;
                g_dragid = -1;
            }
        } else if (m->left_pressed || m->right_pressed) {
            /* New press: topmost window under the cursor gets it. */
            for (int i = g_norder - 1; i >= 0; i--) {
                int id = g_order[i];
                wm_window *w = &g_win[id];
                if (!w->used) continue;
                if (!pt_in(m->x, m->y, w->x, w->y, w->w, w->h)) continue;

                raise_id(id);

                int bx, by, bs;
                closebox_rect(w, &bx, &by, &bs);
                if (pt_in(m->x, m->y, bx, by, bs, bs)) { w->wants_close = 1; g_any_wants_close = 1; break; }

                int th = titlebar_h();
                if (pt_in(m->x, m->y, w->x, w->y, w->w, th)) {
                    /* Titlebar press: only the LEFT button actually moves the
                     * window (drag continuation below is gated on m->left); a
                     * right press here is a harmless no-op, not a client event. */
                    w->dragging = 1;
                    g_dragid = id;
                    w->drag_off_x = m->x - w->x;
                    w->drag_off_y = m->y - w->y;
                } else {
                    dispatch_mouse(w, WM_EV_MOUSE_DOWN, m->x, m->y, m->right ? 1 : 0, 0);
                }
                break;
            }
        } else {
            wm_window *f = wm_focused();
            if (f) {
                if (m->left_released) dispatch_mouse(f, WM_EV_MOUSE_UP, m->x, m->y, 0, 0);
                else if (m->moved)    dispatch_mouse(f, WM_EV_MOUSE_MOVE, m->x, m->y, 0, 0);
                if (m->wheel)         dispatch_mouse(f, WM_EV_MOUSE_WHEEL, m->x, m->y, 0, m->wheel);
            }
        }
    }

    /* --- keyboard: goes to the focused (top) window --- */
    if (key) {
        wm_window *f = wm_focused();
        if (f) {
            g_event_frame = 1;         /* a key reaches the focused window */
            int handled = 0;
            if (f->evcb) {
                wm_event ev;
                ev.type = WM_EV_KEY;
                ev.mx = ev.my = 0; ev.button = 0; ev.wheel = 0;
                ev.scancode = key->ScanCode;
                ev.unicode  = key->UnicodeChar;
                if (f->evcb(f, &ev) == WM_CLOSE_REQUEST) { f->wants_close = 1; g_any_wants_close = 1; }
                else handled = 1;
            }
            /* Esc closes the focused window unless the callback consumed it. */
            if (!handled && key->ScanCode == SCAN_ESC) { f->wants_close = 1; g_any_wants_close = 1; }
        }
    }

    /* --- reap windows that asked to close (gated: rare event) --- */
    if (g_any_wants_close) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++)
            if (g_win[i].used && g_win[i].wants_close) wm_close(&g_win[i]);
        g_any_wants_close = 0;
    }
}

/* ---- drawing ---- */
/* Linear blend of two 0x00RRGGBB colors, t in 0..256. */
UINT32 wm_blend(UINT32 a, UINT32 b, int t)
{
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r=ar+((br-ar)*t)/256, g=ag+((bg-ag)*t)/256, bl=ab+((bb-ab)*t)/256;
    return ((UINT32)r<<16)|((UINT32)g<<8)|(UINT32)bl;
}

UINT32 wm_theme_color(int which)
{
    switch (which) {
        case WM_COL_WINDOW:   return g_theme.color_window;
        case WM_COL_FG:       return g_theme.color_fg;
        case WM_COL_ACCENT:   return g_theme.color_accent;
        case WM_COL_SEL_BG:   return g_theme.color_sel_bg;
        case WM_COL_SEL_FG:   return g_theme.color_sel_fg;
        case WM_COL_TITLEBAR: return g_theme.color_titlebar;
        default:              return g_theme.color_window;
    }
}

/* Client origin (+ size) of the window currently being composited, in SCREEN
 * coordinates. Set by draw_one() around the draw callback so the button
 * widget can be positioned in client coordinates. */
static int g_cli_ox, g_cli_oy, g_cli_cw, g_cli_ch;

static void draw_one(wm_window *w, int focused, int glass_fx, int shadow_on)
{
    struct forebo_winskin *ws = &g_theme.winskin;
    int sc = ui_scale();                 /* cached: opaque cross-TU call        */
    int th = titlebar_h();
    int bw = ws_border_w();
    int gh = FOREB_GLYPH_H * sc;
    UINT32 tb  = ws_or(ws->title_fill,
                       focused ? g_theme.color_titlebar
                               : wm_blend(g_theme.color_titlebar, 0x00202520u, 140));
    UINT32 cli = g_theme.color_window;
    UINT32 acc = g_theme.color_accent;
    UINT32 frame = ws_or(ws->border_color, acc);
    UINT32 tfg   = ws_or(ws->title_fg, g_theme.color_sel_fg);

    /* Drop shadow (win_shadow can suppress it). shadow_on/glass_fx are computed
     * once per frame by wm_draw() and passed in (they depend only on theme/fx). */
    /* Only the visible 4px fringe is painted (the full w*h rect was almost
     * entirely overdrawn by the opaque client/titlebar fill below). */
    if (shadow_on) {
        fill_rect(w->x + w->w, w->y + 4, 4, w->h, 0x00040804u);   /* right strip  */
        fill_rect(w->x + 4, w->y + w->h, w->w, 4, 0x00040804u);   /* bottom strip */
    }

    /* Client fill. Glass skin + effects on: frost the backdrop, then tint it
     * translucently instead of an opaque fill (see-through blur). */
    if (glass_fx) {
        /* Restrict to the client band: the title bar gets its own opaque fill
         * below, so frosting/blending the top `th` px was wasted work. */
        ui_backdrop(w->x, w->y + th, w->w, w->h - th);
        ui_blend_rect(w->x, w->y + th, w->w, w->h - th, cli, 170);
    } else {
        /* Below the title bar only: the titlebar band is painted once, later. */
        fill_rect(w->x, w->y + th, w->w, w->h - th, cli);
    }
    /* Optional custom window face over the client fill. */
    if (g_win_img && g_win_img->pixels)
        img_blit_scaled(g_win_img, w->x, w->y, w->w, w->h);

    /* Frame: beveled skin keeps its raised edges; every other skin uses a
     * configurable-width outline in the (overridable) frame color. */
    if (g_theme.window_skin == FOREB_SKIN_BEVELED) {
        draw_hline(w->x, w->y, w->w, 0x00A8C0AEu);              /* top highlight */
        draw_vline(w->x, w->y, w->h, 0x00A8C0AEu);
        draw_hline(w->x, w->y + w->h - 1, w->w, 0x00060B08u);   /* bottom shadow */
        draw_vline(w->x + w->w - 1, w->y, w->h, 0x00060B08u);
    } else {
        draw_rect_outline(w->x, w->y, w->w, w->h, bw, frame);
    }

    /* Close-box geometry (needed early to bound the title text). Inlined from
     * closebox_rect() to reuse the th already in scope (avoids re-calling
     * titlebar_h()->ui_scale()). */
    int bs = th - 8; if (bs < 10) bs = 10;
    int bx = w->x + w->w - bs - 4;
    int by = w->y + (th - bs) / 2;

    /* Title bar (glass skin lightens it toward the client color). */
    if (g_theme.window_skin == FOREB_SKIN_GLASS)
        tb = wm_blend(tb, cli, 96);
    fill_rect(w->x, w->y, w->w, th, tb);
    /* Optional custom title-bar face over the bar fill. */
    if (g_tb_img && g_tb_img->pixels)
        img_blit_scaled(g_tb_img, w->x, w->y, w->w, th);
    if (focused) draw_hline(w->x, w->y + th - 1, w->w, acc);
    /* Title, truncated with "..." so it never runs into the close box. */
    {
        int tsc = (sc < 1) ? 1 : sc;
        int maxc = ((bx - 4) - (w->x + 8)) / (FOREB_GLYPH_W * tsc);
        char tbuf[WM_TITLE_LEN];
        int tl = w->tlen;                /* cached at wm_open() (title is immutable) */
        if (maxc > 0 && tl > maxc) {
            int keep = (maxc > 3) ? maxc - 3 : maxc, i = 0;
            for (; i < keep; i++) tbuf[i] = w->title[i];
            if (maxc > 3) { tbuf[i++]='.'; tbuf[i++]='.'; tbuf[i++]='.'; }
            tbuf[i] = '\0';
            draw_string(w->x + 8, w->y + (th - gh) / 2, tbuf,
                        tfg, tb, 1, 1);
        } else if (maxc > 0) {
            draw_string(w->x + 8, w->y + (th - gh) / 2,
                        w->title, tfg, tb, 1, 1);
        }
    }

    /* Close box. */
    UINT32 cc = ws_or(ws->close_color, 0x00B03030u);
    fill_rect(bx, by, bs, bs, cc);
    draw_string(bx + (bs - FOREB_GLYPH_W * sc) / 2,
                by + (bs - gh) / 2, "x", 0x00FFFFFFu, cc, 1, 1);

    /* Corner notch (round/cut). Painted LAST, in the drop-shadow color, so it
     * reads as the window's top corners being cut back to the desktop (painting
     * it earlier hid it under the opaque title bar, making win_corner a no-op). */
    if (ws_corner() != FCN_SQUARE) {
        int n = (ws_corner() == FCN_ROUND) ? 4 : 6;
        for (int i = 0; i < n; i++) {
            int cw = n - i;
            fill_rect(w->x, w->y + i, cw, 1, 0x00040804u);
            fill_rect(w->x + w->w - cw, w->y + i, cw, 1, 0x00040804u);
        }
    }

    /* Client content. Client rect inlined from client_rect() to reuse the th/bw
     * already in scope (avoids a second titlebar_h()->ui_scale() + ws_border_w()). */
    if (w->draw) {
        int cx = w->x + bw;
        int cy = w->y + th;
        int cw = w->w - 2 * bw; if (cw < 0) cw = 0;
        int ch = w->h - th - bw; if (ch < 0) ch = 0;
        g_cli_ox = cx; g_cli_oy = cy; g_cli_cw = cw; g_cli_ch = ch;
        w->draw(w, cx, cy, cw, ch);
    }
}

void wm_draw(void)
{
    /* Occlusion culling.
     *
     * Previously every window was painted back-to-front every frame, so N
     * stacked panels cost N full window repaints even when only the top one
     * was visible (each panel's client callback re-rendered content that the
     * panels above it immediately overwrote -> the more panels open, the
     * laggier it got).
     *
     * Now a top-down pass computes, per window, the region still visible
     * after subtracting the footprints of the OPAQUE windows above it:
     *   - fully covered windows are skipped entirely (callback never runs);
     *   - the rest are painted once, clipped to the bounding box of their
     *     visible region (overdraw inside the bbox is harmless: covering
     *     windows paint later, back-to-front, and overwrite it).
     * Glass windows (frosted backdrop) never occlude: what is beneath them
     * must still be painted for the blur to read.
     *
     * Correctness invariant: every pixel of window W that is visible in the
     * final frame lies inside W's visible bbox, because the only windows
     * subtracted are opaque and paint after W. */
#define WM_VIS_MAX 6    /* per-window visible-rect cap (conservative beyond) */
    static int cov[WM_MAX_WINDOWS][4];   /* opaque footprints above current  */
    static int skip[WM_MAX_WINDOWS];     /* per z-slot: fully covered         */
    static int vb[WM_MAX_WINDOWS][4];    /* per z-slot: visible bbox          */
    int ncov = 0;

    int glass_fx = (g_theme.window_skin == FOREB_SKIN_GLASS && ui_fx_enabled());
    int shadow_on = (g_theme.winskin.shadow >= 0) ? g_theme.winskin.shadow : 1;
    int se = shadow_on ? 4 : 0;          /* drop-shadow fringe extent        */

    for (int i = g_norder - 1; i >= 0; i--) {
        wm_window *w = &g_win[g_order[i]];
        int fp[4] = { w->x, w->y, w->w + se, w->h + se };   /* footprint */

        int set[WM_VIS_MAX][4];
        int nset = 1, overflow = 0;
        set[0][0] = fp[0]; set[0][1] = fp[1];
        set[0][2] = fp[2]; set[0][3] = fp[3];

        for (int c = 0; c < ncov && nset > 0 && !overflow; c++) {
            int next[WM_VIS_MAX][4];
            int nn = 0;
            for (int r = 0; r < nset && !overflow; r++) {
                /* Subtract coverage rect cov[c] from set[r] (<=4 pieces). */
                int *q = set[r], *s = cov[c];
                int qx1 = q[0] + q[2], qy1 = q[1] + q[3];
                int sx1 = s[0] + s[2], sy1 = s[1] + s[3];
                int ix0 = q[0] > s[0] ? q[0] : s[0];
                int iy0 = q[1] > s[1] ? q[1] : s[1];
                int ix1 = qx1 < sx1 ? qx1 : sx1;
                int iy1 = qy1 < sy1 ? qy1 : sy1;
                if (ix0 >= ix1 || iy0 >= iy1) {
                    if (nn >= WM_VIS_MAX) { overflow = 1; break; }
                    next[nn][0]=q[0]; next[nn][1]=q[1];
                    next[nn][2]=q[2]; next[nn][3]=q[3]; nn++;
                    continue;
                }
                int piece[4][4]; int np = 0;
                if (q[1] < iy0) { piece[np][0]=q[0]; piece[np][1]=q[1]; piece[np][2]=q[2];     piece[np][3]=iy0-q[1]; np++; }
                if (iy1 < qy1)  { piece[np][0]=q[0]; piece[np][1]=iy1;  piece[np][2]=q[2];     piece[np][3]=qy1-iy1;  np++; }
                if (q[0] < ix0) { piece[np][0]=q[0]; piece[np][1]=iy0;  piece[np][2]=ix0-q[0]; piece[np][3]=iy1-iy0;  np++; }
                if (ix1 < qx1)  { piece[np][0]=ix1;  piece[np][1]=iy0;  piece[np][2]=qx1-ix1;  piece[np][3]=iy1-iy0;  np++; }
                for (int t = 0; t < np; t++) {
                    if (nn >= WM_VIS_MAX) { overflow = 1; break; }
                    next[nn][0]=piece[t][0]; next[nn][1]=piece[t][1];
                    next[nn][2]=piece[t][2]; next[nn][3]=piece[t][3]; nn++;
                }
            }
            if (!overflow) {
                for (int r = 0; r < nn; r++) {
                    set[r][0]=next[r][0]; set[r][1]=next[r][1];
                    set[r][2]=next[r][2]; set[r][3]=next[r][3];
                }
                nset = nn;
            }
        }

        if (overflow || nset > 0) {
            /* bbox of the visible set (full footprint when conservative). */
            int x0 = overflow ? fp[0] : set[0][0];
            int y0 = overflow ? fp[1] : set[0][1];
            int x1 = overflow ? fp[0] + fp[2] : set[0][0] + set[0][2];
            int y1 = overflow ? fp[1] + fp[3] : set[0][1] + set[0][3];
            if (!overflow) {
                for (int r = 1; r < nset; r++) {
                    int rx1 = set[r][0] + set[r][2], ry1 = set[r][1] + set[r][3];
                    if (set[r][0] < x0) x0 = set[r][0];
                    if (set[r][1] < y0) y0 = set[r][1];
                    if (rx1 > x1) x1 = rx1;
                    if (ry1 > y1) y1 = ry1;
                }
            }
            skip[i] = 0;
            vb[i][0] = x0; vb[i][1] = y0;
            vb[i][2] = x1 - x0; vb[i][3] = y1 - y0;
        } else {
            skip[i] = 1;
        }

        /* Only opaque windows occlude what is beneath them. */
        if (!glass_fx && ncov < WM_MAX_WINDOWS) {
            cov[ncov][0]=fp[0]; cov[ncov][1]=fp[1];
            cov[ncov][2]=fp[2]; cov[ncov][3]=fp[3];
            ncov++;
        }
    }

    ui_clip_reset();
    for (int i = 0; i < g_norder; i++) {
        int id = g_order[i];
        if (!g_win[id].used || skip[i]) continue;
        ui_clip_push(vb[i][0], vb[i][1], vb[i][2], vb[i][3]);
        draw_one(&g_win[id], i == g_norder - 1, glass_fx, shadow_on);
        ui_clip_pop();
    }
}

/* ---- button widget ---- */
int wm_button_h(void)
{
    int sc = ui_scale(); if (sc < 1) sc = 1;
    return FOREB_GLYPH_H * sc + 8 * sc;
}

int wm_button_measure(const char *label)
{
    int sc = ui_scale(); if (sc < 1) sc = 1;
    int n = 0; while (label && label[n]) n++;
    return n * FOREB_GLYPH_W * sc + 16 * sc;    /* 8*sc padding each side */
}

int wm_button_hit(const wm_button *b, int mx, int my)
{
    if (!b) return 0;
    return mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h;
}

void wm_button_draw(const wm_button *b, int hover, int pressed)
{
    if (!b || b->w <= 0 || b->h <= 0) return;
    int sc = ui_scale(); if (sc < 1) sc = 1;

    /* Graceful clip: only drawn when fully inside the current client area. */
    if (b->x < 0 || b->y < 0 || b->x + b->w > g_cli_cw || b->y + b->h > g_cli_ch)
        return;

    int x = g_cli_ox + b->x, y = g_cli_oy + b->y;
    UINT32 cli = g_theme.color_window, fg = g_theme.color_fg;
    UINT32 acc = g_theme.color_accent;
    int en = b->enabled;
    if (!en) { hover = 0; pressed = 0; }

    /* Face: subtly raised over the client color; brighter on hover, darker
     * when pressed-in, nearly flat when disabled. */
    UINT32 face = !en      ? wm_blend(cli, 0x00FFFFFFu, 8)
                  : pressed ? wm_blend(cli, 0x00000000u, 28)
                  : hover   ? wm_blend(cli, 0x00FFFFFFu, 56)
                            : wm_blend(cli, 0x00FFFFFFu, 30);
    fill_rect(x, y, b->w, b->h, face);

    /* Optional custom button face over the fill. */
    if (g_btn_img && g_btn_img->pixels)
        img_blit_scaled(g_btn_img, x, y, b->w, b->h);

    /* Edge treatment. win_button_style overrides the implicit window-skin look:
     *   FBTN_FLAT / FBTN_GHOST -> no edges (flat face)
     *   FBTN_OUTLINE           -> accent outline
     *   others                 -> follow the window skin (bevel or outline).
     * When win_button_style is unset (-1) the original window-skin logic runs. */
    int bstyle = g_theme.winskin.button_style;
    UINT32 oc = !en ? wm_blend(cli, fg, 40)
                : (hover || pressed) ? acc : wm_blend(cli, fg, 80);
    if (bstyle == FBTN_FLAT || bstyle == FBTN_GHOST) {
        /* no edges */
    } else if (bstyle == FBTN_OUTLINE) {
        draw_rect_outline(x, y, b->w, b->h, 1, en && (hover || pressed) ? acc
                                             : wm_blend(cli, fg, 100));
    } else if (bstyle == FBTN_RAISED ||
               (bstyle < 0 && g_theme.window_skin == FOREB_SKIN_BEVELED)) {
        /* Beveled: light top/left + dark bottom/right, swapped when pressed. */
        UINT32 hi = 0x00A8C0AEu, lo = 0x00060B08u;
        UINT32 tlc = pressed ? lo : hi, brc = pressed ? hi : lo;
        draw_hline(x, y, b->w, tlc);
        draw_vline(x, y, b->h, tlc);
        draw_hline(x, y + b->h - 1, b->w, brc);
        draw_vline(x + b->w - 1, y, b->h, brc);
    } else {
        draw_rect_outline(x, y, b->w, b->h, 1, oc);
    }

    /* Centered label, clipped to the button width; nudged 1px when pressed. */
    int fit = (b->w - 8 * sc) / (FOREB_GLYPH_W * sc);
    if (fit > 0) {
        char buf[28];
        int n = 0;
        for (; b->label[n] && n < fit && n + 1 < (int)sizeof(buf); n++)
            buf[n] = b->label[n];
        buf[n] = '\0';
        UINT32 tc = en ? fg : wm_blend(fg, cli, 128);
        int tx = x + (b->w - n * FOREB_GLYPH_W * sc) / 2;
        int ty = y + (b->h - FOREB_GLYPH_H * sc) / 2;
        if (pressed) { tx += 1; ty += 1; }
        draw_string(tx, ty, buf, tc, face, 1, 1);   /* scale 1: ui_scale() applies */
    }
}
