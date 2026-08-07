/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_toys.c - "Toys & Audio" tool category (KEY = toys).
 * =============================================================================
 * Nine self-contained template-B wm.c windows: Pixel Paint, Piano, Tone
 * Generator, Metronome, Colour Mixer, ASCII Aquarium, Starfield, Drum Pads and
 * a Bouncing DVD logo. Audio is the legacy PC speaker (PIT ch2 + port 0x61);
 * gBS->Stall (captured in cat_toys_init) provides tone timing.
 *
 * Freestanding: no libc, no heap, no float/SSE. Integer + fixed-point only.
 * Every draw callback CLIPS to the client rect [cx,cx+cw) x [cy,cy+ch).
 * ========================================================================== */
#include "tools_toys.h"
#include "../efi.h"
#include "../core/wm.h"
#include "../ui.h"
#include "../core/input.h"
#include "../standalone/audio.h"
#include "../../include/forebo_theme.h"

/* ==========================================================================
 * Captured firmware services (for Stall + RTC).
 * ========================================================================== */
static EFI_SYSTEM_TABLE     *gST;
static EFI_BOOT_SERVICES    *gBS;
static EFI_RUNTIME_SERVICES *gRT;

void cat_toys_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices    : 0;
    gRT = st ? st->RuntimeServices : 0;
}

/* ==========================================================================
 * Low-level x86 port I/O + PC-speaker (PIT channel 2).
 * ========================================================================== */
#if defined(__x86_64__) || defined(__i386__)
static inline void t_outb(UINT16 p, UINT8 v)
{ __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline UINT8 t_inb(UINT16 p)
{ UINT8 v; __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline UINT32 t_tsc(void)
{ UINT32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return lo ^ (hi << 1); }
#else
static inline void  t_outb(UINT16 p, UINT8 v) { (void)p; (void)v; }
static inline UINT8 t_inb(UINT16 p) { (void)p; return 0; }
static inline UINT32 t_tsc(void) { return 0x2468ACEu; }
#endif

/* Start a continuous square-wave tone (does not block). */
static void spk_tone_on(unsigned freq)
{
    if (freq < 20)    freq = 20;
    if (freq > 20000) freq = 20000;
    unsigned div = 1193182u / freq;
    if (div < 1)     div = 1;
    if (div > 65535) div = 65535;
    t_outb(0x43, 0xB6);                               /* ch2, lo/hi, mode3    */
    t_outb(0x42, (UINT8)(div & 0xFF));
    t_outb(0x42, (UINT8)((div >> 8) & 0xFF));
    UINT8 s = t_inb(0x61);
    if ((s & 3) != 3) t_outb(0x61, (UINT8)(s | 3));   /* gate + speaker on    */
}
static void spk_off(void)
{
    UINT8 s = t_inb(0x61);
    t_outb(0x61, (UINT8)(s & ~3));
}
/* Tones are non-blocking: callers start a tone with spk_tone_on() and store an
 * "off deadline" as a frame counter in their tool state; each draw callback
 * decrements it and calls spk_off() when it reaches zero.  This keeps the input
 * and draw paths free of gBS->Stall (which stalls the whole compositor and, on
 * real uncached-VRAM hardware, causes visible lag and fps-calibration drift). */

/* ==========================================================================
 * Shared integer helpers.
 * ========================================================================== */
/* xorshift PRNG. */
static UINT32 g_rng = 0x2545F491u;
static UINT32 rnd(void)
{ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static void seed_rng(void) { g_rng = t_tsc() | 1u; }
static int rnd_range(int lo, int hi) /* inclusive */
{ if (hi <= lo) return lo; return lo + (int)(rnd() % (UINT32)(hi - lo + 1)); }

/* sin(deg)*10000, integer, via a 0..90 quarter table. */
static const short g_sin90[91] = {
    0,175,349,523,698,872,1045,1219,1392,1564,1736,1908,2079,2250,2419,2588,
    2756,2924,3090,3256,3420,3584,3746,3907,4067,4226,4384,4540,4695,4848,5000,
    5150,5299,5446,5592,5736,5878,6018,6157,6293,6428,6561,6691,6820,6947,7071,
    7193,7314,7431,7547,7660,7771,7880,7986,8090,8192,8290,8387,8480,8572,8660,
    8746,8829,8910,8988,9063,9135,9205,9272,9336,9397,9455,9511,9563,9613,9659,
    9703,9744,9781,9816,9848,9877,9903,9925,9945,9962,9976,9986,9994,9998,10000
};
static int isin(int d)
{
    d %= 360; if (d < 0) d += 360;
    if (d <= 90)  return  g_sin90[d];
    if (d <= 180) return  g_sin90[180 - d];
    if (d <= 270) return -g_sin90[d - 180];
    return               -g_sin90[360 - d];
}

/* Clipped fill to a client rect. */
static void cfill(int cx, int cy, int cw, int ch, int x, int y, int w, int h, UINT32 col)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < cx) x0 = cx;  if (y0 < cy) y0 = cy;
    if (x1 > cx + cw) x1 = cx + cw;  if (y1 > cy + ch) y1 = cy + ch;
    if (x1 > x0 && y1 > y0) fill_rect(x0, y0, x1 - x0, y1 - y0, col);
}
/* Draw a glyph only if it lands fully inside the client rect (precomputed scale). */
static void cchar_s(int cx, int cy, int cw, int ch, int x, int y, char c,
                    UINT32 fg, UINT32 bg, int tr, int sc, int uis)
{
    int gw = 8 * sc * uis, gh = 16 * sc * uis;
    if (x < cx || y < cy || x + gw > cx + cw || y + gh > cy + ch) return;
    draw_char(x, y, c, fg, bg, tr, sc);
}
/* Read an RTC "seconds within the hour" (0..3599), or -1 if unavailable. */
typedef EFI_STATUS (EFIAPI *TOYS_GET_TIME)(EFI_TIME *, VOID *);
static int rtc_secs(void)
{
    if (!gRT) return -1;
    TOYS_GET_TIME gt = (TOYS_GET_TIME)gRT->GetTime;
    if (!gt) return -1;
    EFI_TIME t = {0};
    if (EFI_ERROR(gt(&t, NULL))) return -1;
    if (t.Minute > 59 || t.Second > 59) return -1;
    return (int)t.Minute * 60 + (int)t.Second;
}

/* Unsigned int -> decimal string; returns length. */
static int u2s(char *o, unsigned v)
{
    char tmp[12]; int d = 0;
    do { tmp[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 11);
    for (int i = 0; i < d; i++) o[i] = tmp[d - 1 - i];
    o[d] = 0; return d;
}

/* A shared 16-colour palette. */
static const UINT32 PAL16[16] = {
    0x00000000, 0x00FFFFFF, 0x00FF3030, 0x00FF9030, 0x00FFF030, 0x0060FF60,
    0x0030C0FF, 0x003040FF, 0x00C040FF, 0x00FF40C0, 0x00A05820, 0x00909090,
    0x0000FF90, 0x008000FF, 0x0000FFFF, 0x00C0C0C0
};

/* ==========================================================================
 * 1. PIXEL PAINT
 * ========================================================================== */
#define PGW 32
#define PGH 24
#define PAINT_EMPTY 0xFF
typedef struct {
    wm_window *win;
    UINT8 grid[PGH][PGW];
    int   color;      /* palette index 0..15 */
    int   down;       /* mouse held          */
    int   erase;      /* right button        */
    int   lgx, lgy;   /* last painted grid cell, -1 = none (drag continuity) */
} paint_state;
static paint_state g_paint;

static void paint_layout(int cw, int ch, int *sw, int *strip, int *ox, int *oy, int *cs)
{
    int s = (cw - 100) / 16; if (s < 12) s = 12; if (s > 26) s = 26;
    *sw = s;
    *strip = s + 12;
    int aw = cw - 12, ah = ch - *strip - 8;
    int a = aw / PGW, b = ah / PGH;
    int c = a < b ? a : b; if (c < 2) c = 2;
    *cs = c;
    *ox = (cw - c * PGW) / 2;
    *oy = *strip + (ah - c * PGH) / 2;
}
/* Clear-button rect (client coords). */
static void paint_clear_rect(int cw, int sw, int *bx, int *by, int *bw, int *bh)
{
    *bx = 8 + 16 * sw + 8; *by = 6; *bh = sw; *bw = cw - *bx - 8;
    if (*bw < 30) { *bx = cw - 34; *bw = 28; }
}

static void paint_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    int sw, strip, ox, oy, cs;
    paint_layout(cw, ch, &sw, &strip, &ox, &oy, &cs);
    fill_rect(cx, cy, cw, ch, 0x00202428);

    /* palette swatches */
    for (int i = 0; i < 16; i++) {
        int x = cx + 8 + i * sw, y = cy + 6;
        cfill(cx, cy, cw, ch, x, y, sw - 2, sw - 2, PAL16[i]);
        if (i == g_paint.color)
            draw_rect_outline(x - 1, y - 1, sw, sw, 2, 0x00FFFFFF);
        else
            draw_rect_outline(x, y, sw - 2, sw - 2, 1, 0x00404850);
    }
    /* clear button */
    int bx, by, bw, bh; paint_clear_rect(cw, sw, &bx, &by, &bw, &bh);
    cfill(cx, cy, cw, ch, cx + bx, cy + by, bw, bh, 0x00503030);
    draw_rect_outline(cx + bx, cy + by, bw, bh, 1, 0x00A05050);
    draw_string_clip(cx + bx + 4, cy + by + (bh - 8) / 2, bw - 8, "Clear",
                     0x00FFD0D0, 0, 1, 1);

    /* grid backing + cells */
    cfill(cx, cy, cw, ch, cx + ox, cy + oy, cs * PGW, cs * PGH, 0x00101418);
    for (int gy = 0; gy < PGH; gy++)
        for (int gx = 0; gx < PGW; gx++) {
            UINT8 v = g_paint.grid[gy][gx];
            if (v == PAINT_EMPTY) continue;
            cfill(cx, cy, cw, ch, cx + ox + gx * cs, cy + oy + gy * cs,
                  cs, cs, PAL16[v & 15]);
        }
    /* subtle grid lines */
    for (int gx = 0; gx <= PGW; gx++)
        cfill(cx, cy, cw, ch, cx + ox + gx * cs, cy + oy, 1, cs * PGH, 0x001A2028);
    for (int gy = 0; gy <= PGH; gy++)
        cfill(cx, cy, cw, ch, cx + ox, cy + oy + gy * cs, cs * PGW, 1, 0x001A2028);

    draw_string_clip(cx + 8, cy + ch - 14, cw - 16,
                     "Drag=paint  R-drag=erase  C=clear  Esc=close",
                     0x0080909A, 0, 1, 1);
}

/* Paint one grid cell (bounds-checked). */
static void paint_set(int gx, int gy)
{
    if (gx < 0 || gx >= PGW || gy < 0 || gy >= PGH) return;
    g_paint.grid[gy][gx] = g_paint.erase ? PAINT_EMPTY : (UINT8)g_paint.color;
}

static void paint_apply(int cw, int ch, int mx, int my)
{
    int sw, strip, ox, oy, cs;
    paint_layout(cw, ch, &sw, &strip, &ox, &oy, &cs);
    if (mx < ox || my < oy) return;
    int gx = (mx - ox) / cs, gy = (my - oy) / cs;
    if (gx < 0 || gx >= PGW || gy < 0 || gy >= PGH) return;
    /* Fill a Bresenham line from the previous cell so fast drags (where the
     * mouse jumps several cells per frame) stay continuous instead of dotted. */
    if (g_paint.lgx >= 0 && (g_paint.lgx != gx || g_paint.lgy != gy)) {
        int x0 = g_paint.lgx, y0 = g_paint.lgy;
        int dx =  (gx - x0 < 0) ? x0 - gx : gx - x0;
        int dy = -((gy - y0 < 0) ? y0 - gy : gy - y0);
        int sxs = x0 < gx ? 1 : -1, sys = y0 < gy ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            paint_set(x0, y0);
            if (x0 == gx && y0 == gy) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sxs; }
            if (e2 <= dx) { err += dx; y0 += sys; }
        }
    } else {
        paint_set(gx, gy);
    }
    g_paint.lgx = gx; g_paint.lgy = gy;
}

