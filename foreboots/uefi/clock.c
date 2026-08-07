/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/clock.c - Clock tool: live firmware RTC date + time (digital + analog).
 * =============================================================================
 * TEMPLATE B window. Re-reads gST->RuntimeServices->GetTime(EFI_TIME) in the
 * draw callback so it ticks live. Digital HH:MM:SS (big), date + weekday, the
 * firmware UTC offset / daylight flag, and a fixed-point integer analog face.
 *
 * Freestanding: no libc, no heap, no float/SSE. See project build flags.
 * ========================================================================== */
#include "clock.h"
#include "ui.h"
#include "wm.h"
#include "../include/forebo_theme.h"

/* GetTime is a VOID* placeholder in EFI_RUNTIME_SERVICES (efi.h); give it a
 * proper callable type. Capabilities pointer is optional (we pass NULL). */
typedef EFI_STATUS (EFIAPI *CLK_GET_TIME)(EFI_TIME *Time, VOID *Capabilities);

/* EFI_TIME.TimeZone sentinel meaning "timezone is not specified". */
#define CLK_TZ_UNSPECIFIED  2047

/* RTC poll throttle. The CMOS second only advances at 1Hz, but the compositor
 * repaints this window on every cursor/damage frame (~60Hz) while it is open,
 * so re-reading gST->RuntimeServices->GetTime each frame is wasted firmware
 * work (GetTime can busy-poll the CMOS update-in-progress flag). Sample at
 * ~5-6Hz - well above the 1Hz Nyquist rate for a seconds display - and reuse
 * the cached reading between polls. Mirrors tools_datetime.c's DT_RTC_PERIOD. */
#define CLK_FPS         30
#define CLK_RTC_PERIOD  (CLK_FPS / 6 > 0 ? CLK_FPS / 6 : 1)

static EFI_SYSTEM_TABLE *gST;

void clock_init(EFI_SYSTEM_TABLE *st) { gST = st; }

/* ------------------------------------------------------------------------- *
 * Fixed-point trig: sine of 0..90 degrees scaled by 10000.
 * ------------------------------------------------------------------------- */
static const int g_sin90[91] = {
    0,175,349,523,698,872,1045,1219,1392,1564,
    1736,1908,2079,2250,2419,2588,2756,2924,3090,3256,
    3420,3584,3746,3907,4067,4226,4384,4540,4695,4848,
    5000,5150,5299,5446,5592,5736,5878,6018,6157,6293,
    6428,6561,6691,6820,6947,7071,7193,7314,7431,7547,
    7660,7771,7880,7986,8090,8192,8290,8387,8480,8572,
    8660,8746,8829,8910,8988,9063,9135,9205,9272,9336,
    9397,9455,9511,9563,9613,9659,9703,9744,9781,9816,
    9848,9877,9903,9925,9945,9962,9976,9986,9994,9998,
    10000
};

/* sin(deg)*10000 for any integer degree, via quadrant symmetry. */
static int isin(int d)
{
    d %= 360; if (d < 0) d += 360;
    if (d <= 90)  return  g_sin90[d];
    if (d <= 180) return  g_sin90[180 - d];
    if (d <= 270) return -g_sin90[d - 180];
    return               -g_sin90[360 - d];
}
static int icos(int d) { return isin(d + 90); }

/* ------------------------------------------------------------------------- *
 * Per-window state.
 * ------------------------------------------------------------------------- */
typedef struct {
    wm_window *win;
    int scale_cw;    /* last client width the digit scale was fit to */
    int scale_uis;   /* last ui_scale() the digit scale was fit to */
    int scale_val;   /* cached best-fit digit scale for (scale_cw, scale_uis) */
    int      rtc_ctr;   /* frames left before the next GetTime() poll         */
    EFI_TIME cached_t;  /* last successful (or attempted) RTC reading         */
    int      cached_ok; /* validity of cached_t (read_rtc() return)           */
} clkstate;
static clkstate g_clk;

