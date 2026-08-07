/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_datetime.c - "Time & Date" tool category (KEY = datetime).
 * =============================================================================
 * Eight self-contained wm windows: Stopwatch, Countdown Timer, Month Calendar,
 * World Clocks, Unix-time converter, Day-of-week calculator, Uptime counter and
 * a Binary/Word clock. All integer / fixed-point; the RTC is read live in the
 * draw callbacks via gST->RuntimeServices->GetTime so the clocks tick.
 *
 * Freestanding: no libc, no heap, no float/SSE. See project build flags.
 * ========================================================================== */
#include "tools_datetime.h"
#include "../ui.h"
#include "../core/wm.h"
#include "../core/input.h"
#include "../efi.h"
#include "../../include/forebo_theme.h"

/* GetTime is a VOID* placeholder in EFI_RUNTIME_SERVICES (efi.h); give it a
 * proper callable type (Capabilities pointer optional, we pass NULL). */
typedef EFI_STATUS (EFIAPI *DT_GET_TIME)(EFI_TIME *Time, VOID *Capabilities);

/* Estimated compositor cadence (frames/second) - used for the tick-driven
 * stopwatch and countdown. The RTC gives the accurate wall-clock second; the
 * frame clock only interpolates the sub-second digits.
 *
 * The menu/WM loop (bootx64.c) spends a ~16000 us budget per frame, i.e. it
 * runs at ~62 Hz when idle - not 30 Hz. Frame-counted timers here advance once
 * per draw, so DT_FPS must match that real cadence or they run ~2x fast. */
#define DT_FPS  62

/* RTC poll cadence: the CMOS second only ticks at 1Hz, so re-reading it on
 * every ~30Hz redraw is wasted firmware calls (GetTime can busy-poll the
 * CMOS UIP flag). Sample at roughly DT_FPS/6 (~5-6Hz, well above the 1Hz
 * Nyquist rate for a seconds display) and hold the value between polls. */
#define DT_RTC_PERIOD (DT_FPS / 6 > 0 ? DT_FPS / 6 : 1)

/* ------------------------------------------------------------------------- *
 * Captured firmware services.
 * ------------------------------------------------------------------------- */
static EFI_SYSTEM_TABLE    *gST;
static EFI_BOOT_SERVICES   *gBS;
static EFI_RUNTIME_SERVICES *gRT;

void cat_datetime_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices    : 0;
    gRT = st ? st->RuntimeServices : 0;
}

/* ------------------------------------------------------------------------- *
 * Low-level x86 port IO (PC-speaker beep for the countdown alarm).
 * ------------------------------------------------------------------------- */
static inline void dt_outb(UINT16 port, UINT8 val)
{ __asm__ __volatile__("outb %0,%1" :: "a"(val), "Nd"(port)); }
static inline UINT8 dt_inb(UINT16 port)
{ UINT8 r; __asm__ __volatile__("inb %1,%0" : "=a"(r) : "Nd"(port)); return r; }

/* Non-blocking PC-speaker control. dt_speaker_on() starts a square wave on PIT
 * channel 2 and returns immediately; the caller silences it later with
 * dt_speaker_off() (e.g. counting frames in the draw callback). This keeps the
 * countdown alarm from stalling the compositor / freezing the cursor. */
static void dt_speaker_on(UINT32 freq)
{
    if (freq == 0) return;
    UINT32 div = 1193182u / freq;
    if (div == 0) div = 1;
    dt_outb(0x43, 0xB6);
    dt_outb(0x42, (UINT8)(div & 0xFF));
    dt_outb(0x42, (UINT8)((div >> 8) & 0xFF));
    dt_outb(0x61, (UINT8)(dt_inb(0x61) | 0x03));   /* gate + data on */
}
static void dt_speaker_off(void)
{
    dt_outb(0x61, (UINT8)(dt_inb(0x61) & 0xFC));    /* speaker off */
}

/* ------------------------------------------------------------------------- *
 * Freestanding formatting + calendar math (all integer).
 * ------------------------------------------------------------------------- */
static int dt_sc(void) { int s = ui_scale(); return s < 1 ? 1 : s; }

static void put2(char *o, int v)
{ if (v < 0) v = 0; o[0] = (char)('0' + (v / 10) % 10); o[1] = (char)('0' + v % 10); }

static void put4(char *o, int v)
{ if (v < 0) v = 0; o[0] = (char)('0' + (v / 1000) % 10); o[1] = (char)('0' + (v / 100) % 10);
  o[2] = (char)('0' + (v / 10) % 10); o[3] = (char)('0' + v % 10); }

/* Append an unsigned integer; returns new position. */
static int put_u(char *o, int p, UINT64 v)
{
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n) o[p++] = t[--n];
    return p;
}
/* Append a signed integer. */
static int put_i(char *o, int p, INT64 v)
{ if (v < 0) { o[p++] = '-'; v = -v; } return put_u(o, p, (UINT64)v); }

/* Append a C string; returns new position (guards against buffer end). */
static int put_s(char *o, int p, int cap, const char *s)
{ while (s && *s && p < cap - 1) o[p++] = *s++; return p; }

/* Day of week via Sakamoto (0=Sunday). Integer only. */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (m < 1 || m > 12) return -1;
    if (m < 3) y -= 1;
    int w = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    return w < 0 ? w + 7 : w;
}

static int is_leap(int y) { return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0); }

