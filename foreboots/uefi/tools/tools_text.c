/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_text.c - "Text Tools" category: editors & string utilities.
 * =============================================================================
 * Eight template-B windows. Notepad writes to the ESP (needs gST/gBS/gRT); the
 * remaining seven are pure integer compute/draw with fixed buffers, no heap,
 * no libc, no float. See tools_cat.h CATEGORY MODULE CONTRACT + clock.c idiom.
 * ========================================================================== */
#include "tools_text.h"
#include "../ui.h"
#include "../core/wm.h"
#include "../core/input.h"
#include "../efi.h"
#include "../../include/forebo_theme.h"
#include "../../include/font8x8.h"

/* ------------------------------------------------------------------ *
 *  Firmware globals (clock.c idiom).  Only Notepad uses them.         *
 * ------------------------------------------------------------------ */
static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES  *gBS;
static EFI_RUNTIME_SERVICES *gRT;

void cat_text_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices    : 0;
    gRT = st ? st->RuntimeServices : 0;
}

typedef EFI_STATUS (EFIAPI *TXT_GET_TIME)(EFI_TIME *Time, VOID *Capabilities);

static EFI_GUID txt_sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

/* ------------------------------------------------------------------ *
 *  Tiny freestanding helpers.                                        *
 * ------------------------------------------------------------------ */
