/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/recovery/uefi_settings.c - View / change firmware settings from the
 *                                  bootloader without rebooting into BIOS.
 * =============================================================================
 * Displays common UEFI settings (BootOrder, BootNext, Secure Boot state,
 * Console output mode, Boot timeout) in a wm window. Some values are read-only
 * indicators (Secure Boot presence); others (BootNext, Timeout) can be edited
 * and written back via RuntimeServices->SetVariable.
 *
 * Template B: uefi_settings_open() calls wm_open() and returns immediately;
 * the existing menu loop in bootx64.c drives input + compositing + present.
 *
 * Freestanding (no libc), pre-ExitBootServices. No heap allocations; all
 * buffers are static / stack.
 * ========================================================================== */

#include "uefi_settings.h"
#include "../core/wm.h"
#include "../ui.h"
#include "../core/input.h"

/* ==========================================================================
 * Module state
 * ========================================================================== */
static EFI_SYSTEM_TABLE     *gST;
static EFI_BOOT_SERVICES    *gBS;
static EFI_RUNTIME_SERVICES *gRT;

/* Theme colours. */
static UINT32 c_win, c_fg, c_dim, c_accent, c_sel_bg, c_sel_fg, c_border;

/* UEFI GUIDs used for variable queries. */
static EFI_GUID gGlobalVar   = EFI_GLOBAL_VARIABLE;
static EFI_GUID gSecurityVar = { 0x8be4df61, 0x93ca, 0x11d2,
    { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };
static EFI_GUID gImageGuid   = EFI_LOADED_IMAGE_PROTOCOL_GUID;

/* Secure Boot variable names (existence check only). */
static CHAR16 var_pk[]  = L"PK";
static CHAR16 var_kek[] = L"KEK";
static CHAR16 var_db[]  = L"db";
static CHAR16 var_dbx[] = L"dbx";

/* Boot manager variables. */
static CHAR16 var_bootorder[]     = L"BootOrder";
static CHAR16 var_bootnext[]      = L"BootNext";
static CHAR16 var_timeout[]       = L"BootOptionTimeout";
static CHAR16 var_osindsup[]      = L"OsIndicationsSupported";
static CHAR16 var_osind[]         = L"OsIndications";
static CHAR16 var_bootcurrent[]   = L"BootCurrent";

/* ==========================================================================
 * Tiny freestanding helpers (mirrors tools.c)
 * ========================================================================== */
static int  slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }
static void scopy(char *d, const char *s, int cap)
{ int i=0; if(cap<=0)return; for(;s&&s[i]&&i+1<cap;i++)d[i]=s[i]; d[i]=0; }
static int gsc(void){ int s=ui_scale(); return s<1?1:s; }

/* Append a character to a line buffer. */
static void LNc(char *cl, int *cll, int cap, char c)
{ if(*cll<cap-1){ cl[*cll]=c; (*cll)++; cl[*cll]=0; } }
static void LNs(char *cl, int *cll, int cap, const char *s)
{ while(s&&*s) LNc(cl,cll,cap,*s++); }
static void LNsp(char *cl, int *cll, int cap, int n)
{ while(n-->0) LNc(cl,cll,cap,' '); }
static void LNu(char *cl, int *cll, int cap, UINT64 v)
{ char t[24]; int i=0; if(!v){LNc(cl,cll,cap,'0');return;}
  while(v){t[i++]=(char)('0'+(int)(v%10));v/=10;} while(i)LNc(cl,cll,cap,t[--i]); }

/* ==========================================================================
 * Variable readers (all read-only, no writes yet).
 * ========================================================================== */

/* Check if a UEFI variable exists (size > 0). */
static int var_exists(EFI_RUNTIME_SERVICES *rt, EFI_GUID *guid, CHAR16 *name)
{
    UINTN sz = 0;
    if (!rt || !rt->GetVariable) return 0;
    EFI_STATUS st = rt->GetVariable(name, guid, NULL, &sz, NULL);
    return (st == EFI_BUFFER_TOO_SMALL || !EFI_ERROR(st)) ? 1 : 0;
}