static int days_in_month(int y, int m)
{
    static const int d[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 30;
    if (m == 2 && is_leap(y)) return 29;
    return d[m];
}

static int day_of_year(int y, int m, int d)
{
    int n = d;
    for (int i = 1; i < m; i++) n += days_in_month(y, i);
    return n;
}

/* Days since 1970-01-01 (Howard Hinnant's civil algorithm, integer). */
static INT64 days_from_civil(INT64 y, int m, int d)
{
    y -= (m <= 2);
    INT64 era = (y >= 0 ? y : y - 399) / 400;
    INT64 yoe = y - era * 400;                          /* 0..399 */
    INT64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    INT64 doe = yoe * 365 + yoe/4 - yoe/100 + doy;      /* 0..146096 */
    return era * 146097 + doe - 719468;
}
static void civil_from_days(INT64 z, int *y, int *m, int *d)
{
    z += 719468;
    INT64 era = (z >= 0 ? z : z - 146096) / 146097;
    INT64 doe = z - era * 146097;                       /* 0..146096 */
    INT64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    INT64 yy  = yoe + era * 400;
    INT64 doy = doe - (365*yoe + yoe/4 - yoe/100);
    INT64 mp  = (5*doy + 2) / 153;
    INT64 dd  = doy - (153*mp + 2)/5 + 1;
    INT64 mm  = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

/* Read the RTC. Returns 1 on success, 0 if unavailable. Zeroes t first. */
static int read_rtc(EFI_TIME *t)
{
    if (!t || !gRT || !gRT->GetTime) return 0;
    DT_GET_TIME gt = (DT_GET_TIME)gRT->GetTime;
    for (unsigned i = 0; i < sizeof(*t); i++) ((UINT8*)t)[i] = 0;
    if (EFI_ERROR(gt(t, NULL))) return 0;
    if (t->Month < 1 || t->Month > 12 || t->Day < 1 || t->Day > 31) return 0;
    if (t->Hour > 23 || t->Minute > 59 || t->Second > 59) return 0;
    return 1;
}

static const char *g_wday_long[7] =
    { "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };
static const char *g_wday_short[7] =
    { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *g_month[13] = { "",
    "January","February","March","April","May","June",
    "July","August","September","October","November","December" };

/* ------------------------------------------------------------------------- *
 * Shared button-bar helper (mouse). Buttons are laid out centered along the
 * bottom of the client area. Coordinates are CLIENT-relative (wm.h contract).
 * ------------------------------------------------------------------------- */
#define DT_MAXBTN 6
static int dt_bar(int cw, int ch, const int *ids, const char *const *labels,
                  int n, wm_button *out)
{
    if (n > DT_MAXBTN) n = DT_MAXBTN;
    int bh = wm_button_h();
    int gap = 8;
    int w[DT_MAXBTN], tot = 0;
    for (int i = 0; i < n; i++) { w[i] = wm_button_measure(labels[i]); tot += w[i]; }
    tot += gap * (n - 1);
    int x = (cw - tot) / 2; if (x < 6) x = 6;
    int y = ch - bh - 8; if (y < 0) y = 0;
    for (int i = 0; i < n; i++) {
        out[i].x = x; out[i].y = y; out[i].w = w[i]; out[i].h = bh;
        out[i].id = ids[i]; out[i].enabled = 1;
        int j = 0; for (; labels[i][j] && j < 27; j++) out[i].label[j] = labels[i][j];
        out[i].label[j] = 0;
        x += w[i] + gap;
    }
    return n;
}
static void dt_bar_draw(const wm_button *b, int n, int hover, int press)
{ for (int i = 0; i < n; i++) wm_button_draw(&b[i], hover == b[i].id, press == b[i].id); }
static int dt_bar_hit(const wm_button *b, int n, int mx, int my)
{ for (int i = 0; i < n; i++) if (wm_button_hit(&b[i], mx, my)) return b[i].id; return 0; }

/* dt_bar() output is a pure function of (cw, ch, label selector). Each tool
 * has exactly one live window, so a single cached layout per *_btns() call
 * site (keyed on those inputs) is safe: recompute only on resize or when a
 * toggle (e.g. Start/Stop) changes the label set; otherwise reuse. */
typedef struct {
    int inited, cw, ch, sel, n;
    wm_button b[DT_MAXBTN];
} dt_bar_cache;

static int dt_bar_cached(dt_bar_cache *c, int cw, int ch, int sel,
                         const int *ids, const char *const *labels, int n, wm_button *out)
{
    if (!c->inited || c->cw != cw || c->ch != ch || c->sel != sel || c->n != n) {
        c->n = dt_bar(cw, ch, ids, labels, n, c->b);
        c->cw = cw; c->ch = ch; c->sel = sel; c->inited = 1;
    }
    for (int i = 0; i < c->n; i++) out[i] = c->b[i];
    return c->n;
}

/* ==========================================================================
 * 1) STOPWATCH  - start/stop/lap via frame ticks.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int   running;
    UINT64 frames;          /* accumulated running frames                     */
    UINT64 lap[20];         /* lap split times (frames)                       */
    int    nlap;
    int    hover, press;
} sw_state;
static sw_state g_sw;

enum { SW_TOGGLE = 1, SW_LAP = 2, SW_RESET = 3 };

static void sw_fmt(char *o, UINT64 frames)   /* -> "H:MM:SS.CC" */
{
    UINT64 cs  = (frames * 100) / DT_FPS;
    UINT64 sec = cs / 100;
    int cc = (int)(cs % 100);
    int h = (int)(sec / 3600), m = (int)((sec / 60) % 60), s = (int)(sec % 60);
    int p = 0;
    p = put_u(o, p, (UINT64)h); o[p++] = ':';
    put2(&o[p], m); p += 2; o[p++] = ':';
    put2(&o[p], s); p += 2; o[p++] = '.';
    put2(&o[p], cc); p += 2; o[p] = 0;
}

static int sw_btns(sw_state *s, wm_button *b)
{
    static dt_bar_cache cache;
    const char *labels[3] = { s->running ? "Stop" : "Start", "Lap", "Reset" };
    const int   ids[3]    = { SW_TOGGLE, SW_LAP, SW_RESET };
    return dt_bar_cached(&cache, wm_client_w(s->win), wm_client_h(s->win),
                         s->running, ids, labels, 3, b);
}

static void sw_action(sw_state *s, int id)
{
    if (id == SW_TOGGLE) s->running = !s->running;
    else if (id == SW_LAP) { if (s->running && s->nlap < 20) s->lap[s->nlap++] = s->frames; }
    else if (id == SW_RESET) { s->running = 0; s->frames = 0; s->nlap = 0; }
}

static void sw_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    sw_state *s = &g_sw;
    if (s->running) s->frames++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);

    int sc = dt_sc(), pad = 12;
    char big[24]; sw_fmt(big, s->frames);
    int bl = 0; while (big[bl]) bl++;
    int scale = 4;
    while (scale > 1 && 8 * scale * sc * bl > cw - 2*pad) scale--;
    draw_string_clip(cx + pad, cy + pad, cw - 2*pad, big,
                     s->running ? FOREB_TITLE : FOREB_WHITE, FOREB_BG, 1, scale);
    int y = cy + pad + 16 * scale * sc + 10;

    draw_string_clip(cx + pad, y, cw - 2*pad,
                     s->running ? "RUNNING" : "STOPPED",
                     s->running ? FOREB_TITLE : FOREB_DIM, FOREB_BG, 1, 1);
    y += 16 * sc + 8;
    draw_string_clip(cx + pad, y, cw - 2*pad,
                     "Space=start/stop  L=lap  R=reset", FOREB_DIM, FOREB_BG, 1, 1);
    y += 16 * sc + 8;

    /* laps (newest first) */
    int bh = wm_button_h();
    int list_bottom = cy + ch - bh - 16;
    for (int i = s->nlap - 1; i >= 0 && y < list_bottom; i--) {
        char line[48]; int p = 0;
        line[p++] = 'L'; p = put_u(line, p, (UINT64)(i + 1)); line[p++] = ':'; line[p++] = ' ';
        UINT64 prev = (i > 0) ? s->lap[i-1] : 0;
        char tot[24], split[24];
        sw_fmt(tot, s->lap[i]); sw_fmt(split, s->lap[i] - prev);
        p = put_s(line, p, sizeof(line), tot);
        p = put_s(line, p, sizeof(line), "  (+");
        p = put_s(line, p, sizeof(line), split);
        p = put_s(line, p, sizeof(line), ")");
        line[p] = 0;
        draw_string_clip(cx + pad, y, cw - 2*pad, line, FOREB_TEXT, FOREB_BG, 1, 1);
        y += 16 * sc + 2;
    }

    wm_button b[3]; int n = sw_btns(s, b);
    (void)w;
    dt_bar_draw(b, n, s->hover, s->press);
}

static int sw_event(wm_window *w, const wm_event *ev)
{
    sw_state *s = &g_sw; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        CHAR16 u = ev->unicode;
        if (u == ' ' || u == CHAR_CR) sw_action(s, SW_TOGGLE);
        else if (u == 'l' || u == 'L') sw_action(s, SW_LAP);
        else if (u == 'r' || u == 'R') sw_action(s, SW_RESET);
        return 0;
    }
    wm_button b[3]; int n = sw_btns(s, b);
    if (ev->type == WM_EV_MOUSE_MOVE) s->hover = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_DOWN) s->press = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_UP) {
        int id = dt_bar_hit(b, n, ev->mx, ev->my);
        if (id && id == s->press) sw_action(s, id);
        s->press = 0;
    }
    return 0;
}

void tool_datetime_stopwatch_open(void)
{
    if (g_sw.win) return;
    g_sw.running = 0; g_sw.frames = 0; g_sw.nlap = 0; g_sw.hover = 0; g_sw.press = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*40/100; if (ww < 340) ww = 340; if (ww > 480) ww = 480; if (ww > W-40) ww = W-40;
    int wh = H*52/100; if (wh < 320) wh = 320; if (wh > 520) wh = 520; if (wh > H-40) wh = H-40;
    g_sw.win = wm_open("Stopwatch", ww, wh, sw_draw, sw_event, &g_sw);
}

