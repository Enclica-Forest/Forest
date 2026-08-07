#include "errorbox.h"
#include "../ui.h"
#include "wm.h"
#include "../../include/forebo_theme.h"

static char eb_title[48];
static char eb_line1[80];
static char eb_line2[80];

static void eb_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; (void)cw; (void)ch;
    int sc = ui_scale(); if (sc < 1) sc = 1;
    int gh = FOREB_GLYPH_H * sc;
    int lh = gh + 4;
    int y = cy + 10;
    if (eb_line1[0]) { draw_string(cx + 12, y, eb_line1, FOREB_TEXT, 0, 1, 1); y += lh; }
    if (eb_line2[0]) { draw_string(cx + 12, y, eb_line2, FOREB_DIM, 0, 1, 1); y += lh; }
    y += gh;
    draw_string(cx + 12, y, "Press Esc or [x] to close.", FOREB_TIMER, 0, 1, 1);
}

static int eb_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    if (ev->type == WM_EV_KEY && ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
    return 0;
}

void errorbox_show(const char *title, const char *line1, const char *line2)
{
    int i;
    for (i = 0; title && title[i] && i < (int)sizeof(eb_title) - 1; i++)
        eb_title[i] = title[i];
    eb_title[i] = 0;
    for (i = 0; line1 && line1[i] && i < (int)sizeof(eb_line1) - 1; i++)
        eb_line1[i] = line1[i];
    eb_line1[i] = 0;
    for (i = 0; line2 && line2[i] && i < (int)sizeof(eb_line2) - 1; i++)
        eb_line2[i] = line2[i];
    eb_line2[i] = 0;

    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 50 / 100; if (ww < 400) ww = 400; if (ww > 720) ww = 720;
    int wh = H * 28 / 100; if (wh < 180) wh = 180; if (wh > 340) wh = 340;
    wm_open(eb_title, ww, wh, eb_draw, eb_event, NULL);
}