static int  sc(void)                 { int s = ui_scale(); return s < 1 ? 1 : s; }
static int  ci_lower(int c)          { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int  ci_upper(int c)          { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int  is_alpha(int c)          { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int  is_space(int c)          { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int  slen(const char *s)      { int n = 0; if (s) while (s[n]) n++; return n; }

static void scopy(char *d, const char *s, int cap)
{
    int i = 0; if (!d || cap <= 0) return;
    if (s) for (; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

/* clip a fill_rect to a box */
static void fill_clip(int x, int y, int w, int h, UINT32 col,
                      int bx, int by, int bw, int bh)
{
    int x2 = x + w, y2 = y + h;
    if (x < bx) x = bx; if (y < by) y = by;
    if (x2 > bx + bw) x2 = bx + bw;
    if (y2 > by + bh) y2 = by + bh;
    if (x2 > x && y2 > y) fill_rect(x, y, x2 - x, y2 - y, col);
}

/* animated blink phase (draw is called every frame) */
static unsigned g_blink;
static int caret_on(void) { return ((g_blink / 16) & 1) == 0; }

/* ------------------------------------------------------------------ *
 *  Bottom button bar (self-contained; mirrors tools.c look).         *
 * ------------------------------------------------------------------ */
#define TXT_MAXBTN 4
static int bar_h(void)         { return wm_button_h() + 10 * sc(); }
static int content_h(int ch)   { return ch - bar_h(); }

/* Lay a left-aligned row of buttons in CLIENT coords along the bottom. */
static int bar_layout(int ch, const char *const labels[], const int ids[],
                      int n, wm_button *out)
{
    int s = sc(), y = ch - 4 * s - wm_button_h(), x = 6 * s;
    if (n > TXT_MAXBTN) n = TXT_MAXBTN;
    for (int i = 0; i < n; i++) {
        int w = wm_button_measure(labels[i]);
        out[i].x = x; out[i].y = y; out[i].w = w; out[i].h = wm_button_h();
        out[i].id = ids[i]; out[i].enabled = 1;
        scopy(out[i].label, labels[i], (int)sizeof(out[i].label));
        x += w + 6 * s;
    }
    return n;
}

static void bar_draw(int cx, int cy, int cw, int ch, const wm_button *b, int n,
                     int hover, int press)
{
    int s = sc();
    draw_hline(cx + 4 * s, cy + ch - 4 * s - wm_button_h() - 4 * s,
               cw - 8 * s, FOREB_BORDER);
    for (int i = 0; i < n; i++)
        wm_button_draw(&b[i], b[i].id == hover, b[i].id == press);
}

static int bar_hit(const wm_button *b, int n, int mx, int my)
{
    for (int i = 0; i < n; i++)
        if (b[i].enabled && wm_button_hit(&b[i], mx, my)) return b[i].id;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Editable text buffer (single- or multi-line).                     *
 * ------------------------------------------------------------------ */
static void buf_ins(char *buf, int cap, int *len, int *cur, int c)
{
    if (*len + 1 >= cap) return;
    for (int i = *len; i > *cur; i--) buf[i] = buf[i - 1];
    buf[*cur] = (char)c; (*len)++; (*cur)++; buf[*len] = 0;
}
static void buf_del(char *buf, int *len, int pos)
{
    if (pos < 0 || pos >= *len) return;
    for (int i = pos; i < *len - 1; i++) buf[i] = buf[i + 1];
    (*len)--; buf[*len] = 0;
}
static int line_start(const char *buf, int pos)
{ int i = pos; while (i > 0 && buf[i - 1] != '\n') i--; return i; }
static int line_end(const char *buf, int len, int pos)
{ int i = pos; while (i < len && buf[i] != '\n') i++; return i; }

/* Apply one WM_EV_KEY to an editable buffer. */
static void edit_key(char *buf, int cap, int *len, int *cur,
                     const wm_event *ev, int multiline)
{
    if (!ev || ev->type != WM_EV_KEY) return;
    UINT16 sk = ev->scancode; CHAR16 u = ev->unicode;

    if (sk == SCAN_LEFT)  { if (*cur > 0) (*cur)--; return; }
    if (sk == SCAN_RIGHT) { if (*cur < *len) (*cur)++; return; }
    if (sk == SCAN_HOME)  { *cur = multiline ? line_start(buf, *cur) : 0; return; }
    if (sk == SCAN_END)   { *cur = multiline ? line_end(buf, *len, *cur) : *len; return; }
    if (sk == SCAN_DELETE){ buf_del(buf, len, *cur); if (*cur > *len) *cur = *len; return; }
    if (multiline && sk == SCAN_UP) {
        int ls = line_start(buf, *cur), col = *cur - ls;
        if (ls == 0) { *cur = 0; }
        else { int pls = line_start(buf, ls - 1); int plen = (ls - 1) - pls;
               *cur = pls + (col < plen ? col : plen); }
        return;
    }
    if (multiline && sk == SCAN_DOWN) {
        int col = *cur - line_start(buf, *cur);
        int le  = line_end(buf, *len, *cur);
        if (le >= *len) { *cur = *len; }
        else { int nls = le + 1; int nle = line_end(buf, *len, nls);
               int nlen = nle - nls;
               *cur = nls + (col < nlen ? col : nlen); }
        return;
    }
    if (u == CHAR_BACKSPACE) { if (*cur > 0) { buf_del(buf, len, *cur - 1); (*cur)--; } return; }
    if (u == CHAR_CR || u == CHAR_LINEFEED) {
        if (multiline) buf_ins(buf, cap, len, cur, '\n');
        return;
    }
    if (u == CHAR_TAB) { buf_ins(buf, cap, len, cur, ' '); buf_ins(buf, cap, len, cur, ' '); return; }
    if (u >= 0x20 && u < 0x7f) buf_ins(buf, cap, len, cur, (int)u);
}

/* Per-buffer cache for edit_draw's caret line/col rescan. Lives in the owning
 * tool state (one instance each for Notepad + Text Counter) so two open
 * editable windows never thrash a single shared slot into O(cur) rescans on
 * every frame. Zero-initialised state is valid (buf=0 forces a first compute). */
typedef struct {
    const char *buf;
    int len, cur, cl, cc;
} edit_caret_cache;

/* One caret cache per editable window (Notepad + Text Counter). */
static edit_caret_cache g_note_cc;
static edit_caret_cache g_cnt_cc;

/* Draw a multi-line editable buffer inside [x,y,w,h] (screen coords), keeping
 * the caret (index `cur`) visible via *scrollp (first visible line). Draws the
 * caret when `focus`. `cc_cache` is the owner's per-buffer caret cache. */
static void edit_draw(char *buf, int len, int cur, int *scrollp, int focus,
                      int x, int y, int w, int h, edit_caret_cache *cc_cache)
{
    int CW = 8 * sc(), LH = 16 * sc();
    int rows = h / LH; if (rows < 1) rows = 1;

    /* caret line/col: cached per-buffer since this is a full rescan and
     * edit_draw is called every frame (incl. pure caret-blink redraws
     * where buf/len/cur have not changed since the previous call). */
    int cl, cc;
    if (cc_cache && buf == cc_cache->buf && len == cc_cache->len && cur == cc_cache->cur) {
        cl = cc_cache->cl; cc = cc_cache->cc;
    } else {
        cl = 0; cc = 0;
        for (int i = 0; i < cur && i < len; i++) {
            if (buf[i] == '\n') { cl++; cc = 0; } else cc++;
        }
        if (cc_cache) {
            cc_cache->buf = buf; cc_cache->len = len; cc_cache->cur = cur;
            cc_cache->cl = cl; cc_cache->cc = cc;
        }
    }
    if (*scrollp > cl) *scrollp = cl;
    if (cl >= *scrollp + rows) *scrollp = cl - rows + 1;
    if (*scrollp < 0) *scrollp = 0;

    int ln = 0, i = 0;
    char line[256];
    while (ln < *scrollp + rows && i <= len) {
        if (ln >= *scrollp) {
            int p = 0;
            while (i < len && buf[i] != '\n') { if (p < 255) line[p++] = buf[i]; i++; }
            line[p] = 0;
            int ry = y + (ln - *scrollp) * LH;
            draw_string_clip(x, ry, w, line, FOREB_TEXT, FOREB_BG, 1, 1);
        } else {
            while (i < len && buf[i] != '\n') i++;
        }
        if (i < len && buf[i] == '\n') i++;   /* consume newline */
        else if (i >= len) { ln++; break; }
        ln++;
    }

    if (focus && caret_on() && cl >= *scrollp && cl < *scrollp + rows) {
        int cxp = x + cc * CW;
        int cyp = y + (cl - *scrollp) * LH;
        fill_clip(cxp, cyp, (sc() > 1 ? 2 : 1) + 1, LH, FOREB_WHITE, x, y, w, h);
    }
}

/* ==================================================================
 *  Tool 1: NOTEPAD  (writes to ESP \forebo\notes\)
 * ================================================================== */
#define NOTE_CAP 4096
enum { NB_SAVE = 1, NB_CLEAR = 2, NB_NEW = 3 };

typedef struct {
    wm_window *win;
    char  buf[NOTE_CAP];
    int   len, cur, scroll;
    int   b_hover, b_press;
    char  status[96];
    int   saved_ok;      /* 0 unknown, 1 ok, -1 fail */
    int   lines_cache;      /* cached newline-derived line count for the header */
    int   lines_cache_len;  /* g_note.len at the time lines_cache was computed */
} note_state;
static note_state g_note;

/* ASCII path -> CHAR16 with '/'->'\\'. */
static void ascii_to_c16(const char *in, CHAR16 *out, int cap)
{
    int i = 0; if (!out || cap <= 0) return;
    if (in) for (; in[i] && i + 1 < cap; i++)
        out[i] = (CHAR16)((in[i] == '/') ? '\\' : (unsigned char)in[i]);
    out[i] = 0;
}

/* Locate a writable filesystem root (ESP is normally the first). */
static EFI_FILE_PROTOCOL *note_open_root(void)
{
    if (!gBS) return 0;
    UINTN nh = 0; EFI_HANDLE *hs = 0;
    EFI_STATUS st = gBS->LocateHandleBuffer(ByProtocol, &txt_sfs_guid, 0, &nh, &hs);
    if (EFI_ERROR(st) || !hs || nh == 0) return 0;
    EFI_FILE_PROTOCOL *root = 0;
    for (UINTN k = 0; k < nh; k++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        if (EFI_ERROR(gBS->HandleProtocol(hs[k], &txt_sfs_guid, (VOID **)&fs)) || !fs)
            continue;
        EFI_FILE_PROTOCOL *r = 0;
        if (EFI_ERROR(fs->OpenVolume(fs, &r)) || !r) continue;
        root = r; break;      /* first mountable volume */
    }
    if (gBS->FreePool) gBS->FreePool(hs);
    return root;
}

static void note_mkdir(EFI_FILE_PROTOCOL *root, const char *path)
{
    CHAR16 wp[64]; ascii_to_c16(path, wp, 64);
    EFI_FILE_PROTOCOL *d = 0;
    if (!EFI_ERROR(root->Open(root, &d, wp,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
            EFI_FILE_DIRECTORY)) && d)
        d->Close(d);
}

static void note_save(note_state *n)
{
    n->saved_ok = -1;
    scopy(n->status, "Save failed (no writable volume)", sizeof(n->status));
    EFI_FILE_PROTOCOL *root = note_open_root();
    if (!root) return;
    /* volume found: any failure from here is a create/write error */
    scopy(n->status, "Save failed (create/write error)", sizeof(n->status));

    note_mkdir(root, "/forebo");
    note_mkdir(root, "/forebo/notes");

    /* timestamped file name so saves do not clobber each other */
    char name[64]; scopy(name, "/forebo/notes/note-", sizeof(name));
    int p = slen(name);
    EFI_TIME t; int have = 0;
    if (gRT) {
        TXT_GET_TIME gt = (TXT_GET_TIME)gRT->GetTime;
        if (gt && !EFI_ERROR(gt(&t, 0))) have = 1;
    }
    if (have) {
        int vals[6] = { t.Hour, t.Minute, t.Second, t.Day, t.Month, (int)(t.Year % 100) };
        /* HHMMSS then -DDMMYY */
        const int order[6] = { 0, 1, 2, 3, 4, 5 };
        for (int k = 0; k < 6 && p < 58; k++) {
            if (k == 3) name[p++] = '-';
            int v = vals[order[k]];
            name[p++] = (char)('0' + (v / 10) % 10);
            name[p++] = (char)('0' + v % 10);
        }
    } else {
        static int ctr = 0; ctr++;
        name[p++] = (char)('0' + (ctr / 10) % 10);
        name[p++] = (char)('0' + ctr % 10);
    }
    name[p++] = '.'; name[p++] = 't'; name[p++] = 'x'; name[p++] = 't'; name[p] = 0;

    CHAR16 wp[80]; ascii_to_c16(name, wp, 80);
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS st = root->Open(root, &f, wp,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR(st) && f) {
        UINTN sz = (UINTN)n->len;
        st = f->Write(f, &sz, n->buf);
        if (f->Flush) f->Flush(f);
        f->Close(f);
        if (!EFI_ERROR(st)) {
            n->saved_ok = 1;
            char m[96]; scopy(m, "Saved ", sizeof(m));
            int q = slen(m); const char *s = name;
            for (; *s && q < 94; s++) m[q++] = *s; m[q] = 0;
            scopy(n->status, m, sizeof(n->status));
        }
    }
    root->Close(root);
}

static const char *const note_labels[] = { "Save", "Clear", "New" };
static const int         note_ids[]    = { NB_SAVE, NB_CLEAR, NB_NEW };

static void note_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 8 * s;
    int hy = cy + 4 * s;
    char hdr[64]; scopy(hdr, "Notepad  ", sizeof(hdr));
    int hp = slen(hdr);
    /* char/line stats in header (len-change is a reliable dirty signal: all
     * edits go through buf_ins/buf_del, which always change len) */
    if (g_note.len != g_note.lines_cache_len) {
        int lines = 1; for (int i = 0; i < g_note.len; i++) if (g_note.buf[i] == '\n') lines++;
        g_note.lines_cache = lines; g_note.lines_cache_len = g_note.len;
    }
    int lines = g_note.lines_cache;
    hdr[hp++] = '[';
    { int v = g_note.len; char tmp[12]; int tp = 0; if (v==0) tmp[tp++]='0';
      while (v>0){ tmp[tp++]=(char)('0'+v%10); v/=10; }
      while (tp>0 && hp<60) hdr[hp++]=tmp[--tp]; }
    hdr[hp++] = 'c'; hdr[hp++] = ' ';
    { int v = lines; char tmp[12]; int tp = 0; if (v==0) tmp[tp++]='0';
      while (v>0){ tmp[tp++]=(char)('0'+v%10); v/=10; }
      while (tp>0 && hp<62) hdr[hp++]=tmp[--tp]; }
    hdr[hp++] = 'L'; hdr[hp++] = ']'; hdr[hp] = 0;
    draw_string_clip(cx + pad, hy, cw - 2 * pad, hdr, FOREB_TITLE, FOREB_BG, 1, 1);

    int ex = cx + pad, ey = hy + 18 * s;
    int ew = cw - 2 * pad;
    int eh = (cy + content_h(ch)) - ey - 20 * s;
    if (eh < 16 * s) eh = 16 * s;
    draw_rect_outline(ex - 3, ey - 3, ew + 6, eh + 6, 1, FOREB_BORDER);
    edit_draw(g_note.buf, g_note.len, g_note.cur, &g_note.scroll, 1,
              ex, ey, ew, eh, &g_note_cc);

    /* status line */
    UINT32 sco = g_note.saved_ok == 1 ? FOREB_TITLE :
                 g_note.saved_ok == -1 ? FOREB_TIMER : FOREB_DIM;
    const char *stx = g_note.status[0] ? g_note.status :
        "Type; arrows/Home/End move; Save writes to ESP \\forebo\\notes";
    draw_string_clip(cx + pad, ey + eh + 4 * s, cw - 2 * pad, stx, sco, FOREB_BG, 1, 1);

    wm_button b[3]; int nb = bar_layout(ch, note_labels, note_ids, 3, b);
    bar_draw(cx, cy, cw, ch, b, nb, g_note.b_hover, g_note.b_press);
}

static int note_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    note_state *n = &g_note;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        edit_key(n->buf, NOTE_CAP, &n->len, &n->cur, ev, 1);
        n->status[0] = 0; n->saved_ok = 0;
        return 0;
    case WM_EV_MOUSE_MOVE: {
        wm_button b[3]; int nb = bar_layout(wm_client_h(w), note_labels, note_ids, 3, b);
        n->b_hover = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_DOWN: {
        wm_button b[3]; int nb = bar_layout(wm_client_h(w), note_labels, note_ids, 3, b);
        n->b_press = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_UP: {
        if (!n->b_press) return 0;
        wm_button b[3]; int nb = bar_layout(wm_client_h(w), note_labels, note_ids, 3, b);
        int id = bar_hit(b, nb, ev->mx, ev->my), p = n->b_press; n->b_press = 0;
        if (id == p) {
            if (p == NB_SAVE) note_save(n);
            else if (p == NB_CLEAR) { n->len = 0; n->cur = 0; n->scroll = 0;
                                      n->buf[0] = 0; n->status[0] = 0; n->saved_ok = 0; }
            else if (p == NB_NEW)   { n->len = 0; n->cur = 0; n->scroll = 0;
                                      n->buf[0] = 0;
                                      scopy(n->status, "New note", sizeof(n->status));
                                      n->saved_ok = 0; }
        }
        return 0; }
    case WM_EV_MOUSE_WHEEL:
        n->scroll -= ev->wheel; if (n->scroll < 0) n->scroll = 0; return 0;
    case WM_EV_CLOSE: n->win = 0; return 0;
    default: return 0;
    }
}

void tool_text_notepad_open(void)
{
    if (g_note.win) return;
    g_note.b_hover = g_note.b_press = 0; g_note.status[0] = 0; g_note.saved_ok = 0;
    g_note.lines_cache_len = -1;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 60 / 100; if (ww < 480) ww = 480; if (ww > 860) ww = 860; if (ww > W - 40) ww = W - 40;
    int wh = H * 62 / 100; if (wh < 340) wh = 340; if (wh > 640) wh = 640; if (wh > H - 40) wh = H - 40;
    g_note.win = wm_open("Notepad", ww, wh, note_draw, note_event, &g_note);
}

/* ==================================================================
 *  Tool 2: ASCII BANNER  (big block letters from font8x8)
 * ================================================================== */
#define BAN_CAP 40
typedef struct {
    wm_window *win;
    char buf[BAN_CAP];
    int  len, cur;
} ban_state;
static ban_state g_ban;

static void ban_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s;
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad,
                     "ASCII Banner  -  type text below", FOREB_TITLE, FOREB_BG, 1, 1);

    /* input field */
    int iy = cy + 26 * s;
    draw_rect_outline(cx + pad - 3, iy - 3, cw - 2 * pad + 6, 16 * s + 6, 1, FOREB_BORDER);
    const char *shown = g_ban.len ? g_ban.buf : "(empty)";
    draw_string_clip(cx + pad, iy, cw - 2 * pad, shown,
                     g_ban.len ? FOREB_TEXT : FOREB_DIM, FOREB_BG, 1, 1);
    if (caret_on()) {
        int caretx = cx + pad + g_ban.cur * 8 * s;
        fill_clip(caretx, iy, 2, 16 * s, FOREB_WHITE, cx + pad, iy, cw - 2 * pad, 16 * s);
    }

    /* banner area */
    int by = iy + 30 * s;
    int bw = cw - 2 * pad, bh = (cy + ch) - by - 8 * s;
    if (bh < 8 || g_ban.len == 0) {
        if (g_ban.len == 0)
            draw_string_clip(cx + pad, by, bw, "Result renders here.",
                             FOREB_DIM, FOREB_BG, 1, 1);
        return;
    }
    int n = g_ban.len;
    /* pick block pixel size to fit width (9 cols/char incl. gap) + height (9 rows) */
    int px = bw / (n * 9); if (px < 1) px = 1;
    if (px * 9 > bh) px = bh / 9; if (px < 1) px = 1;
    int totw = n * 9 * px;
    int ox = cx + pad + (bw - totw) / 2; if (ox < cx + pad) ox = cx + pad;
    int oy = by + (bh - 9 * px) / 2; if (oy < by) oy = by;

    for (int i = 0; i < n; i++) {
        int c = (unsigned char)g_ban.buf[i];
        int gi = (c < 0x20 || c > 0x7f) ? 0 : (c - 0x20);
        int gx = ox + i * 9 * px;
        for (int row = 0; row < 8; row++) {
            unsigned bits = font8x8[gi][row];
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col))
                    fill_clip(gx + col * px, oy + row * px, px, px,
                              FOREB_TITLE, cx + pad, by, bw, bh);
        }
    }
}

static int ban_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        edit_key(g_ban.buf, BAN_CAP, &g_ban.len, &g_ban.cur, ev, 0);
        return 0;
    }
    if (ev->type == WM_EV_CLOSE) g_ban.win = 0;
    return 0;
}