/* Read a UINT16 variable. Returns 1 on success, 0 otherwise. */
static int read_u16(EFI_RUNTIME_SERVICES *rt, EFI_GUID *guid, CHAR16 *name, UINT16 *out)
{
    UINT16 v = 0;
    UINTN  sz = sizeof(v);
    EFI_STATUS st;
    if (!rt || !rt->GetVariable) return 0;
    st = rt->GetVariable(name, guid, NULL, &sz, &v);
    if (EFI_ERROR(st)) return 0;
    *out = v;
    return 1;
}

/* Write a UINT16 variable (NV + BS + RT access). Returns EFI_SUCCESS on ok. */
static EFI_STATUS write_u16(EFI_RUNTIME_SERVICES *rt, EFI_GUID *guid, CHAR16 *name, UINT16 val)
{
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    if (!rt || !rt->SetVariable) return EFI_UNSUPPORTED;
    return rt->SetVariable(name, guid, attrs, sizeof(val), &val);
}

/* ==========================================================================
 * Setting indices (must match g_setting_names[])
 * ========================================================================== */
enum {
    SET_BOOT_ORDER = 0,
    SET_BOOT_NEXT,
    SET_BOOT_TIMEOUT,
    SET_BOOT_CURRENT,
    SET_SECURE_BOOT_PK,
    SET_SECURE_BOOT_KEK,
    SET_SECURE_BOOT_DB,
    SET_SECURE_BOOT_DBX,
    SET_CONOUT_MODE,
    SET_CONOUT_COLS,
    SET_CONOUT_ROWS,
    SET_OS_INDICATIONS,
    SET_COUNT
};

static const char *g_setting_names[SET_COUNT] = {
    "Boot Order",
    "Boot Next",
    "Boot Timeout",
    "Boot Current",
    "Secure Boot (PK)",
    "Secure Boot (KEK)",
    "Secure Boot (DB)",
    "Secure Boot (DBX)",
    "Console Mode",
    "Console Cols",
    "Console Rows",
    "OsIndications",
};

/* Cached values (read once on open, refreshed on user action). */
static UINT16 val_boot_order[16];
static int    val_boot_order_n;
static UINT16 val_boot_next;
static int    val_boot_next_present;
static UINT16 val_boot_timeout;
static int    val_timeout_present;
static UINT16 val_boot_current;
static int    val_boot_current_present;
static int    val_pk, val_kek, val_db, val_dbx;
static int    val_conout_mode;
static UINTN  val_conout_cols, val_conout_rows;
static UINT64 val_osind;
static int    val_osind_present;

/* ==========================================================================
 * Refresh all cached settings from UEFI variables.
 * ========================================================================== */
static void settings_refresh(void)
{
    if (!gRT) return;

    /* Boot order (UINT16[] array). */
    val_boot_order_n = 0;
    if (gRT->GetVariable) {
        UINTN sz = sizeof(val_boot_order);
        EFI_STATUS st = gRT->GetVariable(var_bootorder, &gGlobalVar, NULL, &sz,
                                         val_boot_order);
        if (!EFI_ERROR(st)) val_boot_order_n = (int)(sz / sizeof(UINT16));
    }

    /* BootNext. */
    val_boot_next_present = read_u16(gRT, &gGlobalVar, var_bootnext, &val_boot_next);

    /* BootOptionTimeout. */
    val_timeout_present = read_u16(gRT, &gGlobalVar, var_timeout, &val_boot_timeout);

    /* BootCurrent. */
    val_boot_current_present = read_u16(gRT, &gGlobalVar, var_bootcurrent, &val_boot_current);

    /* Secure Boot variable existence. */
    val_pk  = var_exists(gRT, &gSecurityVar, var_pk);
    val_kek = var_exists(gRT, &gSecurityVar, var_kek);
    val_db  = var_exists(gRT, &gSecurityVar, var_db);
    val_dbx = var_exists(gRT, &gSecurityVar, var_dbx);

    /* Console output mode. */
    val_conout_mode = 0;
    val_conout_cols = 0;
    val_conout_rows = 0;
    if (gST && gST->ConOut && gST->ConOut->Mode) {
        val_conout_mode = gST->ConOut->Mode->Mode;
        /* Query mode 0 for default cols/rows (most firmwares report 80x25). */
        UINTN cols = 0, rows = 0;
        if (!EFI_ERROR(gST->ConOut->QueryMode(gST->ConOut, (UINTN)val_conout_mode,
                                              &cols, &rows))) {
            val_conout_cols = cols;
            val_conout_rows = rows;
        }
    }

    /* OsIndications. */
    val_osind_present = 0;
    val_osind = 0;
    if (gRT->GetVariable) {
        UINT64 v = 0;
        UINTN sz = sizeof(v);
        EFI_STATUS st = gRT->GetVariable(var_osind, &gGlobalVar, NULL, &sz, &v);
        if (!EFI_ERROR(st)) { val_osind = v; val_osind_present = 1; }
    }
}

