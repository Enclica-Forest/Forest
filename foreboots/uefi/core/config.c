/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/config.c - ESP file access + grub.cfg-like forebo.cfg parser.
 * =============================================================================
 * Freestanding (no libc). Built with the same clang invocation as bootx64.c:
 *   clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
 *         -mno-red-zone -mno-mmx -mno-sse -std=c11 -I. -Iinclude -c uefi/config.c
 * and linked into BOOTX64.EFI alongside bootx64.o / ui.o / modules.o.
 *
 * Two responsibilities:
 *   1. ESP I/O helpers (esp_open_root / esp_ascii_to_char16 / esp_read_file /
 *      esp_free_file) shared with modules.c and the image/icon loaders. These
 *      mirror bootx64.c's static load_kernel_file() pattern but generalise the
 *      filename and factor out root-open so every asset shares one path.
 *   2. A tolerant lexer + iterative block-stack parser turning forebo.cfg text
 *      into a POD struct forebo_config (include/forebo_cfg.h), with submenu
 *      blocks (nested up to 8 deep, tracked on a fixed stack, no heap), title-
 *      path 'default=' resolution and the remember_last toggle. Malformed
 *      input is skipped, never faulted, so a partial file still boots what it
 *      can.
 * =============================================================================
 */

#include "config.h"
#include "../include/boot_protocol.h"
#include "audio.h"                 /* struct audio_cfg + AUDIO_EV_* for audio_ keys */

/* =============================================================================
 * Local GUIDs (file-scope copies; each TU that touches the ESP keeps its own,
 * exactly like bootx64.c's gLoadedImgGuid/gSfsGuid/gFileInfoGuid statics).
 * ========================================================================== */
static EFI_GUID cfg_loaded_img_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID cfg_sfs_guid        = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID cfg_file_info_guid  = EFI_FILE_INFO_ID;

/* =============================================================================
 * Tiny freestanding string helpers (kept static to avoid clashing with the
 * global memset/memcpy defined in bootx64.c; the compiler may still emit calls
 * to those for aggregate init, which resolve at link time).
 * ========================================================================== */
static void zero(void *p, UINTN n)
{
    unsigned char *d = (unsigned char *)p;
    for (UINTN i = 0; i < n; i++) d[i] = 0;
}