void tool_text_banner_open(void)
{
    if (g_ban.win) return;
    if (g_ban.len == 0) { scopy(g_ban.buf, "FOREB", BAN_CAP); g_ban.len = 5; g_ban.cur = 5; }
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 60 / 100; if (ww < 460) ww = 460; if (ww > 820) ww = 820; if (ww > W - 40) ww = W - 40;
    int wh = H * 45 / 100; if (wh < 260) wh = 260; if (wh > 480) wh = 480; if (wh > H - 40) wh = H - 40;
    g_ban.win = wm_open("ASCII Banner", ww, wh, ban_draw, ban_event, &g_ban);
}

/* ==================================================================
 *  Tool 3: HEX  <->  TEXT
 * ================================================================== */
#define HEX_CAP 512
enum { HXB_MODE = 1 };
typedef struct {
    wm_window *win;
    char buf[HEX_CAP];
    int  len, cur;
    int  mode;         /* 0 = text->hex, 1 = hex->text */
    int  b_hover, b_press;
} hex_state;
static hex_state g_hxt;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* build output into out[cap], return length */
static int hex_build(const hex_state *h, char *out, int cap)
{
    int o = 0;
    if (h->mode == 0) {                       /* text -> hex */
        const char hd[] = "0123456789ABCDEF";
        for (int i = 0; i < h->len && o + 3 < cap; i++) {
            unsigned c = (unsigned char)h->buf[i];
            out[o++] = hd[(c >> 4) & 0xf];
            out[o++] = hd[c & 0xf];
            out[o++] = ' ';
        }
    } else {                                  /* hex -> text */
        int hi = -1;
        for (int i = 0; i < h->len && o + 1 < cap; i++) {
            int v = hexval((unsigned char)h->buf[i]);
            if (v < 0) continue;              /* skip spaces/punct */
            if (hi < 0) hi = v;
            else { int byte = hi * 16 + v; hi = -1;
                   out[o++] = (byte >= 0x20 && byte < 0x7f) ? (char)byte : '.'; }
        }
    }
    out[o] = 0;
    return o;
}