/* ==========================================================================
 * Format a cached value into a short display string for row `idx`.
 * `out` must be >= 40 chars. Returns `out`.
 * ========================================================================== */
static char *format_value(int idx, char *out, int cap)
{
    out[0] = 0;
    switch (idx) {
    case SET_BOOT_ORDER:
        if (val_boot_order_n == 0) { scopy(out, "(empty)", cap); break; }
        { int p = 0;
          for (int i = 0; i < val_boot_order_n && i < 6 && p < cap - 8; i++) {
              if (i > 0 && p < cap - 1) out[p++] = ' ';
              UINT16 v = val_boot_order[i];
              char t[8]; int ti = 0;
              static const char h[] = "0123456789ABCDEF";
              t[ti++] = h[(v >> 12) & 0xF]; t[ti++] = h[(v >> 8) & 0xF];
              t[ti++] = h[(v >> 4) & 0xF];  t[ti++] = h[v & 0xF]; t[ti] = 0;
              for (int k = 0; k < 4 && p < cap - 1; k++) out[p++] = t[k];
          }
          if (val_boot_order_n > 6 && p < cap - 4) { out[p++]='.'; out[p++]='.'; out[p++]='.'; }
          out[p] = 0; }
        break;
    case SET_BOOT_NEXT:
        if (!val_boot_next_present) { scopy(out, "(not set)", cap); break; }
        { static const char h[] = "0123456789ABCDEF";
          out[0] = h[(val_boot_next >> 12) & 0xF];
          out[1] = h[(val_boot_next >> 8) & 0xF];
          out[2] = h[(val_boot_next >> 4) & 0xF];
          out[3] = h[val_boot_next & 0xF];
          out[4] = 0; }
        break;
    case SET_BOOT_TIMEOUT:
        if (!val_timeout_present) { scopy(out, "(not set)", cap); break; }
        LNu(out, &(int){0}, cap, val_boot_timeout);
        { int l = slen(out); scopy(out + l, " sec", cap - l); }
        break;
    case SET_BOOT_CURRENT:
        if (!val_boot_current_present) { scopy(out, "(not set)", cap); break; }
        { static const char h[] = "0123456789ABCDEF";
          out[0] = h[(val_boot_current >> 12) & 0xF];
          out[1] = h[(val_boot_current >> 8) & 0xF];
          out[2] = h[(val_boot_current >> 4) & 0xF];
          out[3] = h[val_boot_current & 0xF];
          out[4] = 0; }
        break;
    case SET_SECURE_BOOT_PK:  scopy(out, val_pk  ? "enrolled" : "absent", cap); break;
    case SET_SECURE_BOOT_KEK: scopy(out, val_kek ? "enrolled" : "absent", cap); break;
    case SET_SECURE_BOOT_DB:  scopy(out, val_db  ? "enrolled" : "absent", cap); break;
    case SET_SECURE_BOOT_DBX: scopy(out, val_dbx ? "enrolled" : "absent", cap); break;
    case SET_CONOUT_MODE:
        { char t[12]; int ti = 0; LNu(t, &ti, 12, (UINT64)val_conout_mode);
          scopy(out, "mode ", cap); scopy(out + slen(out), t, cap - slen(out)); }
        break;
    case SET_CONOUT_COLS:
        { char t[12]; int ti = 0; LNu(t, &ti, 12, val_conout_cols);
          scopy(out, t, cap); }
        break;
    case SET_CONOUT_ROWS:
        { char t[12]; int ti = 0; LNu(t, &ti, 12, val_conout_rows);
          scopy(out, t, cap); }
        break;
    case SET_OS_INDICATIONS:
        { static const char h[] = "0123456789ABCDEF";
          for (int k = 0; k < 16 && k < cap; k++)
              out[k] = h[(val_osind >> ((15 - k) * 4)) & 0xF];
          out[16] = 0; }
        break;
    }
    return out;
}