static int paint_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    int sw, strip, ox, oy, cs;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == 'c' || ev->unicode == 'C') {
            for (int y = 0; y < PGH; y++) for (int x = 0; x < PGW; x++)
                g_paint.grid[y][x] = PAINT_EMPTY;
        }
        return 0;
    case WM_EV_MOUSE_DOWN:
        g_paint.erase = (ev->button == 1);
        g_paint.down = 1;
        g_paint.lgx = g_paint.lgy = -1;   /* start a fresh stroke */
        paint_layout(cw, ch, &sw, &strip, &ox, &oy, &cs);
        /* palette hit? */
        if (ev->my >= 6 && ev->my < 6 + sw) {
            int i = (ev->mx - 8) / sw;
            if (ev->mx >= 8 && i >= 0 && i < 16) { g_paint.color = i; g_paint.down = 0; return 0; }
        }
        { int bx, by, bw, bh; paint_clear_rect(cw, sw, &bx, &by, &bw, &bh);
          if (ev->mx >= bx && ev->mx < bx + bw && ev->my >= by && ev->my < by + bh) {
              for (int y = 0; y < PGH; y++) for (int x = 0; x < PGW; x++)
                  g_paint.grid[y][x] = PAINT_EMPTY;
              g_paint.down = 0; return 0; } }
        paint_apply(cw, ch, ev->mx, ev->my);
        return 0;
    case WM_EV_MOUSE_MOVE:
        if (g_paint.down) paint_apply(cw, ch, ev->mx, ev->my);
        return 0;
    case WM_EV_MOUSE_UP:
        g_paint.down = 0; g_paint.erase = 0;
        return 0;
    case WM_EV_CLOSE:
        g_paint.win = NULL;
        return 0;
    default: return 0;
    }
}