/* draw text wrapped to width into [x,y,w,h] */
static void draw_wrapped(const char *s, int slen_, int x, int y, int w, int h, UINT32 fg)
{
    int CW = 8 * sc(), LH = 16 * sc();
    int perline = w / CW; if (perline < 1) perline = 1;
    int rows = h / LH; if (rows < 1) rows = 1;
    char line[256];
    int i = 0, r = 0;
    while (i < slen_ && r < rows) {
        int p = 0;
        while (i < slen_ && p < perline && p < 255) {
            if (s[i] == '\n') { i++; break; }
            line[p++] = s[i++];
        }
        line[p] = 0;
        draw_string_clip(x, y + r * LH, w, line, fg, FOREB_BG, 1, 1);
        r++;
    }
}

static const char *const hex_labels0[] = { "Mode: Text->Hex" };
static const char *const hex_labels1[] = { "Mode: Hex->Text" };
static const int         hex_ids[]     = { HXB_MODE };

static void hex_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s;
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad,
        g_hxt.mode == 0 ? "Hex Encoder  -  input text:"
                        : "Hex Decoder  -  input hex bytes:",
        FOREB_TITLE, FOREB_BG, 1, 1);

    int iy = cy + 26 * s, ih = 16 * s;
    draw_rect_outline(cx + pad - 3, iy - 3, cw - 2 * pad + 6, ih + 6, 1, FOREB_BORDER);
    const char *shown = g_hxt.len ? g_hxt.buf : "(empty)";
    draw_string_clip(cx + pad, iy, cw - 2 * pad, shown,
                     g_hxt.len ? FOREB_TEXT : FOREB_DIM, FOREB_BG, 1, 1);
    if (caret_on()) {
        int caretx = cx + pad + g_hxt.cur * 8 * s;
        fill_clip(caretx, iy, 2, ih, FOREB_WHITE, cx + pad, iy, cw - 2 * pad, ih);
    }

    draw_string_clip(cx + pad, iy + 24 * s, cw - 2 * pad, "Output:",
                     FOREB_DIM, FOREB_BG, 1, 1);
    char out[HEX_CAP * 3 + 4];
    int ol = hex_build(&g_hxt, out, (int)sizeof(out));
    int oy = iy + 42 * s;
    int oh = (cy + content_h(ch)) - oy - 4 * s;
    draw_wrapped(out, ol, cx + pad, oy, cw - 2 * pad, oh, FOREB_TEXT);

    wm_button b[1]; int nb = bar_layout(ch, g_hxt.mode ? hex_labels1 : hex_labels0, hex_ids, 1, b);
    bar_draw(cx, cy, cw, ch, b, nb, g_hxt.b_hover, g_hxt.b_press);
}

static int hex_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    hex_state *h = &g_hxt;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == CHAR_TAB) { h->mode = !h->mode; return 0; }
        edit_key(h->buf, HEX_CAP, &h->len, &h->cur, ev, 0);
        return 0;
    case WM_EV_MOUSE_MOVE: {
        wm_button b[1]; int nb = bar_layout(wm_client_h(w), h->mode?hex_labels1:hex_labels0, hex_ids, 1, b);
        h->b_hover = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_DOWN: {
        wm_button b[1]; int nb = bar_layout(wm_client_h(w), h->mode?hex_labels1:hex_labels0, hex_ids, 1, b);
        h->b_press = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_UP: {
        if (!h->b_press) return 0;
        wm_button b[1]; int nb = bar_layout(wm_client_h(w), h->mode?hex_labels1:hex_labels0, hex_ids, 1, b);
        int id = bar_hit(b, nb, ev->mx, ev->my), p = h->b_press; h->b_press = 0;
        if (id == p && p == HXB_MODE) h->mode = !h->mode;
        return 0; }
    case WM_EV_CLOSE: h->win = 0; return 0;
    default: return 0;
    }
}