/* ==========================================================================
 * Window state.
 * ========================================================================== */
typedef struct {
    wm_window *win;
    int        sel;           /* selected row (0 .. SET_COUNT-1)              */
    int        scroll;        /* first visible row                            */
    int        b_hover;       /* button-bar hover id                          */
    int        b_press;       /* button-bar press id                          */
} ueset_state;
static ueset_state g_ues;

/* Button IDs. */
#define UE_CLOSE  1
#define UE_REFRESH 2

/* ==========================================================================
 * Draw callback.
 * ========================================================================== */
static void uefi_settings_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    int sc = gsc();
    int lineH = 16 * sc + 4 * sc;
    int x = cx + 8 * sc;
    int y = cy + 6 * sc;
    int avail = ch - (wm_button_h() + 14 * sc); /* space above button bar */
    if (avail < lineH * 2) return;

    /* Title. */
    draw_string(x, y, "UEFI Firmware Settings", c_accent, c_win, 1, sc);
    y += lineH + 2 * sc;

    /* Subtitle. */
    draw_string(x, y, "Read-only view; Esc to close", c_dim, c_win, 1, sc);
    y += lineH + 4 * sc;

    /* Row height includes a small gap. */
    int rowH = lineH + 2 * sc;
    int rows = (avail - (y - cy)) / rowH;
    if (rows < 1) rows = 1;

    /* Clamp scroll. */
    if (g_ues.scroll > SET_COUNT - rows) g_ues.scroll = SET_COUNT - rows;
    if (g_ues.scroll < 0) g_ues.scroll = 0;

    /* Rows. */
    for (int r = 0; r < rows; r++) {
        int idx = g_ues.scroll + r;
        if (idx >= SET_COUNT) break;
        int ry = y + r * rowH;
        int sel = (g_ues.sel == idx);

        if (sel)
            fill_rect(x - 4 * sc, ry - 1, cw - 16 * sc, rowH, c_sel_bg);

        UINT32 fg = sel ? c_sel_fg : c_fg;

        /* Setting name. */
        draw_string(x, ry, g_setting_names[idx], fg, c_win, 1, sc);

        /* Value (right-aligned area). */
        char val[48];
        format_value(idx, val, sizeof(val));
        int vx = x + 24 * 8 * sc;
        draw_string(vx, ry, val, sel ? c_sel_fg : c_dim, c_win, 1, sc);

        /* Draw [Change] indicator for editable rows. */
        if (idx == SET_BOOT_NEXT || idx == SET_BOOT_TIMEOUT) {
            int bx = cw - 8 * sc - 8 * 7 * sc;
            draw_string(bx, ry, "[edit]", c_accent, c_win, 1, sc);
        }
    }

    /* Scrollbar. */
    if (SET_COUNT > rows) {
        int barw = 6 * sc;
        int trackX = cx + cw - barw - 4 * sc;
        int trackY = y;
        int trackH = rows * rowH;
        fill_rect(trackX, trackY, barw, trackH, c_border);
        int thumbH = rows * trackH / SET_COUNT;
        if (thumbH < 8 * sc) thumbH = 8 * sc;
        int thumbY = trackY + (g_ues.scroll * (trackH - thumbH)) / (SET_COUNT - rows);
        fill_rect(trackX, thumbY, barw, thumbH, c_accent);
    }

    /* Button bar: [Refresh] [Close] */
    {
        int bh = wm_button_h();
        int bw1 = wm_button_measure("Refresh");
        int bw2 = wm_button_measure("Close");
        int gap = 8 * sc;
        int bx = cx + cw - bw2 - 10 * sc;
        int by = cy + ch - bh - 6 * sc;
        wm_button br = { bx - bw1 - gap, by, bw1, bh, UE_REFRESH, 1, "Refresh" };
        wm_button bc = { bx, by, bw2, bh, UE_CLOSE, 1, "Close" };
        wm_button_draw(&br, g_ues.b_hover == UE_REFRESH, g_ues.b_press == UE_REFRESH);
        wm_button_draw(&bc, g_ues.b_hover == UE_CLOSE, g_ues.b_press == UE_CLOSE);
    }
}