/* ------------------------------------------------------------------------- *
 * Tiny freestanding formatting helpers (no libc).
 * ------------------------------------------------------------------------- */
static void put2(char *o, int v)        /* zero-padded 2 digits */
{ if (v < 0) v = 0; o[0] = (char)('0' + (v / 10) % 10); o[1] = (char)('0' + v % 10); }

static void put4(char *o, int v)        /* zero-padded 4 digits */
{ if (v < 0) v = 0; o[0] = (char)('0' + (v / 1000) % 10); o[1] = (char)('0' + (v / 100) % 10);
  o[2] = (char)('0' + (v / 10) % 10);   o[3] = (char)('0' + v % 10); }

/* Day-of-week via Sakamoto's algorithm (0=Sunday). Integer only. */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (m < 1 || m > 12) return -1;
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

/* ------------------------------------------------------------------------- *
 * Read the RTC. Returns 1 on success, 0 if unavailable.
 * ------------------------------------------------------------------------- */
static int read_rtc(EFI_TIME *t)
{
    if (!t || !gST || !gST->RuntimeServices) return 0;
    CLK_GET_TIME gt = (CLK_GET_TIME)gST->RuntimeServices->GetTime;
    if (!gt) return 0;
    /* zero out first so a partial/failed call can't leak garbage into display.
     * EFI_TIME's size is a fixed multiple of 4 bytes per the UEFI spec, so a
     * UINT32-strided clear is byte-identical to the old byte-wise loop. */
    UINT32 *tw = (UINT32*)t;
    for (unsigned i = 0; i < sizeof(*t) / sizeof(UINT32); i++) tw[i] = 0;
    EFI_STATUS st = gt(t, NULL);
    if (EFI_ERROR(st)) return 0;
    /* sanity clamp: firmware could hand back nonsense */
    if (t->Month < 1 || t->Month > 12 || t->Day < 1 || t->Day > 31) return 0;
    if (t->Hour > 23 || t->Minute > 59 || t->Second > 59) return 0;
    return 1;
}

/* ------------------------------------------------------------------------- *
 * Analog face: draw a hand from (cx,cy) at `deg` clockwise-from-12, length
 * `len` px, thickness `th`, clipped to the face bounding box.
 * ------------------------------------------------------------------------- */
static void draw_hand(int cx, int cy, int deg, int len, int th, UINT32 col,
                      int bx, int by, int bw, int bh)
{
    int ex = cx + (len * isin(deg)) / 10000;
    int ey = cy - (len * icos(deg)) / 10000;
    int dx = ex - cx, dy = ey - cy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int px = cx + (dx * i) / steps;
        int py = cy + (dy * i) / steps;
        int rx = px - th/2, ry = py - th/2;
        /* clip the little square to the face box: clamp top-left in, then clamp
         * the th*th size so it never overdraws past the right/bottom edge. */
        if (rx < bx) rx = bx; if (ry < by) ry = by;
        int w = th, h = th;
        if (rx + w > bx + bw) w = bx + bw - rx;
        if (ry + h > by + bh) h = by + bh - ry;
        if (w > 0 && h > 0) fill_rect(rx, ry, w, h, col);
    }
}