void tool_text_hex_open(void)
{
    if (g_hxt.win) return;
    g_hxt.b_hover = g_hxt.b_press = 0;
    if (g_hxt.len == 0) { scopy(g_hxt.buf, "Hello", HEX_CAP); g_hxt.len = 5; g_hxt.cur = 5; }
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 58 / 100; if (ww < 460) ww = 460; if (ww > 820) ww = 820; if (ww > W - 40) ww = W - 40;
    int wh = H * 55 / 100; if (wh < 320) wh = 320; if (wh > 560) wh = 560; if (wh > H - 40) wh = H - 40;
    g_hxt.win = wm_open("Hex <-> Text", ww, wh, hex_draw, hex_event, &g_hxt);
}

/* ==================================================================
 *  Tool 4: CHAR / WORD / LINE COUNTER
 * ================================================================== */
#define CNT_CAP 4096
typedef struct {
    wm_window *win;
    char buf[CNT_CAP];
    int  len, cur, scroll;
    int  cache_len;                            /* len the cached counts below were computed for */
    int  cache_chars, cache_nospace, cache_words, cache_lines;
} cnt_state;
static cnt_state g_cnt;

static void put_int(char *o, int *p, int cap, int v)
{
    char tmp[16]; int tp = 0;
    if (v == 0) tmp[tp++] = '0';
    if (v < 0) { if (*p < cap - 1) o[(*p)++] = '-'; v = -v; }
    while (v > 0 && tp < 16) { tmp[tp++] = (char)('0' + v % 10); v /= 10; }
    while (tp > 0 && *p < cap - 1) o[(*p)++] = tmp[--tp];
    o[*p] = 0;
}

static void cnt_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s;
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad,
                     "Counter  -  type/paste text:", FOREB_TITLE, FOREB_BG, 1, 1);

    int ey = cy + 26 * s;
    int stat_h = 40 * s;
    int ew = cw - 2 * pad;
    int eh = (cy + ch) - ey - stat_h - 8 * s;
    if (eh < 16 * s) eh = 16 * s;
    draw_rect_outline(cx + pad - 3, ey - 3, ew + 6, eh + 6, 1, FOREB_BORDER);
    edit_draw(g_cnt.buf, g_cnt.len, g_cnt.cur, &g_cnt.scroll, 1,
              cx + pad, ey, ew, eh, &g_cnt_cc);

    /* compute counts: cached, recomputed only when len actually changed
     * (edits are single-character atomic via edit_key, so len always
     * changes on a real edit; pure caret-blink/hover redraws do not). */
    if (g_cnt.len != g_cnt.cache_len) {
        int chars = g_cnt.len, nospace = 0, words = 0, lines = 1, inword = 0;
        for (int i = 0; i < g_cnt.len; i++) {
            char c = g_cnt.buf[i];
            if (c == '\n') lines++;
            if (!is_space((unsigned char)c)) { nospace++; if (!inword) { words++; inword = 1; } }
            else inword = 0;
        }
        if (g_cnt.len == 0) lines = 0;
        g_cnt.cache_chars = chars; g_cnt.cache_nospace = nospace;
        g_cnt.cache_words = words; g_cnt.cache_lines = lines;
        g_cnt.cache_len = g_cnt.len;
    }
    int chars = g_cnt.cache_chars, nospace = g_cnt.cache_nospace;
    int words = g_cnt.cache_words, lines = g_cnt.cache_lines;

    char line[128]; int p = 0;
    scopy(line, "Chars: ", sizeof(line)); p = slen(line);
    put_int(line, &p, (int)sizeof(line), chars);
    scopy(line + p, "   No-spaces: ", (int)sizeof(line) - p); p = slen(line);
    put_int(line, &p, (int)sizeof(line), nospace);
    draw_string_clip(cx + pad, ey + eh + 6 * s, ew, line, FOREB_TEXT, FOREB_BG, 1, 1);

    p = 0; scopy(line, "Words: ", sizeof(line)); p = slen(line);
    put_int(line, &p, (int)sizeof(line), words);
    scopy(line + p, "   Lines: ", (int)sizeof(line) - p); p = slen(line);
    put_int(line, &p, (int)sizeof(line), lines);
    scopy(line + p, "   Bytes: ", (int)sizeof(line) - p); p = slen(line);
    put_int(line, &p, (int)sizeof(line), chars);
    draw_string_clip(cx + pad, ey + eh + 6 * s + 18 * s, ew, line, FOREB_TITLE, FOREB_BG, 1, 1);
}

static int cnt_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        edit_key(g_cnt.buf, CNT_CAP, &g_cnt.len, &g_cnt.cur, ev, 1);
        return 0;
    case WM_EV_MOUSE_WHEEL:
        g_cnt.scroll -= ev->wheel; if (g_cnt.scroll < 0) g_cnt.scroll = 0; return 0;
    case WM_EV_CLOSE: g_cnt.win = 0; return 0;
    default: return 0;
    }
}

void tool_text_count_open(void)
{
    if (g_cnt.win) return;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 56 / 100; if (ww < 440) ww = 440; if (ww > 780) ww = 780; if (ww > W - 40) ww = W - 40;
    int wh = H * 55 / 100; if (wh < 320) wh = 320; if (wh > 560) wh = 560; if (wh > H - 40) wh = H - 40;
    g_cnt.win = wm_open("Text Counter", ww, wh, cnt_draw, cnt_event, &g_cnt);
}

/* ==================================================================
 *  Tool 5: TEXT TRANSFORM (reverse / UPPER / lower / Title)
 * ================================================================== */
#define TR_CAP 512
typedef struct {
    wm_window *win;
    char buf[TR_CAP];
    int  len, cur, scroll;
    int  dirty;                 /* 1 = out_* below stale, recompute on next draw */
    char out_rev[TR_CAP], out_upper[TR_CAP], out_lower[TR_CAP], out_title[TR_CAP];
} tr_state;
static tr_state g_tr;

static void tr_row(const char *label, const char *val, int vl,
                   int x, int y, int w, UINT32 lc)
{
    int s = sc();
    draw_string_clip(x, y, w, label, lc, FOREB_BG, 1, 1);
    draw_wrapped(val, vl, x, y + 16 * s, w, 16 * s, FOREB_TEXT);
}