void tool_toys_paint_open(void)
{
    if (g_paint.win) return;
    for (int y = 0; y < PGH; y++) for (int x = 0; x < PGW; x++)
        g_paint.grid[y][x] = PAINT_EMPTY;
    g_paint.color = 2; g_paint.down = 0; g_paint.erase = 0;
    g_paint.lgx = g_paint.lgy = -1;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 55 / 100; if (ww < 480) ww = 480; if (ww > 760) ww = 760; if (ww > W - 40) ww = W - 40;
    int wh = H * 62 / 100; if (wh < 380) wh = 380; if (wh > 640) wh = 640; if (wh > H - 40) wh = H - 40;
    g_paint.win = wm_open("Pixel Paint", ww, wh, paint_draw, paint_event, &g_paint);
}

/* ==========================================================================
 * 2. PIANO
 * ========================================================================== */
static const unsigned PIANO_WHITE[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
static const char     PIANO_WLBL[8]  = { 'C','D','E','F','G','A','B','C' };
/* black key sits AFTER white index [i]; -1 terminates. */
static const int      PIANO_BAFTER[5] = { 0, 1, 3, 4, 5 };
static const unsigned PIANO_BLACK[5]  = { 277, 311, 370, 415, 466 };
typedef struct { wm_window *win; int flash_w; int flash_b; int flasht; int sndt; } piano_state;
#define PIANO_SNDT 6   /* tone-off deadline in frames (~140-200ms) */
static piano_state g_piano;

static void piano_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, 0x00181818);
    draw_string_clip(cx + 8, cy + 6, cw - 16,
                     "Click keys or A S D F G H J K   Esc=close",
                     0x00B0B0B0, 0, 1, 1);
    int top = cy + 28, kh = ch - 40;
    int ww = cw / 8;
    /* white keys */
    for (int i = 0; i < 8; i++) {
        int x = cx + i * ww;
        UINT32 col = (g_piano.flasht > 0 && g_piano.flash_w == i) ? 0x00A0D0FF : 0x00F0F0F0;
        cfill(cx, cy, cw, ch, x + 1, top, ww - 2, kh, col);
        draw_rect_outline(x + 1, top, ww - 2, kh, 1, 0x00303030);
        char l[2] = { PIANO_WLBL[i], 0 };
        draw_string_clip(x + ww / 2 - 4, top + kh - 18, ww, l, 0x00303030, 0, 1, 1);
    }
    /* black keys */
    int bw = ww * 6 / 10, bh = kh * 6 / 10;
    for (int i = 0; i < 5; i++) {
        int x = cx + (PIANO_BAFTER[i] + 1) * ww - bw / 2;
        UINT32 col = (g_piano.flasht > 0 && g_piano.flash_b == i) ? 0x004060A0 : 0x00101010;
        cfill(cx, cy, cw, ch, x, top, bw, bh, col);
        draw_rect_outline(x, top, bw, bh, 1, 0x00000000);
    }
    if (g_piano.flasht > 0) g_piano.flasht--;
    if (g_piano.sndt && --g_piano.sndt == 0) spk_off();
}

static void piano_hit(int cx, int cy, int cw, int ch, int mx, int my)
{
    (void)cx; (void)cy;
    int top = 28, kh = ch - 40, ww = cw / 8;
    int bw = ww * 6 / 10, bh = kh * 6 / 10;
    if (my < top) return;
    /* black keys first (on top) */
    for (int i = 0; i < 5; i++) {
        int x = (PIANO_BAFTER[i] + 1) * ww - bw / 2;
        if (mx >= x && mx < x + bw && my >= top && my < top + bh) {
            g_piano.flash_b = i; g_piano.flash_w = -1; g_piano.flasht = 4;
            spk_tone_on(PIANO_BLACK[i]); g_piano.sndt = PIANO_SNDT; return;
        }
    }
    int i = mx / ww; if (i < 0) i = 0; if (i > 7) i = 7;
    g_piano.flash_w = i; g_piano.flash_b = -1; g_piano.flasht = 4;
    spk_tone_on(PIANO_WHITE[i]); g_piano.sndt = PIANO_SNDT;
}

static int piano_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    switch (ev->type) {
    case WM_EV_KEY: {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        const char keys[9] = "asdfghjk";
        for (int i = 0; i < 8; i++)
            if (ev->unicode == keys[i] || ev->unicode == (keys[i] - 32)) {
                g_piano.flash_w = i; g_piano.flash_b = -1; g_piano.flasht = 4;
                spk_tone_on(PIANO_WHITE[i]); g_piano.sndt = PIANO_SNDT; return 0;
            }
        return 0;
    }
    case WM_EV_MOUSE_DOWN: piano_hit(0, 0, cw, ch, ev->mx, ev->my); return 0;
    case WM_EV_CLOSE: g_piano.win = NULL; spk_off(); return 0;
    default: return 0;
    }
}

void tool_toys_piano_open(void)
{
    if (g_piano.win) return;
    g_piano.flash_w = g_piano.flash_b = -1; g_piano.flasht = 0; g_piano.sndt = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 52 / 100; if (ww < 480) ww = 480; if (ww > 720) ww = 720; if (ww > W - 40) ww = W - 40;
    int wh = H * 40 / 100; if (wh < 260) wh = 260; if (wh > 380) wh = 380; if (wh > H - 40) wh = H - 40;
    g_piano.win = wm_open("Piano", ww, wh, piano_draw, piano_event, &g_piano);
}

/* ==========================================================================
 * 3. TONE GENERATOR
 * ========================================================================== */
#define TONE_MIN 50
#define TONE_MAX 3000
typedef struct { wm_window *win; int freq; int playing; int drag; int prog_freq; } tone_state;
static tone_state g_tone;

static void tone_slider_rect(int cw, int ch, int *sx, int *sy, int *sw, int *sh)
{ *sx = 24; *sy = ch / 2 - 10; *sw = cw - 48; *sh = 20; }