/* ==========================================================================
 * 2) COUNTDOWN TIMER  - set + beep at zero.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int    running;
    int    fired;               /* alarm already beeped                       */
    int    set_sec;             /* configured duration (seconds)              */
    UINT64 rem_frames;          /* remaining frames while running             */
    int    beep_frames;         /* frames left of the non-blocking alarm tone */
    int    hover, press;
} cd_state;
static cd_state g_cd;

enum { CD_TOGGLE=1, CD_RESET=2, CD_MIN_UP=3, CD_MIN_DN=4, CD_SEC_UP=5, CD_SEC_DN=6 };

static void cd_adjust(cd_state *s, int dsec)
{
    if (s->running) return;
    s->set_sec += dsec;
    if (s->set_sec < 0) s->set_sec = 0;
    if (s->set_sec > 99*3600) s->set_sec = 99*3600;
    s->fired = 0;
}
static void cd_action(cd_state *s, int id)
{
    switch (id) {
    case CD_TOGGLE:
        if (!s->running) {
            if (s->set_sec <= 0) return;
            s->rem_frames = (UINT64)s->set_sec * DT_FPS;
            s->running = 1; s->fired = 0;
        } else s->running = 0;
        break;
    case CD_RESET:
        s->running = 0; s->fired = 0; s->rem_frames = 0;
        if (s->beep_frames) { s->beep_frames = 0; dt_speaker_off(); }
        break;
    case CD_MIN_UP: cd_adjust(s, 60);  break;
    case CD_MIN_DN: cd_adjust(s, -60); break;
    case CD_SEC_UP: cd_adjust(s, 1);   break;
    case CD_SEC_DN: cd_adjust(s, -1);  break;
    }
}

static int cd_btns(cd_state *s, wm_button *b)
{
    static dt_bar_cache cache;
    const char *labels[2] = { s->running ? "Stop" : "Start", "Reset" };
    const int   ids[2]    = { CD_TOGGLE, CD_RESET };
    return dt_bar_cached(&cache, wm_client_w(s->win), wm_client_h(s->win),
                         s->running, ids, labels, 2, b);
}

static void cd_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    cd_state *s = &g_cd; (void)w;
    /* Advance the non-blocking alarm tone: silence it when its frames run out. */
    if (s->beep_frames > 0 && --s->beep_frames == 0) dt_speaker_off();
    if (s->running && s->rem_frames > 0) {
        s->rem_frames--;
        if (s->rem_frames == 0) {
            s->running = 0;
            if (!s->fired) {
                s->fired = 1;
                dt_speaker_on(880);          /* start tone, no Stall */
                s->beep_frames = DT_FPS / 2;  /* ~0.5 s, decremented per frame */
            }
        }
    }
    fill_rect(cx, cy, cw, ch, FOREB_BG);

    int sc = dt_sc(), pad = 12;
    int rem = s->running ? (int)((s->rem_frames + DT_FPS - 1) / DT_FPS) : s->set_sec;
    int h = rem/3600, m = (rem/60)%60, ss = rem%60;

    char big[16]; int p = 0;
    if (h > 0) { p = put_u(big, p, (UINT64)h); big[p++] = ':'; put2(&big[p], m); p += 2; }
    else { put2(&big[p], m); p += 2; }
    big[p++] = ':'; put2(&big[p], ss); p += 2; big[p] = 0;

    int scale = 5;
    while (scale > 1 && 8 * scale * sc * p > cw - 2*pad) scale--;
    UINT32 col = (s->fired && rem == 0) ? FOREB_TIMER
               : s->running ? FOREB_WHITE : FOREB_TEXT;
    draw_string_clip(cx + pad, cy + pad, cw - 2*pad, big, col, FOREB_BG, 1, scale);
    int y = cy + pad + 16 * scale * sc + 12;

    const char *st = s->fired ? "TIME'S UP" : s->running ? "COUNTING" : "SET DURATION";
    draw_string_clip(cx + pad, y, cw - 2*pad, st,
                     s->fired ? FOREB_TIMER : s->running ? FOREB_TITLE : FOREB_DIM,
                     FOREB_BG, 1, 2);
    y += 32 * sc + 10;
    draw_string_clip(cx + pad, y, cw - 2*pad,
                     "Up/Dn=minutes  Left/Right=seconds", FOREB_DIM, FOREB_BG, 1, 1);
    y += 16 * sc + 2;
    draw_string_clip(cx + pad, y, cw - 2*pad,
                     "Space=start/stop  R=reset", FOREB_DIM, FOREB_BG, 1, 1);

    wm_button b[2]; int n = cd_btns(s, b);
    dt_bar_draw(b, n, s->hover, s->press);
}

static int cd_event(wm_window *w, const wm_event *ev)
{
    cd_state *s = &g_cd; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) {
        s->win = NULL;
        if (s->beep_frames) { s->beep_frames = 0; dt_speaker_off(); }
        return 0;
    }
    if (ev->type == WM_EV_KEY) {
        switch (ev->scancode) {
        case SCAN_ESC:   return WM_CLOSE_REQUEST;
        case SCAN_UP:    cd_action(s, CD_MIN_UP); return 0;
        case SCAN_DOWN:  cd_action(s, CD_MIN_DN); return 0;
        case SCAN_RIGHT: cd_action(s, CD_SEC_UP); return 0;
        case SCAN_LEFT:  cd_action(s, CD_SEC_DN); return 0;
        default: break;
        }
        CHAR16 u = ev->unicode;
        if (u == ' ' || u == CHAR_CR) cd_action(s, CD_TOGGLE);
        else if (u == 'r' || u == 'R') cd_action(s, CD_RESET);
        return 0;
    }
    wm_button b[2]; int n = cd_btns(s, b);
    if (ev->type == WM_EV_MOUSE_MOVE) s->hover = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_DOWN) s->press = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_UP) {
        int id = dt_bar_hit(b, n, ev->mx, ev->my);
        if (id && id == s->press) cd_action(s, id);
        s->press = 0;
    }
    return 0;
}

void tool_datetime_countdown_open(void)
{
    if (g_cd.win) return;
    g_cd.running = 0; g_cd.fired = 0; g_cd.set_sec = 300; g_cd.rem_frames = 0;
    g_cd.beep_frames = 0; g_cd.hover = 0; g_cd.press = 0;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*40/100; if (ww < 340) ww = 340; if (ww > 480) ww = 480; if (ww > W-40) ww = W-40;
    int wh = H*46/100; if (wh < 300) wh = 300; if (wh > 460) wh = 460; if (wh > H-40) wh = H-40;
    g_cd.win = wm_open("Countdown Timer", ww, wh, cd_draw, cd_event, &g_cd);
}

/* ==========================================================================
 * 3) MONTH CALENDAR  - prev/next month, today from RTC.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int year, month;            /* displayed month                            */
    int ty, tm, td;             /* today (RTC), td=0 if unknown               */
    int hover, press;
} cal_state;
static cal_state g_cal;

enum { CAL_PREV=1, CAL_NEXT=2, CAL_TODAY=3 };

static void cal_set_today(cal_state *s)
{
    EFI_TIME t;
    if (read_rtc(&t)) { s->ty = t.Year; s->tm = t.Month; s->td = t.Day; }
    else { s->ty = 2026; s->tm = 1; s->td = 0; }
}
static void cal_goto_today(cal_state *s)
{ cal_set_today(s); s->year = s->ty; s->month = s->tm; }

static void cal_step(cal_state *s, int dir)
{
    s->month += dir;
    while (s->month < 1)  { s->month += 12; s->year--; }
    while (s->month > 12) { s->month -= 12; s->year++; }
    if (s->year < 1) s->year = 1;
    if (s->year > 9999) s->year = 9999;
}