static void tr_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s;
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad,
                     "Text Transform  -  input:", FOREB_TITLE, FOREB_BG, 1, 1);

    int iy = cy + 26 * s, ih = 16 * s;
    draw_rect_outline(cx + pad - 3, iy - 3, cw - 2 * pad + 6, ih + 6, 1, FOREB_BORDER);
    const char *shown = g_tr.len ? g_tr.buf : "(empty)";
    draw_string_clip(cx + pad, iy, cw - 2 * pad, shown,
                     g_tr.len ? FOREB_TEXT : FOREB_DIM, FOREB_BG, 1, 1);
    if (caret_on()) {
        int caretx = cx + pad + g_tr.cur * 8 * s;
        fill_clip(caretx, iy, 2, ih, FOREB_WHITE, cx + pad, iy, cw - 2 * pad, ih);
    }

    int n = g_tr.len; int w2 = cw - 2 * pad;
    int y = iy + 26 * s;

    /* the four transforms only depend on g_tr.buf, which only changes on a
     * WM_EV_KEY edit (tr_event marks dirty then); idle redraws (caret blink,
     * hover) reuse the cached strings instead of recomputing every frame. */
    if (g_tr.dirty) {
        for (int i = 0; i < n; i++) g_tr.out_rev[i] = g_tr.buf[n - 1 - i]; g_tr.out_rev[n] = 0;
        for (int i = 0; i < n; i++) g_tr.out_upper[i] = (char)ci_upper((unsigned char)g_tr.buf[i]); g_tr.out_upper[n] = 0;
        for (int i = 0; i < n; i++) g_tr.out_lower[i] = (char)ci_lower((unsigned char)g_tr.buf[i]); g_tr.out_lower[n] = 0;
        { int start = 1;
          for (int i = 0; i < n; i++) {
              int c = (unsigned char)g_tr.buf[i];
              if (is_alpha(c)) { g_tr.out_title[i] = (char)(start ? ci_upper(c) : ci_lower(c)); start = 0; }
              else { g_tr.out_title[i] = (char)c; start = 1; }
          }
          g_tr.out_title[n] = 0; }
        g_tr.dirty = 0;
    }

    tr_row("reverse:", g_tr.out_rev, n, cx + pad, y, w2, FOREB_DIM); y += 34 * s;
    tr_row("UPPER:", g_tr.out_upper, n, cx + pad, y, w2, FOREB_DIM); y += 34 * s;
    tr_row("lower:", g_tr.out_lower, n, cx + pad, y, w2, FOREB_DIM); y += 34 * s;
    tr_row("Title:", g_tr.out_title, n, cx + pad, y, w2, FOREB_DIM);
}

static int tr_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        edit_key(g_tr.buf, TR_CAP, &g_tr.len, &g_tr.cur, ev, 0);
        g_tr.dirty = 1;
        return 0;
    }
    if (ev->type == WM_EV_CLOSE) g_tr.win = 0;
    return 0;
}

void tool_text_transform_open(void)
{
    if (g_tr.win) return;
    if (g_tr.len == 0) { scopy(g_tr.buf, "forest boot", TR_CAP); g_tr.len = 11; g_tr.cur = 11; }
    g_tr.dirty = 1;
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 58 / 100; if (ww < 460) ww = 460; if (ww > 820) ww = 820; if (ww > W - 40) ww = W - 40;
    int wh = H * 52 / 100; if (wh < 320) wh = 320; if (wh > 540) wh = 540; if (wh > H - 40) wh = H - 40;
    g_tr.win = wm_open("Text Transform", ww, wh, tr_draw, tr_event, &g_tr);
}

/* ==================================================================
 *  Tool 6: LOREM-IPSUM-ISH GENERATOR (canned words + LCG)
 * ================================================================== */
#define LOR_CAP 2048
enum { LB_GEN = 1, LB_MORE = 2, LB_LESS = 3 };
static const char *const lor_words[] = {
    "lorem","ipsum","dolor","sit","amet","forest","boot","kernel","loader",
    "pixel","render","buffer","frame","window","tool","string","glyph","vector",
    "silent","amber","leaf","branch","root","seed","canopy","spruce","cedar",
    "photon","raster","cursor","widget","module","sector","volume","handle",
    "quantum","lambda","nimbus","zephyr","echo","delta","sigma","umbra","verde"
};
#define LOR_NW ((int)(sizeof(lor_words)/sizeof(lor_words[0])))

typedef struct {
    wm_window *win;
    char buf[LOR_CAP];
    int  len;
    int  count;      /* words requested */
    unsigned seed;
    int  b_hover, b_press;
} lor_state;
static lor_state g_lor;

static unsigned lcg(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static void lor_gen(lor_state *l)
{
    if (l->count < 4) l->count = 4;
    if (l->count > 200) l->count = 200;
    int o = 0; int sent = 0; int cap = 0;   /* words in current sentence */
    int newsent = 1;
    for (int i = 0; i < l->count && o + 20 < LOR_CAP; i++) {
        const char *wd = lor_words[lcg(&l->seed) % LOR_NW];
        if (o > 0 && !newsent) l->buf[o++] = ' ';
        int first = 1;
        for (const char *p = wd; *p && o + 2 < LOR_CAP; p++) {
            int c = *p;
            if (first && newsent) c = ci_upper(c);
            l->buf[o++] = (char)c; first = 0;
        }
        newsent = 0;
        cap++;
        /* end a sentence every 6-13 words (guarded: leave room for the NUL) */
        if (cap >= 6 + (int)(lcg(&l->seed) % 8) && o + 2 < LOR_CAP) {
            l->buf[o++] = '.'; l->buf[o++] = ' ';
            cap = 0; newsent = 1; sent++;
        }
    }
    if (o > 0 && l->buf[o - 1] != ' ' && l->buf[o - 1] != '.' && o + 1 < LOR_CAP)
        l->buf[o++] = '.';
    l->buf[o] = 0; l->len = o;
    (void)sent;
}

static const char *const lor_labels[] = { "Generate", "More", "Less" };
static const int         lor_ids[]    = { LB_GEN, LB_MORE, LB_LESS };

static void lor_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s;
    char hdr[64]; scopy(hdr, "Lorem Generator  -  words: ", sizeof(hdr));
    int hp = slen(hdr); put_int(hdr, &hp, (int)sizeof(hdr), g_lor.count);
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad, hdr, FOREB_TITLE, FOREB_BG, 1, 1);
    draw_string_clip(cx + pad, cy + 24 * s, cw - 2 * pad,
                     "Enter/Generate = new; +/- or More/Less change count",
                     FOREB_DIM, FOREB_BG, 1, 1);

    int oy = cy + 44 * s;
    int oh = (cy + content_h(ch)) - oy - 4 * s;
    draw_wrapped(g_lor.buf, g_lor.len, cx + pad, oy, cw - 2 * pad, oh, FOREB_TEXT);

    wm_button b[3]; int nb = bar_layout(ch, lor_labels, lor_ids, 3, b);
    bar_draw(cx, cy, cw, ch, b, nb, g_lor.b_hover, g_lor.b_press);
}

static int lor_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    lor_state *l = &g_lor;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == CHAR_CR || ev->unicode == CHAR_LINEFEED || ev->unicode == ' ')
            { lor_gen(l); return 0; }
        if (ev->unicode == '+' || ev->unicode == '=' || ev->scancode == SCAN_UP)
            { l->count += 5; lor_gen(l); return 0; }
        if (ev->unicode == '-' || ev->unicode == '_' || ev->scancode == SCAN_DOWN)
            { l->count -= 5; lor_gen(l); return 0; }
        return 0;
    case WM_EV_MOUSE_MOVE: {
        wm_button b[3]; int nb = bar_layout(wm_client_h(w), lor_labels, lor_ids, 3, b);
        l->b_hover = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_DOWN: {
        wm_button b[3]; int nb = bar_layout(wm_client_h(w), lor_labels, lor_ids, 3, b);
        l->b_press = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_UP: {
        if (!l->b_press) return 0;
        wm_button b[3]; int nb = bar_layout(wm_client_h(w), lor_labels, lor_ids, 3, b);
        int id = bar_hit(b, nb, ev->mx, ev->my), p = l->b_press; l->b_press = 0;
        if (id == p) {
            if (p == LB_GEN)  lor_gen(l);
            if (p == LB_MORE) { l->count += 5; lor_gen(l); }
            if (p == LB_LESS) { l->count -= 5; lor_gen(l); }
        }
        return 0; }
    case WM_EV_CLOSE: l->win = 0; return 0;
    default: return 0;
    }
}