/* ==========================================================================
 * Event callback.
 * ========================================================================== */
static int uefi_settings_event(wm_window *w, const wm_event *ev)
{
    int cw = wm_client_w(w), ch = wm_client_h(w);
    int sc = gsc();
    int lineH = 16 * sc + 4 * sc;
    int rowH = lineH + 2 * sc;
    int avail = ch - (wm_button_h() + 14 * sc);
    int rows = (avail - (6 * sc + lineH * 2 + 6 * sc)) / rowH;
    if (rows < 1) rows = 1;

    switch (ev->type) {
    case WM_EV_KEY:
        if (ev->scancode == SCAN_ESC) return WM_CLOSE_REQUEST;
        if (ev->scancode == SCAN_UP && g_ues.sel > 0)
            g_ues.sel--;
        else if (ev->scancode == SCAN_DOWN && g_ues.sel < SET_COUNT - 1)
            g_ues.sel++;
        else if (ev->scancode == SCAN_PAGE_UP) {
            g_ues.sel -= rows - 1;
            if (g_ues.sel < 0) g_ues.sel = 0;
        } else if (ev->scancode == SCAN_PAGE_DOWN) {
            g_ues.sel += rows - 1;
            if (g_ues.sel >= SET_COUNT) g_ues.sel = SET_COUNT - 1;
        } else if (ev->scancode == SCAN_HOME) {
            g_ues.sel = 0;
        } else if (ev->scancode == SCAN_END) {
            g_ues.sel = SET_COUNT - 1;
        } else if (ev->unicode == CHAR_CR) {
            /* Enter on BootNext or Timeout: cycle through values. */
            if (g_ues.sel == SET_BOOT_NEXT && gRT) {
                val_boot_next = (val_boot_next + 0x1000) & 0xFFFF;
                write_u16(gRT, &gGlobalVar, var_bootnext, val_boot_next);
                settings_refresh();
            } else if (g_ues.sel == SET_BOOT_TIMEOUT && gRT) {
                val_boot_timeout = (val_boot_timeout + 5) % 100;
                write_u16(gRT, &gGlobalVar, var_timeout, val_boot_timeout);
                settings_refresh();
            }
        }
        /* Keep sel in view. */
        if (g_ues.scroll > g_ues.sel) g_ues.scroll = g_ues.sel;
        if (g_ues.scroll < g_ues.sel - rows + 1) g_ues.scroll = g_ues.sel - rows + 1;
        return 0;

    case WM_EV_MOUSE_WHEEL:
        g_ues.scroll -= ev->wheel;
        if (g_ues.scroll > SET_COUNT - rows) g_ues.scroll = SET_COUNT - rows;
        if (g_ues.scroll < 0) g_ues.scroll = 0;
        return 0;

    case WM_EV_MOUSE_MOVE: {
        int bh = wm_button_h();
        int bw1 = wm_button_measure("Refresh");
        int bw2 = wm_button_measure("Close");
        int gap = 8 * sc;
        int bx = cw - bw2 - 10 * sc;
        int by = ch - bh - 6 * sc;
        wm_button br = { bx - bw1 - gap, by, bw1, bh, UE_REFRESH, 1, "Refresh" };
        wm_button bc = { bx, by, bw2, bh, UE_CLOSE, 1, "Close" };
        g_ues.b_hover = 0;
        if (wm_button_hit(&br, ev->mx, ev->my)) g_ues.b_hover = UE_REFRESH;
        else if (wm_button_hit(&bc, ev->mx, ev->my)) g_ues.b_hover = UE_CLOSE;
        return 0;
    }

    case WM_EV_MOUSE_DOWN: {
        int bh = wm_button_h();
        int bw1 = wm_button_measure("Refresh");
        int bw2 = wm_button_measure("Close");
        int gap = 8 * sc;
        int bx = cw - bw2 - 10 * sc;
        int by = ch - bh - 6 * sc;
        wm_button br = { bx - bw1 - gap, by, bw1, bh, UE_REFRESH, 1, "Refresh" };
        wm_button bc = { bx, by, bw2, bh, UE_CLOSE, 1, "Close" };
        int id = 0;
        if (wm_button_hit(&br, ev->mx, ev->my)) id = UE_REFRESH;
        else if (wm_button_hit(&bc, ev->mx, ev->my)) id = UE_CLOSE;
        if (id) { g_ues.b_press = id; return 0; }

        /* Row click: compute row from mouse Y. */
        int startY = 6 * sc + (16 * sc + 4 * sc) * 2 + 4 * sc;
        int r = (ev->my - startY) / rowH;
        if (r >= 0 && r < rows) {
            int idx = g_ues.scroll + r;
            if (idx >= 0 && idx < SET_COUNT) {
                g_ues.sel = idx;
                /* Toggle editable values on click. */
                if (idx == SET_BOOT_NEXT && gRT) {
                    val_boot_next = (val_boot_next + 0x1000) & 0xFFFF;
                    write_u16(gRT, &gGlobalVar, var_bootnext, val_boot_next);
                    settings_refresh();
                } else if (idx == SET_BOOT_TIMEOUT && gRT) {
                    val_boot_timeout = (val_boot_timeout + 5) % 100;
                    write_u16(gRT, &gGlobalVar, var_timeout, val_boot_timeout);
                    settings_refresh();
                }
            }
        }
        return 0;
    }

    case WM_EV_MOUSE_UP: {
        if (!g_ues.b_press) return 0;
        int bh = wm_button_h();
        int bw1 = wm_button_measure("Refresh");
        int bw2 = wm_button_measure("Close");
        int gap = 8 * sc;
        int bx = cw - bw2 - 10 * sc;
        int by = ch - bh - 6 * sc;
        wm_button br = { bx - bw1 - gap, by, bw1, bh, UE_REFRESH, 1, "Refresh" };
        wm_button bc = { bx, by, bw2, bh, UE_CLOSE, 1, "Close" };
        int id = 0;
        if (wm_button_hit(&br, ev->mx, ev->my)) id = UE_REFRESH;
        else if (wm_button_hit(&bc, ev->mx, ev->my)) id = UE_CLOSE;
        int p = g_ues.b_press;
        g_ues.b_press = 0;
        if (id == p) {
            if (p == UE_CLOSE) return WM_CLOSE_REQUEST;
            if (p == UE_REFRESH) { settings_refresh(); }
        }
        return 0;
    }

    case WM_EV_CLOSE:
        g_ues.win = NULL;
        return 0;

    default:
        return 0;
    }
}