static int cal_btns(cal_state *s, wm_button *b)
{
    static dt_bar_cache cache;
    const char *labels[3] = { "< Prev", "Today", "Next >" };
    const int   ids[3]    = { CAL_PREV, CAL_TODAY, CAL_NEXT };
    return dt_bar_cached(&cache, wm_client_w(s->win), wm_client_h(s->win),
                         0, ids, labels, 3, b);
}

static void cal_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    cal_state *s = &g_cal; (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int sc = dt_sc(), pad = 10;

    /* header: Month Year */
    char hdr[32]; int p = 0;
    p = put_s(hdr, p, sizeof(hdr), g_month[s->month]);
    hdr[p++] = ' '; p = put_u(hdr, p, (UINT64)s->year); hdr[p] = 0;
    draw_string_center(cx + cw/2, cy + pad, hdr, FOREB_TITLE, FOREB_BG, 1, 2);
    int top = cy + pad + 32 * sc + 8;

    /* grid geometry */
    int gx = cx + pad, gw = cw - 2*pad;
    int cellw = gw / 7;
    int bh = wm_button_h();
    int gh = (cy + ch - bh - 14) - top;
    int rows = 7;                                  /* 1 header + 6 week rows   */
    int cellh = gh / rows; if (cellh < 12) cellh = 12;

    /* weekday header row */
    for (int i = 0; i < 7; i++) {
        int x = gx + i * cellw;
        draw_string_center(x + cellw/2, top + 2, g_wday_short[i],
                           (i == 0) ? FOREB_TIMER : FOREB_DIM, FOREB_BG, 1, 1);
    }

    int first = weekday(s->year, s->month, 1);     /* 0=Sun */
    if (first < 0) first = 0;
    int dim = days_in_month(s->year, s->month);
    int today_here = (s->td && s->ty == s->year && s->tm == s->month);

    for (int d = 1; d <= dim; d++) {
        int cell = first + d - 1;
        int rr = cell / 7 + 1;                     /* +1: below header         */
        int cc = cell % 7;
        int x = gx + cc * cellw;
        int y = top + rr * cellh;
        if (today_here && d == s->td)
            fill_rect(x + 1, y, cellw - 2, cellh - 1, FOREB_SELECT);
        char db[3]; int q = 0;
        if (d >= 10) db[q++] = (char)('0' + d/10);
        db[q++] = (char)('0' + d%10); db[q] = 0;
        UINT32 col = (today_here && d == s->td) ? FOREB_WHITE
                   : (cc == 0) ? FOREB_TIMER : FOREB_TEXT;
        draw_string_center(x + cellw/2, y + 2, db, col, FOREB_BG, 1, 1);
    }

    draw_string_clip(cx + pad, cy + ch - bh - 30, cw - 2*pad,
                     "Left/Right=month  Up/Dn=year  Home=today",
                     FOREB_DIM, FOREB_BG, 1, 1);

    wm_button b[3]; int n = cal_btns(s, b);
    dt_bar_draw(b, n, s->hover, s->press);
}

static int cal_event(wm_window *w, const wm_event *ev)
{
    cal_state *s = &g_cal; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        switch (ev->scancode) {
        case SCAN_ESC:   return WM_CLOSE_REQUEST;
        case SCAN_LEFT:  case SCAN_PAGE_UP:   cal_step(s, -1); return 0;
        case SCAN_RIGHT: case SCAN_PAGE_DOWN: cal_step(s, 1);  return 0;
        case SCAN_UP:    if (s->year < 9999) s->year++; return 0;
        case SCAN_DOWN:  if (s->year > 1)    s->year--; return 0;
        case SCAN_HOME:  cal_goto_today(s); return 0;
        default: break;
        }
        return 0;
    }
    wm_button b[3]; int n = cal_btns(s, b);
    if (ev->type == WM_EV_MOUSE_MOVE) s->hover = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_DOWN) s->press = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_UP) {
        int id = dt_bar_hit(b, n, ev->mx, ev->my);
        if (id == s->press) {
            if (id == CAL_PREV) cal_step(s, -1);
            else if (id == CAL_NEXT) cal_step(s, 1);
            else if (id == CAL_TODAY) cal_goto_today(s);
        }
        s->press = 0;
    }
    return 0;
}

void tool_datetime_calendar_open(void)
{
    if (g_cal.win) return;
    g_cal.hover = 0; g_cal.press = 0;
    cal_goto_today(&g_cal);
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*46/100; if (ww < 380) ww = 380; if (ww > 560) ww = 560; if (ww > W-40) ww = W-40;
    int wh = H*56/100; if (wh < 360) wh = 360; if (wh > 540) wh = 540; if (wh > H-40) wh = H-40;
    g_cal.win = wm_open("Month Calendar", ww, wh, cal_draw, cal_event, &g_cal);
}

/* ==========================================================================
 * 4) WORLD CLOCKS  - RTC base + fixed UTC offsets.
 * ========================================================================== */
typedef struct { const char *name; int off_min; } dt_zone;
static const dt_zone g_zones[] = {
    { "UTC / GMT",        0    },
    { "London",           0    },
    { "Berlin / Paris",   60   },
    { "Athens / Cairo",   120  },
    { "Moscow",           180  },
    { "Dubai",            240  },
    { "India (IST)",      330  },
    { "Bangkok",          420  },
    { "Beijing / HK",     480  },
    { "Tokyo / Seoul",    540  },
    { "Sydney",           600  },
    { "Auckland",         720  },
    { "Azores",           -60  },
    { "Sao Paulo",        -180 },
    { "New York (EST)",   -300 },
    { "Chicago (CST)",    -360 },
    { "Denver (MST)",     -420 },
    { "Los Angeles",      -480 },
    { "Anchorage",        -540 },
    { "Honolulu",         -600 },
};
#define DT_NZONES (int)(sizeof(g_zones)/sizeof(g_zones[0]))

typedef struct {
    wm_window *win; int scroll;
    int     rtc_ctr;            /* frames left before next RTC poll           */
    EFI_TIME cached_t;          /* cached RTC reading                         */
    int     cached_ok;
    /* change-gate: the display only ticks at 1Hz, so skip the full recompose
     * when nothing visible changed since the last painted frame.             */
    int     valid, last_sec, last_ok, last_scroll;
    int     last_cx, last_cy, last_cw, last_ch;
} wc_state;
static wc_state g_wc;

