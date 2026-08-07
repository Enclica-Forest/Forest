/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_gfx.c - "Graphics Demos" tool category (KEY = gfx).
 * =============================================================================
 * Nine pure compute/draw animated demos. INTEGER / fixed-point math ONLY
 * (-mno-sse: no float/double anywhere; trig via the internal 256-entry sine
 * LUT below). Each demo is a template-B wm window: open() calls wm_open() and
 * returns; the bootx64.c menu loop drives draw_cb (called every frame while the
 * window is open, so we advance animation there) + evcb.
 *
 * Every draw callback CLIPS all output to the client rect [cx,cx+cw) x
 * [cy,cy+ch) via the gpx/grect/gline/gdisc helpers (client-relative coords).
 * Freestanding, no libc, no heap, fixed buffers.
 * ========================================================================== */
#include "tools_gfx.h"
#include "ui.h"
#include "wm.h"
#include "input.h"
#include "efi.h"

/* Upper bound on any gfx window's client area. Windows are size-capped at
 * open() (max 640 wide/tall) and are never resized, so these bounds hold for
 * every demo. Used to size the frame-cache buffers of the static toys below.
 * (The pre-existing gr_* per-column scratch arrays already assumed 640.) */
#ifndef MAXW
#define MAXW 640
#endif
#ifndef MAXH
#define MAXH 640
#endif

/* ==========================================================================
 * Shared integer math + drawing helpers
 * ========================================================================== */

/* 256-entry full-circle sine LUT, amplitude 4096 (12-bit fixed point). */
static const short gfx_sin[256] = {
        0,   101,   201,   301,   401,   501,   601,   700,
      799,   897,   995,  1092,  1189,  1285,  1380,  1474,
     1567,  1660,  1751,  1842,  1931,  2019,  2106,  2191,
     2276,  2359,  2440,  2520,  2598,  2675,  2751,  2824,
     2896,  2967,  3035,  3102,  3166,  3229,  3290,  3349,
     3406,  3461,  3513,  3564,  3612,  3659,  3703,  3745,
     3784,  3822,  3857,  3889,  3920,  3948,  3973,  3996,
     4017,  4036,  4052,  4065,  4076,  4085,  4091,  4095,
     4096,  4095,  4091,  4085,  4076,  4065,  4052,  4036,
     4017,  3996,  3973,  3948,  3920,  3889,  3857,  3822,
     3784,  3745,  3703,  3659,  3612,  3564,  3513,  3461,
     3406,  3349,  3290,  3229,  3166,  3102,  3035,  2967,
     2896,  2824,  2751,  2675,  2598,  2520,  2440,  2359,
     2276,  2191,  2106,  2019,  1931,  1842,  1751,  1660,
     1567,  1474,  1380,  1285,  1189,  1092,   995,   897,
      799,   700,   601,   501,   401,   301,   201,   101,
        0,  -101,  -201,  -301,  -401,  -501,  -601,  -700,
     -799,  -897,  -995, -1092, -1189, -1285, -1380, -1474,
    -1567, -1660, -1751, -1842, -1931, -2019, -2106, -2191,
    -2276, -2359, -2440, -2520, -2598, -2675, -2751, -2824,
    -2896, -2967, -3035, -3102, -3166, -3229, -3290, -3349,
    -3406, -3461, -3513, -3564, -3612, -3659, -3703, -3745,
    -3784, -3822, -3857, -3889, -3920, -3948, -3973, -3996,
    -4017, -4036, -4052, -4065, -4076, -4085, -4091, -4095,
    -4096, -4095, -4091, -4085, -4076, -4065, -4052, -4036,
    -4017, -3996, -3973, -3948, -3920, -3889, -3857, -3822,
    -3784, -3745, -3703, -3659, -3612, -3564, -3513, -3461,
    -3406, -3349, -3290, -3229, -3166, -3102, -3035, -2967,
    -2896, -2824, -2751, -2675, -2598, -2520, -2440, -2359,
    -2276, -2191, -2106, -2019, -1931, -1842, -1751, -1660,
    -1567, -1474, -1380, -1285, -1189, -1092,  -995,  -897,
     -799,  -700,  -601,  -501,  -401,  -301,  -201,  -101,
};
/* sin/cos in LUT units: angle 0..255 == 0..2pi, result -4096..4096. */
static int isin(int a){ return gfx_sin[(unsigned)a & 255]; }
static int icos(int a){ return gfx_sin[((unsigned)a + 64) & 255]; }

