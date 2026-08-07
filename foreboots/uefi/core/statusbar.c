#include "statusbar.h"
#include "../ui.h"
#include "../../include/forebo_theme.h"

typedef EFI_STATUS (EFIAPI *SB_GET_TIME)(EFI_TIME *Time, VOID *Capabilities);

static EFI_SYSTEM_TABLE *g_st;
static EFI_RUNTIME_SERVICES *g_rt;
static int g_enabled = 1;
static int g_h_bar;
static int g_prev_sec = -1;
static int g_prev_min = -1;
static int g_prev_hour = -1;
static int g_mouse_present;
static int g_prev_mouse_present;

void statusbar_init(EFI_SYSTEM_TABLE *st)
{
    g_st = st;
    g_rt = (st && st->RuntimeServices) ? st->RuntimeServices : 0;
    int sc = ui_scale(); if (sc < 1) sc = 1;
    g_h_bar = 24 * sc;
    g_prev_sec = -1;
    g_prev_min = -1;
    g_prev_hour = -1;
    g_prev_mouse_present = -1;
}

void statusbar_set_mouse(int present) { g_mouse_present = present; }

int statusbar_height(void) { return g_enabled ? g_h_bar : 0; }

void statusbar_draw(void)
{
    int w = (int)ui_width();
    int sc = ui_scale(); if (sc < 1) sc = 1;
    int bar_h = g_h_bar;
    if (!g_enabled || bar_h <= 0 || w <= 0) return;

    /* Check if anything changed: time tick or mouse state toggled. Skip the
     * full clear+redraw (and the ui_mark_dirty) when identical to last frame,
     * avoiding ~184KB of uncached VRAM writes per idle frame on real HW. */
    EFI_TIME t;
    int got_time = 0;
    if (g_rt && g_rt->GetTime) {
        SB_GET_TIME gt = (SB_GET_TIME)g_rt->GetTime;
        if (gt) {
            UINT32 *tw = (UINT32 *)&t;
            for (unsigned i = 0; i < sizeof(t) / sizeof(UINT32); i++) tw[i] = 0;
            if (!EFI_ERROR(gt(&t, NULL))) {
                if (t.Hour <= 23 && t.Minute <= 59 && t.Second <= 59)
                    got_time = 1;
            }
        }
    }

    int time_changed = 0;
    if (got_time) {
        time_changed = (t.Second != g_prev_sec ||
                        t.Minute != g_prev_min ||
                        t.Hour   != g_prev_hour);
    }
    int mouse_changed = (g_mouse_present != g_prev_mouse_present);
    /* No change -> skip entirely. */
    if (!time_changed && !mouse_changed) return;

    fill_rect(0, 0, w, bar_h, FOREB_BG);

    int ty = (bar_h - 16 * sc) / 2;
    if (ty < 0) ty = 0;

    draw_string(8, ty, "ForeB", FOREB_TITLE, FOREB_BG, 1, sc);

    if (got_time) {
        if (time_changed) {
            int cx = w / 2;
            int tw_str = 8 * 8 * sc;
            int tx = cx - tw_str / 2;

            char buf[9];
            int h = t.Hour, m = t.Minute, s = t.Second;
            buf[0] = (char)('0' + h / 10);
            buf[1] = (char)('0' + h % 10);
            buf[2] = ':';
            buf[3] = (char)('0' + m / 10);
            buf[4] = (char)('0' + m % 10);
            buf[5] = ':';
            buf[6] = (char)('0' + s / 10);
            buf[7] = (char)('0' + s % 10);
            buf[8] = 0;

            fill_rect(cx - tw_str / 2 - 4, 0, tw_str + 8, bar_h, FOREB_BG);
            draw_string(tx, ty, buf, FOREB_DIM, FOREB_BG, 1, sc);
            g_prev_sec = s;
            g_prev_min = m;
            g_prev_hour = h;
        }
    } else {
        int cx = w / 2;
        int tw_str = 3 * 8 * sc;
        fill_rect(cx - tw_str / 2 - 4, 0, tw_str + 8, bar_h, FOREB_BG);
        draw_string(cx - tw_str / 2, ty, "---", FOREB_DIM, FOREB_BG, 1, sc);
    }

    int mx = w - 8;
    const char *mp = g_mouse_present ? "[M]" : "[-]";
    int mlen = 3;
    mx -= mlen * 8 * sc;
    draw_string(mx, ty, mp, FOREB_DIM, FOREB_BG, 1, sc);

    ui_mark_dirty(0, 0, w, bar_h);
    g_prev_mouse_present = g_mouse_present;
}