static void tone_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, 0x00141820);
    if (g_tone.playing) {                                     /* live update */
        if (g_tone.freq != g_tone.prog_freq) {                /* only re-program on change */
            spk_tone_on((unsigned)g_tone.freq);
            g_tone.prog_freq = g_tone.freq;
        }
    } else {
        g_tone.prog_freq = -1;                                /* speaker off -> force reprogram */
    }

    char line[48]; int p = 0;
    const char *pre = "Frequency: ";
    while (*pre) line[p++] = *pre++;
    p += u2s(line + p, (unsigned)g_tone.freq);
    line[p++] = ' '; line[p++] = 'H'; line[p++] = 'z'; line[p] = 0;
    draw_string_clip(cx + 24, cy + 24, cw - 48, line, 0x00E0F0FF, 0, 1, 2);

    int sx, sy, sw, sh; tone_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
    /* gradient track: coalesce runs of equal hue into a single wider fill */
    int sden = (sw > 1 ? sw - 1 : 1);
    for (int i = 0; i < sw; ) {
        int hue = 40 + i * 200 / sden;
        int j = i + 1;
        while (j < sw && 40 + j * 200 / sden == hue) j++;
        cfill(cx, cy, cw, ch, cx + sx + i, cy + sy, j - i, sh,
              ((UINT32)hue << 16) | ((UINT32)(120) << 8) | 0x40);
        i = j;
    }
    draw_rect_outline(cx + sx, cy + sy, sw, sh, 1, 0x00405060);
    int kx = (g_tone.freq - TONE_MIN) * (sw - 1) / (TONE_MAX - TONE_MIN);
    cfill(cx, cy, cw, ch, cx + sx + kx - 3, cy + sy - 4, 7, sh + 8, 0x00FFFFFF);

    /* play/stop button */
    int bw = 120, bh = 34, bxx = cx + cw / 2 - bw / 2, byy = cy + sy + 44;
    cfill(cx, cy, cw, ch, bxx, byy, bw, bh, g_tone.playing ? 0x00803030 : 0x00306030);
    draw_rect_outline(bxx, byy, bw, bh, 1, 0x00A0A0A0);
    draw_string_clip(bxx + 12, byy + (bh - 16) / 2, bw - 20,
                     g_tone.playing ? "STOP" : "PLAY", 0x00FFFFFF, 0, 1, 2);

    draw_string_clip(cx + 24, cy + ch - 16, cw - 48,
                     "Drag slider  Up/Down=nudge  Space=play/stop  Esc=close",
                     0x0070808A, 0, 1, 1);
}

static void tone_set_from_x(int cw, int ch, int mx)
{
    int sx, sy, sw, sh; tone_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
    int v = TONE_MIN + (mx - sx) * (TONE_MAX - TONE_MIN) / (sw > 1 ? sw - 1 : 1);
    if (v < TONE_MIN) v = TONE_MIN; if (v > TONE_MAX) v = TONE_MAX;
    g_tone.freq = v;
}

static int tone_btn_rect(int cw, int ch, int mx, int my)
{
    int sx, sy, sw, sh; tone_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
    int bw = 120, bh = 34, bxx = cw / 2 - bw / 2, byy = sy + 44;
    return (mx >= bxx && mx < bxx + bw && my >= byy && my < byy + bh);
}

static int tone_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    int sx, sy, sw, sh;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) { spk_off(); g_tone.playing = 0; return WM_CLOSE_REQUEST; }
        if (ev->scancode == SCAN_UP)   { g_tone.freq += 10; if (g_tone.freq > TONE_MAX) g_tone.freq = TONE_MAX; }
        if (ev->scancode == SCAN_DOWN) { g_tone.freq -= 10; if (g_tone.freq < TONE_MIN) g_tone.freq = TONE_MIN; }
        if (ev->unicode == ' ') { g_tone.playing = !g_tone.playing; if (!g_tone.playing) spk_off(); }
        return 0;
    case WM_EV_MOUSE_DOWN:
        if (tone_btn_rect(cw, ch, ev->mx, ev->my)) {
            g_tone.playing = !g_tone.playing; if (!g_tone.playing) spk_off(); return 0;
        }
        tone_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
        if (ev->my >= sy - 6 && ev->my <= sy + sh + 6) { g_tone.drag = 1; tone_set_from_x(cw, ch, ev->mx); }
        return 0;
    case WM_EV_MOUSE_MOVE:
        if (g_tone.drag) tone_set_from_x(cw, ch, ev->mx);
        return 0;
    case WM_EV_MOUSE_UP: g_tone.drag = 0; return 0;
    case WM_EV_CLOSE: g_tone.win = NULL; spk_off(); return 0;
    default: return 0;
    }
}

void tool_toys_tone_open(void)
{
    if (g_tone.win) return;
    g_tone.freq = 440; g_tone.playing = 0; g_tone.drag = 0; g_tone.prog_freq = -1;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 46 / 100; if (ww < 420) ww = 420; if (ww > 620) ww = 620; if (ww > W - 40) ww = W - 40;
    int wh = H * 40 / 100; if (wh < 240) wh = 240; if (wh > 340) wh = 340; if (wh > H - 40) wh = H - 40;
    g_tone.win = wm_open("Tone Generator", ww, wh, tone_draw, tone_event, &g_tone);
}

/* ==========================================================================
 * 4. METRONOME
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int  bpm;
    int  running;
    int  drag;
    int  fps;          /* RTC-calibrated frames/sec  */
    int  last_sec;     /* RTC second at last sample  */
    unsigned fcount;   /* free-running frame counter */
    unsigned fbase;    /* fcount at last RTC sample  */
    unsigned acc;      /* frames since last beat     */
    int  beat;         /* current beat 0..3          */
    int  flash;        /* flash countdown            */
    int  sndt;         /* tick tone-off deadline     */
} metro_state;
static metro_state g_metro;
#define METRO_MIN 40
#define METRO_MAX 240
#define METRO_BEATS 4

static void metro_slider_rect(int cw, int ch, int *sx, int *sy, int *sw, int *sh)
{ *sx = 24; *sy = ch - 70; *sw = cw - 48; *sh = 18; }