static void draw_face(int fx, int fy, int fsz, const EFI_TIME *t)
{
    int cx = fx + fsz/2, cy = fy + fsz/2;
    int r  = fsz/2 - 2;
    if (r < 8) return;
    UINT32 rim  = FOREB_BORDER;
    UINT32 tick = FOREB_TITLE;

    /* dial background + rim (approx circle via inscribed filled square is ugly;
     * draw the 60 tick marks around the circumference for a clock look). */
    for (int m = 0; m < 60; m++) {
        int deg = m * 6;
        int outer = r;
        int inner = (m % 5 == 0) ? r - r/6 : r - r/12;   /* longer 5-min ticks */
        int s = isin(deg), co = icos(deg);   /* outer/inner share the angle */
        int ox = cx + (outer * s) / 10000;
        int oy = cy - (outer * co) / 10000;
        int ix = cx + (inner * s) / 10000;
        int iy = cy - (inner * co) / 10000;
        UINT32 c = (m % 15 == 0) ? tick : rim;
        int th = (m % 5 == 0) ? 2 : 1;
        /* Draw a short segment inner->outer. i=0 is the inner endpoint, so the
         * old standalone inner-point plot was redundant with this loop; it is
         * folded in here and drawn once. Each square is edge-clipped to the
         * face box (same clamp as draw_hand) so ticks never draw outside it. */
        int dx = ox - ix, dy = oy - iy;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        int steps = (adx>ady?adx:ady); if (steps < 1) steps = 1;
        for (int i = 0; i <= steps; i++) {
            int rx = ix + dx*i/steps - th/2, ry = iy + dy*i/steps - th/2;
            if (rx < fx) rx = fx; if (ry < fy) ry = fy;
            int w = th, h = th;
            if (rx + w > fx + fsz) w = fx + fsz - rx;
            if (ry + h > fy + fsz) h = fy + fsz - ry;
            if (w > 0 && h > 0) fill_rect(rx, ry, w, h, c);
        }
    }

    /* hands: hour uses 12h*30deg + minute contribution; minute; second */
    int hh = t->Hour % 12;
    int hour_deg = hh * 30 + t->Minute / 2;      /* 0.5 deg / minute */
    int min_deg  = t->Minute * 6 + t->Second / 10;
    int sec_deg  = t->Second * 6;

    draw_hand(cx, cy, hour_deg, r * 55/100, 4, FOREB_WHITE, fx, fy, fsz, fsz);
    draw_hand(cx, cy, min_deg,  r * 80/100, 3, FOREB_TITLE, fx, fy, fsz, fsz);
    draw_hand(cx, cy, sec_deg,  r * 88/100, 1, FOREB_TIMER, fx, fy, fsz, fsz);

    /* center hub */
    fill_rect(cx - 3, cy - 3, 6, 6, FOREB_WHITE);
}

/* ------------------------------------------------------------------------- *
 * Draw callback.
 * ------------------------------------------------------------------------- */
static const char *g_wday[7] =
    { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };

static void clk_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_BG);

    /* RTC caching: GetTime() is only re-polled once every CLK_RTC_PERIOD frames;
     * the cached EFI_TIME + validity are reused for the many cursor/damage-driven
     * repaints in between (the displayed seconds only change at 1Hz anyway). This
     * keeps the firmware call off the hot mouse-move path on uncached-VRAM HW. */
    if (g_clk.rtc_ctr <= 0) {
        g_clk.cached_ok = read_rtc(&g_clk.cached_t);
        g_clk.rtc_ctr   = CLK_RTC_PERIOD;
    } else {
        g_clk.rtc_ctr--;
    }
    EFI_TIME t = g_clk.cached_t;
    if (!g_clk.cached_ok) {
        draw_string_clip(cx + 12, cy + ch/2 - 8, cw - 24,
                         "RTC unavailable", FOREB_TIMER, FOREB_BG, 1, 2);
        draw_string_clip(cx + 12, cy + ch/2 + 16, cw - 24,
                         "Firmware GetTime() not supported",
                         FOREB_DIM, FOREB_BG, 1, 1);
        return;
    }

    int pad = 14;
    int tx  = cx + pad;
    int ty  = cy + pad;

    /* --- big digital HH:MM:SS ------------------------------------------- */
    char big[9];
    put2(&big[0], t.Hour);  big[2] = ':';
    put2(&big[3], t.Minute);big[5] = ':';
    put2(&big[6], t.Second);big[8] = 0;

    /* pick a scale that fits the client width: advance = 8*scale*ui_scale */
    int uis = ui_scale(); if (uis < 1) uis = 1;
    int scale;
    if (g_clk.scale_cw == cw && g_clk.scale_uis == uis) {
        scale = g_clk.scale_val;               /* (cw, uis) unchanged: reuse fit */
    } else {
        scale = 5;
        while (scale > 1 && (int)(8 * scale * uis * 8 /*chars*/) > cw - 2*pad)
            scale--;
        g_clk.scale_cw = cw; g_clk.scale_uis = uis; g_clk.scale_val = scale;
    }
    draw_string_clip(tx, ty, cw - 2*pad, big, FOREB_WHITE, FOREB_BG, 1, scale);
    ty += 16 * scale * uis + 10;

    /* --- date + weekday -------------------------------------------------- */
    int wd = weekday(t.Year, t.Month, t.Day);
    char date[48]; int p = 0;
    put4(&date[p], t.Year);  p += 4; date[p++] = '-';
    put2(&date[p], t.Month); p += 2; date[p++] = '-';
    put2(&date[p], t.Day);   p += 2;
    if (wd >= 0 && wd < 7) {
        date[p++] = ' '; date[p++] = ' ';
        for (const char *s = g_wday[wd]; *s && p < 47; ) date[p++] = *s++;
    }
    date[p] = 0;
    draw_string_clip(tx, ty, cw - 2*pad, date, FOREB_TEXT, FOREB_BG, 1, 2);
    ty += 32 * uis + 8;

    /* --- timezone / daylight -------------------------------------------- */
    char tz[48]; int q = 0;
    if (t.TimeZone == CLK_TZ_UNSPECIFIED) {
        const char *s = "TZ: local (unspecified)";
        while (*s) tz[q++] = *s++;
    } else {
        const char *s = "UTC"; while (*s) tz[q++] = *s++;
        int off = t.TimeZone;                 /* minutes east(+)/west(-) of UTC */
        tz[q++] = (off <= 0) ? '+' : '-';     /* EFI: minutes to ADD to get UTC */
        int ao = off < 0 ? -off : off;
        put2(&tz[q], ao / 60); q += 2; tz[q++] = ':';
        put2(&tz[q], ao % 60); q += 2;
    }
    if (t.Daylight & 0x02 /* EFI_TIME_IN_DAYLIGHT */) {
        const char *s = "  (DST)"; while (*s) tz[q++] = *s++;
    }
    tz[q] = 0;
    draw_string_clip(tx, ty, cw - 2*pad, tz, FOREB_DIM, FOREB_BG, 1, 1);
    ty += 16 * uis + 8;

    /* --- analog face (fills remaining vertical space, right-aligned box) - */
    int avail_h = (cy + ch - pad) - ty;
    int avail_w = cw - 2*pad;
    int fsz = avail_h < avail_w ? avail_h : avail_w;
    if (fsz > 200) fsz = 200;
    if (fsz >= 40) {
        int fx = cx + (cw - fsz) / 2;
        int fy = ty + (avail_h - fsz) / 2;
        draw_face(fx, fy, fsz, &t);
    }
}

/* ------------------------------------------------------------------------- *
 * Event callback: Esc closes; otherwise nothing (the frame ticks itself).
 * ------------------------------------------------------------------------- */
static int clk_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    if (ev && ev->type == WM_EV_KEY && ev->scancode == SCAN_ESC)
        return WM_CLOSE_REQUEST;
    if (ev && ev->type == WM_EV_CLOSE)
        g_clk.win = NULL;
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Open.
 * ------------------------------------------------------------------------- */
void tool_clock_open(void)
{
    if (g_clk.win) return;               /* already open */

    g_clk.rtc_ctr = 0;                   /* force an immediate GetTime() poll   */

    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 42/100; if (ww < 320) ww = 320; if (ww > 520) ww = 520;
    if (ww > W - 40) ww = W - 40;
    int wh = H * 62/100; if (wh < 360) wh = 360; if (wh > 620) wh = 620;
    if (wh > H - 40) wh = H - 40;

    g_clk.win = wm_open("Clock", ww, wh, clk_draw, clk_event, &g_clk);
}