void tool_text_lorem_open(void)
{
    if (g_lor.win) return;
    g_lor.b_hover = g_lor.b_press = 0;
    if (g_lor.count == 0) g_lor.count = 40;
    if (g_lor.seed == 0) {
        unsigned long long t = 0;
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ __volatile__("rdtsc" : "=A"(t));
#else
        t = 0x9E3779B97F4A7C15ull;
#endif
        g_lor.seed = (unsigned)t ^ 0x1234abcdu;
        if (g_lor.seed == 0) g_lor.seed = 0xC0FFEEu;
    }
    lor_gen(&g_lor);
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 58 / 100; if (ww < 460) ww = 460; if (ww > 820) ww = 820; if (ww > W - 40) ww = W - 40;
    int wh = H * 55 / 100; if (wh < 320) wh = 320; if (wh > 560) wh = 560; if (wh > H - 40) wh = H - 40;
    g_lor.win = wm_open("Lorem Generator", ww, wh, lor_draw, lor_event, &g_lor);
}

/* ==================================================================
 *  Tool 7: MORSE ENCODE / DECODE
 * ================================================================== */
#define MOR_CAP 256
enum { MOB_MODE = 1 };
/* index 0-25 A-Z, 26-35 0-9 */
static const char *const morse_tab[36] = {
    ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---",
    "-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-",
    "..-","...-",".--","-..-","-.--","--..",
    "-----",".----","..---","...--","....-",".....","-....","--...","---..","----."
};
typedef struct {
    wm_window *win;
    char buf[MOR_CAP];
    int  len, cur;
    int  mode;   /* 0 = encode text->morse, 1 = decode morse->text */
    int  b_hover, b_press;
} mor_state;
static mor_state g_mor;

static const char *morse_for(int c)
{
    c = ci_upper(c);
    if (c >= 'A' && c <= 'Z') return morse_tab[c - 'A'];
    if (c >= '0' && c <= '9') return morse_tab[26 + (c - '0')];
    return 0;
}
static int morse_lookup(const char *tok, int tl)
{
    for (int i = 0; i < 36; i++) {
        const char *m = morse_tab[i]; int j = 0;
        while (j < tl && m[j] && tok[j] == m[j]) j++;
        if (j == tl && m[j] == 0) return i < 26 ? ('A' + i) : ('0' + (i - 26));
    }
    return '?';
}

static int mor_build(const mor_state *m, char *out, int cap)
{
    int o = 0;
    if (m->mode == 0) {                      /* text -> morse */
        for (int i = 0; i < m->len && o + 8 < cap; i++) {
            int c = (unsigned char)m->buf[i];
            if (c == ' ') { out[o++] = '/'; out[o++] = ' '; continue; }
            const char *ms = morse_for(c);
            if (!ms) continue;
            for (const char *p = ms; *p && o + 1 < cap; p++) out[o++] = *p;
            out[o++] = ' ';
        }
    } else {                                 /* morse -> text */
        char tok[8]; int tl = 0;
        /* o + 2: the '/' branch appends two chars, plus room for the NUL */
        for (int i = 0; i <= m->len && o + 2 < cap; i++) {
            int c = (i < m->len) ? (unsigned char)m->buf[i] : ' ';
            if (c == '.' || c == '-') { if (tl < 7) tok[tl++] = (char)c; }
            else if (c == '/') { if (tl) { out[o++] = (char)morse_lookup(tok, tl); tl = 0; }
                                 out[o++] = ' '; }
            else { /* space / separator ends a letter */
                if (tl) { out[o++] = (char)morse_lookup(tok, tl); tl = 0; }
            }
        }
    }
    out[o] = 0;
    return o;
}

static const char *const mor_labels0[] = { "Mode: Text->Morse" };
static const char *const mor_labels1[] = { "Mode: Morse->Text" };
static const int         mor_ids[]     = { MOB_MODE };

static void mor_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s;
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad,
        g_mor.mode == 0 ? "Morse Encode  -  input text:"
                        : "Morse Decode  -  input  . - / :",
        FOREB_TITLE, FOREB_BG, 1, 1);

    int iy = cy + 26 * s, ih = 16 * s;
    draw_rect_outline(cx + pad - 3, iy - 3, cw - 2 * pad + 6, ih + 6, 1, FOREB_BORDER);
    const char *shown = g_mor.len ? g_mor.buf : "(empty)";
    draw_string_clip(cx + pad, iy, cw - 2 * pad, shown,
                     g_mor.len ? FOREB_TEXT : FOREB_DIM, FOREB_BG, 1, 1);
    if (caret_on()) {
        int caretx = cx + pad + g_mor.cur * 8 * s;
        fill_clip(caretx, iy, 2, ih, FOREB_WHITE, cx + pad, iy, cw - 2 * pad, ih);
    }

    draw_string_clip(cx + pad, iy + 24 * s, cw - 2 * pad, "Output:",
                     FOREB_DIM, FOREB_BG, 1, 1);
    char out[MOR_CAP * 6 + 8];
    int ol = mor_build(&g_mor, out, (int)sizeof(out));
    int oy = iy + 42 * s;
    int oh = (cy + content_h(ch)) - oy - 4 * s;
    draw_wrapped(out, ol, cx + pad, oy, cw - 2 * pad, oh, FOREB_TITLE);

    wm_button b[1]; int nb = bar_layout(ch, g_mor.mode ? mor_labels1 : mor_labels0, mor_ids, 1, b);
    bar_draw(cx, cy, cw, ch, b, nb, g_mor.b_hover, g_mor.b_press);
}

static int mor_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    mor_state *m = &g_mor;
    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->unicode == CHAR_TAB) { m->mode = !m->mode; return 0; }
        edit_key(m->buf, MOR_CAP, &m->len, &m->cur, ev, 0);
        return 0;
    case WM_EV_MOUSE_MOVE: {
        wm_button b[1]; int nb = bar_layout(wm_client_h(w), m->mode?mor_labels1:mor_labels0, mor_ids, 1, b);
        m->b_hover = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_DOWN: {
        wm_button b[1]; int nb = bar_layout(wm_client_h(w), m->mode?mor_labels1:mor_labels0, mor_ids, 1, b);
        m->b_press = bar_hit(b, nb, ev->mx, ev->my); return 0; }
    case WM_EV_MOUSE_UP: {
        if (!m->b_press) return 0;
        wm_button b[1]; int nb = bar_layout(wm_client_h(w), m->mode?mor_labels1:mor_labels0, mor_ids, 1, b);
        int id = bar_hit(b, nb, ev->mx, ev->my), p = m->b_press; m->b_press = 0;
        if (id == p && p == MOB_MODE) m->mode = !m->mode;
        return 0; }
    case WM_EV_CLOSE: m->win = 0; return 0;
    default: return 0;
    }
}