/* ==========================================================================
 * Public API.
 * ========================================================================== */

void uefi_settings_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices  : NULL;
    gRT = st ? st->RuntimeServices : NULL;

    /* Resolve theme colours. */
    c_win    = wm_theme_color(WM_COL_WINDOW);
    c_fg     = wm_theme_color(WM_COL_FG);
    c_accent = wm_theme_color(WM_COL_ACCENT);
    c_sel_bg = wm_theme_color(WM_COL_SEL_BG);
    c_sel_fg = wm_theme_color(WM_COL_SEL_FG);
    c_dim    = FOREB_DIM;
    c_border = FOREB_BORDER;
}

void uefi_settings_open(void)
{
    if (g_ues.win) return;   /* already open -- just raise it */

    settings_refresh();

    g_ues.sel = 0;
    g_ues.scroll = 0;
    g_ues.b_hover = 0;
    g_ues.b_press = 0;

    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 62 / 100;
    if (ww < 520) ww = 520;
    if (ww > 840) ww = 840;
    if (ww > W - 40) ww = W - 40;
    int wh = H * 66 / 100;
    if (wh < 320) wh = 320;
    if (wh > 680) wh = 680;
    if (wh > H - 40) wh = H - 40;

    g_ues.win = wm_open("UEFI Firmware Settings", ww, wh,
                         uefi_settings_draw, uefi_settings_event, &g_ues);
}