static void metro_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, 0x00101418);

    /* fps calibration from the RTC.  GetTime() can be slow on real hardware, so
     * only sample it once per ~fps frames (i.e. roughly once a second) rather
     * than every frame, and derive fps from the frame/second deltas between
     * samples.  fcount is now free-running (never reset) so the gate is stable. */
    g_metro.fcount++;
    if (g_metro.fps < 5) g_metro.fps = 30;
    if ((g_metro.fcount % (unsigned)g_metro.fps) == 0) {
        int s = rtc_secs();
        if (s >= 0) {
            if (g_metro.last_sec >= 0) {
                int d = s - g_metro.last_sec; if (d < 0) d += 3600;
                unsigned df = g_metro.fcount - g_metro.fbase;
                if (d >= 1 && d < 60 && df > 0 && df < 6000)
                    g_metro.fps = (int)(df / (unsigned)d);
            }
            g_metro.last_sec = s;
            g_metro.fbase = g_metro.fcount;
        }
    }
    if (g_metro.fps < 5) g_metro.fps = 30;

    /* advance beat scheduler (non-blocking tick: start the tone here and let the
     * off-deadline below silence it, so no gBS->Stall runs inside the draw). */
    if (g_metro.running) {
        int fpb = g_metro.fps * 60 / g_metro.bpm; if (fpb < 1) fpb = 1;
        g_metro.acc++;
        if (g_metro.acc >= (unsigned)fpb) {
            g_metro.acc = 0;
            g_metro.beat = (g_metro.beat + 1) % METRO_BEATS;
            g_metro.flash = 8;
            spk_tone_on(g_metro.beat == 0 ? 1760 : 988);
            int sd = g_metro.fps * 28 / 1000; if (sd < 1) sd = 1;
            g_metro.sndt = sd;                 /* ~28ms click */
        }
    }
    if (g_metro.flash > 0) g_metro.flash--;
    if (g_metro.sndt && --g_metro.sndt == 0) spk_off();

    /* big BPM readout */
    char line[24]; int p = u2s(line, (unsigned)g_metro.bpm);
    line[p++] = ' '; line[p++] = 'B'; line[p++] = 'P'; line[p++] = 'M'; line[p] = 0;
    draw_string_center(cx + cw / 2, cy + 18, line, 0x00E0FFE0, 0, 1, 3);

    /* beat dots */
    int n = METRO_BEATS, dr = 14, gap = 12;
    int tot = n * (dr * 2) + (n - 1) * gap;
    int x0 = cx + (cw - tot) / 2, dy = cy + 80;
    for (int i = 0; i < n; i++) {
        int cxx = x0 + i * (dr * 2 + gap) + dr;
        UINT32 col = 0x00203028;
        if (i == g_metro.beat && g_metro.running)
            col = g_metro.flash > 0 ? (i == 0 ? 0x00FFF060 : 0x0060FF60) : 0x00305038;
        else if (i == 0) col = 0x00404830;
        cfill(cx, cy, cw, ch, cxx - dr, dy - dr, dr * 2, dr * 2, col);
        draw_rect_outline(cxx - dr, dy - dr, dr * 2, dr * 2, 1, 0x00506050);
    }

    /* start/stop button */
    int bw = 120, bh = 30, bxx = cx + cw / 2 - bw / 2, byy = dy + dr + 12;
    cfill(cx, cy, cw, ch, bxx, byy, bw, bh, g_metro.running ? 0x00803030 : 0x00306030);
    draw_rect_outline(bxx, byy, bw, bh, 1, 0x00A0A0A0);
    draw_string_clip(bxx + 14, byy + (bh - 16) / 2, bw - 24,
                     g_metro.running ? "STOP" : "START", 0x00FFFFFF, 0, 1, 2);

    /* BPM slider */
    int sx, sy, sw, sh; metro_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
    cfill(cx, cy, cw, ch, cx + sx, cy + sy, sw, sh, 0x00202828);
    draw_rect_outline(cx + sx, cy + sy, sw, sh, 1, 0x00405048);
    int kx = (g_metro.bpm - METRO_MIN) * (sw - 1) / (METRO_MAX - METRO_MIN);
    cfill(cx, cy, cw, ch, cx + sx + kx - 3, cy + sy - 4, 7, sh + 8, 0x0080FF80);

    draw_string_clip(cx + 24, cy + ch - 16, cw - 48,
                     "Slider/Left-Right=BPM  Space=start/stop  Esc=close",
                     0x0070807A, 0, 1, 1);
}

static void metro_set_from_x(int cw, int ch, int mx)
{
    int sx, sy, sw, sh; metro_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
    int v = METRO_MIN + (mx - sx) * (METRO_MAX - METRO_MIN) / (sw > 1 ? sw - 1 : 1);
    if (v < METRO_MIN) v = METRO_MIN; if (v > METRO_MAX) v = METRO_MAX;
    g_metro.bpm = v;
}

static int metro_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    int sx, sy, sw, sh;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->scancode == SCAN_RIGHT || ev->scancode == SCAN_UP)
            { g_metro.bpm += 2; if (g_metro.bpm > METRO_MAX) g_metro.bpm = METRO_MAX; }
        if (ev->scancode == SCAN_LEFT || ev->scancode == SCAN_DOWN)
            { g_metro.bpm -= 2; if (g_metro.bpm < METRO_MIN) g_metro.bpm = METRO_MIN; }
        if (ev->unicode == ' ') {
            g_metro.running = !g_metro.running; g_metro.acc = 0; g_metro.beat = METRO_BEATS - 1;
        }
        return 0;
    case WM_EV_MOUSE_DOWN: {
        int bw = 120, bh = 30, dr = 14, gap = 12;
        int dy = 80, byy = dy + dr + 12, bxx = cw / 2 - bw / 2;
        if (ev->mx >= bxx && ev->mx < bxx + bw && ev->my >= byy && ev->my < byy + bh) {
            g_metro.running = !g_metro.running; g_metro.acc = 0; g_metro.beat = METRO_BEATS - 1; return 0;
        }
        (void)gap;
        metro_slider_rect(cw, ch, &sx, &sy, &sw, &sh);
        if (ev->my >= sy - 6 && ev->my <= sy + sh + 6) { g_metro.drag = 1; metro_set_from_x(cw, ch, ev->mx); }
        return 0;
    }
    case WM_EV_MOUSE_MOVE:
        if (g_metro.drag) metro_set_from_x(cw, ch, ev->mx);
        return 0;
    case WM_EV_MOUSE_UP: g_metro.drag = 0; return 0;
    case WM_EV_CLOSE: g_metro.win = NULL; spk_off(); return 0;
    default: return 0;
    }
}

void tool_toys_metronome_open(void)
{
    if (g_metro.win) return;
    g_metro.bpm = 120; g_metro.running = 0; g_metro.drag = 0;
    g_metro.fps = 30; g_metro.last_sec = -1; g_metro.fcount = 0;
    g_metro.acc = 0; g_metro.beat = METRO_BEATS - 1; g_metro.flash = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 42 / 100; if (ww < 360) ww = 360; if (ww > 520) ww = 520; if (ww > W - 40) ww = W - 40;
    int wh = H * 46 / 100; if (wh < 300) wh = 300; if (wh > 420) wh = 420; if (wh > H - 40) wh = H - 40;
    g_metro.win = wm_open("Metronome", ww, wh, metro_draw, metro_event, &g_metro);
}

/* ==========================================================================
 * 5. COLOUR MIXER
 * ========================================================================== */
typedef struct { wm_window *win; int r, g, b; int drag; /* -1 none, 0..2 */ } mix_state;
static mix_state g_mix;

static void mix_bar(int cw, int ch, int i, int *bx, int *by, int *bw, int *bh)
{
    (void)ch;
    *bx = 40; *bw = cw - 80 - 60; if (*bw < 60) *bw = 60;
    *by = 40 + i * 46; *bh = 22;
}
static int *mix_chan(int i) { return i == 0 ? &g_mix.r : i == 1 ? &g_mix.g : &g_mix.b; }