void tool_text_morse_open(void)
{
    if (g_mor.win) return;
    g_mor.b_hover = g_mor.b_press = 0;
    if (g_mor.len == 0) { scopy(g_mor.buf, "SOS FOREB", MOR_CAP); g_mor.len = 9; g_mor.cur = 9; }
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 58 / 100; if (ww < 460) ww = 460; if (ww > 820) ww = 820; if (ww > W - 40) ww = W - 40;
    int wh = H * 52 / 100; if (wh < 320) wh = 320; if (wh > 540) wh = 540; if (wh > H - 40) wh = H - 40;
    g_mor.win = wm_open("Morse Code", ww, wh, mor_draw, mor_event, &g_mor);
}

/* ==================================================================
 *  Tool 8: FIND + HIGHLIGHT in a sample text
 * ================================================================== */
#define FND_QCAP 48
static const char g_sample[] =
    "The Forest Bootloader boots your operating system quickly and quietly. "
    "A boot loader is the first program that runs when a computer starts. "
    "It finds a kernel, loads it into memory, and hands over control. "
    "ForeB draws its menu straight to the graphics framebuffer, so the boot "
    "screen looks the same before and after ExitBootServices. Type a word "
    "in the box above to find and highlight every match in this sample text. "
    "Searching is case-insensitive: try boot, forest, kernel, or the.";

typedef struct {
    wm_window *win;
    char q[FND_QCAP];
    int  qlen, qcur;
} fnd_state;
static fnd_state g_fnd;

static void fnd_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; g_blink++;
    fill_rect(cx, cy, cw, ch, FOREB_BG);
    int s = sc(), pad = 10 * s, CW = 8 * s, LH = 16 * s;

    /* query field */
    draw_string_clip(cx + pad, cy + 6 * s, cw - 2 * pad,
                     "Find + Highlight  -  search:", FOREB_TITLE, FOREB_BG, 1, 1);
    int iy = cy + 26 * s, ih = 16 * s;
    draw_rect_outline(cx + pad - 3, iy - 3, cw - 2 * pad + 6, ih + 6, 1, FOREB_BORDER);
    const char *shown = g_fnd.qlen ? g_fnd.q : "(type to search)";
    draw_string_clip(cx + pad, iy, cw - 2 * pad, shown,
                     g_fnd.qlen ? FOREB_TEXT : FOREB_DIM, FOREB_BG, 1, 1);
    if (caret_on()) {
        int caretx = cx + pad + g_fnd.qcur * CW;
        fill_clip(caretx, iy, 2, ih, FOREB_WHITE, cx + pad, iy, cw - 2 * pad, ih);
    }

    /* build highlight mask */
    int sl = (int)(sizeof(g_sample) - 1);
    static unsigned char hi[sizeof(g_sample)];
    for (int i = 0; i < sl; i++) hi[i] = 0;
    int matches = 0, ql = g_fnd.qlen;
    if (ql > 0) {
        for (int i = 0; i + ql <= sl; i++) {
            int j = 0;
            for (; j < ql; j++)
                if (ci_lower((unsigned char)g_sample[i + j]) != ci_lower((unsigned char)g_fnd.q[j]))
                    break;
            if (j == ql) { matches++; for (int k = 0; k < ql; k++) hi[i + k] = 1; }
        }
    }

    /* count line */
    char cl[48]; int p = 0; scopy(cl, "Matches: ", sizeof(cl)); p = slen(cl);
    put_int(cl, &p, (int)sizeof(cl), matches);
    draw_string_clip(cx + pad, iy + 22 * s, cw - 2 * pad, cl,
                     matches ? FOREB_TITLE : FOREB_DIM, FOREB_BG, 1, 1);

    /* render sample with word-wrap + per-char highlight */
    int tx = cx + pad, ty = iy + 44 * s;
    int right = cx + cw - pad, bottom = cy + ch - 4 * s;
    int x = tx, y = ty;
    int i = 0;
    while (i < sl && y + LH <= bottom) {
        /* measure next word length (to wrap on word boundary) */
        int wl = 0; while (i + wl < sl && g_sample[i + wl] != ' ') wl++;
        if (x + wl * CW > right && x > tx) { x = tx; y += LH; if (y + LH > bottom) break; }
        for (int k = 0; k < wl && i < sl; k++, i++) {
            if (x + CW > right) { x = tx; y += LH; if (y + LH > bottom) { i = sl; break; } }
            char c = g_sample[i];
            if (hi[i]) fill_clip(x, y, CW, LH, FOREB_SELECT, tx, ty, right - tx, bottom - ty);
            draw_char(x, y, c, hi[i] ? FOREB_WHITE : FOREB_TEXT, FOREB_BG, 1, s);
            x += CW;
        }
        /* the space */
        if (i < sl && g_sample[i] == ' ') {
            if (x + CW > right) { x = tx; y += LH; }
            else x += CW;
            i++;
        }
    }
}

static int fnd_event(wm_window *w, const wm_event *ev)
{
    (void)w; if (!ev) return 0;
    if (ev->type == WM_EV_KEY) {
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        edit_key(g_fnd.q, FND_QCAP, &g_fnd.qlen, &g_fnd.qcur, ev, 0);
        return 0;
    }
    if (ev->type == WM_EV_CLOSE) g_fnd.win = 0;
    return 0;
}

void tool_text_find_open(void)
{
    if (g_fnd.win) return;
    if (g_fnd.qlen == 0) { scopy(g_fnd.q, "boot", FND_QCAP); g_fnd.qlen = 4; g_fnd.qcur = 4; }
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 60 / 100; if (ww < 480) ww = 480; if (ww > 840) ww = 840; if (ww > W - 40) ww = W - 40;
    int wh = H * 56 / 100; if (wh < 340) wh = 340; if (wh > 580) wh = 580; if (wh > H - 40) wh = H - 40;
    g_fnd.win = wm_open("Find + Highlight", ww, wh, fnd_draw, fnd_event, &g_fnd);
}

/* ==================================================================
 *  Category registry.
 * ================================================================== */
const struct forebo_tool cat_text_tools[] = {
    { "Notepad",        "Edit text; save to ESP \\forebo\\notes",          "text",     tool_text_notepad_open   },
    { "ASCII Banner",   "Big block-letter banner from an 8-wide font",     "text",     tool_text_banner_open    },
    { "Hex <-> Text",   "Encode text to hex bytes and decode back",        "terminal", tool_text_hex_open       },
    { "Text Counter",   "Count chars, words, lines and bytes",             "text",     tool_text_count_open     },
    { "Text Transform", "reverse / UPPER / lower / Title case",            "text",     tool_text_transform_open },
    { "Lorem Generator","Canned-word filler text via an LCG",              "text",     tool_text_lorem_open     },
    { "Morse Code",     "Encode text to Morse and decode it back",         "terminal", tool_text_morse_open     },
    { "Find + Highlight","Search + highlight matches in sample text",       "text",     tool_text_find_open      },
};
const int cat_text_count = (int)(sizeof(cat_text_tools) / sizeof(cat_text_tools[0]));