static void wc_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    wc_state *s = &g_wc; (void)w;
    int sc = dt_sc(), pad = 10;

    /* The RTC's own resolution is 1Hz; sample it at ~DT_FPS/6 instead of
     * every ~30Hz redraw and reuse the cached reading between polls. */
    if (s->rtc_ctr <= 0) {
        s->cached_ok = read_rtc(&s->cached_t);
        s->rtc_ctr = DT_RTC_PERIOD;
    } else s->rtc_ctr--;
    EFI_TIME t = s->cached_t; int ok = s->cached_ok;
    /* base UTC minutes-of-day: normalise the RTC to UTC when it advertises a
     * timezone (EFI: TimeZone = minutes to ADD to local to reach UTC). */
    int base = 0, have = 0;
    if (ok) {
        base = t.Hour*60 + t.Minute;
        if (t.TimeZone != 2047) base += t.TimeZone;   /* -> UTC */
        base %= 1440; if (base < 0) base += 1440;
        have = 1;
    }
    int secs = ok ? t.Second : 0;

    int first = s->scroll; if (first < 0) first = 0;
    if (first > DT_NZONES - 1) first = DT_NZONES - 1;
    s->scroll = first;

    /* Skip the whole recompose when the visible state is unchanged. The window
     * content persists in the framebuffer between frames (the cursor uses
     * save-under), so leaving it untouched is correct. Re-key on geometry too
     * so a move/resize always repaints. */
    if (s->valid && s->last_sec == secs && s->last_ok == ok &&
        s->last_scroll == first && s->last_cx == cx && s->last_cy == cy &&
        s->last_cw == cw && s->last_ch == ch)
        return;
    s->valid = 1; s->last_sec = secs; s->last_ok = ok; s->last_scroll = first;
    s->last_cx = cx; s->last_cy = cy; s->last_cw = cw; s->last_ch = ch;

    fill_rect(cx, cy, cw, ch, FOREB_BG);

    draw_string_clip(cx + pad, cy + pad, cw - 2*pad,
                     have ? "World Clocks (from firmware RTC)"
                          : "RTC unavailable - offsets only",
                     FOREB_TITLE, FOREB_BG, 1, 1);
    int y = cy + pad + 16*sc + 6;
    int row = 16*sc + 6;
    int bottom = cy + ch - pad - (16*sc + 2);   /* reserve the hint line */

    for (int i = first; i < DT_NZONES && y + row <= bottom; i++) {
        int lm = base + g_zones[i].off_min;
        int day = 0;
        while (lm < 0)     { lm += 1440; day--; }
        while (lm >= 1440) { lm -= 1440; day++; }
        int hh = lm/60, mm = lm%60;

        char line[64]; int p = 0;
        p = put_s(line, p, sizeof(line), g_zones[i].name);
        while (p < 18 && p < (int)sizeof(line)-1) line[p++] = ' ';
        put2(&line[p], hh); p += 2; line[p++] = ':';
        put2(&line[p], mm); p += 2;
        if (have) { line[p++] = ':'; put2(&line[p], secs); p += 2; }
        if (day > 0) p = put_s(line, p, sizeof(line), " (+1d)");
        else if (day < 0) p = put_s(line, p, sizeof(line), " (-1d)");
        /* UTC offset tag */
        p = put_s(line, p, sizeof(line), "  UTC");
        int off = g_zones[i].off_min;
        line[p++] = (off < 0) ? '-' : '+';
        int ao = off < 0 ? -off : off;
        p = put_u(line, p, (UINT64)(ao/60));
        if (ao % 60) { line[p++] = ':'; put2(&line[p], ao%60); p += 2; }
        line[p] = 0;

        UINT32 col = (g_zones[i].off_min == 0) ? FOREB_WHITE : FOREB_TEXT;
        draw_string_clip(cx + pad, y, cw - 2*pad, line, col, FOREB_BG, 1, 1);
        y += row;
    }

    draw_string_clip(cx + pad, cy + ch - pad - 16*sc, cw - 2*pad,
                     "Up/Dn/wheel = scroll  Esc = close", FOREB_DIM, FOREB_BG, 1, 1);
}

static int wc_event(wm_window *w, const wm_event *ev)
{
    wc_state *s = &g_wc; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->scancode == SCAN_DOWN && s->scroll < DT_NZONES-1) s->scroll++;
        if (ev->scancode == SCAN_UP   && s->scroll > 0)           s->scroll--;
        return 0;
    }
    if (ev->type == WM_EV_MOUSE_WHEEL) {
        s->scroll -= ev->wheel;
        if (s->scroll < 0) s->scroll = 0;
        if (s->scroll > DT_NZONES-1) s->scroll = DT_NZONES-1;
    }
    return 0;
}

void tool_datetime_worldclock_open(void)
{
    if (g_wc.win) return;
    g_wc.scroll = 0;
    g_wc.valid = 0;     /* force a full paint on the first draw */
    g_wc.rtc_ctr = 0;   /* force an immediate RTC poll on the first draw */
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*44/100; if (ww < 400) ww = 400; if (ww > 560) ww = 560; if (ww > W-40) ww = W-40;
    int wh = H*60/100; if (wh < 360) wh = 360; if (wh > 560) wh = 560; if (wh > H-40) wh = H-40;
    g_wc.win = wm_open("World Clocks", ww, wh, wc_draw, wc_event, &g_wc);
}

/* ==========================================================================
 * 5) UNIX-TIME <-> DATE CONVERTER.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    INT64 ts;                   /* current unix timestamp (>= 0)              */
    int   hover, press;
} ut_state;
static ut_state g_ut;

enum { UT_NOW=1, UT_ZERO=2, UT_PDAY=3, UT_MDAY=4, UT_PHR=5 };

static void ut_from_rtc(ut_state *s)
{
    EFI_TIME t;
    if (!read_rtc(&t)) { s->ts = 0; return; }
    INT64 days = days_from_civil(t.Year, t.Month, t.Day);
    INT64 v = days*86400 + t.Hour*3600 + t.Minute*60 + t.Second;
    if (t.TimeZone != 2047) v += (INT64)t.TimeZone * 60;   /* local -> UTC */
    if (v < 0) v = 0;
    s->ts = v;
}
static void ut_add(ut_state *s, INT64 d)
{ s->ts += d; if (s->ts < 0) s->ts = 0; if (s->ts > 253402300799LL) s->ts = 253402300799LL; }

static int ut_btns(ut_state *s, wm_button *b)
{
    static dt_bar_cache cache;
    const char *labels[3] = { "Now", "+1 day", "+1 hour" };
    const int   ids[3]    = { UT_NOW, UT_PDAY, UT_PHR };
    return dt_bar_cached(&cache, wm_client_w(s->win), wm_client_h(s->win),
                         0, ids, labels, 3, b);
}

static void ut_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    ut_state *s = &g_ut; (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int sc = dt_sc(), pad = 12;

    INT64 z = s->ts / 86400;
    int sod = (int)(s->ts - z*86400);
    int y_, mo, d; civil_from_days(z, &y_, &mo, &d);
    int hh = sod/3600, mm = (sod/60)%60, ssx = sod%60;
    int wd = weekday(y_, mo, d);

    draw_string_clip(cx + pad, cy + pad, cw - 2*pad,
                     "Unix timestamp (seconds since 1970-01-01 UTC):",
                     FOREB_DIM, FOREB_BG, 1, 1);
    int y = cy + pad + 16*sc + 6;

    char ts[24]; int p = put_i(ts, 0, s->ts); ts[p] = 0;
    int scale = 3;
    while (scale > 1 && 8 * scale * sc * p > cw - 2*pad) scale--;
    draw_string_clip(cx + pad, y, cw - 2*pad, ts, FOREB_WHITE, FOREB_BG, 1, scale);
    y += 16*scale*sc + 12;

    /* -> Gregorian UTC */
    char date[48]; p = 0;
    if (wd >= 0) { p = put_s(date, p, sizeof(date), g_wday_short[wd]); date[p++] = ' '; }
    put4(&date[p], y_); p += 4; date[p++] = '-';
    put2(&date[p], mo); p += 2; date[p++] = '-';
    put2(&date[p], d);  p += 2; date[p++] = ' '; date[p++] = ' ';
    put2(&date[p], hh); p += 2; date[p++] = ':';
    put2(&date[p], mm); p += 2; date[p++] = ':';
    put2(&date[p], ssx); p += 2;
    p = put_s(date, p, sizeof(date), " UTC");
    date[p] = 0;
    draw_string_clip(cx + pad, y, cw - 2*pad, date, FOREB_TITLE, FOREB_BG, 1, 2);
    y += 32*sc + 12;

    draw_string_clip(cx + pad, y, cw - 2*pad,
                     "Type digits to edit  Backspace=del", FOREB_DIM, FOREB_BG, 1, 1);
    y += 16*sc + 2;
    draw_string_clip(cx + pad, y, cw - 2*pad,
                     "Up/Dn=+/-day  Left/Right=+/-hour  N=now", FOREB_DIM, FOREB_BG, 1, 1);

    wm_button b[3]; int n = ut_btns(s, b);
    dt_bar_draw(b, n, s->hover, s->press);
}