static void mix_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, 0x00181818);
    draw_string_clip(cx + 16, cy + 12, cw - 32, "Colour Mixer", 0x00FFFFFF, 0, 1, 2);
    const char *nm[3] = { "R", "G", "B" };
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh; mix_bar(cw, ch, i, &bx, &by, &bw, &bh);
        int v = *mix_chan(i);
        draw_string_clip(cx + bx - 24, cy + by + 3, 20, nm[i],
                         i == 0 ? 0x00FF6060 : i == 1 ? 0x0060FF60 : 0x006090FF, 0, 1, 2);
        /* fixed (non-swept) channel bits + the swept channel's shift, once per bar */
        UINT32 fixbits = i == 0 ? ((UINT32)g_mix.g << 8) | (UINT32)g_mix.b
                       : i == 1 ? ((UINT32)g_mix.r << 16) | (UINT32)g_mix.b
                                : ((UINT32)g_mix.r << 16) | ((UINT32)g_mix.g << 8);
        int shift = i == 0 ? 16 : i == 1 ? 8 : 0;
        int bden = (bw > 1 ? bw - 1 : 1);
        for (int px = 0; px < bw; ) {
            int cv = px * 255 / bden;
            int j = px + 1;
            while (j < bw && j * 255 / bden == cv) j++;
            cfill(cx, cy, cw, ch, cx + bx + px, cy + by, j - px, bh,
                  fixbits | ((UINT32)cv << shift));
            px = j;
        }
        draw_rect_outline(cx + bx, cy + by, bw, bh, 1, 0x00404040);
        int kx = v * (bw - 1) / 255;
        cfill(cx, cy, cw, ch, cx + bx + kx - 3, cy + by - 3, 7, bh + 6, 0x00FFFFFF);
        draw_rect_outline(cx + bx + kx - 3, cy + by - 3, 7, bh + 6, 1, 0x00202020);
        char nb[6]; u2s(nb, (unsigned)v);
        draw_string_clip(cx + bx + bw + 8, cy + by + 3, 50, nb, 0x00D0D0D0, 0, 1, 2);
    }
    /* swatch + hex + rgb */
    UINT32 v = ((UINT32)g_mix.r << 16) | ((UINT32)g_mix.g << 8) | (UINT32)g_mix.b;
    int swx = cx + 40, swy = cy + 40 + 3 * 46 + 6, sww = cw - 80, swh = ch - (swy - cy) - 40;
    if (swh < 24) swh = 24;
    cfill(cx, cy, cw, ch, swx, swy, sww, swh, v);
    draw_rect_outline(swx, swy, sww, swh, 2, 0x00404040);
    static const char hx[] = "0123456789ABCDEF";
    char hb[10]; hb[0] = '#';
    for (int k = 0; k < 6; k++) hb[1 + k] = hx[(v >> ((5 - k) * 4)) & 0xF];
    hb[7] = 0;
    draw_string_clip(swx + 8, swy + swh - 22, sww - 16, hb, 0x00FFFFFF, 0, 1, 2);
    draw_string_clip(cx + 16, cy + ch - 14, cw - 32,
                     "Drag bars  Tab=next  Up/Down=adjust  Esc=close", 0x00808080, 0, 1, 1);
}

static void mix_set_from_x(int i, int cw, int ch, int mx)
{
    int bx, by, bw, bh; mix_bar(cw, ch, i, &bx, &by, &bw, &bh);
    int v = (mx - bx) * 255 / (bw > 1 ? bw - 1 : 1);
    if (v < 0) v = 0; if (v > 255) v = 255;
    *mix_chan(i) = v;
}

static int mix_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    static int sel = 0;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == CHAR_TAB) sel = (sel + 1) % 3;
        if (ev->scancode == SCAN_UP)   { int *c = mix_chan(sel); *c += 5; if (*c > 255) *c = 255; }
        if (ev->scancode == SCAN_DOWN) { int *c = mix_chan(sel); *c -= 5; if (*c < 0) *c = 0; }
        return 0;
    case WM_EV_MOUSE_DOWN:
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh; mix_bar(cw, ch, i, &bx, &by, &bw, &bh);
            if (ev->mx >= bx - 4 && ev->mx <= bx + bw + 4 && ev->my >= by - 4 && ev->my <= by + bh + 4) {
                g_mix.drag = i; sel = i; mix_set_from_x(i, cw, ch, ev->mx); return 0;
            }
        }
        return 0;
    case WM_EV_MOUSE_MOVE:
        if (g_mix.drag >= 0) mix_set_from_x(g_mix.drag, cw, ch, ev->mx);
        return 0;
    case WM_EV_MOUSE_UP: g_mix.drag = -1; return 0;
    case WM_EV_CLOSE: g_mix.win = NULL; return 0;
    default: return 0;
    }
}

void tool_toys_mixer_open(void)
{
    if (g_mix.win) return;
    g_mix.r = 80; g_mix.g = 160; g_mix.b = 220; g_mix.drag = -1;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 44 / 100; if (ww < 420) ww = 420; if (ww > 600) ww = 600; if (ww > W - 40) ww = W - 40;
    int wh = H * 52 / 100; if (wh < 320) wh = 320; if (wh > 460) wh = 460; if (wh > H - 40) wh = H - 40;
    g_mix.win = wm_open("Colour Mixer", ww, wh, mix_draw, mix_event, &g_mix);
}

/* ==========================================================================
 * 6. ASCII AQUARIUM
 * ========================================================================== */
#define AQ_FISH 8
#define AQ_BUB  10
typedef struct { int x; int y; int vx; int type; UINT32 col; } aq_fish;
typedef struct { int x; int y; int spd; } aq_bub;
typedef struct {
    wm_window *win;
    unsigned frame;
    aq_fish fish[AQ_FISH];
    aq_bub  bub[AQ_BUB];
    int     inited_w, inited_h;
} aq_state;
static aq_state g_aq;

static const UINT32 AQ_COLS[6] = {
    0x00FFB030, 0x00FF6060, 0x0060D0FF, 0x00FFF060, 0x0080FF80, 0x00FF80D0
};

static void aq_reset(int cw, int ch)
{
    for (int i = 0; i < AQ_FISH; i++) {
        g_aq.fish[i].type = (int)(rnd() & 1);
        g_aq.fish[i].vx = g_aq.fish[i].type ? -rnd_range(4, 12) : rnd_range(4, 12);
        g_aq.fish[i].x = rnd_range(0, (cw > 0 ? cw : 400) - 1) * 256;
        g_aq.fish[i].y = rnd_range(30, (ch > 40 ? ch - 40 : 200));
        g_aq.fish[i].col = AQ_COLS[rnd() % 6];
    }
    for (int i = 0; i < AQ_BUB; i++) {
        g_aq.bub[i].x = rnd_range(0, (cw > 0 ? cw : 400) - 1);
        g_aq.bub[i].y = rnd_range(0, (ch > 0 ? ch : 300) - 1);
        g_aq.bub[i].spd = rnd_range(1, 3);
    }
    g_aq.inited_w = cw; g_aq.inited_h = ch;
}