static UINTN slen(const char *s)
{
    UINTN n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Copy at most cap-1 bytes of src into dst and NUL-terminate. */
static void scopy(char *dst, const char *src, UINTN cap)
{
    UINTN i = 0;
    if (cap == 0) return;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive equality. */
static int ieq(const char *a, const char *b)
{
    UINTN i = 0;
    for (; a[i] && b[i]; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return a[i] == b[i];
}

/* Does s (of known length ls) end with the (case-insensitive) suffix suf?
 * Takes the caller's already-known length of s so callers that test several
 * suffixes against the same string need not re-scan it with slen() each
 * time (icon_resolve() checks both ".tga" and ".bmp" off one length). */
static int has_suffix_len(const char *s, UINTN ls, const char *suf)
{
    UINTN lf = slen(suf);
    if (lf > ls) return 0;
    for (UINTN i = 0; i < lf; i++)
        if (lower(s[ls - lf + i]) != lower(suf[i])) return 0;
    return 1;
}

/*
 * Resolve a forebo.cfg 'icon=' value into an ESP-absolute icon path.
 *   - A value that already contains a path separator ('/' or '\\') or a known
 *     image extension (".tga"/".bmp") is treated as a LITERAL path (copied as
 *     is), so full paths like /forebo/icons/os.tga keep working.
 *   - A bare short NAME (e.g. "arch", "tux", "gear", "usb") is rewritten to
 *     "/forebo/icons/<name>.tga", letting forebo.cfg use the icon=arch shorthand
 *     that maps to the icons tools/gen_assets.py emits.
 * This mirrors tools_icon_path() (uefi/tools.h) -- the single naming convention
 * shared by menu entries and the GUI tool registry -- and is idempotent: an
 * already-resolved path passes through unchanged.
 */
static void icon_resolve(char *dst, const char *val)
{
    int raw = 0;
    for (UINTN i = 0; val && val[i]; i++)
        if (val[i] == '/' || val[i] == '\\') { raw = 1; break; }
    if (!raw && val) {
        UINTN lv = slen(val);
        if (has_suffix_len(val, lv, ".tga") || has_suffix_len(val, lv, ".bmp")) raw = 1;
    }
    if (raw || !val || !val[0]) {
        scopy(dst, val, FOREB_CFG_PATH_LEN);
        return;
    }
    /* "/forebo/icons/" + <name> + ".tga" (bounded to FOREB_CFG_PATH_LEN). */
    static const char pre[] = "/forebo/icons/";
    static const char ext[] = ".tga";
    UINTN i = 0, j;
    for (j = 0; pre[j] && i + 1 < FOREB_CFG_PATH_LEN; j++) dst[i++] = pre[j];
    for (j = 0; val[j] && i + 1 < FOREB_CFG_PATH_LEN; j++) dst[i++] = val[j];
    for (j = 0; ext[j] && i + 1 < FOREB_CFG_PATH_LEN; j++) dst[i++] = ext[j];
    dst[i] = '\0';
}

/* Very small decimal parser; stops at first non-digit. Returns 0 for
 * empty/garbage input. A leading '-' is honored (negative values mean
 * "inherit/auto" for keys like win_title_h=-1 and the menu_* int overrides). */
static int to_int(const char *s)
{
    int v = 0, neg = 0;
    UINTN i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '+') i++;
    else if (s[i] == '-') { neg = 1; i++; }
    for (; s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

/* Parse an unsigned 32-bit color/number. Accepts optional 0x / # hex prefix
 * (e.g. 0x3FB56B, #3FB56B) OR plain decimal. Stops at first non-hex/-digit. */
static unsigned int to_color(const char *s)
{
    unsigned int v = 0;
    UINTN i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '#') { i++; }
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        for (;; i++) {
            char c = s[i];
            if (c >= '0' && c <= '9')      v = (v << 4) | (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (v << 4) | (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (v << 4) | (unsigned)(c - 'A' + 10);
            else break;
        }
        return v & 0x00FFFFFFu;
    }
    /* '#RRGGBB' hex without 0x, or plain decimal fallback. */
    if (s[0] == '#') {
        for (; ; i++) {
            char c = s[i];
            if (c >= '0' && c <= '9')      v = (v << 4) | (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (v << 4) | (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (v << 4) | (unsigned)(c - 'A' + 10);
            else break;
        }
        return v & 0x00FFFFFFu;
    }
    return (unsigned int)to_int(s);
}

/* Parse a boolean toggle: 1/on/yes/true -> 1, else 0. */
static int to_bool(const char *s)
{
    if (ieq(s, "1") || ieq(s, "on") || ieq(s, "yes") || ieq(s, "true"))
        return 1;
    if (ieq(s, "0") || ieq(s, "off") || ieq(s, "no") || ieq(s, "false"))
        return 0;
    return to_int(s) ? 1 : 0;
}

/* Map a type= value to enum forebo_entry_type (defaults to FOREST). */
static int entry_type_from_str(const char *v)
{
    if (ieq(v, "forest"))    return FOREB_ENTRY_FOREST;
    if (ieq(v, "linux"))     return FOREB_ENTRY_LINUX;
    if (ieq(v, "chainload") || ieq(v, "chain")) return FOREB_ENTRY_CHAINLOAD;
    if (ieq(v, "windows") || ieq(v, "win")) return FOREB_ENTRY_CHAINLOAD;
    if (ieq(v, "shell"))     return FOREB_ENTRY_SHELL;
    if (ieq(v, "recovery"))  return FOREB_ENTRY_RECOVERY;
    if (ieq(v, "reboot"))    return FOREB_ENTRY_REBOOT;
    if (ieq(v, "tools"))     return FOREB_ENTRY_TOOLS;
    if (ieq(v, "setup") || ieq(v, "firmware")) return FOREB_ENTRY_FWSETUP;
    if (ieq(v, "settings") || ieq(v, "theme")) return FOREB_ENTRY_SETTINGS;
    return FOREB_ENTRY_FOREST;
}

/* Map a window_skin= value to enum forebo_window_skin. */
static int skin_from_str(const char *v)
{
    if (ieq(v, "flat"))    return FOREB_SKIN_FLAT;
    if (ieq(v, "beveled")) return FOREB_SKIN_BEVELED;
    if (ieq(v, "glass"))   return FOREB_SKIN_GLASS;
    return FOREB_SKIN_BEVELED;
}

/* ---- menu-style enum parsers (all return -1 on an unknown value, which the
 *      resolver treats as "inherit from the preset") ---- */
static int pos_from_str(const char *v)
{
    if (ieq(v, "center")) return FMP_CENTER;
    if (ieq(v, "left"))   return FMP_LEFT;
    if (ieq(v, "right"))  return FMP_RIGHT;
    if (ieq(v, "top"))    return FMP_TOP;
    if (ieq(v, "bottom")) return FMP_BOTTOM;
    if (ieq(v, "full") || ieq(v, "fullscreen")) return FMP_FULL;
    if (ieq(v, "custom")) return FMP_CUSTOM;
    return -1;
}
static int sel_from_str(const char *v)
{
    if (ieq(v, "bar"))       return FSS_BAR;
    if (ieq(v, "box"))       return FSS_BOX;
    if (ieq(v, "outline"))   return FSS_OUTLINE;
    if (ieq(v, "underline")) return FSS_UNDERLINE;
    if (ieq(v, "arrow"))     return FSS_ARROW;
    if (ieq(v, "bracket") || ieq(v, "brackets")) return FSS_BRACKET;
    if (ieq(v, "invert"))    return FSS_INVERT;
    if (ieq(v, "pill"))      return FSS_PILL;
    if (ieq(v, "gradient"))  return FSS_GRADIENT;
    if (ieq(v, "glow"))      return FSS_GLOW;
    if (ieq(v, "none"))      return FSS_NONE;
    if (ieq(v, "doublebar") || ieq(v, "stripe")) return FSS_DOUBLEBAR;
    return -1;
}
static int border_from_str(const char *v)
{
    if (ieq(v, "none"))   return FBD_NONE;
    if (ieq(v, "thin"))   return FBD_THIN;
    if (ieq(v, "thick"))  return FBD_THICK;
    if (ieq(v, "double")) return FBD_DOUBLE;
    if (ieq(v, "shadow")) return FBD_SHADOW;
    if (ieq(v, "glow"))   return FBD_GLOW;
    if (ieq(v, "dashed")) return FBD_DASHED;
    return -1;
}
static int corner_from_str(const char *v)
{
    if (ieq(v, "square")) return FCN_SQUARE;
    if (ieq(v, "round") || ieq(v, "rounded")) return FCN_ROUND;
    if (ieq(v, "cut") || ieq(v, "bevel"))     return FCN_CUT;
    return -1;
}
static int align_from_str(const char *v)
{
    if (ieq(v, "left"))   return FAL_LEFT;
    if (ieq(v, "center") || ieq(v, "centre")) return FAL_CENTER;
    if (ieq(v, "right"))  return FAL_RIGHT;
    return -1;
}

static int cfg_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int btn_style_from_str(const char *v)
{
    if (ieq(v, "flat"))    return FBTN_FLAT;
    if (ieq(v, "raised"))  return FBTN_RAISED;
    if (ieq(v, "pill"))    return FBTN_PILL;
    if (ieq(v, "outline")) return FBTN_OUTLINE;
    if (ieq(v, "ghost"))   return FBTN_GHOST;
    if (ieq(v, "glass"))   return FBTN_GLASS;
    return -1;
}

/* Assign one widget (button / UI-element) key=value. Keys are prefixed btn_/ui_
 * so they never collide with color/menu keys. Returns 1 if consumed. */
static int widget_set(struct forebo_widget *w, const char *key, const char *val)
{
    if (ieq(key, "btn_style"))            w->style       = btn_style_from_str(val);
    else if (ieq(key, "btn_corner"))      w->corner      = corner_from_str(val);
    else if (ieq(key, "btn_border"))      w->border_w    = to_int(val);
    else if (ieq(key, "btn_pad_x"))       w->pad_x       = to_int(val);
    else if (ieq(key, "btn_pad_y"))       w->pad_y       = to_int(val);
    else if (ieq(key, "btn_gradient"))    w->gradient    = to_bool(val);
    else if (ieq(key, "btn_shadow"))      w->shadow      = to_bool(val);
    else if (ieq(key, "btn_glow"))        w->glow        = to_bool(val);
    else if (ieq(key, "btn_fill"))        w->face_normal = to_color(val);
    else if (ieq(key, "btn_fill_hover"))  w->face_hover  = to_color(val);
    else if (ieq(key, "btn_fill_active")) w->face_active = to_color(val);
    else if (ieq(key, "btn_fill_disabled")) w->face_disabled = to_color(val);
    else if (ieq(key, "btn_text"))        w->text_normal = to_color(val);
    else if (ieq(key, "btn_text_hover"))  w->text_hover  = to_color(val);
    else if (ieq(key, "btn_text_active")) w->text_active = to_color(val);
    else if (ieq(key, "btn_border_color")) w->border_col = to_color(val);
    else if (ieq(key, "btn_focus_color")) w->focus_col   = to_color(val);
    else if (ieq(key, "ui_window_corner")) w->window_corner = corner_from_str(val);
    else if (ieq(key, "ui_window_border")) w->window_border_w = to_int(val);
    else if (ieq(key, "ui_panel_alpha"))  w->panel_alpha = cfg_clampi(to_int(val), 0, 255);
    else if (ieq(key, "ui_separator"))    w->separator   = to_color(val);
    else if (ieq(key, "ui_scrollbar_w"))  w->scrollbar_w = to_int(val);
    else if (ieq(key, "ui_scrollbar_color")) w->scrollbar_color = to_color(val);
    else if (ieq(key, "ui_focus_color"))  w->focus_color = to_color(val);
    else if (ieq(key, "ui_focus_width"))  w->focus_width = to_int(val);
    else if (ieq(key, "ui_font_scale"))   w->font_scale  = cfg_clampi(to_int(val), 25, 800);
    else return 0;
    return 1;
}

/* Assign one window-skin key=value. Keys are prefixed win_ so they never
 * collide with other keys. Returns 1 if consumed. */
static int winskin_set(struct forebo_winskin *w, const char *key, const char *val)
{
    if (ieq(key, "win_title_h"))           w->title_h      = ieq(val, "auto") ? -1 : to_int(val);
    else if (ieq(key, "win_title_fill"))   w->title_fill   = to_color(val);
    else if (ieq(key, "win_title_fg"))     w->title_fg     = to_color(val);
    else if (ieq(key, "win_border_color")) w->border_color = to_color(val);
    else if (ieq(key, "win_border_w"))     w->border_w     = to_int(val);
    else if (ieq(key, "win_corner"))       w->corner       = corner_from_str(val);
    else if (ieq(key, "win_shadow"))       w->shadow       = to_bool(val);
    else if (ieq(key, "win_close_color"))  w->close_color  = to_color(val);
    else if (ieq(key, "win_button_style")) w->button_style = btn_style_from_str(val);
    else return 0;
    return 1;
}

static void winskin_default(struct forebo_winskin *w)
{
    w->title_h = -1;
    w->title_fill = w->title_fg = w->border_color = w->close_color = FOREB_COLOR_UNSET;
    w->border_w = -1;
    w->corner = -1;
    w->shadow = -1;
    w->button_style = -1;
}

/* ---- audio (PC-speaker) config. Kept file-static so forebo_cfg.h need not
 *      include audio.h. bootx64 reads it via forebo_cfg_audio(). ---- */
static struct audio_cfg g_audio_cfg;
static int              g_audio_seen;

static int audio_set(const char *key, const char *val)
{
    if (ieq(key, "pcspeaker"))         g_audio_cfg.enabled = to_bool(val);
    else if (ieq(key, "audio_volume")) g_audio_cfg.volume  = cfg_clampi(to_int(val), 0, 100);
    else if (ieq(key, "audio_nav_freq"))    g_audio_cfg.tone[AUDIO_EV_NAV].freq    = (UINT16)to_int(val);
    else if (ieq(key, "audio_nav_ms"))      g_audio_cfg.tone[AUDIO_EV_NAV].ms      = (UINT16)to_int(val);
    else if (ieq(key, "audio_select_freq")) g_audio_cfg.tone[AUDIO_EV_SELECT].freq = (UINT16)to_int(val);
    else if (ieq(key, "audio_select_ms"))   g_audio_cfg.tone[AUDIO_EV_SELECT].ms   = (UINT16)to_int(val);
    else if (ieq(key, "audio_open_freq"))   g_audio_cfg.tone[AUDIO_EV_OPEN].freq   = (UINT16)to_int(val);
    else if (ieq(key, "audio_open_ms"))     g_audio_cfg.tone[AUDIO_EV_OPEN].ms     = (UINT16)to_int(val);
    else if (ieq(key, "audio_error_freq"))  g_audio_cfg.tone[AUDIO_EV_ERROR].freq  = (UINT16)to_int(val);
    else if (ieq(key, "audio_error_ms"))    g_audio_cfg.tone[AUDIO_EV_ERROR].ms    = (UINT16)to_int(val);
    else if (ieq(key, "audio_back_freq"))   g_audio_cfg.tone[AUDIO_EV_BACK].freq   = (UINT16)to_int(val);
    else if (ieq(key, "audio_back_ms"))     g_audio_cfg.tone[AUDIO_EV_BACK].ms     = (UINT16)to_int(val);
    else return 0;
    g_audio_seen = 1;
    return 1;
}

const struct audio_cfg *forebo_cfg_audio(void)
{
    return g_audio_seen ? &g_audio_cfg : 0;
}

static void widget_default(struct forebo_widget *w)
{
    /* Everything inherits the built-in look until the user sets it. */
    w->style = w->corner = w->border_w = w->pad_x = w->pad_y = -1;
    w->gradient = w->shadow = w->glow = -1;
    w->face_normal = w->face_hover = w->face_active = w->face_disabled = FOREB_COLOR_UNSET;
    w->text_normal = w->text_hover = w->text_active = w->text_disabled = FOREB_COLOR_UNSET;
    w->border_col = w->focus_col = FOREB_COLOR_UNSET;
    w->window_corner = w->window_border_w = -1;
    w->panel_alpha = -1;
    w->separator      = FOREB_DEF_UI_SEPARATOR;
    w->scrollbar_w    = -1;
    w->scrollbar_color = FOREB_DEF_UI_SCROLLBAR;
    w->focus_color    = FOREB_DEF_UI_FOCUS;
    w->focus_width    = -1;
    w->font_scale     = -1;
}

/* Assign one menu-style key=value pair. Returns 1 if consumed. All keys are
 * prefixed "menu_" so they never collide with the color/theme keys. */
static int style_set(struct forebo_style *s, const char *key, const char *val)
{
    if (ieq(key, "menu_style"))          scopy(s->preset, val, FOREB_CFG_NAME_LEN);
    else if (ieq(key, "menu_pos"))       s->pos           = pos_from_str(val);
    else if (ieq(key, "menu_x"))         s->panel_x       = to_int(val);
    else if (ieq(key, "menu_y"))         s->panel_y       = to_int(val);
    else if (ieq(key, "menu_w"))         s->panel_w       = to_int(val);
    else if (ieq(key, "menu_h"))         s->panel_h       = to_int(val);
    else if (ieq(key, "menu_entry_h"))   s->entry_h       = to_int(val);
    else if (ieq(key, "menu_pad"))       s->pad           = to_int(val);
    else if (ieq(key, "menu_align"))     s->align         = align_from_str(val);
    else if (ieq(key, "menu_selection")) s->sel_style     = sel_from_str(val);
    else if (ieq(key, "menu_border"))    s->border        = border_from_str(val);
    else if (ieq(key, "menu_corner"))    s->corner        = corner_from_str(val);
    else if (ieq(key, "menu_accent_strip")) s->accent_strip = to_bool(val);
    else if (ieq(key, "menu_dividers"))  s->dividers      = to_bool(val);
    else if (ieq(key, "menu_gradient"))  s->gradient      = to_bool(val);
    else if (ieq(key, "menu_shadow"))    s->shadow        = to_bool(val);
    else if (ieq(key, "menu_title_bar")) s->title_bar     = to_bool(val);
    else if (ieq(key, "menu_show_title"))  s->show_title  = to_bool(val);
    else if (ieq(key, "menu_show_footer")) s->show_footer = to_bool(val);
    else if (ieq(key, "menu_show_timer"))  s->show_timer  = to_bool(val);
    else if (ieq(key, "menu_show_icons"))  s->show_icons  = to_bool(val);
    else if (ieq(key, "menu_icon_side"))   s->icon_right  = ieq(val, "left") ? 0 : 1;
    else if (ieq(key, "menu_scrollbar"))   s->show_scrollbar = to_bool(val);
    else if (ieq(key, "menu_caret"))       s->show_caret  = to_bool(val);
    else return 0;
    return 1;
}

/* Reset a style block so every field inherits from the chosen preset. */
static void style_default(struct forebo_style *s)
{
    s->preset[0] = '\0';
    s->pos = s->panel_x = s->panel_y = s->panel_w = s->panel_h = -1;
    s->entry_h = s->pad = s->align = -1;
    s->sel_style = s->border = s->corner = -1;
    s->accent_strip = s->dividers = s->gradient = s->shadow = -1;
    s->title_bar = s->show_title = s->show_footer = s->show_timer = -1;
    s->show_icons = s->icon_right = s->show_scrollbar = s->show_caret = -1;
}

/* =============================================================================
 * ESP I/O helpers
 * ========================================================================== */
EFI_STATUS esp_open_root(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         EFI_FILE_PROTOCOL **out_root)
{
    EFI_STATUS st;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;

    if (out_root) *out_root = NULL;
    if (!bs || !out_root) return EFI_INVALID_PARAMETER;

    st = bs->HandleProtocol(image, &cfg_loaded_img_guid, (VOID **)&li);
    if (EFI_ERROR(st) || !li) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    st = bs->HandleProtocol(li->DeviceHandle, &cfg_sfs_guid, (VOID **)&fs);
    if (EFI_ERROR(st) || !fs) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st) || !root) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    *out_root = root;
    return EFI_SUCCESS;
}

void esp_ascii_to_char16(const char *in, CHAR16 *out, UINTN cap)
{
    UINTN i = 0;
    if (!out || cap == 0) return;
    if (in) {
        for (; in[i] && i + 1 < cap; i++) {
            char c = in[i];
            out[i] = (CHAR16)((c == '/') ? '\\' : (unsigned char)c);
        }
    }
    out[i] = 0;
}

EFI_STATUS esp_read_file(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                         const char *path, void **out_buf, UINTN *out_size)
{
    EFI_STATUS st;
    EFI_FILE_PROTOCOL *root = NULL, *f = NULL;
    CHAR16 wpath[FOREB_CFG_PATH_LEN + 2];
    UINT8 infobuf[512];
    UINTN infosz = sizeof(infobuf);
    UINTN fsize, done = 0;
    VOID *buf = NULL;

    if (out_buf) *out_buf = NULL;
    if (out_size) *out_size = 0;
    if (!bs || !path || !out_buf || !out_size) return EFI_INVALID_PARAMETER;

    st = esp_open_root(image, bs, &root);
    if (EFI_ERROR(st)) return st;

    esp_ascii_to_char16(path, wpath, FOREB_CFG_PATH_LEN + 2);
    st = root->Open(root, &f, wpath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) {
        root->Close(root);
        return EFI_ERROR(st) ? st : EFI_NOT_FOUND;
    }

    st = f->GetInfo(f, &cfg_file_info_guid, &infosz, infobuf);
    if (EFI_ERROR(st)) {
        f->Close(f);
        root->Close(root);
        return st;
    }
    fsize = (UINTN)((EFI_FILE_INFO *)infobuf)->FileSize;

    /* +1 for a safety NUL so parsers can treat the buffer as a C string. */
    st = bs->AllocatePool(EfiLoaderData, fsize + 1, &buf);
    if (EFI_ERROR(st) || !buf) {
        f->Close(f);
        root->Close(root);
        return EFI_ERROR(st) ? st : EFI_OUT_OF_RESOURCES;
    }

    while (done < fsize) {
        UINTN want = fsize - done;
        UINTN got = want;
        st = f->Read(f, &got, (UINT8 *)buf + done);
        if (EFI_ERROR(st)) {
            bs->FreePool(buf);
            f->Close(f);
            root->Close(root);
            return st;
        }
        if (got == 0) break;   /* EOF / short read */
        done += got;
    }
    ((UINT8 *)buf)[done] = 0;

    f->Close(f);
    root->Close(root);
    *out_buf = buf;
    *out_size = done;
    return EFI_SUCCESS;
}

void esp_free_file(EFI_BOOT_SERVICES *bs, void *buf)
{
    if (bs && buf) bs->FreePool(buf);
}

/* =============================================================================
 * Lexer
 * ---------------------------------------------------------------------------
 * Token stream over the raw config bytes. Whitespace and '#'-to-EOL comments
 * are skipped. A token is one of:
 *   TOK_WORD    - a bareword (path/key/number/menuentry), stops at whitespace
 *                 or any of  = { } " #
 *   TOK_STRING  - contents of a "double-quoted" run (quotes stripped; a missing
 *                 closing quote is tolerated - runs to EOF/newline)
 *   TOK_EQ '='  TOK_LBRACE '{'  TOK_RBRACE '}'
 *   TOK_EOF
 * ========================================================================== */
enum { TOK_EOF = 0, TOK_WORD, TOK_STRING, TOK_EQ, TOK_LBRACE, TOK_RBRACE };

#define CFG_TOK_MAX 192

struct lexer {
    const char *p;
    const char *end;
};

/* Per-byte classification table for the lexer's hottest per-character tests
 * (is_space/is_word_end are called once per byte of the whole forebo.cfg
 * buffer). Built once at compile time via designated initializers -- no
 * heap, no libc -- so the lexer trades a branch cascade for one lookup. */
#define CFG_CC_SPACE    0x1  /* ' ' '\t' '\r' '\n' '\v' '\f'          */
#define CFG_CC_WORDEND2 0x2  /* '=' '{' '}' '"' '#' (word-end, not space) */
static const unsigned char cfg_charclass[256] = {
    [' ']  = CFG_CC_SPACE, ['\t'] = CFG_CC_SPACE, ['\r'] = CFG_CC_SPACE,
    ['\n'] = CFG_CC_SPACE, ['\v'] = CFG_CC_SPACE, ['\f'] = CFG_CC_SPACE,
    ['=']  = CFG_CC_WORDEND2, ['{'] = CFG_CC_WORDEND2, ['}'] = CFG_CC_WORDEND2,
    ['"']  = CFG_CC_WORDEND2, ['#'] = CFG_CC_WORDEND2,
};

static int is_space(char c)
{
    return cfg_charclass[(unsigned char)c] & CFG_CC_SPACE;
}

static int is_word_end(char c)
{
    return cfg_charclass[(unsigned char)c] & (CFG_CC_SPACE | CFG_CC_WORDEND2);
}

/* Fetch the next token; text (if WORD/STRING) copied NUL-terminated into buf. */
static int lex_next(struct lexer *lx, char *buf, UINTN cap)
{
    if (buf && cap) buf[0] = '\0';

    for (;;) {
        /* skip whitespace */
        while (lx->p < lx->end && is_space(*lx->p)) lx->p++;
        if (lx->p >= lx->end) return TOK_EOF;
        if (*lx->p == '#') {                /* comment to end of line */
            while (lx->p < lx->end && *lx->p != '\n') lx->p++;
            continue;
        }
        break;
    }

    char c = *lx->p;
    if (c == '=') { lx->p++; return TOK_EQ; }
    if (c == '{') { lx->p++; return TOK_LBRACE; }
    if (c == '}') { lx->p++; return TOK_RBRACE; }

    if (c == '"') {
        lx->p++;                            /* consume opening quote */
        UINTN n = 0;
        while (lx->p < lx->end && *lx->p != '"' && *lx->p != '\n') {
            if (buf && n + 1 < cap) buf[n++] = *lx->p;
            lx->p++;
        }
        if (buf && cap) buf[n < cap ? n : cap - 1] = '\0';
        if (lx->p < lx->end && *lx->p == '"') lx->p++;  /* consume close quote */
        return TOK_STRING;
    }

    /* bareword */
    UINTN n = 0;
    while (lx->p < lx->end && !is_word_end(*lx->p)) {
        if (buf && n + 1 < cap) buf[n++] = *lx->p;
        lx->p++;
    }
    if (buf && cap) buf[n < cap ? n : cap - 1] = '\0';
    return TOK_WORD;
}

/* =============================================================================
 * Config model helpers
 * ========================================================================== */
/* Reset a theme block to the built-in Forest defaults. */
void forebo_theme_default(struct forebo_theme *t)
{
    if (!t) return;
    zero(t, sizeof(*t));
    t->color_bg       = FOREB_DEF_COLOR_BG;
    t->color_fg       = FOREB_DEF_COLOR_FG;
    t->color_accent   = FOREB_DEF_COLOR_ACCENT;
    t->color_sel_bg   = FOREB_DEF_COLOR_SEL_BG;
    t->color_sel_fg   = FOREB_DEF_COLOR_SEL_FG;
    t->color_titlebar = FOREB_DEF_COLOR_TITLEBAR;
    t->color_window   = FOREB_DEF_COLOR_WINDOW;
    t->color_cursor   = FOREB_DEF_COLOR_CURSOR;
    t->cursor_path[0]     = '\0';
    t->cursor_enabled     = 1;
    t->mouse_enabled      = 1;
    t->animations_enabled = 1;
    t->double_buffer      = 1;
    t->window_skin        = FOREB_SKIN_BEVELED;
    t->preset[0]          = '\0';   /* "" -> forest (built-in default) */
    style_default(&t->style);       /* every menu_* field inherits the preset */
    widget_default(&t->widget);     /* buttons / UI elements inherit built-ins */
    winskin_default(&t->winskin);   /* window chrome inherits skin/theme       */
    t->fx_glass = 0;                /* effects off by default (perf-safe)      */
    t->fx_blur = 8; t->fx_opacity = 72; t->fx_vignette = 0; t->fx_scanlines = 0;
}

void forebo_cfg_init(struct forebo_config *cfg)
{
    if (!cfg) return;
    zero(cfg, sizeof(*cfg));
    cfg->count       = 0;
    cfg->default_idx = 0;
    cfg->timeout     = FOREB_CFG_DEFAULT_TIMEOUT;
    cfg->background[0] = '\0';
    /* All rows start at top level; remember_last off; no default path. */
    for (int i = 0; i < FOREB_CFG_MAX_ENTRIES; i++) cfg->entries[i].parent = -1;
    cfg->remember_last   = 0;
    cfg->default_path[0] = '\0';
    forebo_theme_default(&cfg->theme);
}

/* Assign one global theme/customization key=value pair. Returns 1 if consumed. */
static int theme_set(struct forebo_theme *t, const char *key, const char *val)
{
    if (ieq(key, "color_bg"))            t->color_bg       = to_color(val);
    else if (ieq(key, "color_fg"))       t->color_fg       = to_color(val);
    else if (ieq(key, "color_accent"))   t->color_accent   = to_color(val);
    else if (ieq(key, "color_sel_bg"))   t->color_sel_bg   = to_color(val);
    else if (ieq(key, "color_sel_fg"))   t->color_sel_fg   = to_color(val);
    else if (ieq(key, "color_titlebar")) t->color_titlebar = to_color(val);
    else if (ieq(key, "color_window"))   t->color_window   = to_color(val);
    else if (ieq(key, "color_cursor"))   t->color_cursor   = to_color(val);
    else if (ieq(key, "cursor"))         scopy(t->cursor_path, val, FOREB_CFG_PATH_LEN);
    else if (ieq(key, "cursor_enabled")) t->cursor_enabled     = to_bool(val);
    else if (ieq(key, "mouse_enabled"))  t->mouse_enabled      = to_bool(val);
    else if (ieq(key, "animations"))     t->animations_enabled = to_bool(val);
    else if (ieq(key, "double_buffer"))  t->double_buffer      = to_bool(val);
    else if (ieq(key, "window_skin"))    t->window_skin        = skin_from_str(val);
    else if (ieq(key, "theme"))          scopy(t->preset, val, FOREB_CFG_NAME_LEN);
    /* widget_set()'s keys are all btn_/ui_ prefixed (see its comment); skip
     * the whole ~20-way ieq() chain for keys that can't possibly match. */
    else if ((lower(key[0]) == 'b' || lower(key[0]) == 'u') &&
             widget_set(&t->widget, key, val)) return 1;   /* btn_/ui_ appearance */
    else if (ieq(key, "fx_glass"))     t->fx_glass     = to_bool(val);
    else if (ieq(key, "fx_blur"))      t->fx_blur      = cfg_clampi(to_int(val), 0, 32);
    else if (ieq(key, "fx_opacity"))   t->fx_opacity   = cfg_clampi(to_int(val), 0, 255);
    else if (ieq(key, "fx_vignette"))  t->fx_vignette  = cfg_clampi(to_int(val), 0, 255);
    else if (ieq(key, "fx_scanlines")) t->fx_scanlines = cfg_clampi(to_int(val), 0, 255);
    else if (ieq(key, "img_background")) scopy(t->img_background, val, FOREB_CFG_PATH_LEN);
    else if (ieq(key, "img_panel"))      scopy(t->img_panel,      val, FOREB_CFG_PATH_LEN);
    else if (ieq(key, "img_window"))     scopy(t->img_window,     val, FOREB_CFG_PATH_LEN);
    else if (ieq(key, "img_titlebar"))   scopy(t->img_titlebar,   val, FOREB_CFG_PATH_LEN);
    else if (ieq(key, "img_button"))     scopy(t->img_button,     val, FOREB_CFG_PATH_LEN);
    else if (ieq(key, "img_cursor"))     scopy(t->img_cursor,     val, FOREB_CFG_PATH_LEN);
    /* winskin_set()'s keys are all win_ prefixed; style_set()'s are all
     * menu_ prefixed (see their comments) -- same short-circuit as above. */
    else if (lower(key[0]) == 'w' && lower(key[1]) == 'i' && lower(key[2]) == 'n' &&
             key[3] == '_' && winskin_set(&t->winskin, key, val)) return 1;
    else if (lower(key[0]) == 'm' && lower(key[1]) == 'e' && lower(key[2]) == 'n' &&
             lower(key[3]) == 'u' && key[4] == '_' && style_set(&t->style, key, val)) return 1;
    else return 0;
    return 1;
}

/* Assign one key=value pair inside the current menuentry block. */
static void entry_set(struct forebo_menuentry *e, const char *key, const char *val)
{
    if (ieq(key, "type")) {
        e->type = entry_type_from_str(val);
        /* type=windows is a CHAINLOAD alias that defaults its chain path to the
         * standard Windows Boot Manager (FOREB_CHAIN_WINDOWS_PATH). An explicit
         * later chain= line still overrides this (entry_set copies it verbatim). */
        if ((ieq(val, "windows") || ieq(val, "win")) && e->chain[0] == 0)
            scopy(e->chain, "/EFI/Microsoft/Boot/bootmgfw.efi", FOREB_CFG_PATH_LEN);
    } else if (ieq(key, "kernel")) {
        scopy(e->kernel, val, FOREB_CFG_PATH_LEN);
        /* Legacy: kernel=reboot is the firmware-reset pseudo-entry. */
        if (ieq(val, "reboot")) e->type = FOREB_ENTRY_REBOOT;
    } else if (ieq(key, "vmlinuz")) {
        scopy(e->vmlinuz, val, FOREB_CFG_PATH_LEN);
        if (e->type == FOREB_ENTRY_FOREST) e->type = FOREB_ENTRY_LINUX;
    } else if (ieq(key, "initrd")) {
        scopy(e->initrd, val, FOREB_CFG_PATH_LEN);
    } else if (ieq(key, "chain")) {
        scopy(e->chain, val, FOREB_CFG_PATH_LEN);
        if (e->type == FOREB_ENTRY_FOREST) e->type = FOREB_ENTRY_CHAINLOAD;
    } else if (ieq(key, "module") || ieq(key, "module2")) {
        /* module and module2 are aliases: both append in file order. */
        if (e->module_count < FOREB_CFG_MAX_MODULES && val[0]) {
            scopy(e->modules[e->module_count], val, FOREB_CFG_PATH_LEN);
            e->module_count++;
        }
    } else if (ieq(key, "cmdline")) {
        scopy(e->cmdline, val, FOREB_CFG_CMDLINE_LEN);
    } else if (ieq(key, "icon")) {
        /* Accept both full ESP paths and short names (icon=arch -> ...arch.tga). */
        icon_resolve(e->icon, val);
    } else if (ieq(key, "background")) {
        scopy(e->background, val, FOREB_CFG_PATH_LEN);
    }
    /* unknown keys are silently ignored (forward compatibility) */
}

/* =============================================================================
 *  Submenu-aware parser
 * ---------------------------------------------------------------------------
 * One iterative loop handles top-level globals, menuentry blocks and submenu
 * blocks. Block context lives on a small fixed-size stack (no heap): submenu
 * blocks nest up to FOREB_CFG_MAX_SUBMENU_DEPTH levels deep, plus one
 * innermost menuentry block. A '}' closes the innermost open block of either
 * kind. Rows are stored flat in file order; each row's 'parent' is the flat
 * index of its enclosing submenu row (-1 at top level).
 * ========================================================================== */
struct blk_ctx {
    int idx;         /* flat row of the block's entry (-1: discarded/no slot) */
    int is_submenu;  /* 1 = submenu block (may contain children), 0 = menuentry */
};

/* Skip the rest of the current physical line (error recovery). */
static void skip_line(struct lexer *lx)
{
    while (lx->p < lx->end && *lx->p != '\n') lx->p++;
}

/* Exact case-sensitive equality (default= path segments match titles exactly). */
static int seq(const char *a, const char *b)
{
    UINTN i = 0;
    for (; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

/* 1 when every character is a decimal digit (empty string -> 0). */
static int all_digits(const char *s)
{
    UINTN i = 0;
    if (!s[0]) return 0;
    for (; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Flat index of the first child row of 'parent' (-1 = top level), or -1. */
static int first_child_of(const struct forebo_config *cfg, int parent)
{
    for (int i = 0; i < cfg->count; i++)
        if (cfg->entries[i].parent == parent) return i;
    return -1;
}

/* DESCEND RULE: while idx is a submenu row, descend to its first child.
 * Returns the resolved flat row, or -1 if a submenu on the path is empty
 * (or the parent links are corrupt). */
static int descend_submenus(const struct forebo_config *cfg, int idx)
{
    int guard = 0;
    while (idx >= 0 && idx < cfg->count &&
           cfg->entries[idx].type == FOREB_ENTRY_SUBMENU) {
        if (++guard > FOREB_CFG_MAX_SUBMENU_DEPTH + 2) return -1;
        idx = first_child_of(cfg, idx);
    }
    return idx;
}

/* Resolution-failure fallback: first top-level non-submenu row, else 0. */
static int fallback_default(const struct forebo_config *cfg)
{
    for (int i = 0; i < cfg->count; i++)
        if (cfg->entries[i].parent == -1 &&
            cfg->entries[i].type != FOREB_ENTRY_SUBMENU)
            return i;
    return 0;
}

/* Resolve a 'default=' title path ("Sub/Sub2/Entry") to a flat row index.
 * Segments match row titles case-sensitively among the children of the
 * current level (starting at top level); intermediate segments must land on
 * submenu rows. Empty segments and >TITLE_LEN-1 segments fail. Returns the
 * flat index, or -1 on any mismatch. */
static int resolve_default_path(const struct forebo_config *cfg, const char *path)
{
    int parent = -1, idx = -1;
    UINTN i = 0;
    if (!path[0]) return -1;
    for (;;) {
        char seg[FOREB_CFG_TITLE_LEN];
        UINTN n = 0, start = i;
        while (path[i] && path[i] != '/') {         /* copy one segment */
            if (n + 1 < sizeof(seg)) seg[n++] = path[i];
            i++;
        }
        seg[n] = '\0';
        if (n == 0) return -1;                      /* empty segment (//, lead- */
        if (i - start >= sizeof(seg)) return -1;    /* ing/trailing /) or >63  */
        idx = -1;
        for (int r = 0; r < cfg->count; r++) {
            if (cfg->entries[r].parent == parent &&
                seq(cfg->entries[r].title, seg)) { idx = r; break; }
        }
        if (idx < 0) return -1;                     /* no such child here      */
        if (!path[i]) break;                        /* final segment matched   */
        i++;                                        /* consume '/'             */
        if (cfg->entries[idx].type != FOREB_ENTRY_SUBMENU) return -1;
        parent = idx;
    }
    return idx;
}

/* Resolve the pending default after the whole file is parsed: an integer
 * counts ONLY top-level rows (submenu rows included in that count); a string
 * is a title path. Then apply the DESCEND RULE, the failure fallback, and
 * clamp into [0, count-1]. */
static void resolve_default(struct forebo_config *cfg, int pending_int)
{
    int idx = -1;
    if (cfg->count <= 0) { cfg->default_idx = 0; return; }

    if (cfg->default_path[0]) {
        idx = resolve_default_path(cfg, cfg->default_path);
    } else if (pending_int >= 0) {
        int seen = 0;
        for (int i = 0; i < cfg->count; i++) {
            if (cfg->entries[i].parent != -1) continue;
            if (seen == pending_int) { idx = i; break; }
            seen++;
        }
    } else {
        idx = 0;    /* no default= key: first row */
    }
    if (idx < 0) idx = fallback_default(cfg);
    idx = descend_submenus(cfg, idx);
    if (idx < 0) idx = fallback_default(cfg);       /* empty submenu on path   */
    if (idx < 0 || idx >= cfg->count) idx = 0;      /* clamp [0, count-1]      */
    cfg->default_idx = idx;
}

int forebo_cfg_parse(struct forebo_config *cfg, const char *text, unsigned long len)
{
    if (!cfg) return 0;
    if (!text || len == 0) {
        if (cfg->default_idx < 0) cfg->default_idx = 0;
        return cfg->count;
    }

    struct lexer lx;
    lx.p = text;
    lx.end = text + len;

    char tok[CFG_TOK_MAX], val[CFG_TOK_MAX];

    /* Open-block stack (fixed, no heap): up to FOREB_CFG_MAX_SUBMENU_DEPTH
     * submenu blocks plus one innermost menuentry block. */
    struct blk_ctx stack[FOREB_CFG_MAX_SUBMENU_DEPTH + 1];
    int depth = 0;
    int sub_depth = 0;          /* submenu blocks currently open */
    int pending_default = -1;   /* integer default= (top-level idx); -1 = none/path */

    for (;;) {
        int t = lex_next(&lx, tok, sizeof(tok));
        if (t == TOK_EOF) break;

        if (t == TOK_RBRACE) {
            /* '}' closes the innermost open block (menuentry OR submenu). */
            if (depth > 0) {
                if (stack[depth - 1].is_submenu) sub_depth--;
                depth--;
            }
            continue;
        }

        if (t == TOK_WORD && (ieq(tok, "menuentry") || ieq(tok, "submenu"))) {
            int want_sub = ieq(tok, "submenu");

            /* Block openers are only valid at top level or inside a submenu;
             * inside a menuentry the line is an error: skip to the next line. */
            if (depth > 0 && !stack[depth - 1].is_submenu) {
                skip_line(&lx);
                continue;
            }
            /* Submenu nesting cap exceeded: discard the whole block (header
             * plus body up to the matching '}') and keep parsing after it. */
            if (want_sub && sub_depth >= FOREB_CFG_MAX_SUBMENU_DEPTH) {
                int extra = 1;
                skip_line(&lx);                     /* finish the header line  */
                for (;;) {
                    int ts = lex_next(&lx, val, sizeof(val));
                    if (ts == TOK_EOF) break;
                    if (ts == TOK_LBRACE) extra++;
                    if (ts == TOK_RBRACE && --extra == 0) break;
                }
                continue;
            }

            /* <kind> "Title" {   (title may be a bareword; may be omitted) */
            char title[FOREB_CFG_TITLE_LEN];
            int tt = lex_next(&lx, title, sizeof(title));
            if (tt == TOK_EOF) break;

            int haveslot = (cfg->count < FOREB_CFG_MAX_ENTRIES);
            int idx = haveslot ? cfg->count : -1;
            struct forebo_menuentry *e = haveslot ? &cfg->entries[cfg->count] : NULL;
            if (e) {
                int parent = -1;
                for (int s = depth - 1; s >= 0; s--)
                    if (stack[s].is_submenu) { parent = stack[s].idx; break; }
                zero(e, sizeof(*e));
                e->parent = parent;
                if (want_sub) e->type = FOREB_ENTRY_SUBMENU;
                if (tt == TOK_STRING || tt == TOK_WORD)
                    scopy(e->title, title, FOREB_CFG_TITLE_LEN);
            }

            if (tt != TOK_LBRACE) {
                int tb = lex_next(&lx, val, sizeof(val));
                if (tb == TOK_EOF) break;           /* no row (as before) */
                if (tb != TOK_LBRACE) {
                    /* Malformed header; skip forward until a '}' or EOF so we
                     * don't misread the block body as globals, and discard
                     * the partial row (existing recovery behaviour). */
                    for (;;) {
                        int ts = lex_next(&lx, val, sizeof(val));
                        if (ts == TOK_EOF || ts == TOK_RBRACE) break;
                    }
                    continue;
                }
            }

            if (e) cfg->count++;
            /* Push the block context; the matching '}' pops it. */
            stack[depth].idx = idx;
            stack[depth].is_submenu = want_sub;
            if (want_sub) sub_depth++;
            depth++;
            continue;
        }

        if (t == TOK_WORD) {
            /* key = value (global, per-entry, or submenu attribute) */
            int te = lex_next(&lx, val, sizeof(val));
            if (te == TOK_EOF) break;
            if (te == TOK_RBRACE) {                 /* "key" then block end */
                if (depth > 0) {
                    if (stack[depth - 1].is_submenu) sub_depth--;
                    depth--;
                }
                continue;
            }
            if (te != TOK_EQ) continue;             /* not "key = ..."; resync */

            int tv = lex_next(&lx, val, sizeof(val));
            if (tv == TOK_EOF) break;
            if (tv == TOK_RBRACE) {                 /* "key =" then block end */
                if (depth > 0) {
                    if (stack[depth - 1].is_submenu) sub_depth--;
                    depth--;
                }
                continue;
            }
            if (tv != TOK_WORD && tv != TOK_STRING) continue;

            if (depth > 0) {
                struct blk_ctx *top = &stack[depth - 1];
                if (!top->is_submenu) {
                    /* Inside a menuentry: the usual per-entry keys. */
                    if (top->idx >= 0) entry_set(&cfg->entries[top->idx], tok, val);
                } else {
                    /* Inside a submenu: only 'icon' is meaningful (the icon
                     * of the submenu row itself); other keys are ignored. */
                    if (top->idx >= 0 && ieq(tok, "icon"))
                        icon_resolve(cfg->entries[top->idx].icon, val);
                }
            } else if (ieq(tok, "timeout")) {
                cfg->timeout = to_int(val);
            } else if (ieq(tok, "default")) {
                /* All digits -> N-th top-level row (resolved after parse);
                 * otherwise a title path kept raw in cfg->default_path. The
                 * LAST default= key wins. */
                if (all_digits(val)) {
                    pending_default = to_int(val);
                    cfg->default_path[0] = '\0';
                } else {
                    scopy(cfg->default_path, val, FOREB_CFG_PATH_LEN);
                    pending_default = -1;
                }
            } else if (ieq(tok, "remember_last")) {
                cfg->remember_last = to_bool(val);
            } else if (ieq(tok, "background")) {
                scopy(cfg->background, val, FOREB_CFG_PATH_LEN);
            } else if (audio_set(tok, val)) {
                /* pcspeaker / audio_* tones (file-static g_audio_cfg) */
            } else {
                /* theme / customization keys (color_*, cursor, toggles, skin,
                 * btn_/ui_/fx_/menu_ appearance) */
                theme_set(&cfg->theme, tok, val);
            }
            /* unknown global keys ignored (theme_set returns 0, harmless) */
            continue;
        }

        /* stray '{', '=', or string: ignore and continue */
    }

    resolve_default(cfg, pending_default);
    if (cfg->timeout < 0) cfg->timeout = 0;
    return cfg->count;
}

/* =============================================================================
 * Built-in default (matches today's 4 menu entries when no forebo.cfg exists)
 * ========================================================================== */
void forebo_config_default(struct forebo_config *cfg)
{
    if (!cfg) return;
    forebo_cfg_init(cfg);

    struct forebo_menuentry *e;

    /* 0: Forest OS (default boot) */
    e = &cfg->entries[0];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_FOREST;
    scopy(e->title,   "Forest OS",            FOREB_CFG_TITLE_LEN);
    scopy(e->kernel,  "\\forebo\\kernel.elf", FOREB_CFG_PATH_LEN);
    icon_resolve(e->icon, "os");
    e->cmdline[0] = '\0';

    /* 1: Forest OS (no framebuffer) */
    e = &cfg->entries[1];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_FOREST;
    scopy(e->title,   "Forest OS (no framebuffer)", FOREB_CFG_TITLE_LEN);
    scopy(e->kernel,  "\\forebo\\kernel.elf",       FOREB_CFG_PATH_LEN);
    scopy(e->cmdline, "nofb",                       FOREB_CFG_CMDLINE_LEN);
    icon_resolve(e->icon, "text");

    /* 2: Forest OS (safe mode) */
    e = &cfg->entries[2];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_FOREST;
    scopy(e->title,   "Forest OS (safe mode)", FOREB_CFG_TITLE_LEN);
    scopy(e->kernel,  "\\forebo\\kernel.elf",  FOREB_CFG_PATH_LEN);
    scopy(e->cmdline, "safe nofb",             FOREB_CFG_CMDLINE_LEN);
    icon_resolve(e->icon, "safe");

    /* 3: ForeB Shell (opens the interactive shell window from the menu) */
    e = &cfg->entries[3];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_SHELL;
    scopy(e->title, "ForeB Shell", FOREB_CFG_TITLE_LEN);
    icon_resolve(e->icon, "terminal");

    /* 4: Recovery / Disk Tools (opens the recovery window) */
    e = &cfg->entries[4];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_RECOVERY;
    scopy(e->title, "Recovery / Disk Tools", FOREB_CFG_TITLE_LEN);
    icon_resolve(e->icon, "gear");

    /* 5: Tools (opens the windowed GUI Tools launcher) */
    e = &cfg->entries[5];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_TOOLS;
    scopy(e->title, "Tools", FOREB_CFG_TITLE_LEN);
    icon_resolve(e->icon, "gear");

    /* 6: Firmware Setup (reboot into the UEFI/BIOS setup screen) */
    e = &cfg->entries[6];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_FWSETUP;
    scopy(e->title, "Firmware Setup (UEFI)", FOREB_CFG_TITLE_LEN);
    icon_resolve(e->icon, "settings");

    /* 7: Reboot (firmware reset pseudo-entry) */
    e = &cfg->entries[7];
    zero(e, sizeof(*e));
    e->parent = -1;
    e->type = FOREB_ENTRY_REBOOT;
    scopy(e->title,  "Reboot", FOREB_CFG_TITLE_LEN);
    scopy(e->kernel, "reboot", FOREB_CFG_PATH_LEN);
    icon_resolve(e->icon, "reboot");

    cfg->count       = 8;
    cfg->default_idx = 0;
    cfg->timeout     = FOREB_CFG_DEFAULT_TIMEOUT;
    (void)slen;   /* silence unused in some build configs */
}

int forebo_config_load(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                       const char *path, struct forebo_config *cfg)
{
    if (!cfg) return 0;
    forebo_cfg_init(cfg);

    void *buf = NULL;
    UINTN size = 0;
    EFI_STATUS st = esp_read_file(image, bs, path ? path : FOREB_CFG_ESP_PATH,
                                  &buf, &size);
    if (EFI_ERROR(st) || !buf || size == 0) {
        forebo_config_default(cfg);
        if (buf) esp_free_file(bs, buf);
        return cfg->count;
    }

    forebo_cfg_parse(cfg, (const char *)buf, (unsigned long)size);
    esp_free_file(bs, buf);

    if (cfg->count == 0)
        forebo_config_default(cfg);
    return cfg->count;
}