static int ut_event(wm_window *w, const wm_event *ev)
{
    ut_state *s = &g_ut; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        switch (ev->scancode) {
        case SCAN_ESC:   return WM_CLOSE_REQUEST;
        case SCAN_UP:    ut_add(s, 86400);  return 0;
        case SCAN_DOWN:  ut_add(s, -86400); return 0;
        case SCAN_RIGHT: ut_add(s, 3600);   return 0;
        case SCAN_LEFT:  ut_add(s, -3600);  return 0;
        default: break;
        }
        CHAR16 u = ev->unicode;
        if (u >= '0' && u <= '9') {
            if (s->ts <= 25340230079LL)        /* keep < ~253402300799 after *10+d */
                s->ts = s->ts * 10 + (int)(u - '0');
        } else if (u == CHAR_BACKSPACE) {
            s->ts /= 10;
        } else if (u == 'n' || u == 'N') {
            ut_from_rtc(s);
        }
        return 0;
    }
    wm_button b[3]; int n = ut_btns(s, b);
    if (ev->type == WM_EV_MOUSE_MOVE) s->hover = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_DOWN) s->press = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_UP) {
        int id = dt_bar_hit(b, n, ev->mx, ev->my);
        if (id == s->press) {
            if (id == UT_NOW) ut_from_rtc(s);
            else if (id == UT_PDAY) ut_add(s, 86400);
            else if (id == UT_PHR)  ut_add(s, 3600);
        }
        s->press = 0;
    }
    return 0;
}

void tool_datetime_unixtime_open(void)
{
    if (g_ut.win) return;
    g_ut.hover = 0; g_ut.press = 0;
    ut_from_rtc(&g_ut);
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*48/100; if (ww < 420) ww = 420; if (ww > 620) ww = 620; if (ww > W-40) ww = W-40;
    int wh = H*44/100; if (wh < 300) wh = 300; if (wh > 440) wh = 440; if (wh > H-40) wh = H-40;
    g_ut.win = wm_open("Unix Time Converter", ww, wh, ut_draw, ut_event, &g_ut);
}

/* ==========================================================================
 * 6) DAY-OF-WEEK CALCULATOR  - any date.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int year, month, day;
    int field;                  /* 0=year 1=month 2=day                       */
    int hover, press;
} dw_state;
static dw_state g_dw;

enum { DW_YEAR=1, DW_MONTH=2, DW_DAY=3, DW_TODAY=4 };

static void dw_clamp(dw_state *s)
{
    if (s->year < 1) s->year = 1; if (s->year > 9999) s->year = 9999;
    if (s->month < 1) s->month = 1; if (s->month > 12) s->month = 12;
    int dim = days_in_month(s->year, s->month);
    if (s->day < 1) s->day = 1; if (s->day > dim) s->day = dim;
}
static void dw_today(dw_state *s)
{
    EFI_TIME t;
    if (read_rtc(&t)) { s->year = t.Year; s->month = t.Month; s->day = t.Day; }
    else { s->year = 2026; s->month = 1; s->day = 1; }
    dw_clamp(s);
}
static void dw_adjust(dw_state *s, int dir)
{
    if (s->field == 0) s->year += dir;
    else if (s->field == 1) s->month += dir;
    else s->day += dir;
    if (s->month < 1) s->month = 12; if (s->month > 12) s->month = 1;
    dw_clamp(s);
}

static int dw_btns(dw_state *s, wm_button *b)
{
    static dt_bar_cache cache;
    const char *labels[4] = { "Year", "Month", "Day", "Today" };
    const int   ids[4]    = { DW_YEAR, DW_MONTH, DW_DAY, DW_TODAY };
    return dt_bar_cached(&cache, wm_client_w(s->win), wm_client_h(s->win),
                         0, ids, labels, 4, b);
}

static void dw_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    dw_state *s = &g_dw; (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int sc = dt_sc(), pad = 12;

    /* editable date line YYYY-MM-DD, active field highlighted */
    char yy[8]; put4(yy, s->year); yy[4] = 0;
    char mo[4]; put2(mo, s->month); mo[2] = 0;
    char dd[4]; put2(dd, s->day); dd[2] = 0;
    int scale = 3;
    int x = cx + pad, y = cy + pad;
    int adv = 8*scale*sc;
    UINT32 hi = FOREB_WHITE, lo = FOREB_TEXT, sep = FOREB_DIM;
    draw_string(x, y, yy, s->field==0?hi:lo, FOREB_BG, 1, scale); x += 4*adv;
    draw_string(x, y, "-", sep, FOREB_BG, 1, scale); x += adv;
    draw_string(x, y, mo, s->field==1?hi:lo, FOREB_BG, 1, scale); x += 2*adv;
    draw_string(x, y, "-", sep, FOREB_BG, 1, scale); x += adv;
    draw_string(x, y, dd, s->field==2?hi:lo, FOREB_BG, 1, scale);
    y += 16*scale*sc + 14;

    int wd = weekday(s->year, s->month, s->day);
    const char *wn = (wd >= 0 && wd < 7) ? g_wday_long[wd] : "?";
    draw_string_clip(cx + pad, y, cw - 2*pad, wn, FOREB_TITLE, FOREB_BG, 1, 3);
    y += 48*sc + 12;

    char info[64]; int p = 0;
    p = put_s(info, p, sizeof(info), g_month[s->month]);
    info[p++] = ' '; p = put_u(info, p, (UINT64)s->day);
    info[p++] = ','; info[p++] = ' '; p = put_u(info, p, (UINT64)s->year);
    info[p] = 0;
    draw_string_clip(cx + pad, y, cw - 2*pad, info, FOREB_TEXT, FOREB_BG, 1, 1);
    y += 16*sc + 4;

    char doy[48]; p = 0;
    p = put_s(doy, p, sizeof(doy), "Day ");
    p = put_u(doy, p, (UINT64)day_of_year(s->year, s->month, s->day));
    p = put_s(doy, p, sizeof(doy), " of ");
    p = put_u(doy, p, (UINT64)(is_leap(s->year) ? 366 : 365));
    p = put_s(doy, p, sizeof(doy), is_leap(s->year) ? "  (leap year)" : "  (common year)");
    doy[p] = 0;
    draw_string_clip(cx + pad, y, cw - 2*pad, doy, FOREB_TEXT, FOREB_BG, 1, 1);
    y += 16*sc + 4;

    draw_string_clip(cx + pad, y, cw - 2*pad,
                     "Tab=field  Up/Dn=change  H=today", FOREB_DIM, FOREB_BG, 1, 1);

    wm_button b[4]; int n = dw_btns(s, b);
    dt_bar_draw(b, n, s->hover, s->press);
}

static int dw_event(wm_window *w, const wm_event *ev)
{
    dw_state *s = &g_dw; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        switch (ev->scancode) {
        case SCAN_ESC:   return WM_CLOSE_REQUEST;
        case SCAN_UP:    dw_adjust(s, 1);  return 0;
        case SCAN_DOWN:  dw_adjust(s, -1); return 0;
        case SCAN_LEFT:  s->field = (s->field + 2) % 3; return 0;
        case SCAN_RIGHT: s->field = (s->field + 1) % 3; return 0;
        default: break;
        }
        CHAR16 u = ev->unicode;
        if (u == CHAR_TAB) s->field = (s->field + 1) % 3;
        else if (u == 'h' || u == 'H') dw_today(s);
        return 0;
    }
    wm_button b[4]; int n = dw_btns(s, b);
    if (ev->type == WM_EV_MOUSE_MOVE) s->hover = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_DOWN) s->press = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_UP) {
        int id = dt_bar_hit(b, n, ev->mx, ev->my);
        if (id == s->press) {
            if (id == DW_YEAR)  s->field = 0;
            else if (id == DW_MONTH) s->field = 1;
            else if (id == DW_DAY)   s->field = 2;
            else if (id == DW_TODAY) dw_today(s);
        }
        s->press = 0;
    }
    return 0;
}