static void aq_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    if (g_aq.inited_w != cw || g_aq.inited_h != ch) aq_reset(cw, ch);
    /* water gradient: coalesce consecutive equal-colour rows into one fill */
    int cden = (ch > 1 ? ch - 1 : 1);
    for (int y = 0; y < ch; ) {
        int t = y * 60 / cden;
        int y2 = y + 1;
        while (y2 < ch && y2 * 60 / cden == t) y2++;
        UINT32 col = (UINT32)(4 + t / 6) << 16 | (UINT32)(30 + t) << 8 | (UINT32)(60 + t);
        cfill(cx, cy, cw, ch, cx, cy + y, cw, y2 - y, col & 0x00FFFFFF);
        y = y2;
    }
    g_aq.frame++;

    /* seaweed swaying at the bottom */
    int base = cy + ch - 4;
    for (int s = 0; s < cw; s += 40) {
        for (int seg = 0; seg < 8; seg++) {
            int sway = isin((int)(g_aq.frame * 3 + seg * 30 + s)) * (seg + 1) / 10000 * 2;
            int x = cx + s + 10 + sway;
            int y = base - seg * 8;
            cfill(cx, cy, cw, ch, x, y, 4, 8, 0x00207020);
        }
    }
    /* bubbles */
    for (int i = 0; i < AQ_BUB; i++) {
        g_aq.bub[i].y -= g_aq.bub[i].spd;
        if (g_aq.bub[i].y < 2) { g_aq.bub[i].y = ch - 2; g_aq.bub[i].x = rnd_range(0, cw > 0 ? cw - 1 : 1); }
        int wob = isin((int)(g_aq.frame * 4 + i * 40)) * 3 / 10000;
        cfill(cx, cy, cw, ch, cx + g_aq.bub[i].x + wob, cy + g_aq.bub[i].y, 3, 3, 0x00A0D8F0);
    }
    /* fish */
    int fuis = ui_scale(); if (fuis < 1) fuis = 1;   /* constant within a frame */
    for (int i = 0; i < AQ_FISH; i++) {
        g_aq.fish[i].x += g_aq.fish[i].vx;
        int px = (g_aq.fish[i].x >> 8);
        if (g_aq.fish[i].vx > 0 && px > cw + 30) g_aq.fish[i].x = -30 * 256;
        if (g_aq.fish[i].vx < 0 && px < -30)     g_aq.fish[i].x = (cw + 30) * 256;
        int bob = isin((int)(g_aq.frame * 5 + i * 50)) * 6 / 10000;
        int y = cy + g_aq.fish[i].y + bob;
        const char *body = g_aq.fish[i].type ? "<><" : "><>";
        for (int k = 0; k < 3; k++)
            cchar_s(cx, cy, cw, ch, cx + px + k * 8, y, body[k], g_aq.fish[i].col, 0, 1, 1, fuis);
    }
    draw_string_clip(cx + 6, cy + 4, cw - 12, "ASCII Aquarium", 0x0080E0FF, 0, 1, 1);
}

static int aq_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    if (ev->type == WM_EV_KEY && ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
    if (ev->type == WM_EV_CLOSE) g_aq.win = NULL;
    return 0;
}

void tool_toys_aquarium_open(void)
{
    if (g_aq.win) return;
    seed_rng();
    g_aq.frame = 0; g_aq.inited_w = g_aq.inited_h = -1;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 56 / 100; if (ww < 480) ww = 480; if (ww > 760) ww = 760; if (ww > W - 40) ww = W - 40;
    int wh = H * 50 / 100; if (wh < 320) wh = 320; if (wh > 500) wh = 500; if (wh > H - 40) wh = H - 40;
    g_aq.win = wm_open("ASCII Aquarium", ww, wh, aq_draw, aq_event, &g_aq);
}

/* ==========================================================================
 * 7. STARFIELD SCREENSAVER
 * ========================================================================== */
#define STAR_N 96
typedef struct { int x, y, z; } star_t;   /* x,y in [-1000..1000], z in 1..1024 */
typedef struct {
    wm_window *win;
    unsigned frame;
    int speed;
    star_t s[STAR_N];
    int inited;
} star_state;
static star_state g_star;

static void star_spawn(star_t *s)
{
    s->x = rnd_range(-1000, 1000);
    s->y = rnd_range(-1000, 1000);
    s->z = rnd_range(1, 1024);
}

static void star_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    if (!g_star.inited) { for (int i = 0; i < STAR_N; i++) star_spawn(&g_star.s[i]); g_star.inited = 1; }
    fill_rect(cx, cy, cw, ch, 0x00000008);
    g_star.frame++;
    int mcx = cx + cw / 2, mcy = cy + ch / 2;
    for (int i = 0; i < STAR_N; i++) {
        star_t *st = &g_star.s[i];
        st->z -= g_star.speed;
        if (st->z < 1) { star_spawn(st); st->z = 1024; }
        int z = st->z;
        int sx = mcx + st->x * 256 / z;
        int sy = mcy + st->y * 256 / z;
        if (sx < cx || sx >= cx + cw || sy < cy || sy >= cy + ch) continue;
        int d = 1024 - z;
        int b = d >> 2; if (b > 255) b = 255; if (b < 30) b = 30;
        int sz = d / 300 + 1; if (sz > 3) sz = 3;
        UINT32 col = ((UINT32)b << 16) | ((UINT32)b << 8) | (UINT32)b;
        cfill(cx, cy, cw, ch, sx, sy, sz, sz, col);
    }
    char line[32]; int p = 0; const char *pre = "Speed ";
    while (*pre) line[p++] = *pre++;
    p += u2s(line + p, (unsigned)g_star.speed);
    line[p] = 0;
    draw_string_clip(cx + 6, cy + ch - 14, cw - 12, line, 0x004060A0, 0, 1, 1);
}

static int star_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->scancode == SCAN_UP)   { g_star.speed += 2; if (g_star.speed > 60) g_star.speed = 60; }
        if (ev->scancode == SCAN_DOWN) { g_star.speed -= 2; if (g_star.speed < 1) g_star.speed = 1; }
    }
    if (ev->type == WM_EV_MOUSE_WHEEL) {
        g_star.speed += ev->wheel; if (g_star.speed < 1) g_star.speed = 1; if (g_star.speed > 60) g_star.speed = 60;
    }
    if (ev->type == WM_EV_CLOSE) g_star.win = NULL;
    return 0;
}

void tool_toys_starfield_open(void)
{
    if (g_star.win) return;
    seed_rng();
    g_star.frame = 0; g_star.speed = 12; g_star.inited = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 56 / 100; if (ww < 480) ww = 480; if (ww > 760) ww = 760; if (ww > W - 40) ww = W - 40;
    int wh = H * 56 / 100; if (wh < 360) wh = 360; if (wh > 560) wh = 560; if (wh > H - 40) wh = H - 40;
    g_star.win = wm_open("Starfield", ww, wh, star_draw, star_event, &g_star);
}

/* ==========================================================================
 * 8. DRUM PADS
 * ========================================================================== */
#define DRUM_N 8
static const char *const DRUM_LBL[DRUM_N] =
    { "Kick", "Snare", "HiHat", "Tom1", "Tom2", "Clap", "Ride", "Crash" };
static const unsigned DRUM_FREQ[DRUM_N] =
    { 80, 220, 2400, 150, 110, 320, 1600, 900 };
static const unsigned DRUM_MS[DRUM_N] =
    { 90, 60, 30, 70, 80, 50, 40, 120 };
static const UINT32 DRUM_COL[DRUM_N] = {
    0x00C04030, 0x00C0A030, 0x0030A0C0, 0x00A05030, 0x00905030,
    0x00A030A0, 0x0040A040, 0x00C06030
};
typedef struct { wm_window *win; int flash[DRUM_N]; } drum_state;
static drum_state g_drum;

static void drum_pad_rect(int cw, int ch, int i, int *px, int *py, int *pw, int *pht)
{
    int cols = 4, rows = 2;
    int gap = 8, top = 26;
    int aw = cw - gap, ah = ch - top - gap;
    int pwv = aw / cols - gap, phv = ah / rows - gap;
    int r = i / cols, c = i % cols;
    *px = gap + c * (pwv + gap);
    *py = top + r * (phv + gap);
    *pw = pwv; *pht = phv;
}