/* Small integer square root (Newton). v<=0 -> 0. */
static int isqrt_i(int v){
    if(v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while(y < x){ x = y; y = (x + v / x) / 2; }
    return x;
}

/* xorshift-ish LCG PRNG (per-window seed). */
static unsigned gfx_rand(unsigned *s){
    *s = (*s) * 1664525u + 1013904223u;
    return *s;
}

/* Integer HSV -> 0x00RRGGBB. h,s,v all 0..255. */
static UINT32 gfx_hsv(int h, int s, int v){
    h &= 255; if(s < 0) s = 0; if(s > 255) s = 255; if(v < 0) v = 0; if(v > 255) v = 255;
    int region = (h * 6) >> 8;            /* 0..5                          */
    int rem    = (h * 6) & 255;           /* fraction within region *256   */
    int p = v * (255 - s) / 255;
    int q = v * (255 - (s * rem) / 255) / 255;
    int t = v * (255 - (s * (255 - rem)) / 255) / 255;
    int r, g, b;
    switch(region){
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return ((UINT32)(r & 255) << 16) | ((UINT32)(g & 255) << 8) | (UINT32)(b & 255);
}
static UINT32 gfx_rgb(int r, int g, int b){
    if(r < 0) r = 0; if(r > 255) r = 255;
    if(g < 0) g = 0; if(g > 255) g = 255;
    if(b < 0) b = 0; if(b > 255) b = 255;
    return ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
}
/* Scale each channel of a colour by intensity/255 (0..255). */
static UINT32 gfx_dim(UINT32 c, int intensity){
    if(intensity < 0) intensity = 0; if(intensity > 255) intensity = 255;
    int r = ((c >> 16) & 255) * intensity / 255;
    int g = ((c >> 8) & 255) * intensity / 255;
    int b = (c & 255) * intensity / 255;
    return gfx_rgb(r, g, b);
}
/* Push each channel of a colour toward white by `add` (0..255). */
static UINT32 gfx_light(UINT32 c, int add){
    if(add < 0) add = 0; if(add > 255) add = 255;
    int r = ((c >> 16) & 255) + add; if(r > 255) r = 255;
    int g = ((c >> 8) & 255) + add;  if(g > 255) g = 255;
    int b = (c & 255) + add;         if(b > 255) b = 255;
    return gfx_rgb(r, g, b);
}

/* ---- client-clipped drawing (all coords client-relative to cx,cy) -------- */
static void gpx(int cx, int cy, int cw, int ch, int x, int y, UINT32 c){
    if((unsigned)x < (unsigned)cw && (unsigned)y < (unsigned)ch)
        put_pixel(cx + x, cy + y, c);
}
static void grect(int cx, int cy, int cw, int ch, int x, int y, int w, int h, UINT32 c){
    if(x < 0){ w += x; x = 0; }
    if(y < 0){ h += y; y = 0; }
    if(x + w > cw) w = cw - x;
    if(y + h > ch) h = ch - y;
    if(w > 0 && h > 0) fill_rect(cx + x, cy + y, w, h, c);
}
static void gline(int cx, int cy, int cw, int ch, int x0, int y0, int x1, int y1, UINT32 c){
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (ax > ay ? ax : -ay) / 2, e2;
    /* guard against absurd runaway (should never trigger; keeps it bounded) */
    int guard = ax + ay + 4;
    for(;;){
        gpx(cx, cy, cw, ch, x0, y0, c);
        if(x0 == x1 && y0 == y1) break;
        if(--guard < 0) break;
        e2 = err;
        if(e2 > -ax){ err -= ay; x0 += sx; }
        if(e2 <  ay){ err += ax; y0 += sy; }
    }
}
static void gdisc(int cx, int cy, int cw, int ch, int ox, int oy, int r, UINT32 c){
    if(r < 0) return;
    for(int dy = -r; dy <= r; dy++){
        int rem = r * r - dy * dy;
        if(rem < 0) continue;
        int dx = isqrt_i(rem);
        grect(cx, cy, cw, ch, ox - dx, oy + dy, 2 * dx + 1, 1, c);
    }
}

/* Draw a one-line HUD/help string clipped to the window top. */
static void gfx_hud(int cx, int cy, int cw, const char *s, UINT32 fg){
    draw_string_clip(cx + 4, cy + 3, cw - 8, s, fg, 0x00000000, 1, 1);
}

/* Common: is this key an "escape/close" request? */
static int gfx_is_close(const wm_event *ev){
    return ev->type == WM_EV_KEY &&
           (ev->scancode == SCAN_ESC || ev->unicode == 0x1B);
}

/* ==========================================================================
 * 1) Mandelbrot - integer fixed-point, pan + zoom
 * ========================================================================== */
#define MB_F 28                       /* fixed-point fractional bits          */
typedef struct {
    wm_window *win;
    long long  cre, cim;              /* view centre (MB_F fixed)             */
    long long  scale;                 /* units per pixel (MB_F fixed); 0=init */
    int        maxiter;
    int        palshift;
    unsigned   frame;
    int        dirty;                 /* 1 -> escape-time image is stale      */
    int        cw_last, ch_last;      /* client size the cache was built for   */
} mb_state;
static mb_state g_mb;

/* Frame cache: the Mandelbrot image is a static picture that only changes on
 * pan/zoom/iter/palette edits, so it is recomputed only when `dirty`, then
 * blitted from here every frame (2x2 blocks -> (MAXW/2)*(MAXH/2) cells). */
static UINT32 mb_cache[(MAXW / 2) * (MAXH / 2)];

static void mb_reset(mb_state *m){
    m->cre = -((long long)1 << (MB_F - 1));   /* -0.5 */
    m->cim = 0;
    m->scale = 0;                             /* recompute from width         */
    m->maxiter = 64;
    m->palshift = 0;
    m->dirty = 1;                             /* force a full recompute        */
}

/* Keep the view inside a numerically safe region: with |cre|,|cim| <= 4 units
 * the orbit variables stay below 2^31 fixed-point, so zr*zr / zr*zi can never
 * overflow 64-bit no matter how long the user pans or zooms out. */
static void mb_clamp(mb_state *m){
    const long long lim  = (long long)4 << MB_F;  /* |c| bound (units)        */
    const long long smax = (long long)1 << MB_F;  /* 1 unit/px zoom-out limit */
    if(m->cre >  lim) m->cre =  lim;
    if(m->cre < -lim) m->cre = -lim;
    if(m->cim >  lim) m->cim =  lim;
    if(m->cim < -lim) m->cim = -lim;
    if(m->scale > smax) m->scale = smax;
    if(m->scale < 1)    m->scale = 1;
}

static void mb_draw(wm_window *w, int cx, int cy, int cw, int ch){
    mb_state *m = (mb_state *)wm_user(w);
    m->frame++;
    if(cw < 8 || ch < 8) return;
    int top = 20;                     /* HUD band                             */
    grect(cx, cy, cw, ch, 0, 0, cw, top, 0x00101018);

    if(m->scale <= 0){
        /* fit ~3.2 units across the plot width */
        long long span = ((long long)32 << MB_F) / 10;  /* 3.2 in fixed       */
        m->scale = span / (cw ? cw : 1);
        if(m->scale <= 0) m->scale = 1;
    }
    int ph = ch - top;
    long long re0 = m->cre - (long long)(cw / 2) * m->scale;
    long long im0 = m->cim - (long long)(ph / 2) * m->scale;
    const long long four = (long long)4 << MB_F;
    int step = 2;                     /* 2x2 blocks for speed                 */
    long long sstep = (long long)step * m->scale;   /* per-block c increment  */
    const int cap = (int)(sizeof(mb_cache) / sizeof(mb_cache[0]));

    /* A resize changes the block layout, so treat it like an edit. */
    if(cw != m->cw_last || ch != m->ch_last){ m->dirty = 1; m->cw_last = cw; m->ch_last = ch; }

    /* Run the (expensive) escape-time loop only when the view changed; the
     * output is a static image otherwise, so we reuse the cache. */
    if(m->dirty){
        long long ci = im0;
        int idx = 0;
        for(int py = 0; py < ph && idx < cap; py += step){
            long long cr = re0;
            for(int px = 0; px < cw && idx < cap; px += step){
                long long zr = 0, zi = 0;
                int it = 0;
                for(; it < m->maxiter; it++){
                    long long zr2 = (zr * zr) >> MB_F;
                    long long zi2 = (zi * zi) >> MB_F;
                    if(zr2 + zi2 > four) break;
                    long long nzi = ((zr * zi) >> (MB_F - 1)) + ci;   /* 2*zr*zi */
                    zr = zr2 - zi2 + cr;
                    zi = nzi;
                }
                if(it >= m->maxiter) mb_cache[idx++] = 0x00000000;
                else                 mb_cache[idx++] = gfx_hsv(it * 6 + m->palshift, 235, 255);
                cr += sstep;
            }
            ci += sstep;
        }
        m->dirty = 0;
    }

    /* Blit cached blocks every frame (VRAM is uncached; the writes are the
     * unavoidable part, but the compute above is now skipped when idle). */
    {
        int idx = 0;
        for(int py = 0; py < ph && idx < cap; py += step)
            for(int px = 0; px < cw && idx < cap; px += step)
                grect(cx, cy, cw, ch, px, top + py, step, step, mb_cache[idx++]);
    }
    char buf[64]; int p = 0;
    const char *h = "Mandelbrot  arrows pan  +/- zoom  click:center  R:reset";
    while(h[p] && p < 63){ buf[p] = h[p]; p++; } buf[p] = 0;
    gfx_hud(cx, cy, cw, buf, 0x00CFE8FF);
}

static int mb_event(wm_window *w, const wm_event *ev){
    mb_state *m = (mb_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ m->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    int cw = wm_client_w(w), ch = wm_client_h(w);
    long long pan = (long long)(cw / 8) * (m->scale > 0 ? m->scale : 1);
    if(ev->type == WM_EV_KEY){
        switch(ev->scancode){
            case SCAN_UP:    m->cim -= pan; m->dirty = 1; break;
            case SCAN_DOWN:  m->cim += pan; m->dirty = 1; break;
            case SCAN_LEFT:  m->cre -= pan; m->dirty = 1; break;
            case SCAN_RIGHT: m->cre += pan; m->dirty = 1; break;
            default: break;
        }
        switch(ev->unicode){
            case '+': case '=': m->scale = m->scale * 7 / 10; if(m->scale <= 0) m->scale = 1; m->dirty = 1; break;
            case '-': case '_': m->scale = m->scale * 10 / 7; m->dirty = 1; break;
            case 'r': case 'R': mb_reset(m); break;   /* mb_reset() sets dirty */
            case ']': if(m->maxiter < 512) m->maxiter += 16; m->dirty = 1; break;
            case '[': if(m->maxiter > 16)  m->maxiter -= 16; m->dirty = 1; break;
            case 'c': case 'C': m->palshift += 24; m->dirty = 1; break;
            default: break;
        }
    } else if(ev->type == WM_EV_MOUSE_DOWN && m->scale > 0){
        int top = 20, ph = ch - top;
        long long re0 = m->cre - (long long)(cw / 2) * m->scale;
        long long im0 = m->cim - (long long)(ph / 2) * m->scale;
        m->cre = re0 + (long long)ev->mx * m->scale;
        m->cim = im0 + (long long)(ev->my - top) * m->scale;
        if(ev->button == 0){ m->scale = m->scale * 6 / 10; if(m->scale <= 0) m->scale = 1; }
        else                 m->scale = m->scale * 10 / 6;
        m->dirty = 1;
    }
    if(m->scale > 0) mb_clamp(m);
    return 0;
}

void tool_gfx_mandelbrot_open(void){
    if(g_mb.win) return;
    mb_reset(&g_mb);
    g_mb.frame = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 60 / 100; if(ww < 360) ww = 360; if(ww > 640) ww = 640; if(ww > W - 40) ww = W - 40;
    int wh = H * 60 / 100; if(wh < 300) wh = 300; if(wh > 560) wh = 560; if(wh > H - 40) wh = H - 40;
    g_mb.win = wm_open("Mandelbrot", ww, wh, mb_draw, mb_event, &g_mb);
}

/* ==========================================================================
 * 2) Plasma - animated sine-LUT field
 * ========================================================================== */
typedef struct { wm_window *win; int t; int speed; int mode; int cw_last, ch_last; } pl_state;
static pl_state g_pl;

/* Per-cell distance-from-centre is frame-invariant (only the sine phase `t`
 * animates), so cache the isqrt result and rebuild only when the size changes.
 * 2x2 blocks -> (MAXW/2)*(MAXH/2) cells; max radius < 640 fits in u16. */
static unsigned short pl_rad[(MAXW / 2) * (MAXH / 2)];

static void pl_draw(wm_window *w, int cx, int cy, int cw, int ch){
    pl_state *s = (pl_state *)wm_user(w);
    s->t += s->speed;
    if(cw < 4 || ch < 4) return;
    int t = s->t;
    int step = 2;
    int hx = cw / 2, hy = ch / 2;
    const int cap = (int)(sizeof(pl_rad) / sizeof(pl_rad[0]));

    /* Rebuild the radius table only when the client area changed. */
    if(cw != s->cw_last || ch != s->ch_last){
        int idx = 0;
        for(int y = 0; y < ch && idx < cap; y += step){
            int dy = y - hy, dy2 = dy * dy;
            for(int x = 0; x < cw && idx < cap; x += step){
                int dx = x - hx;
                pl_rad[idx++] = (unsigned short)isqrt_i(dx * dx + dy2);
            }
        }
        s->cw_last = cw; s->ch_last = ch;
    }

    int idx = 0;
    for(int y = 0; y < ch && idx < cap; y += step){
        int sy = isin(y - t);             /* only depends on y and t          */
        for(int x = 0; x < cw && idx < cap; x += step){
            int rad = pl_rad[idx++];
            int v = isin(x + t) + sy
                  + isin(((x + y) >> 1) + t)
                  + isin((rad >> 1) - t);       /* -16384..16384             */
            UINT32 col;
            if(s->mode == 2){
                int g = (v + 16384) >> 7;       /* 0..255                     */
                col = gfx_rgb(g, g, g);
            } else if(s->mode == 3){
                int f = (v + 16384) >> 6;       /* 0..512 -> fire ramp        */
                int r = f > 255 ? 255 : f;
                int g = f > 255 ? (f - 255) : 0;
                col = gfx_rgb(r, g, g / 3);
            } else {
                int hue = ((v >> 6) + t) & 255;
                col = gfx_hsv(hue, s->mode == 1 ? 170 : 255, 255);
            }
            grect(cx, cy, cw, ch, x, y, step, step, col);
        }
    }
    gfx_hud(cx, cy, cw, "Plasma  Left/Right:speed  Space:palette", 0x00FFFFFF);
}

static int pl_event(wm_window *w, const wm_event *ev){
    pl_state *s = (pl_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ s->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    if(ev->type == WM_EV_KEY){
        if(ev->scancode == SCAN_RIGHT && s->speed < 12) s->speed++;
        if(ev->scancode == SCAN_LEFT  && s->speed > 1)  s->speed--;
        if(ev->unicode == ' ') s->mode = (s->mode + 1) % 4;
    }
    return 0;
}

void tool_gfx_plasma_open(void){
    if(g_pl.win) return;
    g_pl.t = 0; g_pl.speed = 2; g_pl.mode = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 55 / 100; if(ww < 340) ww = 340; if(ww > 640) ww = 640; if(ww > W - 40) ww = W - 40;
    int wh = H * 52 / 100; if(wh < 260) wh = 260; if(wh > 520) wh = 520; if(wh > H - 40) wh = H - 40;
    g_pl.win = wm_open("Plasma", ww, wh, pl_draw, pl_event, &g_pl);
}

/* ==========================================================================
 * 3) Starfield - 3D-ish flying stars
 * ========================================================================== */
#define ST_MAX 240
#define ST_ZMAX 4096
typedef struct {
    wm_window *win;
    unsigned rng;
    int speed;
    int steer_x, steer_y;
    int x[ST_MAX], y[ST_MAX], z[ST_MAX];
    int init;
} st_state;
static st_state g_st;

static void st_spawn(st_state *s, int i){
    s->x[i] = (int)(gfx_rand(&s->rng) % 4096) - 2048;
    s->y[i] = (int)(gfx_rand(&s->rng) % 4096) - 2048;
    s->z[i] = 1 + (int)(gfx_rand(&s->rng) % ST_ZMAX);
}

static void st_draw(wm_window *w, int cx, int cy, int cw, int ch){
    st_state *s = (st_state *)wm_user(w);
    if(!s->init){ for(int i = 0; i < ST_MAX; i++) st_spawn(s, i); s->init = 1; }
    grect(cx, cy, cw, ch, 0, 0, cw, ch, 0x00000000);
    int hx = cw / 2 + s->steer_x, hy = ch / 2 + s->steer_y;
    int fov = cw / 2;
    for(int i = 0; i < ST_MAX; i++){
        s->z[i] -= s->speed;
        if(s->z[i] < 1){ st_spawn(s, i); s->z[i] = ST_ZMAX; }
        int z = s->z[i];
        int sx = hx + s->x[i] * fov / z;
        int sy = hy + s->y[i] * fov / z;
        if(sx < 0 || sy < 0 || sx >= cw || sy >= ch) continue;
        int b = 255 - z * 255 / ST_ZMAX;      /* nearer = brighter           */
        UINT32 col = gfx_rgb(b, b, b > 200 ? 255 : b + 40);
        if(z < 900){
            grect(cx, cy, cw, ch, sx, sy, 2, 2, col);
        } else {
            gpx(cx, cy, cw, ch, sx, sy, col);
        }
    }
    gfx_hud(cx, cy, cw, "Starfield  arrows:steer  +/-:warp speed", 0x0080FFB0);
}

static int st_event(wm_window *w, const wm_event *ev){
    st_state *s = (st_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ s->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    if(ev->type == WM_EV_KEY){
        int lim = 240;
        switch(ev->scancode){
            case SCAN_LEFT:  if(s->steer_x < lim)  s->steer_x += 16; break;
            case SCAN_RIGHT: if(s->steer_x > -lim) s->steer_x -= 16; break;
            case SCAN_UP:    if(s->steer_y < lim)  s->steer_y += 16; break;
            case SCAN_DOWN:  if(s->steer_y > -lim) s->steer_y -= 16; break;
            default: break;
        }
        if((ev->unicode == '+' || ev->unicode == '=') && s->speed < 200) s->speed += 8;
        if((ev->unicode == '-' || ev->unicode == '_') && s->speed > 8)   s->speed -= 8;
    }
    return 0;
}

void tool_gfx_starfield_open(void){
    if(g_st.win) return;
    g_st.rng = 0x1234abcdu; g_st.speed = 48; g_st.steer_x = 0; g_st.steer_y = 0; g_st.init = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 55 / 100; if(ww < 340) ww = 340; if(ww > 700) ww = 700; if(ww > W - 40) ww = W - 40;
    int wh = H * 55 / 100; if(wh < 280) wh = 280; if(wh > 560) wh = 560; if(wh > H - 40) wh = H - 40;
    g_st.win = wm_open("Starfield", ww, wh, st_draw, st_event, &g_st);
}

/* ==========================================================================
 * 4) Matrix rain
 * ========================================================================== */
#define MX_MAXCOL 160
typedef struct {
    wm_window *win;
    unsigned rng;
    int speed;
    int head[MX_MAXCOL];      /* head position in rows*16 (fixed)             */
    int spd[MX_MAXCOL];       /* per-column fall speed (rows*16 per frame)    */
    int len[MX_MAXCOL];       /* trail length in rows                         */
    int ncol;
    int init;
} mx_state;
static mx_state g_mx;

static void mx_reseed_col(mx_state *m, int c, int rows){
    m->head[c] = -(int)(gfx_rand(&m->rng) % (unsigned)(rows * 16 + 1));
    m->spd[c]  = 4 + (int)(gfx_rand(&m->rng) % 16);
    m->len[c]  = 6 + (int)(gfx_rand(&m->rng) % 18);
}

static void mx_draw(wm_window *w, int cx, int cy, int cw, int ch){
    mx_state *m = (mx_state *)wm_user(w);
    int sc = ui_scale(); if(sc < 1) sc = 1;
    int cellw = 8 * sc, cellh = 16 * sc;
    if(cellw < 1) cellw = 8; if(cellh < 1) cellh = 16;
    int cols = cw / cellw; if(cols > MX_MAXCOL) cols = MX_MAXCOL; if(cols < 1) cols = 1;
    int rows = ch / cellh; if(rows < 1) rows = 1;
    if(!m->init || m->ncol != cols){
        m->ncol = cols;
        for(int c = 0; c < cols; c++) mx_reseed_col(m, c, rows);
        m->init = 1;
    }
    grect(cx, cy, cw, ch, 0, 0, cw, ch, 0x00000000);
    for(int c = 0; c < cols; c++){
        m->head[c] += m->spd[c] * m->speed / 4;
        int hrow = m->head[c] / 16;
        if(hrow - m->len[c] > rows) mx_reseed_col(m, c, rows);
        for(int k = 0; k < m->len[c]; k++){
            int row = hrow - k;
            if(row < 0 || row >= rows) continue;
            /* pseudo-random glyph that flickers over time */
            unsigned h = (unsigned)(c * 131 + row * 977) + ((unsigned)m->head[c] >> 5);
            char ch1 = (char)(33 + (h % 94));
            UINT32 col;
            if(k == 0) col = 0x00E8FFE8;                 /* bright head        */
            else { int g = 255 - k * 220 / m->len[c]; if(g < 30) g = 30; col = gfx_rgb(0, g, g / 4); }
            int px = c * cellw, py = row * cellh;
            /* clip the whole glyph cell to the client rect (draw_char only
             * bounds-clips to the screen, not to our window) */
            if(px + cellw <= cw && py + cellh <= ch)
                draw_char(cx + px, cy + py, ch1, col, 0x00000000, 1, 1);
        }
    }
    gfx_hud(cx, cy, cw, "Matrix  Left/Right:speed", 0x0060FF60);
}

static int mx_event(wm_window *w, const wm_event *ev){
    mx_state *m = (mx_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ m->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    if(ev->type == WM_EV_KEY){
        if(ev->scancode == SCAN_RIGHT && m->speed < 16) m->speed++;
        if(ev->scancode == SCAN_LEFT  && m->speed > 1)  m->speed--;
    }
    return 0;
}

void tool_gfx_matrix_open(void){
    if(g_mx.win) return;
    g_mx.rng = 0x2f6ea1u; g_mx.speed = 4; g_mx.ncol = 0; g_mx.init = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 55 / 100; if(ww < 360) ww = 360; if(ww > 720) ww = 720; if(ww > W - 40) ww = W - 40;
    int wh = H * 58 / 100; if(wh < 300) wh = 300; if(wh > 600) wh = 600; if(wh > H - 40) wh = H - 40;
    g_mx.win = wm_open("Matrix Rain", ww, wh, mx_draw, mx_event, &g_mx);
}

/* ==========================================================================
 * 5) Fireworks - particle system with gravity
 * ========================================================================== */
#define FW_MAX 320
#define FW_G   40                    /* gravity (pos<<8 units / frame^2)      */
enum { FW_DEAD = 0, FW_ROCKET, FW_SPARK };
typedef struct {
    int x, y, vx, vy;                /* position/velocity, pos<<8 fixed       */
    int life, maxlife, type;
    UINT32 col;
} fw_part;
typedef struct {
    wm_window *win;
    unsigned rng;
    fw_part p[FW_MAX];
    int cooldown;
    int init;
} fw_state;
static fw_state g_fw;

static int fw_free(fw_state *f){
    for(int i = 0; i < FW_MAX; i++) if(f->p[i].type == FW_DEAD) return i;
    return -1;
}
static void fw_launch(fw_state *f, int cw, int ch, int tx){
    int i = fw_free(f); if(i < 0) return;
    fw_part *r = &f->p[i];
    r->type = FW_ROCKET;
    r->x = tx << 8;
    r->y = (ch - 1) << 8;
    r->vx = ((int)(gfx_rand(&f->rng) % 200)) - 100;
    r->vy = -(2200 + (int)(gfx_rand(&f->rng) % 1000));  /* bursts high up      */
    r->life = 0; r->maxlife = 0;
    r->col = gfx_hsv((int)(gfx_rand(&f->rng) & 255), 180, 255);
    (void)cw;
}
static void fw_explode(fw_state *f, int x, int y, UINT32 base){
    int n = 26 + (int)(gfx_rand(&f->rng) % 16);
    int slot = 0;                          /* running free-slot cursor         */
    for(int k = 0; k < n; k++){
        while(slot < FW_MAX && f->p[slot].type != FW_DEAD) slot++;
        if(slot >= FW_MAX) return;
        fw_part *s = &f->p[slot++];
        int a = (int)(gfx_rand(&f->rng) & 255);
        int sp = 300 + (int)(gfx_rand(&f->rng) % 1500);
        s->type = FW_SPARK;
        s->x = x; s->y = y;
        s->vx = (icos(a) * sp) >> 12;
        s->vy = (isin(a) * sp) >> 12;
        s->maxlife = 40 + (int)(gfx_rand(&f->rng) % 40);
        s->life = s->maxlife;
        s->col = base;
    }
}

static void fw_draw(wm_window *w, int cx, int cy, int cw, int ch){
    fw_state *f = (fw_state *)wm_user(w);
    if(!f->init){ f->init = 1; f->cooldown = 10; }
    grect(cx, cy, cw, ch, 0, 0, cw, ch, 0x00050510);
    if(--f->cooldown <= 0){
        fw_launch(f, cw, ch, (int)(gfx_rand(&f->rng) % (unsigned)(cw > 0 ? cw : 1)));
        f->cooldown = 30 + (int)(gfx_rand(&f->rng) % 45);
    }
    for(int i = 0; i < FW_MAX; i++){
        fw_part *pt = &f->p[i];
        if(pt->type == FW_DEAD) continue;
        pt->vy += FW_G;
        pt->x += pt->vx;
        pt->y += pt->vy;
        int px = pt->x >> 8, py = pt->y >> 8;
        if(pt->type == FW_ROCKET){
            if(pt->vy >= 0 || py <= ch / 6){          /* apex -> burst        */
                fw_explode(f, pt->x, pt->y, pt->col);
                pt->type = FW_DEAD;
                continue;
            }
            grect(cx, cy, cw, ch, px, py, 2, 3, 0x00FFF0C0);
            grect(cx, cy, cw, ch, px, py + 3, 1, 4, 0x00806040); /* trail      */
        } else {
            if(--pt->life <= 0 || py >= ch){ pt->type = FW_DEAD; continue; }
            UINT32 col = gfx_dim(pt->col, pt->life * 255 / pt->maxlife);
            grect(cx, cy, cw, ch, px, py, 2, 2, col);
        }
    }
    gfx_hud(cx, cy, cw, "Fireworks  Space/click:launch", 0x00FFE0A0);
}

static int fw_event(wm_window *w, const wm_event *ev){
    fw_state *f = (fw_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ f->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    int cw = wm_client_w(w), ch = wm_client_h(w);
    if(ev->type == WM_EV_KEY && ev->unicode == ' ')
        fw_launch(f, cw, ch, cw / 2);
    else if(ev->type == WM_EV_MOUSE_DOWN)
        fw_launch(f, cw, ch, ev->mx < 0 ? 0 : (ev->mx >= cw ? cw - 1 : ev->mx));
    return 0;
}

void tool_gfx_fireworks_open(void){
    if(g_fw.win) return;
    for(int i = 0; i < FW_MAX; i++) g_fw.p[i].type = FW_DEAD;
    g_fw.rng = 0x51ed270fu; g_fw.cooldown = 10; g_fw.init = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 52 / 100; if(ww < 340) ww = 340; if(ww > 640) ww = 640; if(ww > W - 40) ww = W - 40;
    int wh = H * 60 / 100; if(wh < 320) wh = 320; if(wh > 640) wh = 640; if(wh > H - 40) wh = H - 40;
    g_fw.win = wm_open("Fireworks", ww, wh, fw_draw, fw_event, &g_fw);
}

/* ==========================================================================
 * 6) Colour Gradient explorer - 4-corner bilinear interpolation
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int hue[4];               /* corner hues TL,TR,BL,BR                       */
    int sel;                  /* selected corner 0..3                         */
    int radial;               /* 0 = bilinear, 1 = radial                     */
    int dirty;                /* 1 -> gradient field needs re-rendering        */
    int cw_last, ch_last;     /* client size the cache was built for           */
} gr_state;
static gr_state g_gr;

static UINT32 gr_corner_col(gr_state *g, int i){ return gfx_hsv(g->hue[i], 220, 255); }

/* per-column horizontal-blend scratch (bilinear); frame-static, window-bounded */
static unsigned char gr_tr[640], gr_tg[640], gr_tb[640];
static unsigned char gr_br[640], gr_bg[640], gr_bb[640];

/* Rendered-block colour cache: the gradient field itself is static between
 * edits, so we recompute it (incl. the per-pixel radial isqrt) only when
 * `dirty` and blit from here every frame. Markers/HUD are drawn live on top. */
static UINT32 gr_cache[(MAXW / 2) * (MAXH / 2)];

static void gr_draw(wm_window *w, int cx, int cy, int cw, int ch){
    gr_state *g = (gr_state *)wm_user(w);
    int top = 22;
    grect(cx, cy, cw, ch, 0, 0, cw, top, 0x00181820);
    int gw = cw, gh = ch - top;
    if(gw < 2 || gh < 2) return;
    int step = 2;
    const int cap = (int)(sizeof(gr_cache) / sizeof(gr_cache[0]));

    /* A resize changes the block layout; re-render. */
    if(cw != g->cw_last || ch != g->ch_last){ g->dirty = 1; g->cw_last = cw; g->ch_last = ch; }

    /* Recompute the gradient field (incl. the radial isqrt) only on an edit. */
    if(g->dirty){
        UINT32 c0 = gr_corner_col(g, 0), c1 = gr_corner_col(g, 1);
        UINT32 c2 = gr_corner_col(g, 2), c3 = gr_corner_col(g, 3);
        /* corner channel bytes are frame-constant: extract once              */
        int c0r = c0 >> 16 & 255, c0g = c0 >> 8 & 255, c0b = c0 & 255;
        int c1r = c1 >> 16 & 255, c1g = c1 >> 8 & 255, c1b = c1 & 255;
        int c2r = c2 >> 16 & 255, c2g = c2 >> 8 & 255, c2b = c2 & 255;
        int c3r = c3 >> 16 & 255, c3g = c3 >> 8 & 255, c3b = c3 & 255;
        int maxr = (gw + gh) / 4; if(maxr < 1) maxr = 1;   /* radial, loop-invariant */
        if(!g->radial){
            /* horizontal blend depends only on x -> precompute per column    */
            for(int x = 0; x < gw; x += step){
                int fx = x * 255 / (gw - 1);
                gr_tr[x] = (unsigned char)((c0r * (255 - fx) + c1r * fx) / 255);
                gr_tg[x] = (unsigned char)((c0g * (255 - fx) + c1g * fx) / 255);
                gr_tb[x] = (unsigned char)((c0b * (255 - fx) + c1b * fx) / 255);
                gr_br[x] = (unsigned char)((c2r * (255 - fx) + c3r * fx) / 255);
                gr_bg[x] = (unsigned char)((c2g * (255 - fx) + c3g * fx) / 255);
                gr_bb[x] = (unsigned char)((c2b * (255 - fx) + c3b * fx) / 255);
            }
        }
        int idx = 0;
        for(int y = 0; y < gh && idx < cap; y += step){
            int fy = y * 255 / (gh - 1);
            for(int x = 0; x < gw && idx < cap; x += step){
                UINT32 col;
                if(g->radial){
                    int dx = x - gw / 2, dy = y - gh / 2;
                    int rr = isqrt_i(dx * dx + dy * dy) * 255 / maxr;
                    if(rr > 255) rr = 255;
                    int r = (c0r * (255 - rr) + c3r * rr) / 255;
                    int gg = (c0g * (255 - rr) + c3g * rr) / 255;
                    int b = (c0b * (255 - rr) + c3b * rr) / 255;
                    col = gfx_rgb(r, gg, b);
                } else {
                    /* bilinear: horizontal blend cached, apply vertical here  */
                    int r = (gr_tr[x] * (255 - fy) + gr_br[x] * fy) / 255;
                    int gg = (gr_tg[x] * (255 - fy) + gr_bg[x] * fy) / 255;
                    int b = (gr_tb[x] * (255 - fy) + gr_bb[x] * fy) / 255;
                    col = gfx_rgb(r, gg, b);
                }
                gr_cache[idx++] = col;
            }
        }
        g->dirty = 0;
    }

    /* Blit the cached gradient field every frame. */
    {
        int idx = 0;
        for(int y = 0; y < gh && idx < cap; y += step)
            for(int x = 0; x < gw && idx < cap; x += step)
                grect(cx, cy, cw, ch, x, top + y, step, step, gr_cache[idx++]);
    }
    /* selection markers on corners (bilinear only) */
    if(!g->radial){
        int mk[4][2] = {{6,6},{gw-6,6},{6,gh-6},{gw-6,gh-6}};
        for(int i = 0; i < 4; i++){
            UINT32 c = (i == g->sel) ? 0x00FFFFFF : 0x00202020;
            gdisc(cx, cy, cw, ch, mk[i][0], top + mk[i][1], i == g->sel ? 5 : 3, c);
        }
    }
    char buf[64]; int p = 0;
    const char *h = g->radial ? "Gradient RADIAL  Space:mode  arrows:hue"
                              : "Gradient  Tab:corner  arrows:hue  Space:mode";
    while(h[p] && p < 63){ buf[p] = h[p]; p++; } buf[p] = 0;
    gfx_hud(cx, cy, cw, buf, 0x00FFFFFF);
}

static int gr_event(wm_window *w, const wm_event *ev){
    gr_state *g = (gr_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ g->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    if(ev->type == WM_EV_KEY){
        /* Tab or L/R cycles the selected corner; Up/Dn edits its hue. Only hue
         * and radial-mode changes alter the cached field -> set dirty for those;
         * selection changes only move the live markers, so they stay uncached. */
        if(ev->unicode == '\t') g->sel = (g->sel + 1) % 4;
        if(ev->unicode == ' ')  { g->radial = !g->radial; g->dirty = 1; }
        if(ev->scancode == SCAN_UP)    { g->hue[g->sel] = (g->hue[g->sel] + 6) & 255; g->dirty = 1; }
        if(ev->scancode == SCAN_DOWN)  { g->hue[g->sel] = (g->hue[g->sel] - 6) & 255; g->dirty = 1; }
        if(ev->scancode == SCAN_RIGHT) g->sel = (g->sel + 1) % 4;
        if(ev->scancode == SCAN_LEFT)  g->sel = (g->sel + 3) % 4;
    } else if(ev->type == WM_EV_MOUSE_DOWN && !g->radial){
        int cw = wm_client_w(w), ch = wm_client_h(w);
        int right = ev->mx > cw / 2, bottom = (ev->my - 22) > (ch - 22) / 2;
        g->sel = (bottom ? 2 : 0) + (right ? 1 : 0);
    }
    return 0;
}

void tool_gfx_gradient_open(void){
    if(g_gr.win) return;
    g_gr.hue[0] = 0; g_gr.hue[1] = 64; g_gr.hue[2] = 170; g_gr.hue[3] = 210;
    g_gr.sel = 0; g_gr.radial = 0;
    g_gr.dirty = 1;               /* force a rebuild: the static cache/size may
                                     survive from a previous open with old hues */
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 52 / 100; if(ww < 340) ww = 340; if(ww > 600) ww = 600; if(ww > W - 40) ww = W - 40;
    int wh = H * 50 / 100; if(wh < 260) wh = 260; if(wh > 500) wh = 500; if(wh > H - 40) wh = H - 40;
    g_gr.win = wm_open("Gradient Explorer", ww, wh, gr_draw, gr_event, &g_gr);
}

/* ==========================================================================
 * 7) Lissajous curves - int sine LUT
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int a, b;                 /* frequency ratio                              */
    int delta;                /* phase, advances each frame                   */
} li_state;
static li_state g_li;

static void li_draw(wm_window *w, int cx, int cy, int cw, int ch){
    li_state *s = (li_state *)wm_user(w);
    s->delta += 1;
    grect(cx, cy, cw, ch, 0, 0, cw, ch, 0x00000000);
    int mx = cw / 2, my = (ch + 18) / 2;
    int R = (cw < ch ? cw : ch) / 2 - 14; if(R < 8) R = 8;
    int prevx = 0, prevy = 0, have = 0;
    int N = 256;
    for(int i = 0; i <= N; i++){
        int ang = i;                                  /* i*256/N, N==256      */
        int x = mx + (icos(s->a * ang + s->delta) * R >> 12);
        int y = my + (isin(s->b * ang) * R >> 12);
        UINT32 col = gfx_hsv((i + s->delta) & 255, 220, 255);
        if(have) gline(cx, cy, cw, ch, prevx, prevy, x, y, col);
        prevx = x; prevy = y; have = 1;
    }
    char buf[48]; int p = 0;
    const char *pre = "Lissajous a="; while(pre[p]){ buf[p] = pre[p]; p++; }
    buf[p++] = (char)('0' + (s->a % 10));
    buf[p++] = ' '; buf[p++] = 'b'; buf[p++] = '=';
    buf[p++] = (char)('0' + (s->b % 10));
    const char *suf = "  arrows:a/b"; int q = 0; while(suf[q] && p < 47) buf[p++] = suf[q++];
    buf[p] = 0;
    gfx_hud(cx, cy, cw, buf, 0x00FFFFFF);
}

static int li_event(wm_window *w, const wm_event *ev){
    li_state *s = (li_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ s->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    if(ev->type == WM_EV_KEY){
        if(ev->scancode == SCAN_RIGHT && s->a < 9) s->a++;
        if(ev->scancode == SCAN_LEFT  && s->a > 1) s->a--;
        if(ev->scancode == SCAN_UP    && s->b < 9) s->b++;
        if(ev->scancode == SCAN_DOWN  && s->b > 1) s->b--;
    }
    return 0;
}

void tool_gfx_lissajous_open(void){
    if(g_li.win) return;
    g_li.a = 3; g_li.b = 2; g_li.delta = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 48 / 100; if(ww < 320) ww = 320; if(ww > 520) ww = 520; if(ww > W - 40) ww = W - 40;
    int wh = H * 52 / 100; if(wh < 300) wh = 300; if(wh > 520) wh = 520; if(wh > H - 40) wh = H - 40;
    g_li.win = wm_open("Lissajous", ww, wh, li_draw, li_event, &g_li);
}

/* ==========================================================================
 * 8) Bouncing balls - physics toy
 * ========================================================================== */
#define BL_MAX 32
#define BL_G   36
typedef struct {
    int x, y, vx, vy;         /* pos<<8, vel<<8                               */
    int r;
    UINT32 col;
} bl_ball;
typedef struct {
    wm_window *win;
    unsigned rng;
    bl_ball b[BL_MAX];
    int n;
    int gravity;
    int init;
} bl_state;
static bl_state g_bl;

static void bl_add(bl_state *s, int cw, int ch){
    if(s->n >= BL_MAX) return;
    bl_ball *b = &s->b[s->n];
    b->r = 8 + (int)(gfx_rand(&s->rng) % 12);
    b->x = ((b->r + (int)(gfx_rand(&s->rng) % (unsigned)(cw > 2 * b->r ? cw - 2 * b->r : 1)))) << 8;
    b->y = ((b->r + (int)(gfx_rand(&s->rng) % (unsigned)(ch / 2 > b->r ? ch / 2 : 1)))) << 8;
    b->vx = ((int)(gfx_rand(&s->rng) % 800)) - 400;
    b->vy = ((int)(gfx_rand(&s->rng) % 400));
    b->col = gfx_hsv((int)(gfx_rand(&s->rng) & 255), 200, 255);
    s->n++;
}

static void bl_draw(wm_window *w, int cx, int cy, int cw, int ch){
    bl_state *s = (bl_state *)wm_user(w);
    if(!s->init){ s->init = 1; s->n = 0; for(int i = 0; i < 6; i++) bl_add(s, cw, ch); }
    grect(cx, cy, cw, ch, 0, 0, cw, ch, 0x00101018);
    for(int i = 0; i < s->n; i++){
        bl_ball *b = &s->b[i];
        if(s->gravity) b->vy += BL_G;
        b->x += b->vx;
        b->y += b->vy;
        int r = b->r, rf = r << 8;
        int wf = cw << 8, hf = ch << 8;
        if(b->x < rf){ b->x = rf; b->vx = -b->vx * 9 / 10; }
        if(b->x > wf - rf){ b->x = wf - rf; b->vx = -b->vx * 9 / 10; }
        if(b->y < rf){ b->y = rf; b->vy = -b->vy * 9 / 10; }
        if(b->y > hf - rf){
            b->y = hf - rf;
            b->vy = -b->vy * (s->gravity ? 8 : 10) / 10;
            if(s->gravity){ b->vx = b->vx * 98 / 100; }   /* floor friction   */
        }
        int px = b->x >> 8, py = b->y >> 8;
        gdisc(cx, cy, cw, ch, px, py, r, b->col);
        /* simple highlight */
        gdisc(cx, cy, cw, ch, px - r / 3, py - r / 3, r / 4, gfx_light(b->col, 110));
    }
    gfx_hud(cx, cy, cw, "Bouncing Balls  +/-:count  Space:gravity", 0x00FFFFFF);
}

static int bl_event(wm_window *w, const wm_event *ev){
    bl_state *s = (bl_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ s->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    int cw = wm_client_w(w), ch = wm_client_h(w);
    if(ev->type == WM_EV_KEY){
        if(ev->unicode == '+' || ev->unicode == '=') bl_add(s, cw, ch);
        if((ev->unicode == '-' || ev->unicode == '_') && s->n > 0) s->n--;
        if(ev->unicode == ' ') s->gravity = !s->gravity;
    } else if(ev->type == WM_EV_MOUSE_DOWN){
        bl_add(s, cw, ch);
    }
    return 0;
}

void tool_gfx_balls_open(void){
    if(g_bl.win) return;
    g_bl.rng = 0x9e3779b1u; g_bl.n = 0; g_bl.gravity = 1; g_bl.init = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 52 / 100; if(ww < 340) ww = 340; if(ww > 620) ww = 620; if(ww > W - 40) ww = W - 40;
    int wh = H * 55 / 100; if(wh < 300) wh = 300; if(wh > 560) wh = 560; if(wh > H - 40) wh = H - 40;
    g_bl.win = wm_open("Bouncing Balls", ww, wh, bl_draw, bl_event, &g_bl);
}

/* ==========================================================================
 * 9) Sierpinski - chaos game fractal
 * ========================================================================== */
#define SI_PTS  6000
#define SI_BATCH 260
typedef struct {
    wm_window *win;
    unsigned rng;
    int nverts;               /* 3..6                                         */
    int px, py;               /* current point, normalized 0..65535           */
    short bx[SI_PTS], by[SI_PTS];  /* stored normalized points (ring)         */
    unsigned char bc[SI_PTS];      /* chosen-vertex tag for colour            */
    int count;                /* live points                                  */
    int wr;                   /* ring write index                             */
    int init;
} si_state;
static si_state g_si;

/* chaos-game contraction ratio per vertex count (num/den), tuned for looks. */
static void si_ratio(int n, int *num, int *den){
    switch(n){
        case 3:  *num = 1; *den = 2; break;   /* classic Sierpinski triangle  */
        case 4:  *num = 2; *den = 5; break;
        case 5:  *num = 3; *den = 8; break;
        default: *num = 1; *den = 3; break;   /* 6                            */
    }
}

static void si_reset(si_state *s){
    s->px = 32768; s->py = 32768;
    s->count = 0; s->wr = 0;
}

static void si_step(si_state *s){
    int num, den; si_ratio(s->nverts, &num, &den);
    /* the nverts (<=6) vertex positions are fixed for the whole batch         */
    int vx[6], vy[6];
    for(int v = 0; v < s->nverts; v++){
        int ang = v * 256 / s->nverts - 64;   /* start at top                 */
        vx[v] = 32768 + (icos(ang) * 30000 >> 12);
        vy[v] = 32768 + (isin(ang) * 30000 >> 12);
    }
    for(int k = 0; k < SI_BATCH; k++){
        int v = (int)(gfx_rand(&s->rng) % (unsigned)s->nverts);
        /* vertex on a circle of radius ~30000 centred at 32768 */
        s->px += (vx[v] - s->px) * num / den;
        s->py += (vy[v] - s->py) * num / den;
        int idx = s->wr;
        s->bx[idx] = (short)(s->px - 32768);   /* store signed around centre  */
        s->by[idx] = (short)(s->py - 32768);
        s->bc[idx] = (unsigned char)v;
        s->wr = (s->wr + 1) % SI_PTS;
        if(s->count < SI_PTS) s->count++;
    }
}

static void si_draw(wm_window *w, int cx, int cy, int cw, int ch){
    si_state *s = (si_state *)wm_user(w);
    if(!s->init){ s->init = 1; si_reset(s); }
    grect(cx, cy, cw, ch, 0, 0, cw, ch, 0x00000000);
    si_step(s);
    int top = 20;
    int gw = cw, gh = ch - top;
    if(gw < 4 || gh < 4) return;
    int size = (gw < gh ? gw : gh) - 8; if(size < 4) size = 4;
    int ox = cw / 2, oy = top + gh / 2;
    int half = size / 2;                   /* frame-constant                   */
    /* bc[i] only takes nverts (<=6) values -> precompute the colour palette   */
    int nv = s->nverts ? s->nverts : 1;
    UINT32 pal[6];
    for(int v = 0; v < s->nverts; v++)
        pal[v] = gfx_hsv(v * 256 / nv + 20, 210, 255);
    for(int i = 0; i < s->count; i++){
        /* normalized signed -32768..32768 -> pixels within `size` box        */
        int x = ox + (int)s->bx[i] * half / 32768;
        int y = oy + (int)s->by[i] * half / 32768;
        UINT32 col = pal[s->bc[i]];
        gpx(cx, cy, cw, ch, x, y, col);
    }
    char buf[56]; int p = 0;
    const char *pre = "Sierpinski verts="; while(pre[p]){ buf[p] = pre[p]; p++; }
    buf[p++] = (char)('0' + s->nverts);
    const char *suf = "  Up/Dn:verts  R:reset"; int q = 0;
    while(suf[q] && p < 55) buf[p++] = suf[q++];
    buf[p] = 0;
    gfx_hud(cx, cy, cw, buf, 0x00E0D0FF);
}

static int si_event(wm_window *w, const wm_event *ev){
    si_state *s = (si_state *)wm_user(w);
    if(ev->type == WM_EV_CLOSE){ s->win = 0; return 0; }
    if(gfx_is_close(ev)) return WM_CLOSE_REQUEST;
    if(ev->type == WM_EV_KEY){
        if(ev->scancode == SCAN_UP   && s->nverts < 6){ s->nverts++; si_reset(s); }
        if(ev->scancode == SCAN_DOWN && s->nverts > 3){ s->nverts--; si_reset(s); }
        if(ev->unicode == 'r' || ev->unicode == 'R') si_reset(s);
    }
    return 0;
}

void tool_gfx_sierpinski_open(void){
    if(g_si.win) return;
    g_si.rng = 0x13579bdfu; g_si.nverts = 3; g_si.init = 0;
    si_reset(&g_si);
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 50 / 100; if(ww < 340) ww = 340; if(ww > 560) ww = 560; if(ww > W - 40) ww = W - 40;
    int wh = H * 55 / 100; if(wh < 320) wh = 320; if(wh > 560) wh = 560; if(wh > H - 40) wh = H - 40;
    g_si.win = wm_open("Sierpinski", ww, wh, si_draw, si_event, &g_si);
}

/* ==========================================================================
 * Category registry
 * ========================================================================== */
const struct forebo_tool cat_gfx_tools[] = {
    { "Mandelbrot",  "Integer fixed-point set, pan + zoom",        "os",   tool_gfx_mandelbrot_open },
    { "Plasma",      "Animated sine-LUT plasma field",            "os",   tool_gfx_plasma_open     },
    { "Starfield",   "3D-ish flying starfield, steerable",        "os",   tool_gfx_starfield_open  },
    { "Matrix Rain", "Falling green character rain",              "text", tool_gfx_matrix_open     },
    { "Fireworks",   "Particle fireworks with gravity",           "os",   tool_gfx_fireworks_open  },
    { "Gradient",    "Interactive 4-corner colour gradient",      "os",   tool_gfx_gradient_open   },
    { "Lissajous",   "Animated Lissajous curves (sine LUT)",      "os",   tool_gfx_lissajous_open  },
    { "Bouncing Balls","Gravity + wall-bounce physics toy",       "os",   tool_gfx_balls_open      },
    { "Sierpinski",  "Chaos-game fractal (3-6 vertices)",         "os",   tool_gfx_sierpinski_open },
};
const int cat_gfx_count = (int)(sizeof(cat_gfx_tools) / sizeof(cat_gfx_tools[0]));