void tool_datetime_weekday_open(void)
{
    if (g_dw.win) return;
    g_dw.field = 0; g_dw.hover = 0; g_dw.press = 0;
    dw_today(&g_dw);
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*44/100; if (ww < 400) ww = 400; if (ww > 560) ww = 560; if (ww > W-40) ww = W-40;
    int wh = H*48/100; if (wh < 320) wh = 320; if (wh > 460) wh = 460; if (wh > H-40) wh = H-40;
    g_dw.win = wm_open("Day of Week", ww, wh, dw_draw, dw_event, &g_dw);
}

/* ==========================================================================
 * 7) UPTIME COUNTER  - elapsed since the tool opened (RTC delta, frame tick
 * fallback). Firmware exposes no reliable boot time pre-ExitBootServices, so
 * this is an honest "session" timer.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int    rtc_ok;
    INT64  start_ts;            /* RTC seconds baseline                       */
    UINT64 frames;              /* frame fallback                             */
    UINT64 elapsed;             /* seconds                                    */
    int    hover, press;
    int    rtc_ctr;             /* frames left before next RTC poll (rtc_ok)  */
    INT64  last_now;            /* cached RTC-seconds reading (rtc_ok)        */
} up_state;
static up_state g_up;

static INT64 up_rtc_seconds(void)
{
    EFI_TIME t;
    if (!read_rtc(&t)) return -1;
    INT64 days = days_from_civil(t.Year, t.Month, t.Day);
    return days*86400 + t.Hour*3600 + t.Minute*60 + t.Second;
}

enum { UP_RESET = 1 };
static int up_btns(up_state *s, wm_button *b)
{
    static dt_bar_cache cache;
    const char *labels[1] = { "Reset" };
    const int   ids[1]    = { UP_RESET };
    return dt_bar_cached(&cache, wm_client_w(s->win), wm_client_h(s->win),
                         0, ids, labels, 1, b);
}
static void up_reset(up_state *s)
{
    INT64 now = up_rtc_seconds();
    s->rtc_ok = (now >= 0);
    s->start_ts = (now >= 0) ? now : 0;
    s->frames = 0; s->elapsed = 0;
    s->rtc_ctr = 0; s->last_now = s->start_ts;
}

static void up_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    up_state *s = &g_up; (void)w;
    if (s->rtc_ok) {
        if (s->rtc_ctr <= 0) {
            INT64 now = up_rtc_seconds();
            if (now >= 0) s->last_now = now;
            s->rtc_ctr = DT_RTC_PERIOD;
        } else s->rtc_ctr--;
        INT64 d = s->last_now - s->start_ts; if (d < 0) d = 0; s->elapsed = (UINT64)d;
    } else {
        s->frames++;
        s->elapsed = s->frames / DT_FPS;
    }
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int sc = dt_sc(), pad = 14;

    UINT64 e = s->elapsed;
    int days = (int)(e / 86400);
    int hh = (int)((e / 3600) % 24), mm = (int)((e / 60) % 60), ss = (int)(e % 60);

    draw_string_clip(cx + pad, cy + pad, cw - 2*pad,
                     "Session uptime (since opened):", FOREB_DIM, FOREB_BG, 1, 1);
    int y = cy + pad + 16*sc + 8;

    char big[24]; int p = 0;
    put2(&big[p], hh); p += 2; big[p++] = ':';
    put2(&big[p], mm); p += 2; big[p++] = ':';
    put2(&big[p], ss); p += 2; big[p] = 0;
    int scale = 5;
    while (scale > 1 && 8 * scale * sc * p > cw - 2*pad) scale--;
    draw_string_clip(cx + pad, y, cw - 2*pad, big, FOREB_WHITE, FOREB_BG, 1, scale);
    y += 16*scale*sc + 12;

    char dl[48]; p = 0;
    p = put_u(dl, p, (UINT64)days);
    p = put_s(dl, p, sizeof(dl), days == 1 ? " day  " : " days  ");
    p = put_u(dl, p, e);
    p = put_s(dl, p, sizeof(dl), " s total");
    dl[p] = 0;
    draw_string_clip(cx + pad, y, cw - 2*pad, dl, FOREB_TITLE, FOREB_BG, 1, 2);
    y += 32*sc + 10;

    draw_string_clip(cx + pad, y, cw - 2*pad,
                     s->rtc_ok ? "Timebase: firmware RTC (1s accuracy)"
                               : "Timebase: frame counter (RTC absent)",
                     FOREB_DIM, FOREB_BG, 1, 1);

    wm_button b[1]; int n = up_btns(s, b);
    dt_bar_draw(b, n, s->hover, s->press);
}

static int up_event(wm_window *w, const wm_event *ev)
{
    up_state *s = &g_up; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == 'r' || ev->unicode == 'R') up_reset(s);
        return 0;
    }
    wm_button b[1]; int n = up_btns(s, b);
    if (ev->type == WM_EV_MOUSE_MOVE) s->hover = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_DOWN) s->press = dt_bar_hit(b, n, ev->mx, ev->my);
    else if (ev->type == WM_EV_MOUSE_UP) {
        int id = dt_bar_hit(b, n, ev->mx, ev->my);
        if (id && id == s->press) up_reset(s);
        s->press = 0;
    }
    return 0;
}

void tool_datetime_uptime_open(void)
{
    if (g_up.win) return;
    g_up.hover = 0; g_up.press = 0;
    up_reset(&g_up);
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*42/100; if (ww < 380) ww = 380; if (ww > 520) ww = 520; if (ww > W-40) ww = W-40;
    int wh = H*40/100; if (wh < 280) wh = 280; if (wh > 400) wh = 400; if (wh > H-40) wh = H-40;
    g_up.win = wm_open("Uptime", ww, wh, up_draw, up_event, &g_up);
}

/* ==========================================================================
 * 8) BINARY / WORD CLOCK.
 * ========================================================================== */
typedef struct {
    wm_window *win; int mode;   /* mode 0=binary 1=words                      */
    int      rtc_ctr;           /* frames left before next RTC poll           */
    EFI_TIME cached_t;          /* cached RTC reading                         */
    int      cached_ok;
    /* change-gate: the digital line ticks at 1Hz, so skip the full recompose
     * when the second, mode, RTC availability and geometry are all unchanged. */
    int      valid, last_sec, last_mode, last_ok;
    int      last_cx, last_cy, last_cw, last_ch;
} bc_state;
static bc_state g_bc;

static const char *g_num_word[13] = { "twelve",
    "one","two","three","four","five","six",
    "seven","eight","nine","ten","eleven","twelve" };

/* Minute phrase for the word clock (m5 is a multiple of 5 in {5,10,20,25};
 * the hour table above is only 1..12, so minutes need their own words). */
static const char *bc_min_word(int m5)
{
    switch (m5) {
    case 5:  return "five";
    case 10: return "ten";
    case 20: return "twenty";
    case 25: return "twenty five";
    default: return "";
    }
}