static void drum_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, 0x00141414);
    draw_string_clip(cx + 8, cy + 6, cw - 16, "Drum Pads  keys 1-8   Esc=close",
                     0x00C0C0C0, 0, 1, 1);
    for (int i = 0; i < DRUM_N; i++) {
        int px, py, pw, ph; drum_pad_rect(cw, ch, i, &px, &py, &pw, &ph);
        UINT32 base = DRUM_COL[i];
        UINT32 col = g_drum.flash[i] > 0 ? wm_blend(base, 0x00FFFFFF, 140) : base;
        cfill(cx, cy, cw, ch, cx + px, cy + py, pw, ph, col);
        draw_rect_outline(cx + px, cy + py, pw, ph, 2, 0x00202020);
        char lbl[8]; int k = 0; lbl[k++] = (char)('1' + i); lbl[k++] = ' '; lbl[k] = 0;
        draw_string_clip(cx + px + 8, cy + py + 8, pw - 16, lbl, 0x00FFFFFF, 0, 1, 2);
        draw_string_clip(cx + px + 8, cy + py + ph / 2, pw - 16, DRUM_LBL[i], 0x00FFFFFF, 0, 1, 2);
        if (g_drum.flash[i] > 0) g_drum.flash[i]--;
    }
}

static void drum_hit(int i)
{
    if (i < 0 || i >= DRUM_N) return;
    g_drum.flash[i] = 5;
    audio_beep(DRUM_FREQ[i], DRUM_MS[i]);
}

static int drum_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode >= '1' && ev->unicode <= '8') drum_hit((int)ev->unicode - '1');
        return 0;
    case WM_EV_MOUSE_DOWN:
        for (int i = 0; i < DRUM_N; i++) {
            int px, py, pw, ph; drum_pad_rect(cw, ch, i, &px, &py, &pw, &ph);
            if (ev->mx >= px && ev->mx < px + pw && ev->my >= py && ev->my < py + ph) { drum_hit(i); return 0; }
        }
        return 0;
    case WM_EV_CLOSE: g_drum.win = NULL; spk_off(); return 0;
    default: return 0;
    }
}

void tool_toys_drums_open(void)
{
    if (g_drum.win) return;
    for (int i = 0; i < DRUM_N; i++) g_drum.flash[i] = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 52 / 100; if (ww < 460) ww = 460; if (ww > 700) ww = 700; if (ww > W - 40) ww = W - 40;
    int wh = H * 46 / 100; if (wh < 300) wh = 300; if (wh > 440) wh = 440; if (wh > H - 40) wh = H - 40;
    g_drum.win = wm_open("Drum Pads", ww, wh, drum_draw, drum_event, &g_drum);
}

/* ==========================================================================
 * 9. BOUNCING DVD LOGO
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int x, y;      /* fixed-point *16 */
    int vx, vy;
    int colidx;
    int corners;
    int lw, lh;
} dvd_state;
static dvd_state g_dvd;
static const UINT32 DVD_COLS[7] = {
    0x00FF4040, 0x0040FF40, 0x004080FF, 0x00FFFF40, 0x00FF40FF, 0x0040FFFF, 0x00FFA040
};

static void dvd_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, 0x00000000);
    int lw = g_dvd.lw, lh = g_dvd.lh;
    g_dvd.x += g_dvd.vx; g_dvd.y += g_dvd.vy;
    int px = g_dvd.x >> 4, py = g_dvd.y >> 4;
    int hitx = 0, hity = 0;
    if (px < 0) { px = 0; g_dvd.vx = -g_dvd.vx; hitx = 1; }
    if (py < 0) { py = 0; g_dvd.vy = -g_dvd.vy; hity = 1; }
    if (px + lw > cw) { px = cw - lw; g_dvd.vx = -g_dvd.vx; hitx = 1; }
    if (py + lh > ch) { py = ch - lh; g_dvd.vy = -g_dvd.vy; hity = 1; }
    if (hitx || hity) {
        g_dvd.x = px << 4; g_dvd.y = py << 4;
        g_dvd.colidx = (g_dvd.colidx + 1) % 7;
        if (hitx && hity) g_dvd.corners++;
    }
    UINT32 col = DVD_COLS[g_dvd.colidx];
    cfill(cx, cy, cw, ch, cx + px, cy + py, lw, lh, col);
    draw_rect_outline(cx + px, cy + py, lw, lh, 1, 0x00000000);
    draw_string_center(cx + px + lw / 2, cy + py + lh / 2 - 8, "DVD", 0x00101010, col, 0, 2);

    char line[32]; int p = 0; const char *pre = "Corner hits: ";
    while (*pre) line[p++] = *pre++;
    p += u2s(line + p, (unsigned)g_dvd.corners);
    line[p] = 0;
    draw_string_clip(cx + 6, cy + ch - 14, cw - 12, line, 0x00404040, 0, 1, 1);
}

static int dvd_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    if (ev->type == WM_EV_KEY && ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
    if (ev->type == WM_EV_CLOSE) g_dvd.win = NULL;
    return 0;
}

void tool_toys_dvd_open(void)
{
    if (g_dvd.win) return;
    seed_rng();
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 54 / 100; if (ww < 460) ww = 460; if (ww > 740) ww = 740; if (ww > W - 40) ww = W - 40;
    int wh = H * 50 / 100; if (wh < 320) wh = 320; if (wh > 520) wh = 520; if (wh > H - 40) wh = H - 40;
    g_dvd.lw = 84; g_dvd.lh = 40;
    g_dvd.x = rnd_range(0, 200) << 4; g_dvd.y = rnd_range(0, 120) << 4;
    g_dvd.vx = 24; g_dvd.vy = 20; g_dvd.colidx = 0; g_dvd.corners = 0;
    g_dvd.win = wm_open("Bouncing DVD", ww, wh, dvd_draw, dvd_event, &g_dvd);
}

/* ==========================================================================
 * Category table.
 * ========================================================================== */
const struct forebo_tool cat_toys_tools[] = {
    { "Pixel Paint",     "Mouse-draw pixel art with a colour palette", "text",     tool_toys_paint_open     },
    { "Piano",           "Clickable keys play PC-speaker tones",        "os",       tool_toys_piano_open     },
    { "Tone Generator",  "Frequency slider drives the speaker",         "gear",     tool_toys_tone_open      },
    { "Metronome",       "BPM ticks on the PC speaker",                 "reboot",   tool_toys_metronome_open },
    { "Colour Mixer",    "R/G/B sliders with a live swatch",            "text",     tool_toys_mixer_open     },
    { "ASCII Aquarium",  "Animated fish, bubbles and seaweed",          "os",       tool_toys_aquarium_open  },
    { "Starfield",       "Warp-speed starfield screensaver",            "safe",     tool_toys_starfield_open },
    { "Drum Pads",       "Keys 1-8 trigger drum tones",                 "grub",     tool_toys_drums_open     },
    { "Bouncing DVD",    "The classic bouncing logo",                   "disk",     tool_toys_dvd_open       },
};
const int cat_toys_count = (int)(sizeof(cat_toys_tools) / sizeof(cat_toys_tools[0]));