/* Build the "word clock" sentence for hour/minute (nearest 5 minutes). */
static void bc_words(int hour, int minute, char *out, int cap)
{
    int m5 = ((minute + 2) / 5) * 5;      /* round to nearest 5 */
    int h = hour % 12; if (h == 0) h = 12;
    int hnext = (hour + 1) % 12; if (hnext == 0) hnext = 12;
    if (m5 >= 60) { m5 = 0; h = hnext; }

    int p = 0;
    p = put_s(out, p, cap, "It is ");
    if (m5 == 0)       { p = put_s(out, p, cap, g_num_word[h]); p = put_s(out, p, cap, " o'clock"); }
    else if (m5 == 15) { p = put_s(out, p, cap, "quarter past "); p = put_s(out, p, cap, g_num_word[h]); }
    else if (m5 == 30) { p = put_s(out, p, cap, "half past ");    p = put_s(out, p, cap, g_num_word[h]); }
    else if (m5 == 45) { p = put_s(out, p, cap, "quarter to ");   p = put_s(out, p, cap, g_num_word[hnext]); }
    else if (m5 < 30)  {
        p = put_s(out, p, cap, bc_min_word(m5)); p = put_s(out, p, cap, " past ");
        p = put_s(out, p, cap, g_num_word[h]);
    } else {
        p = put_s(out, p, cap, bc_min_word(60 - m5)); p = put_s(out, p, cap, " to ");
        p = put_s(out, p, cap, g_num_word[hnext]);
    }
    out[p] = 0;
}

/* Draw one BCD column of `bits` bits for value 0..9 at (x, top). */
static void bc_column(int x, int top, int cellw, int cellh, int gap,
                      int value, int bits, UINT32 on, UINT32 off)
{
    for (int b = 0; b < bits; b++) {
        int weight = 1 << (bits - 1 - b);
        int lit = (value & weight) != 0;
        int y = top + b * (cellh + gap);
        fill_rect(x, y, cellw, cellh, lit ? on : off);
    }
}

static void bc_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    bc_state *s = &g_bc; (void)w;
    int sc = dt_sc(), pad = 12;

    if (s->rtc_ctr <= 0) {
        s->cached_ok = read_rtc(&s->cached_t);
        s->rtc_ctr = DT_RTC_PERIOD;
    } else s->rtc_ctr--;
    EFI_TIME t = s->cached_t; int ok = s->cached_ok;
    int hh = ok ? t.Hour : 0, mm = ok ? t.Minute : 0, ssx = ok ? t.Second : 0;

    /* Skip the whole recompose when nothing visible changed. Content persists
     * in the framebuffer between frames (the cursor uses save-under), so
     * leaving it untouched is correct. Re-key on geometry so move/resize
     * always repaints. */
    if (s->valid && s->last_sec == ssx && s->last_mode == s->mode &&
        s->last_ok == ok && s->last_cx == cx && s->last_cy == cy &&
        s->last_cw == cw && s->last_ch == ch)
        return;
    s->valid = 1; s->last_sec = ssx; s->last_mode = s->mode; s->last_ok = ok;
    s->last_cx = cx; s->last_cy = cy; s->last_cw = cw; s->last_ch = ch;

    fill_rect(cx, cy, cw, ch, FOREB_BG);

    /* digital line always shown at top */
    char dig[12]; int p = 0;
    put2(&dig[p], hh); p += 2; dig[p++] = ':';
    put2(&dig[p], mm); p += 2; dig[p++] = ':';
    put2(&dig[p], ssx); p += 2; dig[p] = 0;
    draw_string_center(cx + cw/2, cy + pad, dig,
                       ok ? FOREB_WHITE : FOREB_DIM, FOREB_BG, 1, 2);
    int top = cy + pad + 32*sc + 14;

    if (s->mode == 1) {
        /* WORD CLOCK */
        char words[80]; bc_words(hh, mm, words, sizeof(words));
        draw_string_clip(cx + pad, top, cw - 2*pad, words, FOREB_TITLE, FOREB_BG, 1, 2);
        draw_string_clip(cx + pad, top + 32*sc + 16, cw - 2*pad,
                         "(nearest 5 minutes)", FOREB_DIM, FOREB_BG, 1, 1);
    } else {
        /* BINARY BCD CLOCK: 6 columns H10 H1 M10 M1 S10 S1 */
        int vals[6] = { hh/10, hh%10, mm/10, mm%10, ssx/10, ssx%10 };
        int bitsn[6] = { 2, 4, 3, 4, 3, 4 };
        const char *hdr[6] = { "H","H","M","M","S","S" };
        int gcols = 6;
        int gw = cw - 2*pad;
        int colpitch = gw / gcols;
        int cellw = colpitch - 8; if (cellw < 6) cellw = 6; if (cellw > 40) cellw = 40;
        int gap = 4;
        int maxbits = 4;
        int avail = (cy + ch - pad) - (top + 16*sc);
        int cellh = (avail - gap*(maxbits-1)) / maxbits;
        if (cellh < 6) cellh = 6; if (cellh > 34) cellh = 34;

        UINT32 on = FOREB_TITLE, off = FOREB_BORDER;
        for (int c = 0; c < 6; c++) {
            int x = cx + pad + c*colpitch + (colpitch - cellw)/2;
            /* bottom-align columns with fewer bits */
            int coltop = top + 16*sc + (maxbits - bitsn[c]) * (cellh + gap);
            bc_column(x, coltop, cellw, cellh, gap, vals[c], bitsn[c], on, off);
            draw_string_center(x + cellw/2, top, hdr[c], FOREB_DIM, FOREB_BG, 1, 1);
        }
    }

    draw_string_clip(cx + pad, cy + ch - 16*sc - 8, cw - 2*pad,
                     "Space/Tab = binary <-> words", FOREB_DIM, FOREB_BG, 1, 1);
}

static int bc_event(wm_window *w, const wm_event *ev)
{
    bc_state *s = &g_bc; (void)w;
    if (!ev) return 0;
    if (ev->type == WM_EV_CLOSE) { s->win = NULL; return 0; }
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == ' ' || ev->unicode == CHAR_TAB || ev->unicode == CHAR_CR)
            s->mode ^= 1;
        return 0;
    }
    if (ev->type == WM_EV_MOUSE_DOWN) s->mode ^= 1;
    return 0;
}

void tool_datetime_binclock_open(void)
{
    if (g_bc.win) return;
    g_bc.mode = 0;
    g_bc.valid = 0;     /* force a full paint on the first draw */
    g_bc.rtc_ctr = 0;   /* force an immediate RTC poll on the first draw */
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W*42/100; if (ww < 380) ww = 380; if (ww > 540) ww = 540; if (ww > W-40) ww = W-40;
    int wh = H*46/100; if (wh < 300) wh = 300; if (wh > 460) wh = 460; if (wh > H-40) wh = H-40;
    g_bc.win = wm_open("Binary Clock", ww, wh, bc_draw, bc_event, &g_bc);
}

/* ==========================================================================
 * Category registry.
 * ========================================================================== */
const struct forebo_tool cat_datetime_tools[] = {
    { "Stopwatch",       "Start / stop / lap timer (frame ticks)",      "gear", tool_datetime_stopwatch_open  },
    { "Countdown Timer", "Set a duration and beep at zero",             "gear", tool_datetime_countdown_open  },
    { "Month Calendar",  "Browse months, today from the RTC",           "gear", tool_datetime_calendar_open   },
    { "World Clocks",    "RTC time across fixed UTC offsets",           "gear", tool_datetime_worldclock_open },
    { "Unix Time",       "Convert unix timestamp <-> UTC date",         "gear", tool_datetime_unixtime_open   },
    { "Day of Week",     "Weekday + day-of-year for any date",          "gear", tool_datetime_weekday_open    },
    { "Uptime",          "Session uptime counter",                      "gear", tool_datetime_uptime_open     },
    { "Binary Clock",    "Binary BCD + word clock from the RTC",        "gear", tool_datetime_binclock_open   },
};
const int cat_datetime_count = (int)(sizeof(cat_datetime_tools)/sizeof(cat_datetime_tools[0]));
