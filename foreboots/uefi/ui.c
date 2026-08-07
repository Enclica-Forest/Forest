/*
 * ui.c - GOP framebuffer UI implementation for the ForeB UEFI loader.
 *
 * Self-contained, freestanding (no libc). Writes 32bpp pixels straight to
 * the GOP linear framebuffer. Handles BGRX (x86 default) and RGBX byte
 * orders by conditionally swapping R<->B at store time. Valid before and
 * after ExitBootServices (uses only the raw framebuffer address).
 */
#include "ui.h"
#include "image.h"                   /* struct img_image + img_blit_scaled     */
#include "../include/font8x16.h"
#include "../include/forebo_theme.h"
#include "../include/forebo_cfg.h"   /* struct forebo_style + menu-style enums */
#include "arch.h"                 /* FOREB_ARCH_IS_X64 for the WC guard + fence */

/* Optional custom menu-panel face (img_panel=); NULL = drawn fill. */
static const struct img_image *g_panel_img = 0;
void ui_set_panel_image(const struct img_image *img) { g_panel_img = img; }

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */
/* g_fb / g_pitch are the DRAW target: they point at the off-screen RAM back
 * buffer when one was allocated, otherwise straight at VRAM. Every primitive
 * (put_pixel/fill_rect and everything built on them) writes through g_fb, so
 * the whole draw stack redirects to the back buffer with zero changes below. */
static volatile UINT8 *g_fb    = 0;   /* DRAW target base as byte ptr   */
static UINT32 g_pitch          = 0;   /* DRAW target bytes per scanline */
static UINT32 g_w              = 0;   /* width  in pixels              */
static UINT32 g_h              = 0;   /* height in pixels              */
static int    g_swap_rb        = 0;   /* 1 => framebuffer is RGBX      */
static int    g_uiscale        = 1;   /* auto 2x on hi-res (>=1080p)   */

/* Double-buffer plumbing. g_back is the AllocatePool'd RAM buffer (tight
 * g_back_pitch = width*4 stride) that g_fb aliases; g_front/g_front_pitch is
 * the GOP VRAM front buffer ui_present() copies it to. g_back == NULL means the
 * allocation failed (or no BootServices) and g_fb points straight at VRAM, in
 * which case ui_present() is a no-op and behavior matches the pre-DB loader. */
static volatile UINT8 *g_front = 0;   /* VRAM front buffer base         */
static UINT32 g_front_pitch    = 0;   /* VRAM bytes per scanline        */
static UINT8  *g_back          = 0;   /* RAM back buffer (NULL => none) */
static UINT32 g_back_pitch     = 0;   /* back buffer bytes per scanline */
static EFI_BOOT_SERVICES *g_bs = 0;   /* for FX scratch AllocatePool    */

/* -----------------------------------------------------------------------------
 * Dirty-rectangle presentation.
 * -----------------------------------------------------------------------------
 * The whole reason the UI "works on the emulator but crawls on real hardware":
 * ui_present() used to copy the ENTIRE back buffer to the GOP framebuffer every
 * frame. Under QEMU that framebuffer is ordinary cached RAM, so an 8 MB/ frame
 * copy is invisible; on real hardware it is uncached / write-combining MMIO and
 * an 8 MB blit at 60 fps saturates the bus -> visible lag.
 *
 * Fix: track, per scanline, the [min,max) column extent that actually changed
 * this frame, and copy ONLY those spans to VRAM. Because we also remember the
 * PREVIOUS frame's dirty extent, restoring the cached background (which erases
 * last frame's cursor/particles/windows) is flushed correctly without marking
 * the whole screen. Every draw primitive funnels through fill_rect/put_pixel,
 * so tracking there captures the entire ui.c draw stack; sibling writers
 * (anim.c particles) call ui_mark_dirty() directly. */
static int   g_dirty_track = 0;       /* per-row span arrays allocated  */
static int  *g_cmin = 0, *g_cmax = 0; /* this-frame span per row        */
static int  *g_pmin = 0, *g_pmax = 0; /* last-frame span per row        */
static int   g_cy0 = 0, g_cy1 = -1;   /* this-frame touched row range   */
static int   g_py0 = 0, g_py1 = -1;   /* last-frame touched row range   */
static int   g_present_full = 1;      /* force a whole-screen flip once */

/* VSync: gate full-screen flips on the VGA vertical-retrace so a whole-frame
 * content swap lands during blanking (no visible tear) on real x86 HW whose GPU
 * keeps legacy VGA I/O decode alive. g_vsync_state: -1 = not yet probed, 0 =
 * no usable retrace signal (pure GOP -> never wait, zero cost), 1 = usable.
 * g_vsync_enabled is the runtime master switch (default on). Partial (cursor/
 * particle) flips are NEVER gated - they are tiny and their tear is invisible,
 * and gating them would cap the loop to the refresh rate + add input latency. */
static int   g_vsync_state   = -1;
static int   g_vsync_enabled = 1;

/* -----------------------------------------------------------------------------
 * Clip-rect stack.
 * -----------------------------------------------------------------------------
 * Every primitive intersects its output with the current clip rectangle
 * (default: the whole screen). The compositor (wm.c) pushes each window's
 * visible region before painting it, so windows/panels that are covered by
 * opaque windows above them skip real pixel work instead of overdraw-blind
 * repainting (this is what kept adding lag per opened panel). ui_clip_reset()
 * restores the full-screen default; the stack is small and purely internal. */
#define UI_CLIP_MAX 8
static int g_clx0 = 0, g_cly0 = 0;            /* active clip: [x0,y0)-(x1,y1) */
static int g_clx1 = 0x7FFFFFFF, g_cly1 = 0x7FFFFFFF;
/* defined in the clip-rect section below; ui_init() needs ui_clip_reset(). */
void ui_clip_reset(void);
void ui_clip_push(int x, int y, int w, int h);
void ui_clip_pop(void);
void ui_clip_get(int *x, int *y, int *w, int *h);
static int g_clip_sx0[UI_CLIP_MAX], g_clip_sy0[UI_CLIP_MAX];
static int g_clip_sx1[UI_CLIP_MAX], g_clip_sy1[UI_CLIP_MAX];
static int g_clip_n = 0;

/* -----------------------------------------------------------------------------
 * Runtime theme palette.
 * -----------------------------------------------------------------------------
 * The menu/background used to hard-code the FOREB_* forest colors. To support
 * multiple selectable skins we resolve those names to a runtime struct instead,
 * defaulted to the original forest values so nothing changes until a theme is
 * chosen. ui_set_theme_by_name() swaps the whole palette; ui_theme_override()
 * lets forebo.cfg's individual color_* keys tweak single entries on top. */
struct ui_theme {
    UINT32 bg, bg_top, bg_bottom, panel, border, select, title, text,
           dim, timer, white, shadow, tree1, tree2, tree3, prog_track,
           prog_fill, accent;
};
static struct ui_theme g_pal = {
    FOREB_BG, FOREB_BG_TOP, FOREB_BG_BOTTOM, FOREB_PANEL, FOREB_BORDER,
    FOREB_SELECT, FOREB_TITLE, FOREB_TEXT, FOREB_DIM, FOREB_TIMER, FOREB_WHITE,
    FOREB_SHADOW, FOREB_TREE1, FOREB_TREE2, FOREB_TREE3, FOREB_BORDER,
    FOREB_TITLE, FOREB_TITLE
};

/* Redirect the color names to the live palette FOR THIS FILE ONLY (the header
 * macros stay intact for the BIOS path and config defaults). String macros like
 * FOREB_TITLE_STR / FOREB_PANEL_LABEL are untouched. */
#undef  FOREB_BG
#define FOREB_BG            (g_pal.bg)
#undef  FOREB_BG_TOP
#define FOREB_BG_TOP        (g_pal.bg_top)
#undef  FOREB_BG_BOTTOM
#define FOREB_BG_BOTTOM     (g_pal.bg_bottom)
#undef  FOREB_PANEL
#define FOREB_PANEL         (g_pal.panel)
#undef  FOREB_BORDER
#define FOREB_BORDER        (g_pal.border)
#undef  FOREB_SELECT
#define FOREB_SELECT        (g_pal.select)
#undef  FOREB_TITLE
#define FOREB_TITLE         (g_pal.title)
#undef  FOREB_TEXT
#define FOREB_TEXT          (g_pal.text)
#undef  FOREB_DIM
#define FOREB_DIM           (g_pal.dim)
#undef  FOREB_TIMER
#define FOREB_TIMER         (g_pal.timer)
#undef  FOREB_WHITE
#define FOREB_WHITE         (g_pal.white)
#undef  FOREB_SHADOW
#define FOREB_SHADOW        (g_pal.shadow)
#undef  FOREB_TREE1
#define FOREB_TREE1         (g_pal.tree1)
#undef  FOREB_TREE2
#define FOREB_TREE2         (g_pal.tree2)
#undef  FOREB_TREE3
#define FOREB_TREE3         (g_pal.tree3)
#undef  FOREB_PROGRESS_TRACK
#define FOREB_PROGRESS_TRACK (g_pal.prog_track)
#undef  FOREB_PROGRESS_FILL
#define FOREB_PROGRESS_FILL  (g_pal.prog_fill)

/* Accent color (selection bar, focus, particle tint). Not a FOREB_* name. */
UINT32 ui_theme_accent(void)  { return g_pal.accent; }
UINT32 ui_theme_title(void)   { return g_pal.title; }

/* Case-insensitive ASCII compare (freestanding). */
static int ui_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Named palettes. Field order matches struct ui_theme:
 * bg, bg_top, bg_bottom, panel, border, select, title, text, dim, timer,
 * white, shadow, tree1, tree2, tree3, prog_track, prog_fill, accent */
struct ui_named_theme { const char *name; struct ui_theme p; };
static const struct ui_named_theme g_themes[] = {
  { "forest", {
    0x00182D18,0x00102010,0x001E3A1E,0x001C351C,0x00285128,0x00146514,
    0x0051CA3D,0x00B6DFB6,0x00658265,0x00DFA214,0x00FFFFFF,0x00040804,
    0x003D1C08,0x001C791C,0x003DB63D,0x00285128,0x0051CA3D,0x0051CA3D } },
  { "midnight", {
    0x000B1020,0x00070B18,0x00131C33,0x00131B2E,0x002B3B5C,0x001E3A66,
    0x006AA9FF,0x00C7D6EE,0x006A7C99,0x00FFC24B,0x00FFFFFF,0x00030509,
    0x001C2A3A,0x00274A6E,0x004A82C0,0x002B3B5C,0x006AA9FF,0x006AA9FF } },
  { "nord", {
    0x002E3440,0x00272C36,0x003B4252,0x00343B49,0x004C566A,0x00434C5E,
    0x0088C0D0,0x00ECEFF4,0x00818C9C,0x00EBCB8B,0x00ECEFF4,0x00191C22,
    0x004C566A,0x005E81AC,0x0081A1C1,0x004C566A,0x00A3BE8C,0x0088C0D0 } },
  { "dracula", {
    0x00282A36,0x0021222C,0x00343746,0x0031333F,0x0044475A,0x00454863,
    0x00BD93F9,0x00F8F8F2,0x006272A4,0x00FFB86C,0x00FFFFFF,0x00121319,
    0x00343746,0x006272A4,0x00BD93F9,0x0044475A,0x0050FA7B,0x00FF79C6 } },
  { "gruvbox", {
    0x00282828,0x001D2021,0x00323030,0x00323028,0x00504945,0x00453C30,
    0x00FABD2F,0x00EBDBB2,0x00A89984,0x00FE8019,0x00FBF1C7,0x00120F0F,
    0x003C3836,0x00689D6A,0x00B8BB26,0x00504945,0x00B8BB26,0x00FABD2F } },
  { "solarized", {
    0x00002B36,0x00001F27,0x00073642,0x00073642,0x00586E75,0x00094A56,
    0x00268BD2,0x0093A1A1,0x00657B83,0x00B58900,0x00FDF6E3,0x00001015,
    0x00073642,0x002AA198,0x00859900,0x00586E75,0x002AA198,0x00268BD2 } },
  { "amber", {   /* retro CRT: black + phosphor amber */
    0x00120A00,0x000A0600,0x001E1200,0x001A1200,0x004A3300,0x003B2600,
    0x00FFB000,0x00FFCC55,0x00A87A20,0x00FF7818,0x00FFE0A0,0x00080400,
    0x00201400,0x00805000,0x00FFB000,0x004A3300,0x00FFB000,0x00FFB000 } },
  { "matrix", {  /* green phosphor on black */
    0x00001200,0x00000A00,0x00002200,0x00001A00,0x00105010,0x00073807,
    0x0000FF41,0x0090FFA0,0x00309040,0x0000FF41,0x00D0FFD8,0x00000600,
    0x00003000,0x00008820,0x0000FF41,0x00105010,0x0000FF41,0x0000FF41 } },
  { "rose", {    /* rose-pine dawn-ish dark */
    0x00191724,0x0012101B,0x00232135,0x00232135,0x00403D52,0x002A2740,
    0x00EBBCBA,0x00E0DEF4,0x00908CAA,0x00F6C177,0x00FFFFFF,0x000C0B12,
    0x00232135,0x00524F67,0x00C4A7E7,0x00403D52,0x00EBBCBA,0x00EB6F92 } },
  { "ocean", {   /* deep teal */
    0x000A1E24,0x00061418,0x00113038,0x00102A32,0x00285561,0x0013414C,
    0x0033C5D8,0x00CDECEF,0x005F8A92,0x00FFC24B,0x00FFFFFF,0x00030A0C,
    0x00123840,0x001C6E78,0x0033C5D8,0x00285561,0x0040D0A0,0x0033C5D8 } },
  { "mono", {    /* neutral grayscale */
    0x00141414,0x000E0E0E,0x00202020,0x001E1E1E,0x00404040,0x00343434,
    0x00E0E0E0,0x00C8C8C8,0x00808080,0x00E0E0E0,0x00FFFFFF,0x00060606,
    0x00303030,0x00707070,0x00B0B0B0,0x00404040,0x00E0E0E0,0x00E0E0E0 } },
};
#define UI_NUM_THEMES ((int)(sizeof(g_themes)/sizeof(g_themes[0])))

int ui_set_theme_by_name(const char *name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < UI_NUM_THEMES; i++) {
        if (ui_ieq(name, g_themes[i].name)) { g_pal = g_themes[i].p; return 1; }
    }
    return 0;   /* unknown name: keep current palette */
}

const char *ui_theme_name(int i)
{
    if (i < 0 || i >= UI_NUM_THEMES) return 0;
    return g_themes[i].name;
}
int ui_theme_count(void) { return UI_NUM_THEMES; }

/* Overlay a single non-zero, non-"unset" override onto the live palette. */
static void ui_pal_set(UINT32 *dst, UINT32 v)
{
    if (v != 0 && v != 0x00FFFFFFFFu) *dst = v & 0x00FFFFFFu;
}
void ui_theme_override(UINT32 bg, UINT32 fg, UINT32 accent,
                       UINT32 sel_bg, UINT32 sel_fg)
{
    ui_pal_set(&g_pal.bg, bg);
    ui_pal_set(&g_pal.bg_bottom, bg);
    ui_pal_set(&g_pal.text, fg);
    if (accent && accent != 0x00FFFFFFFFu) {
        g_pal.accent = accent & 0x00FFFFFFu;
        g_pal.title  = accent & 0x00FFFFFFu;
        g_pal.prog_fill = accent & 0x00FFFFFFu;
    }
    ui_pal_set(&g_pal.select, sel_bg);
    ui_pal_set(&g_pal.white,  sel_fg);
}

/* =============================================================================
 * Menu STYLE engine.
 * -----------------------------------------------------------------------------
 * g_rs is the fully-resolved style the layout + renderer read. It is built from
 * a concrete BASE, then a named preset's deltas, then the user's per-field
 * config overrides (any field != -1). Presets only list what differs from BASE,
 * which keeps 30 of them readable. Geometry fields (-1) mean "auto from pos".
 * ========================================================================== */
static struct forebo_style g_rs;

static const struct forebo_style STYLE_BASE = {
    "",                     /* preset  */
    FMP_CENTER,             /* pos     */
    -1, -1, -1, -1,         /* panel_x/y/w/h -> auto from pos                */
    -1,                     /* entry_h -> auto                              */
    14,                     /* pad px                                       */
    FAL_LEFT,               /* align                                        */
    FSS_DOUBLEBAR,          /* sel_style                                    */
    FBD_THICK,              /* border                                       */
    FCN_SQUARE,             /* corner                                       */
    1,                      /* accent_strip                                 */
    0,                      /* dividers                                     */
    1,                      /* gradient                                     */
    1,                      /* shadow                                       */
    1,                      /* title_bar                                    */
    1,                      /* show_title (scene title)                     */
    1,                      /* show_footer                                  */
    1,                      /* show_timer                                   */
    1,                      /* show_icons                                   */
    1,                      /* icon_right                                   */
    1,                      /* show_scrollbar                               */
    1                       /* show_caret                                   */
};

static int ui_ieq2(const char *a, const char *b) { return ui_ieq(a, b); }

/* Apply the named preset's deltas on top of BASE (already in *r). Returns 1 if
 * the name matched a preset (or is empty -> classic), 0 for unknown. */
static int style_preset_apply(struct forebo_style *r, const char *name)
{
    *r = STYLE_BASE;
    if (!name || !name[0] || ui_ieq2(name, "classic") || ui_ieq2(name, "forest"))
        return 1;                                   /* BASE is the classic look */

    if (ui_ieq2(name, "minimal")) {
        r->border=FBD_NONE; r->accent_strip=0; r->gradient=0; r->shadow=0;
        r->sel_style=FSS_ARROW; r->show_icons=0; r->title_bar=0; r->show_title=0;
    } else if (ui_ieq2(name, "terminal")) {
        r->align=FAL_LEFT; r->sel_style=FSS_BRACKET; r->border=FBD_THIN;
        r->gradient=0; r->accent_strip=0; r->dividers=1; r->show_icons=0;
        r->corner=FCN_SQUARE;
    } else if (ui_ieq2(name, "flat")) {
        r->gradient=0; r->shadow=0; r->border=FBD_THIN; r->sel_style=FSS_BAR;
        r->accent_strip=0;
    } else if (ui_ieq2(name, "modern")) {
        r->sel_style=FSS_PILL; r->corner=FCN_ROUND; r->border=FBD_NONE;
        r->gradient=1; r->shadow=1; r->accent_strip=1;
    } else if (ui_ieq2(name, "card")) {
        r->corner=FCN_ROUND; r->border=FBD_THICK; r->shadow=1; r->sel_style=FSS_BOX;
    } else if (ui_ieq2(name, "neon")) {
        r->sel_style=FSS_GLOW; r->border=FBD_GLOW; r->accent_strip=1; r->gradient=1;
    } else if (ui_ieq2(name, "outline")) {
        r->border=FBD_THIN; r->sel_style=FSS_OUTLINE; r->gradient=0; r->accent_strip=0;
    } else if (ui_ieq2(name, "underline")) {
        r->sel_style=FSS_UNDERLINE; r->border=FBD_NONE; r->dividers=1; r->gradient=0;
    } else if (ui_ieq2(name, "invert")) {
        r->sel_style=FSS_INVERT; r->border=FBD_THIN; r->gradient=0;
    } else if (ui_ieq2(name, "brackets")) {
        r->sel_style=FSS_BRACKET; r->align=FAL_CENTER; r->border=FBD_THIN;
    } else if (ui_ieq2(name, "sidebar-left") || ui_ieq2(name, "sidebar")) {
        r->pos=FMP_LEFT; r->align=FAL_LEFT; r->icon_right=0; r->show_title=0;
    } else if (ui_ieq2(name, "sidebar-right")) {
        r->pos=FMP_RIGHT; r->align=FAL_LEFT; r->show_title=0;
    } else if (ui_ieq2(name, "banner-top") || ui_ieq2(name, "top")) {
        r->pos=FMP_TOP; r->show_title=0;
    } else if (ui_ieq2(name, "dock-bottom") || ui_ieq2(name, "bottom")) {
        r->pos=FMP_BOTTOM;
    } else if (ui_ieq2(name, "fullscreen") || ui_ieq2(name, "full")) {
        r->pos=FMP_FULL; r->align=FAL_LEFT; r->show_title=0;
    } else if (ui_ieq2(name, "centered")) {
        r->pos=FMP_CENTER; r->align=FAL_CENTER; r->sel_style=FSS_BAR; r->show_icons=0;
    } else if (ui_ieq2(name, "compact")) {
        r->entry_h=42; r->pad=8; r->gradient=0; r->accent_strip=0;
    } else if (ui_ieq2(name, "spacious")) {
        r->entry_h=78; r->pad=20;
    } else if (ui_ieq2(name, "retro")) {
        r->border=FBD_DOUBLE; r->sel_style=FSS_BRACKET; r->align=FAL_LEFT;
        r->corner=FCN_CUT; r->gradient=0;
    } else if (ui_ieq2(name, "glass")) {
        r->gradient=1; r->border=FBD_THIN; r->sel_style=FSS_GRADIENT; r->accent_strip=1;
    } else if (ui_ieq2(name, "hacker") || ui_ieq2(name, "matrix")) {
        r->align=FAL_LEFT; r->sel_style=FSS_NONE; r->dividers=1; r->show_icons=0;
        r->gradient=0; r->border=FBD_THIN; r->accent_strip=0; r->show_title=0;
    } else if (ui_ieq2(name, "ribbon")) {
        r->accent_strip=1; r->sel_style=FSS_BAR; r->border=FBD_NONE; r->gradient=1;
    } else if (ui_ieq2(name, "framed")) {
        r->border=FBD_DOUBLE; r->corner=FCN_SQUARE; r->sel_style=FSS_BAR;
    } else if (ui_ieq2(name, "dashed")) {
        r->border=FBD_DASHED; r->sel_style=FSS_OUTLINE; r->gradient=0;
    } else if (ui_ieq2(name, "spotlight")) {
        r->sel_style=FSS_GLOW; r->accent_strip=0; r->border=FBD_NONE; r->gradient=1;
    } else if (ui_ieq2(name, "pill")) {
        r->sel_style=FSS_PILL; r->corner=FCN_ROUND; r->border=FBD_THIN;
    } else if (ui_ieq2(name, "boxed")) {
        r->sel_style=FSS_BOX; r->border=FBD_THICK; r->gradient=0;
    } else if (ui_ieq2(name, "ghost")) {
        r->border=FBD_NONE; r->gradient=0; r->shadow=0; r->sel_style=FSS_OUTLINE;
        r->accent_strip=0;
    } else if (ui_ieq2(name, "elegant")) {
        r->gradient=1; r->sel_style=FSS_UNDERLINE; r->accent_strip=1;
        r->align=FAL_LEFT; r->show_icons=0; r->corner=FCN_ROUND;
    } else {
        return 0;                                   /* unknown name */
    }
    return 1;
}

void ui_apply_style(const struct forebo_style *cfg)
{
    const char *name = (cfg && cfg->preset[0]) ? cfg->preset : "classic";
    style_preset_apply(&g_rs, name);
    if (!cfg) return;
    /* Overlay each explicitly-set (non -1) field on top of the preset. */
    #define UI_OV(f) do { if (cfg->f != -1) g_rs.f = cfg->f; } while (0)
    UI_OV(pos); UI_OV(panel_x); UI_OV(panel_y); UI_OV(panel_w); UI_OV(panel_h);
    UI_OV(entry_h); UI_OV(pad); UI_OV(align); UI_OV(sel_style); UI_OV(border);
    UI_OV(corner); UI_OV(accent_strip); UI_OV(dividers); UI_OV(gradient);
    UI_OV(shadow); UI_OV(title_bar); UI_OV(show_title); UI_OV(show_footer);
    UI_OV(show_timer); UI_OV(show_icons); UI_OV(icon_right); UI_OV(show_scrollbar);
    UI_OV(show_caret);
    #undef UI_OV
}

/* Accessors used by the compositor (icon layer honors these). */
int ui_style_show_icons(void) { return g_rs.show_icons ? 1 : 0; }
int ui_style_icon_right(void) { return g_rs.icon_right ? 1 : 0; }

int ui_style_count(void) { return 30; }
const char *ui_style_name(int i)
{
    static const char *const N[] = {
        "classic","minimal","terminal","flat","modern","card","neon","outline",
        "underline","invert","brackets","sidebar-left","sidebar-right",
        "banner-top","dock-bottom","fullscreen","centered","compact","spacious",
        "retro","glass","hacker","ribbon","framed","dashed","spotlight","pill",
        "boxed","ghost","elegant"
    };
    if (i < 0 || i >= 30) return 0;
    return N[i];
}

/* PixelRedGreenBlueReserved8BitPerColor is enum value 0 in efi.h. */
#define UI_PIXFMT_RGBX 0u

/* Boot-menu viewport state (owned by the caller through the setters below). */
static int g_menu_first = 0;    /* first visible entry (scroll offset)        */
static int g_menu_hl_y  = -1;   /* highlight-bar Y override; <0 => natural row */

/*
 * Integer fractional layout (permille of screen W/H). Mirrors the FOREB_F_*
 * fractions in forebo_theme.h but as integer numerators over 1000 so the UI
 * needs no floating point (built with -mno-sse, no soft-float, no _fltused).
 */
#define UI_FW(permille) ((int)(((UINTN)g_w * (UINTN)(permille)) / 1000u))
#define UI_FH(permille) ((int)(((UINTN)g_h * (UINTN)(permille)) / 1000u))

#define UIP_TITLEBAR_Y   47
#define UIP_TITLE_Y     100
#define UIP_MARGIN_X     20
#define UIP_LOGO_CY     230
#define UIP_LOGO_W      140
#define UIP_LOGO_H      170
#define UIP_PANEL_X     200
#define UIP_PANEL_Y     360
#define UIP_PANEL_W     600
#define UIP_PANEL_H     420
#define UIP_ENTRY_H      55
#define UIP_PROGRESS_X  220
#define UIP_PROGRESS_Y  860
#define UIP_PROGRESS_W  560
#define UIP_PROGRESS_H   30
#define UIP_FOOTER_Y    940
#define UIP_HALF        500

/* Minimal slice of the DXE CPU Architectural Protocol, enough to reach
 * SetMemoryAttributes(). Layout is fixed by the PI spec and has been stable for
 * ~20 years; only SetMemoryAttributes is typed, the earlier members are opaque
 * pointers so the eighth slot lands at the right offset. Used best-effort in
 * ui_init() to hint the GOP framebuffer as write-combining; never dereferenced
 * beyond that one guarded call. */
#if FOREB_ARCH_IS_X64
typedef EFI_STATUS (EFIAPI *UI_CPU_SET_MEMORY_ATTRIBUTES)(
    VOID *This, EFI_PHYSICAL_ADDRESS BaseAddress, UINT64 Length,
    UINT64 Attributes);
struct ui_cpu_arch_protocol {
    VOID *FlushDataCache;
    VOID *EnableInterrupt;
    VOID *DisableInterrupt;
    VOID *GetInterruptState;
    VOID *Init;
    VOID *RegisterInterruptHandler;
    VOID *GetTimerValue;
    UI_CPU_SET_MEMORY_ATTRIBUTES SetMemoryAttributes;
    UINT32 NumberOfTimers;
    UINT32 DmaBufferAlignment;
};
/* EFI_CPU_ARCH_PROTOCOL_GUID = 26baccb1-6f42-11d4-9a38-0090273fc14d */
static EFI_GUID g_cpu_arch_guid =
    { 0x26baccb1, 0x6f42, 0x11d4,
      { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
#endif /* FOREB_ARCH_IS_X64 */

void ui_init(EFI_BOOT_SERVICES *bs, UINT64 fb_base, UINT32 pitch,
             UINT32 width, UINT32 height, UINT32 pixfmt)
{
    g_bs          = bs;
    g_front       = (volatile UINT8 *)(UINTN)fb_base;
    g_front_pitch = pitch;
    g_w           = width;
    g_h           = height;
    g_swap_rb     = (pixfmt == UI_PIXFMT_RGBX) ? 1 : 0;
    /* Auto-pick an integer magnification: the crisp 8x16 cell doubles to a
     * 16x32 cell on 1080p+ panels so text stays legible at high DPI. */
    g_uiscale     = (height >= 1080) ? 2 : 1;

    /* Allocate the off-screen back buffer (tight width*4 stride). It stores
     * NATIVE (already R/B-packed) framebuffer words exactly as VRAM would, so
     * ui_present() is a straight copy with no per-pixel conversion. */
    g_back       = 0;
    g_back_pitch = 0;
    if (bs && width && height) {
        VOID *p = 0;
        UINTN bytes = (UINTN)width * (UINTN)height * 4u;
        if (!EFI_ERROR(bs->AllocatePool(EfiLoaderData, bytes, &p)) && p) {
            g_back       = (UINT8 *)p;
            g_back_pitch = width * 4u;
        }
    }

    if (g_back) {
        g_fb    = (volatile UINT8 *)g_back;   /* draw into RAM   */
        g_pitch = g_back_pitch;
    } else {
        g_fb    = g_front;                     /* fall back to VRAM */
        g_pitch = g_front_pitch;
    }

    ui_apply_style(0);   /* seed the resolved style with the classic defaults */
    ui_clip_reset();     /* default clip = whole screen                        */

    /* Per-scanline dirty-span arrays for partial presentation. Only useful when
     * a real back buffer exists (otherwise draws already hit VRAM). On failure
     * we simply fall back to full-frame flips (correct, just slower). */
    g_dirty_track = 0;
    g_cmin = g_cmax = g_pmin = g_pmax = 0;
    g_present_full = 1;
    if (g_back && bs && height) {
        VOID *a = 0, *b = 0, *c = 0, *d = 0;
        UINTN n = (UINTN)height * sizeof(int);
        if (!EFI_ERROR(bs->AllocatePool(EfiLoaderData, n, &a)) &&
            !EFI_ERROR(bs->AllocatePool(EfiLoaderData, n, &b)) &&
            !EFI_ERROR(bs->AllocatePool(EfiLoaderData, n, &c)) &&
            !EFI_ERROR(bs->AllocatePool(EfiLoaderData, n, &d)) &&
            a && b && c && d) {
            g_cmin = (int *)a; g_cmax = (int *)b;
            g_pmin = (int *)c; g_pmax = (int *)d;
            for (UINT32 i = 0; i < height; i++) {
                g_cmin[i] = (int)width; g_cmax[i] = 0;
                g_pmin[i] = (int)width; g_pmax[i] = 0;
            }
            g_cy0 = g_py0 = (int)height; g_cy1 = g_py1 = -1;
            g_dirty_track = 1;
        } else {
            /* Partial allocation failed: free whatever succeeded so those pool
             * blocks are not orphaned (we fall back to full-frame flips). */
            if (a) bs->FreePool(a); if (b) bs->FreePool(b);
            if (c) bs->FreePool(c); if (d) bs->FreePool(d);
        }
    }

    /* Best-effort: ask the CPU arch protocol to map the GOP framebuffer
     * write-combining. On real hardware VRAM often defaults to strongly
     * uncached (UC), where every store stalls on the bus; WC lets
     * ui_present()'s sequential back->front copy (and any VRAM-fallback draw)
     * post through the write-combine buffers instead. Purely a hint: a missing
     * protocol, an unaligned range, or an unsupported attribute all just leave
     * the mapping unchanged and the loader behaves exactly as before. Runs only
     * here, inside BootServices; ui_blit_row's sequential stores are untouched. */
#if FOREB_ARCH_IS_X64
    /* WC is x86-only and safe ONLY for ui_blit_row's sequential write-only copy,
     * i.e. only when a real back buffer exists. Best-effort, scoped to the
     * pre-ExitBootServices UI phase: handoff64to32.asm clears CR0.PG at handoff
     * so any PAT-based WC is dropped then -- fine, no drawing runs after EBS. */
    if (bs && g_back && g_front && g_front_pitch && height) {   /* + g_back */
        struct ui_cpu_arch_protocol *cpu = 0;
        if (!EFI_ERROR(bs->LocateProtocol(&g_cpu_arch_guid, 0, (VOID **)&cpu)) &&
            cpu && cpu->SetMemoryAttributes) {
            UINT64 pgmask = (UINT64)EFI_PAGE_SIZE - 1u;
            UINT64 base   = (UINT64)(UINTN)g_front;
            UINT64 end    = base + (UINT64)g_front_pitch * (UINT64)height;
            base &= ~pgmask;                       /* page-align the range so   */
            end   = (end + pgmask) & ~pgmask;      /* firmware is more likely to */
            (void)cpu->SetMemoryAttributes(cpu, base, end - base,
                                           EFI_MEMORY_WC);   /* ignore result */
        }
    }
#endif /* FOREB_ARCH_IS_X64 */
}

/* Expand this frame's dirty region to include the rect (x,y,w,h). Clipped to
 * the screen. Cheap: touches only the covered scanlines' span endpoints. */
void ui_mark_dirty(int x, int y, int w, int h)
{
    if (!g_dirty_track || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((UINT32)x >= g_w || (UINT32)y >= g_h) return;
    if ((UINT32)(x + w) > g_w) w = (int)g_w - x;
    if ((UINT32)(y + h) > g_h) h = (int)g_h - y;
    if (w <= 0 || h <= 0) return;

    int x2 = x + w, y2 = y + h;
    for (int yy = y; yy < y2; yy++) {
        if (x  < g_cmin[yy]) g_cmin[yy] = x;
        if (x2 > g_cmax[yy]) g_cmax[yy] = x2;
    }
    if (y      < g_cy0) g_cy0 = y;
    if (y2 - 1 > g_cy1) g_cy1 = y2 - 1;
}

/* Force the next ui_present() to flip the whole screen (use after a full-screen
 * repaint that bypassed the primitives, e.g. an image blit or a fade). */
void ui_mark_all(void) { g_present_full = 1; }

/* -------- clip-rect stack (see the block comment above) ------------------ */
void ui_clip_reset(void)
{
    g_clx0 = 0; g_cly0 = 0;
    g_clx1 = (int)g_w; g_cly1 = (int)g_h;
    g_clip_n = 0;
}

/* Intersect the active clip with (x,y,w,h) and make it current. */
void ui_clip_push(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x  < g_clx0) x  = g_clx0;
    if (y  < g_cly0) y  = g_cly0;
    if (x1 > g_clx1) x1 = g_clx1;
    if (y1 > g_cly1) y1 = g_cly1;
    if (g_clip_n < UI_CLIP_MAX) {
        g_clip_sx0[g_clip_n] = g_clx0; g_clip_sy0[g_clip_n] = g_cly0;
        g_clip_sx1[g_clip_n] = g_clx1; g_clip_sy1[g_clip_n] = g_cly1;
        g_clip_n++;
    }
    g_clx0 = x; g_cly0 = y; g_clx1 = x1; g_cly1 = y1;
}

void ui_clip_pop(void)
{
    if (g_clip_n <= 0) return;
    g_clip_n--;
    g_clx0 = g_clip_sx0[g_clip_n]; g_cly0 = g_clip_sy0[g_clip_n];
    g_clx1 = g_clip_sx1[g_clip_n]; g_cly1 = g_clip_sy1[g_clip_n];
}

void ui_clip_get(int *x, int *y, int *w, int *h)
{
    if (x) *x = g_clx0;
    if (y) *y = g_cly0;
    if (w) *w = g_clx1 - g_clx0;
    if (h) *h = g_cly1 - g_cly0;
}

UINT32 ui_width(void)  { return g_w; }
UINT32 ui_height(void) { return g_h; }
int    ui_scale(void)  { return g_uiscale; }

/* ------------------------------------------------------------------ */
/*  Double buffering: accessors + present + clear                      */
/* ------------------------------------------------------------------ */
UINT64 ui_backbuffer_base(void) { return (UINT64)(UINTN)g_fb; }
UINT32 ui_draw_pitch(void)      { return g_pitch; }
int    ui_double_buffered(void) { return g_back ? 1 : 0; }

/* Drain write-combining buffers so a WC-mapped framebuffer frame is fully
 * committed to VRAM before scanout / before the next frame overwrites it.
 * x86 SFENCE is the WC drain; the "memory" clobber also stops the non-volatile
 * blit stores from sinking past it. Other arches get a compiler barrier only,
 * so the build stays clean and the call is a non-fatal no-op. */
static inline void ui_wc_fence(void)
{
#if FOREB_ARCH_IS_X64
    __asm__ __volatile__("sfence" ::: "memory");
#elif FOREB_ARCH_IS_AA64
    __asm__ __volatile__("dsb st" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Block copy RAM->VRAM. On x86 this is a single `rep movsq` (+ byte tail):
 * with Enhanced REP MOVSB/STOSB (ERMSB, ~every CPU since Ivy Bridge) it is the
 * fastest large memory copy available and is the correct primitive for a WC or
 * UC framebuffer - the microcode streams full cache-line / WC-buffer bursts that
 * a hand C loop at -O0 cannot match. It is alignment-agnostic, so it serves both
 * the whole-screen flip AND arbitrary partial spans. `bytes` need not be a
 * multiple of 8. Non-x86 arches fall back to a plain byte loop (kept correct;
 * aarch64 UEFI VRAM copies are rare and small here). Caller issues the WC fence. */
static inline void ui_vram_copy(volatile void *dst, const void *src, UINTN bytes)
{
#if FOREB_ARCH_IS_X64
    /* Small spans (the per-frame cursor/particle rows the animated menu flips
     * every tick) are dominated by rep-movs microcode STARTUP latency, so a
     * straight 32-bit word loop is faster there. Only large copies (full or
     * large flips) amortize the startup and win from rep movsq streaming at
     * memory bandwidth. Pixel data => bytes is a multiple of 4; a byte tail
     * covers any odd caller defensively. */
    if (bytes < 512u) {
        volatile UINT8 *d8 = (volatile UINT8 *)dst;
        const UINT8 *s8 = (const UINT8 *)src;
        UINTN w = bytes & ~(UINTN)3, i = 0;
        for (; i < w; i += 4)
            *(volatile UINT32 *)(d8 + i) = *(const UINT32 *)(s8 + i);
        for (; i < bytes; i++) d8[i] = s8[i];
        return;
    }
    UINTN q = bytes >> 3;                 /* 8-byte chunks (rep movsq)          */
    UINTN r = bytes & 7u;                 /* 0..7 byte remainder (rep movsb)    */
    __asm__ __volatile__("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) :: "memory");
    __asm__ __volatile__("rep movsb" : "+D"(dst), "+S"(src), "+c"(r) :: "memory");
#else
    volatile UINT8 *d = (volatile UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < bytes; i++) d[i] = s[i];
#endif
}

/* Flip `n` 32-bit pixels of scanline `fy` starting at x from back->front. */
static void ui_blit_row(UINT32 fy, int x, int n)
{
    if (n <= 0) return;
    volatile UINT8 *d = g_front + (UINTN)fy * g_front_pitch + (UINTN)x * 4u;
    const UINT8 *s = g_back + (UINTN)fy * g_back_pitch + (UINTN)x * 4u;
    ui_vram_copy(d, s, (UINTN)n * 4u);
}

static void ui_present_full(void)
{
    if (g_front_pitch == g_back_pitch) {
        /* Contiguous same-stride flip: one unbroken block copy of the whole
         * frame - rep movsq streams it at memory bandwidth. */
        ui_vram_copy(g_front, g_back, (UINTN)g_back_pitch * (UINTN)g_h);
    } else {
        /* Padded GOP stride (PixelsPerScanLine > width) - the common real-HW
         * case: copy each visible scanline's width, skipping the pad. Still one
         * fast rep-movsq run per row, not a scalar per-pixel loop. */
        UINTN row_bytes = (UINTN)g_w * 4u;
        for (UINT32 y = 0; y < g_h; y++)
            ui_vram_copy(g_front + (UINTN)y * g_front_pitch,
                         g_back  + (UINTN)y * g_back_pitch, row_bytes);
    }
    ui_wc_fence();   /* flush WC buffers to VRAM (full flip) */
}

#if FOREB_ARCH_IS_X64
static inline UINT8 ui_inb(UINT16 port)
{
    UINT8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
#endif

/* Probe for a live VGA vertical-retrace signal on the input-status register
 * (0x3DA bit 3). Returns 1 only if the bit actually TOGGLES within a bounded
 * sample budget - proof the GPU still decodes VGA I/O and is scanning out. On
 * pure-GOP hardware the port reads a constant (0x00/0xFF) and we return 0 so we
 * never busy-wait on a signal that will not come. x86-only. */
static int ui_vsync_probe(void)
{
#if FOREB_ARCH_IS_X64
    UINT8 first = ui_inb(0x3DA) & 0x08u;
    for (int i = 0; i < 1000000; i++)
        if ((ui_inb(0x3DA) & 0x08u) != first) return 1;   /* toggled -> live */
    return 0;
#else
    return 0;
#endif
}

/* Block until the START of the next vertical blank, so the flip that follows is
 * written into VRAM during retrace. Every wait is bounded by a spin guard so a
 * mis-probe or a signal that stops toggling can never hang the loader. x86-only;
 * a no-op elsewhere. Only called when g_vsync_state == 1. */
static void ui_wait_vblank(void)
{
#if FOREB_ARCH_IS_X64
    int guard = 4000000;
    while ((ui_inb(0x3DA) & 0x08u) && --guard) { }    /* leave any active retrace */
    guard = 4000000;
    while (!(ui_inb(0x3DA) & 0x08u) && --guard) { }   /* wait for retrace to begin */
#endif
}

/* Gate a full-screen flip on vblank when a retrace signal is available. Probes
 * lazily on first use; thereafter it is a single branch (state 0 -> no cost). */
static void ui_vsync_gate_full(void)
{
    if (!g_vsync_enabled) return;
    if (g_vsync_state < 0) g_vsync_state = ui_vsync_probe();
    if (g_vsync_state == 1) ui_wait_vblank();
}

/* Runtime master switch (e.g. from a config key); re-probes on next full flip. */
void ui_set_vsync(int on) { g_vsync_enabled = on ? 1 : 0; if (!on) g_vsync_state = -1; }

void ui_present(void)
{
    if (!g_back || !g_front) return;   /* drew straight to VRAM -> nothing to flip */

    /* Full flip: no span tracking, or a whole-screen repaint was flagged. */
    if (!g_dirty_track || g_present_full) {
        ui_vsync_gate_full();   /* land the whole-frame swap during blanking */
        ui_present_full();
        g_present_full = 0;
        if (g_dirty_track) {
            for (int yy = 0; yy < (int)g_h; yy++) {
                g_cmin[yy] = (int)g_w; g_cmax[yy] = 0;
                g_pmin[yy] = (int)g_w; g_pmax[yy] = 0;
            }
            g_cy0 = g_py0 = (int)g_h; g_cy1 = g_py1 = -1;
        }
        return;
    }

    /* Partial flip: union this frame's spans with last frame's (so the cached
     * background restore that erased last frame's sprites is flushed too). */
    int y0 = (g_cy0 < g_py0) ? g_cy0 : g_py0;
    int y1 = (g_cy1 > g_py1) ? g_cy1 : g_py1;
    for (int yy = y0; yy <= y1; yy++) {
        int lo = (g_cmin[yy] < g_pmin[yy]) ? g_cmin[yy] : g_pmin[yy];
        int hi = (g_cmax[yy] > g_pmax[yy]) ? g_cmax[yy] : g_pmax[yy];
        if (hi > lo) ui_blit_row((UINT32)yy, lo, hi - lo);
    }

    /* This frame's spans become "previous"; clear the recycled array for reuse. */
    int *tmn = g_pmin, *tmx = g_pmax;
    g_pmin = g_cmin; g_pmax = g_cmax;
    g_cmin = tmn;    g_cmax = tmx;
    g_py0 = g_cy0;   g_py1 = g_cy1;
    for (int yy = y0; yy <= y1; yy++) { g_cmin[yy] = (int)g_w; g_cmax[yy] = 0; }
    g_cy0 = (int)g_h; g_cy1 = -1;
    ui_wc_fence();   /* flush WC buffers to VRAM (partial flip) */
}

/* Last-frame dirty bounding box (screen px) that ui_present() actually flipped,
 * from the previous-frame span arrays. Lets bootx64.c restore only the rows the
 * prior frame damaged. Returns the whole screen when there is no usable previous
 * span (dirty tracking off, or the last flip was a whole-screen ui_mark_all one,
 * which resets the previous range to empty) so callers over-restore safely. */
void ui_prev_dirty_bbox(int *x, int *y, int *w, int *h)
{
    int x0 = 0, y0 = 0, x1 = (int)g_w, y1 = (int)g_h;   /* default: whole screen */
    if (g_dirty_track && g_py1 >= g_py0) {
        int lo = (int)g_w, hi = 0;
        for (int yy = g_py0; yy <= g_py1; yy++) {
            if (g_pmin[yy] < lo) lo = g_pmin[yy];
            if (g_pmax[yy] > hi) hi = g_pmax[yy];
        }
        if (hi > lo) { x0 = lo; x1 = hi; y0 = g_py0; y1 = g_py1 + 1; }
        else         { x0 = y0 = x1 = y1 = 0; }   /* prev frame touched nothing */
    }
    if (x) *x = x0;
    if (y) *y = y0;
    if (w) *w = x1 - x0;
    if (h) *h = y1 - y0;
}

/* Restore ONLY the pixels the previous frame flipped (its per-row dirty spans)
 * from `src` - a full-screen snapshot with the SAME layout/stride as the back
 * buffer, e.g. the composed static-scene cache - into the back buffer. This lets
 * the animated menu loop erase last frame's cursor + particles with a few KB of
 * span copies instead of a whole ~4 MB back-buffer memcpy EVERY frame, which is
 * the difference between a smooth and a choppy cursor on real hardware once the
 * loop samples at a high rate. Returns 1 on success; returns 0 (caller must do a
 * full restore) when dirty tracking is off or the previous flip was whole-screen
 * (prev span range empty). Uses per-row spans, so it stays cheap even with the
 * particle layer scattered across the screen (a bounding box would not). */
int ui_restore_prev_spans(const void *src)
{
    if (!g_back || !g_dirty_track || g_py1 < g_py0) return 0;
    const UINT8 *sbase = (const UINT8 *)src;
    for (int yy = g_py0; yy <= g_py1; yy++) {
        int lo = g_pmin[yy], hi = g_pmax[yy];
        if (hi <= lo) continue;
        UINTN off = (UINTN)yy * (UINTN)g_back_pitch + (UINTN)lo * 4u;
        UINT32       *d = (UINT32 *)(g_back + off);
        const UINT32 *s = (const UINT32 *)(sbase + off);
        int n = hi - lo, i = 0;
        for (; i + 4 <= n; i += 4) { d[i]=s[i]; d[i+1]=s[i+1]; d[i+2]=s[i+2]; d[i+3]=s[i+3]; }
        for (; i < n; i++) d[i] = s[i];
    }
    return 1;
}

void ui_fill(UINT32 color)
{
    fill_rect(0, 0, (int)g_w, (int)g_h, color);
}

void ui_clear(void)
{
    ui_fill(FOREB_BG);
}

/* Convert a logical 0x00RRGGBB color to the framebuffer's byte order. */
static inline UINT32 ui_pack(UINT32 c)
{
    if (g_swap_rb) {
        return (c & 0x0000FF00u)
             | ((c & 0x00FF0000u) >> 16)
             | ((c & 0x000000FFu) << 16);
    }
    return c;
}

/* ------------------------------------------------------------------ */
/*  Tiny freestanding helpers                                          */
/* ------------------------------------------------------------------ */
static UINTN ui_strlen(const char *s)
{
    UINTN n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Unsigned 64-bit -> decimal ASCII. Returns length (>=1). buf must hold
 * at least 21 bytes. Always NUL-terminates. */
static int ui_u64_dec(UINT64 v, char *buf)
{
    char tmp[24];
    int n = 0, i;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v && n < 20) { tmp[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Primitives                                                         */
/* ------------------------------------------------------------------ */
void put_pixel(int x, int y, UINT32 color)
{
    if (!g_fb) return;
    /* The active clip rect is always kept within [0,g_w)x[0,g_h) (ui_clip_reset
     * sets the full screen, ui_clip_push only ever shrinks), so this clip test
     * already implies the pixel is on-screen - no second bounds check needed. */
    if (x < g_clx0 || y < g_cly0 || x >= g_clx1 || y >= g_cly1) return;
    volatile UINT32 *p =
        (volatile UINT32 *)(g_fb + (UINTN)y * g_pitch + (UINTN)x * 4u);
    *p = ui_pack(color);
    /* Inline single-pixel span update: the pixel already passed the clip test
     * (which keeps it inside [0,g_w)x[0,g_h)), so no re-clamp is needed. This
     * collapses ui_mark_dirty's ~15 branches + a call to 4 compares. */
    if (g_dirty_track) {
        if (x     < g_cmin[y]) g_cmin[y] = x;
        if (x + 1 > g_cmax[y]) g_cmax[y] = x + 1;
        if (y < g_cy0) g_cy0 = y;
        if (y > g_cy1) g_cy1 = y;
    }
}

/* Like put_pixel but skips the dirty mark: bulk plotters (that dirty the whole
 * touched region once with a single ui_mark_dirty) avoid a per-pixel span
 * update. Same clip test and store as put_pixel. */
void put_pixel_nomark(int x, int y, UINT32 color)
{
    if (!g_fb) return;
    if (x < g_clx0 || y < g_cly0 || x >= g_clx1 || y >= g_cly1) return;
    volatile UINT32 *p =
        (volatile UINT32 *)(g_fb + (UINTN)y * g_pitch + (UINTN)x * 4u);
    *p = ui_pack(color);
}

/* Fill an already-clipped, already-packed solid rect straight to the draw
 * buffer. No clip, no ui_pack(), no dirty mark: callers guarantee (x,y,w,h) is
 * inside the buffer and issue their own single ui_mark_dirty(). In the common
 * double-buffered case the target (g_back) is plain cached RAM, so a plain
 * non-volatile pointer lets the compiler widen the stores to 64-bit/string
 * moves (like ui_blit_row); only the VRAM fallback keeps the volatile store. */
static void ui_raw_fill(int x, int y, int w, int h, UINT32 packed)
{
    int yy, xx;
    if (!g_fb) return;
    if (g_back) {
        /* Back buffer is plain cached RAM: store two pixels per 64-bit write so
         * the row fill runs at half the store count; an odd trailing word (and
         * the whole VRAM-fallback branch below) stays a 32-bit store. */
        UINT64 two = ((UINT64)packed << 32) | (UINT64)packed;
        UINT8 *rb = g_back + (UINTN)y * g_pitch + (UINTN)x * 4u;
        int pairs = w >> 1;
        for (yy = 0; yy < h; yy++, rb += g_pitch) {
            UINT64 *row64 = (UINT64 *)rb;
            for (xx = 0; xx < pairs; xx++) row64[xx] = two;
            if (w & 1) ((UINT32 *)rb)[w - 1] = packed;   /* odd trailing word */
        }
    } else {
        volatile UINT8 *rb = g_fb + (UINTN)y * g_pitch + (UINTN)x * 4u;
        for (yy = 0; yy < h; yy++, rb += g_pitch) {
            volatile UINT32 *row = (volatile UINT32 *)rb;
            for (xx = 0; xx < w; xx++) row[xx] = packed;
        }
    }
}

void fill_rect(int x, int y, int w, int h, UINT32 color)
{
    if (!g_fb || w <= 0 || h <= 0) return;
    /* Clip to the active clip rect. The clip is an invariant subset of the
     * screen (ui_clip_reset() sets [0,g_w)x[0,g_h), ui_clip_push() only ever
     * intersects), so after this clamp x>=g_clx0>=0 and x+w<=g_clx1<=g_w (same
     * for y) - the old negative-origin fixups and screen-bounds clamp could
     * never fire and are gone from this hot path. */
    if (x < g_clx0) { w -= g_clx0 - x; x = g_clx0; }
    if (y < g_cly0) { h -= g_cly0 - y; y = g_cly0; }
    if (x + w > g_clx1) w = g_clx1 - x;
    if (y + h > g_cly1) h = g_cly1 - y;
    if (w <= 0 || h <= 0) return;

    ui_raw_fill(x, y, w, h, ui_pack(color));
    ui_mark_dirty(x, y, w, h);   /* one span update for the whole (clipped) rect */
}

void draw_hline(int x, int y, int len, UINT32 color)
{
    fill_rect(x, y, len, 1, color);
}

void draw_vline(int x, int y, int len, UINT32 color)
{
    fill_rect(x, y, 1, len, color);
}

void draw_rect_outline(int x, int y, int w, int h, int t, UINT32 color)
{
    if (t < 1) t = 1;
    if (w <= 0 || h <= 0) return;
    fill_rect(x, y, w, t, color);                 /* top    */
    fill_rect(x, y + h - t, w, t, color);         /* bottom */
    fill_rect(x, y, t, h, color);                 /* left   */
    fill_rect(x + w - t, y, t, h, color);         /* right  */
}

/* ------------------------------------------------------------------ */
/*  Text (font8x16, MSB-first bit order per font8x16.h)                */
/* ------------------------------------------------------------------ */
/* Effective magnification = caller scale * the auto hi-res factor. So a
 * caller asking for scale 1 gets 1x on <1080p panels and 2x on 1080p+.
 * Cell is 8 wide x 16 tall; advance is FONT8X16_W * effective scale. */
void draw_char(int x, int y, char c, UINT32 fg, UINT32 bg,
               int transparent, int scale)
{
    int row, s;
    UINT32 fgp, bgp;
    int cxa, cxb, cya, cyb, idx;
    if (scale < 1) scale = 1;
    s = scale * g_uiscale;
    /* Glyph-level cull against the active clip: fully clipped cells (e.g.
     * text inside an occluded window) cost one compare instead of up to
     * FONT8X16_W*FONT8X16_H clipped fill_rect calls. */
    if (x + FONT8X16_W * s <= g_clx0 || x >= g_clx1 ||
        y + FONT8X16_H * s <= g_cly0 || y >= g_cly1) return;
    if (!g_fb) return;

    /* Pack the two colors once and intersect the whole cell with the clip rect
     * once (the clip is kept within the screen, so this is also the screen
     * intersection). Each font row then decodes its 8-bit pattern into runs of
     * consecutive same-state columns and writes each run straight to the buffer
     * (fg run always, bg run only when opaque); the clipped cell is dirtied one
     * time. Same on/off test, same fg/bg, same clip -> identical pixels, but
     * per-glyph work drops from ~128 clipped fill_rect calls to a few spans. */
    fgp = ui_pack(fg);
    bgp = ui_pack(bg);
    cxa = x;                   if (cxa < g_clx0) cxa = g_clx0;
    cxb = x + FONT8X16_W * s;  if (cxb > g_clx1) cxb = g_clx1;
    cya = y;                   if (cya < g_cly0) cya = g_cly0;
    cyb = y + FONT8X16_H * s;  if (cyb > g_cly1) cyb = g_cly1;
    if (cxa >= cxb || cya >= cyb) return;

    idx = font8x16_index((unsigned char)c);
    for (row = 0; row < FONT8X16_H; row++) {
        unsigned char bits = font8x16[idx][row];
        int ry0 = y + row * s, ry1 = ry0 + s;
        int col = 0;
        if (ry0 < cya) ry0 = cya;
        if (ry1 > cyb) ry1 = cyb;
        if (ry0 >= ry1) continue;              /* this glyph row fully clipped */
        while (col < FONT8X16_W) {
            int on = (bits & (0x80u >> col)) != 0;  /* 0x80 = leftmost column */
            int c1 = col + 1;
            /* Coalesce consecutive columns in the same on/off state. */
            while (c1 < FONT8X16_W && (((bits & (0x80u >> c1)) != 0) == on)) c1++;
            if (on || !transparent) {
                int rx0 = x + col * s, rx1 = x + c1 * s;
                if (rx0 < cxa) rx0 = cxa;
                if (rx1 > cxb) rx1 = cxb;
                if (rx0 < rx1)
                    ui_raw_fill(rx0, ry0, rx1 - rx0, ry1 - ry0, on ? fgp : bgp);
            }
            col = c1;
        }
    }
    ui_mark_dirty(cxa, cya, cxb - cxa, cyb - cya);
}

void draw_string(int x, int y, const char *s, UINT32 fg, UINT32 bg,
                 int transparent, int scale)
{
    int adv;
    if (!s) return;
    if (scale < 1) scale = 1;
    adv = FONT8X16_W * scale * g_uiscale;
    /* Whole-string vertical cull; the per-glyph cull in draw_char does the rest. */
    if (y + FONT8X16_H * scale * g_uiscale <= g_cly0 || y >= g_cly1) return;
    while (*s && x < g_clx1) {
        draw_char(x, y, *s, fg, bg, transparent, scale);
        x += adv;
        s++;
    }
}

void draw_string_clip(int x, int y, int maxw, const char *s, UINT32 fg,
                      UINT32 bg, int transparent, int scale)
{
    int adv, ncell, nch, i;
    const char *p;
    if (!s || maxw <= 0) return;
    if (scale < 1) scale = 1;
    adv = FONT8X16_W * scale * g_uiscale;    /* per-char advance, matches draw_string */

    /* Whole cells (each `adv` px wide) that fit in the maxw-pixel budget. */
    ncell = maxw / adv;
    if (ncell <= 0) return;                  /* not even one glyph fits */

    /* Fast path: the whole string already fits. */
    if ((int)ui_strlen(s) <= ncell) {
        draw_string(x, y, s, fg, bg, transparent, scale);
        return;
    }

    /* Truncate: reserve 2 cells for a ".." ellipsis, draw that many leading
     * characters, then the ellipsis, all within the maxw budget. */
    nch = ncell - 2;                         /* chars before ".." */
    if (nch < 0) nch = 0;
    for (i = 0, p = s; i < nch && *p; i++, p++) {
        draw_char(x, y, *p, fg, bg, transparent, scale);
        x += adv;
    }
    /* Append as many ellipsis dots as still fit (up to 2). */
    for (i = nch; i < ncell && i < nch + 2; i++) {
        draw_char(x, y, '.', fg, bg, transparent, scale);
        x += adv;
    }
}

void draw_string_center(int cx, int y, const char *s, UINT32 fg, UINT32 bg,
                        int transparent, int scale)
{
    int wpx;
    if (scale < 1) scale = 1;
    wpx = (int)ui_strlen(s) * FONT8X16_W * scale * g_uiscale;
    draw_string(cx - wpx / 2, y, s, fg, bg, transparent, scale);
}

/* ------------------------------------------------------------------ */
/*  Background + tree logo                                             */
/* ------------------------------------------------------------------ */
/* Linear interpolation of two 0x00RRGGBB colors, t in 0..256. */
static UINT32 ui_lerp(UINT32 a, UINT32 b, int t)
{
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + ((br - ar) * t) / 256;
    int g = ag + ((bg - ag) * t) / 256;
    int bl = ab + ((bb - ab) * t) / 256;
    return ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)bl;
}

/* Upward-pointing isosceles triangle: apex at (cx, y_top), base at y_bot
 * spanning +/- half_bot around cx. */
static void ui_tri_up(int cx, int y_top, int y_bot, int half_bot, UINT32 color)
{
    int y, span = y_bot - y_top;
    if (span <= 0) span = 1;
    for (y = y_top; y <= y_bot; y++) {
        int half = (half_bot * (y - y_top)) / span;
        fill_rect(cx - half, y, half * 2 + 1, 1, color);
    }
}

static void ui_draw_tree(int cx, int cy, int lw, int lh)
{
    int half = lw / 2;
    int trunk_w = lw / 6;
    int trunk_h = lh / 5;
    int top = cy - lh / 2;
    int foliage_h = lh - trunk_h;
    int seg = foliage_h / 3;
    int s0 = top;
    int s1 = top + seg;
    int s2 = top + 2 * seg;

    if (half < 3) half = 3;
    if (trunk_w < 2) trunk_w = 2;

    /* Three overlapping foliage tiers (top narrow -> bottom wide). */
    ui_tri_up(cx, s0,            s0 + seg + seg / 2, half / 2,        FOREB_TREE3);
    ui_tri_up(cx, s1,            s1 + seg + seg / 2, (half * 3) / 4,  FOREB_TREE2);
    ui_tri_up(cx, s2,            s2 + seg,           half,            FOREB_TREE3);

    /* Trunk. */
    fill_rect(cx - trunk_w / 2, top + foliage_h, trunk_w, trunk_h, FOREB_TREE1);
}

void ui_background(void)
{
    UINT32 y;
    int rx0, rx1;
    if (!g_fb || g_h == 0) return;
    /* Vertical forest gradient, top darker -> bottom lit. The color is constant
     * across each row, so write the row span straight to the buffer (clipped to
     * the active clip rect) and force one whole-screen flip instead of g_h
     * per-row fill_rect calls with g_h separate dirty marks. */
    rx0 = 0;         if (rx0 < g_clx0) rx0 = g_clx0;
    rx1 = (int)g_w;  if (rx1 > g_clx1) rx1 = g_clx1;
    for (y = 0; y < g_h; y++) {
        int yy = (int)y;
        UINT32 c;
        if (yy < g_cly0 || yy >= g_cly1 || rx0 >= rx1) continue;
        c = ui_pack(ui_lerp(FOREB_BG_TOP, FOREB_BG_BOTTOM, (int)((y * 256u) / g_h)));
        ui_raw_fill(rx0, yy, rx1 - rx0, 1, c);
    }
    /* Only the touched columns [rx0,rx1) over the full height changed, so mark
     * exactly that band instead of forcing a whole-screen flip. (An image-based
     * background would blit outside the primitives and must still ui_mark_all.) */
    if (rx1 > rx0) ui_mark_dirty(rx0, 0, rx1 - rx0, (int)g_h);
    /* Tree logo. */
    {
        int cx = UI_FW(UIP_HALF);
        int cy = UI_FH(UIP_LOGO_CY);
        int lw = UI_FW(UIP_LOGO_W);
        int lh = UI_FH(UIP_LOGO_H);
        ui_draw_tree(cx, cy, lw, lh);
    }
    /* Title bar rule + title/subtitle text (suppressed by menu_show_title=0). */
    if (g_rs.show_title) {
        int mx = UI_FW(UIP_MARGIN_X);
        int ty = UI_FH(UIP_TITLEBAR_Y);
        int tty = UI_FH(UIP_TITLE_Y);
        draw_hline(mx, ty, (int)g_w - 2 * mx, FOREB_BORDER);
        draw_string_center((int)g_w / 2, tty,
                           FOREB_TITLE_STR, FOREB_TITLE, 0, 1, 2);
        draw_string_center((int)g_w / 2, tty + 22,
                           FOREB_SUBTITLE_STR, FOREB_DIM, 0, 1, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Boot menu (pure draw)                                              */
/* ------------------------------------------------------------------ */

void ui_menu_set_scroll(int first) { g_menu_first = first; }
int  ui_menu_get_scroll(void)      { return g_menu_first; }
void ui_menu_set_highlight_y(int y) { g_menu_hl_y = y; }

void ui_menu_layout(int count, int *opx, int *opy, int *opw, int *oph,
                    int *oeh, int *oetop, int *ovis)
{
    int gh = FOREB_GLYPH_H * g_uiscale;
    int px, py, pw, ph, eh, entries_top, bottom, vis;

    /* Base panel rect (permille of screen) selected by g_rs.pos. */
    int bx, by, bw, bh;
    switch (g_rs.pos) {
        case FMP_LEFT:   bx=30;  by=110; bw=380; bh=800; break;
        case FMP_RIGHT:  bx=590; by=110; bw=380; bh=800; break;
        case FMP_TOP:    bx=100; by=40;  bw=800; bh=320; break;
        case FMP_BOTTOM: bx=100; by=560; bw=800; bh=400; break;
        case FMP_FULL:   bx=40;  by=40;  bw=920; bh=920; break;
        case FMP_CUSTOM: bx=200; by=360; bw=600; bh=420; break;   /* seed */
        case FMP_CENTER:
        default:         bx=200; by=360; bw=600; bh=420; break;
    }
    /* Explicit permille overrides win over the positional preset. */
    if (g_rs.panel_x >= 0) bx = g_rs.panel_x;
    if (g_rs.panel_y >= 0) by = g_rs.panel_y;
    if (g_rs.panel_w >= 0) bw = g_rs.panel_w;
    if (g_rs.panel_h >= 0) bh = g_rs.panel_h;

    px = UI_FW(bx); py = UI_FH(by); pw = UI_FW(bw); ph = UI_FH(bh);

    eh = (g_rs.entry_h > 0) ? UI_FH(g_rs.entry_h) : UI_FH(UIP_ENTRY_H);
    if (eh < gh + 8) eh = gh + 8;

    /* Top inset: header rule strip when title_bar is on, else just padding. */
    entries_top = py + (g_rs.title_bar ? (8 + gh + 12) : (g_rs.pad + 2));
    /* Reserve the bottom strip for the countdown only when it is shown. */
    bottom = py + ph - (g_rs.show_timer ? (gh + 10) : g_rs.pad);
    vis = (bottom - entries_top) / eh;
    if (vis < 1) vis = 1;
    if (count > 0 && vis > count) vis = count;

    if (opx)   *opx   = px;
    if (opy)   *opy   = py;
    if (opw)   *opw   = pw;
    if (oph)   *oph   = ph;
    if (oeh)   *oeh   = eh;
    if (oetop) *oetop = entries_top;
    if (ovis)  *ovis  = vis;
}

int ui_menu_scrollbar(int count, int *track_x, int *track_y,
                      int *track_w, int *track_h,
                      int *thumb_y, int *thumb_h)
{
    int px, pw, eh, entries_top, vis;
    int tx, ty, tw, th, thh, thy, span, first;

    ui_menu_layout(count, &px, NULL, &pw, NULL, &eh, &entries_top, &vis);
    if (count <= vis) return 0;

    tx = px + pw - 10;
    ty = entries_top;
    tw = 6;
    th = vis * eh;
    thh = th * vis / count; if (thh < 12) thh = 12; if (thh > th) thh = th;
    span = th - thh;

    first = g_menu_first;
    if (first > count - vis) first = count - vis;
    if (first < 0) first = 0;
    thy = ty + (count > vis ? span * first / (count - vis) : 0);

    if (track_x) *track_x = tx;
    if (track_y) *track_y = ty;
    if (track_w) *track_w = tw;
    if (track_h) *track_h = th;
    if (thumb_y) *thumb_y = thy;
    if (thumb_h) *thumb_h = thh;
    return 1;
}

/* ---- style-driven panel frame + selection helpers -------------------- */

static int ui_gw(void) { return FONT8X16_W * g_uiscale; }   /* glyph advance px */
static int ui_strpx(const char *s) { return (int)ui_strlen(s) * ui_gw(); }

/* Draw the panel body (flat or gradient), optional accent strip, corner notch,
 * and the selected border style. */
static void ui_panel_frame(int px, int py, int pw, int ph)
{
    if (g_rs.shadow) {
        fill_rect(px + 6, py + 7, pw, ph, FOREB_SHADOW);
        fill_rect(px + 3, py + 4, pw, ph, ui_lerp(FOREB_SHADOW, FOREB_PANEL, 90));
    }
    if (g_panel_img && g_panel_img->pixels) {
        /* Custom panel face: opaque image, then a translucent theme tint so
         * the row text stays readable over any artwork. */
        img_blit_scaled(g_panel_img, px, py, pw, ph);
        ui_blend_rect(px, py, pw, ph, FOREB_PANEL, 96);
    } else if (g_rs.gradient) {
        UINT32 gtop = ui_lerp(FOREB_PANEL, FOREB_WHITE, 16);
        UINT32 gbot = ui_lerp(FOREB_PANEL, FOREB_SHADOW, 46);
        /* Per-row color, but write the clipped row span straight to the buffer
         * and issue one dirty mark for the whole panel (avoids ph redundant
         * clip evaluations + ph per-row dirty marks). */
        int rx0 = px < g_clx0 ? g_clx0 : px;
        int rx1 = (px + pw > g_clx1) ? g_clx1 : px + pw;
        for (int gy = 0; gy < ph; gy++) {
            int ry = py + gy;
            if (ry < g_cly0 || ry >= g_cly1 || rx0 >= rx1) continue;
            ui_raw_fill(rx0, ry, rx1 - rx0, 1,
                        ui_pack(ui_lerp(gtop, gbot, gy * 256 / ph)));
        }
        ui_mark_dirty(px, py, pw, ph);
    } else {
        fill_rect(px, py, pw, ph, FOREB_PANEL);
    }
    if (g_rs.accent_strip) fill_rect(px, py, pw, 3, g_pal.accent);

    /* Border. */
    switch (g_rs.border) {
        case FBD_NONE: break;
        case FBD_THIN:   draw_rect_outline(px - 1, py - 1, pw + 2, ph + 2, 1, FOREB_BORDER); break;
        case FBD_THICK:  draw_rect_outline(px - 2, py - 2, pw + 4, ph + 4, 2, FOREB_BORDER); break;
        case FBD_DOUBLE:
            draw_rect_outline(px - 3, py - 3, pw + 6, ph + 6, 1, FOREB_BORDER);
            draw_rect_outline(px + 1, py + 1, pw - 2, ph - 2, 1, FOREB_BORDER);
            break;
        case FBD_SHADOW: draw_rect_outline(px - 1, py - 1, pw + 2, ph + 2, 1,
                                           ui_lerp(FOREB_PANEL, FOREB_SHADOW, 60)); break;
        case FBD_GLOW:
            draw_rect_outline(px - 3, py - 3, pw + 6, ph + 6, 1,
                              ui_lerp(FOREB_PANEL, g_pal.accent, 90));
            draw_rect_outline(px - 1, py - 1, pw + 2, ph + 2, 1, g_pal.accent);
            break;
        case FBD_DASHED: {
            UINT32 c = FOREB_BORDER;
            for (int x = px; x < px + pw; x += 10) { fill_rect(x, py - 1, 6, 1, c); fill_rect(x, py + ph, 6, 1, c); }
            for (int y = py; y < py + ph; y += 10) { fill_rect(px - 1, y, 1, 6, c); fill_rect(px + pw, y, 1, 6, c); }
            break;
        }
        default: draw_rect_outline(px - 2, py - 2, pw + 4, ph + 4, 2, FOREB_BORDER); break;
    }

    /* Corner treatment: a small notch in the border color fakes round/cut. */
    if (g_rs.corner != FCN_SQUARE) {
        int n = (g_rs.corner == FCN_ROUND) ? 4 : 6;
        UINT32 c = FOREB_SHADOW;
        for (int i = 0; i < n; i++) {
            int w = n - i;
            fill_rect(px,             py + i,          w, 1, c);
            fill_rect(px + pw - w,    py + i,          w, 1, c);
            fill_rect(px,             py + ph - 1 - i, w, 1, c);
            fill_rect(px + pw - w,    py + ph - 1 - i, w, 1, c);
        }
    }
}

/* Render the selection background for one row's inner rect. Returns the text
 * color to use for the selected label. */
static UINT32 ui_sel_bg(int ix, int rowtop, int iw, int eh)
{
    int by = rowtop, bh = eh - 2;
    UINT32 fg = FOREB_WHITE;
    switch (g_rs.sel_style) {
        case FSS_NONE:
            fg = g_pal.accent; break;
        case FSS_ARROW:
        case FSS_BRACKET:
            fg = FOREB_WHITE; break;
        case FSS_OUTLINE:
            draw_rect_outline(ix, by, iw, bh, 1, g_pal.accent); break;
        case FSS_BOX:
            draw_rect_outline(ix, by, iw, bh, 2, g_pal.accent); break;
        case FSS_UNDERLINE:
            fill_rect(ix, by + bh - 2, iw, 2, g_pal.accent); break;
        case FSS_INVERT:
            fill_rect(ix, by, iw, bh, FOREB_TEXT); fg = FOREB_PANEL; break;
        case FSS_GRADIENT: {
            /* Horizontal gradient: the column color is independent of the row,
             * so walk row-major (rows outer, columns inner writing row[sx])
             * instead of column-major vertical spans. That makes the back-buffer
             * stores sequential/cache-friendly; one dirty mark for the strip. */
            int cxa = ix < g_clx0 ? g_clx0 : ix;
            int cxb = (ix + iw > g_clx1) ? g_clx1 : ix + iw;
            int cya = by < g_cly0 ? g_cly0 : by;
            int cyb = (by + bh > g_cly1) ? g_cly1 : by + bh;
            if (g_fb && cxa < cxb && cya < cyb) {
                if (g_back) {
                    for (int ry = cya; ry < cyb; ry++) {
                        UINT32 *row = (UINT32 *)(g_back + (UINTN)ry * g_pitch);
                        for (int rx = cxa; rx < cxb; rx++)
                            row[rx] = ui_pack(ui_lerp(g_pal.accent, FOREB_PANEL,
                                                      (rx - ix) * 256 / iw));
                    }
                } else {
                    for (int ry = cya; ry < cyb; ry++) {
                        volatile UINT32 *row =
                            (volatile UINT32 *)(g_fb + (UINTN)ry * g_pitch);
                        for (int rx = cxa; rx < cxb; rx++)
                            row[rx] = ui_pack(ui_lerp(g_pal.accent, FOREB_PANEL,
                                                      (rx - ix) * 256 / iw));
                    }
                }
            }
            ui_mark_dirty(ix, by, iw, bh);
            break;
        }
        case FSS_PILL: {
            UINT32 s_top = ui_lerp(g_pal.accent, FOREB_WHITE, 20);
            UINT32 s_bot = ui_lerp(g_pal.accent, FOREB_SHADOW, 30);
            /* Clip the (ix+3 .. ix+iw-3) span once, write each row straight to
             * the buffer, and issue one dirty mark for the whole pill body. */
            int bx0 = ix + 3, bx1 = ix + iw - 3;
            int rx0 = bx0 < g_clx0 ? g_clx0 : bx0;
            int rx1 = (bx1 > g_clx1) ? g_clx1 : bx1;
            for (int sy = 0; sy < bh; sy++) {
                int ry = by + sy;
                if (ry < g_cly0 || ry >= g_cly1 || rx0 >= rx1) continue;
                ui_raw_fill(rx0, ry, rx1 - rx0, 1,
                            ui_pack(ui_lerp(s_top, s_bot, sy * 256 / bh)));
            }
            ui_mark_dirty(bx0, by, iw - 6, bh);
            for (int i = 0; i < 3; i++) {   /* rounded caps */
                fill_rect(ix + 3 - i, by + i + 1, 1, bh - 2 * (i + 1), s_top);
                fill_rect(ix + iw - 4 + i, by + i + 1, 1, bh - 2 * (i + 1), s_bot);
            }
            break;
        }
        case FSS_GLOW:
            fill_rect(ix, by - 1, iw, 1, ui_lerp(FOREB_PANEL, g_pal.accent, 60));
            fill_rect(ix, by + bh, iw, 1, ui_lerp(FOREB_PANEL, g_pal.accent, 60));
            /* fall through to a bar body */
            /* FALLTHROUGH */
        case FSS_BAR:
        case FSS_DOUBLEBAR:
        default: {
            UINT32 s_top = ui_lerp(FOREB_SELECT, FOREB_WHITE, 14);
            UINT32 s_bot = ui_lerp(FOREB_SELECT, FOREB_SHADOW, 24);
            /* Clip the span once, write each row straight to the buffer, then a
             * single dirty mark (vs bh re-clipping/re-marking fill_rect calls). */
            int rx0 = ix < g_clx0 ? g_clx0 : ix;
            int rx1 = (ix + iw > g_clx1) ? g_clx1 : ix + iw;
            for (int sy = 0; sy < bh; sy++) {
                int ry = by + sy;
                if (ry < g_cly0 || ry >= g_cly1 || rx0 >= rx1) continue;
                ui_raw_fill(rx0, ry, rx1 - rx0, 1,
                            ui_pack(ui_lerp(s_top, s_bot, sy * 256 / bh)));
            }
            ui_mark_dirty(ix, by, iw, bh);
            if (g_rs.sel_style == FSS_DOUBLEBAR)
                fill_rect(ix, by, 3, bh, g_pal.accent);
            break;
        }
    }
    return fg;
}

/* =============================================================================
 * Visual effects (blur / frosted backdrop / vignette / scanlines).
 * -----------------------------------------------------------------------------
 * Operate on the RAM back buffer BEFORE ui_present(). Integer + channel-order
 * agnostic (each pixel word carries 3 meaningful lanes; blur/darken each lane
 * independently and repack, so BGRX and RGBX both work). Backdrops are how the
 * "glass" window skin gets its frosted look. Effects are opt-in (off by default)
 * because a full-screen vignette/scanline pass forces a whole-frame flip. */
static struct { int glass, blur, opacity, vignette, scanlines; } g_fx = { 0, 8, 72, 0, 0 };
static UINT32 *g_fx_scratch = 0;
static UINTN   g_fx_scratch_words = 0;

void ui_fx_config(int glass, int blur, int opacity, int vignette, int scanlines)
{
    g_fx.glass = glass ? 1 : 0;
    g_fx.blur = (blur < 0) ? 0 : (blur > 16 ? 16 : blur);
    g_fx.opacity = (opacity < 0) ? 0 : (opacity > 255 ? 255 : opacity);
    g_fx.vignette = (vignette < 0) ? 0 : (vignette > 255 ? 255 : vignette);
    g_fx.scanlines = (scanlines < 0) ? 0 : (scanlines > 255 ? 255 : scanlines);
}
int ui_fx_enabled(void) { return g_fx.glass ? 1 : 0; }
int ui_fx_vignette_amt(void) { return g_fx.vignette; }
int ui_fx_scanline_amt(void) { return g_fx.scanlines; }

static UINT32 *fx_scratch(UINTN words)
{
    if (!g_bs) return 0;
    if (g_fx_scratch && g_fx_scratch_words >= words) return g_fx_scratch;
    if (g_fx_scratch) g_bs->FreePool(g_fx_scratch);
    VOID *p = 0;
    if (EFI_ERROR(g_bs->AllocatePool(EfiLoaderData, words * 4u, &p)) || !p) {
        g_fx_scratch = 0; g_fx_scratch_words = 0; return 0;
    }
    g_fx_scratch = (UINT32 *)p; g_fx_scratch_words = words;
    return g_fx_scratch;
}

/* Separable box blur (O(w*h) via running sums) over a back-buffer region. */
void ui_blur_rect(int x, int y, int w, int h, int r)
{
    if (!g_back || r < 1) return;
    if (x < g_clx0) { w -= g_clx0 - x; x = g_clx0; }
    if (y < g_cly0) { h -= g_cly0 - y; y = g_cly0; }
    if (x + w > g_clx1) w = g_clx1 - x;
    if (y + h > g_cly1) h = g_cly1 - y;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((UINT32)(x + w) > g_w) w = (int)g_w - x;
    if ((UINT32)(y + h) > g_h) h = (int)g_h - y;
    if (w <= 0 || h <= 0) return;
    if (r > 16) r = 16;

    UINT32 *tmp = fx_scratch((UINTN)(w > h ? w : h));
    if (!tmp) return;
    UINT32 pw = g_back_pitch / 4u;

    for (int row = 0; row < h; row++) {
        UINT32 *line = (UINT32 *)g_back + (UINTN)(y + row) * pw + x;
        for (int i = 0; i < w; i++) tmp[i] = line[i];
        int s0 = 0, s1 = 0, s2 = 0, cnt = 0;
        for (int i = 0; i <= r && i < w; i++) { UINT32 p = tmp[i]; s0 += p & 0xFF; s1 += (p >> 8) & 0xFF; s2 += (p >> 16) & 0xFF; cnt++; }
        for (int i = 0; i < w; i++) {
            line[i] = ((UINT32)(s2 / cnt) << 16) | ((UINT32)(s1 / cnt) << 8) | (UINT32)(s0 / cnt);
            int add = i + r + 1, sub = i - r;
            if (add < w) { UINT32 p = tmp[add]; s0 += p & 0xFF; s1 += (p >> 8) & 0xFF; s2 += (p >> 16) & 0xFF; cnt++; }
            if (sub >= 0) { UINT32 p = tmp[sub]; s0 -= p & 0xFF; s1 -= (p >> 8) & 0xFF; s2 -= (p >> 16) & 0xFF; cnt--; }
        }
    }
    for (int col = 0; col < w; col++) {
        for (int i = 0; i < h; i++) tmp[i] = *((UINT32 *)g_back + (UINTN)(y + i) * pw + x + col);
        int s0 = 0, s1 = 0, s2 = 0, cnt = 0;
        for (int i = 0; i <= r && i < h; i++) { UINT32 p = tmp[i]; s0 += p & 0xFF; s1 += (p >> 8) & 0xFF; s2 += (p >> 16) & 0xFF; cnt++; }
        for (int i = 0; i < h; i++) {
            UINT32 *dst = (UINT32 *)g_back + (UINTN)(y + i) * pw + x + col;
            *dst = ((UINT32)(s2 / cnt) << 16) | ((UINT32)(s1 / cnt) << 8) | (UINT32)(s0 / cnt);
            int add = i + r + 1, sub = i - r;
            if (add < h) { UINT32 p = tmp[add]; s0 += p & 0xFF; s1 += (p >> 8) & 0xFF; s2 += (p >> 16) & 0xFF; cnt++; }
            if (sub >= 0) { UINT32 p = tmp[sub]; s0 -= p & 0xFF; s1 -= (p >> 8) & 0xFF; s2 -= (p >> 16) & 0xFF; cnt--; }
        }
    }
    ui_mark_dirty(x, y, w, h);
}

/* Exact x/255 for x in [0,65025] via reciprocal-multiply (verified equal to the
 * integer divide over the whole lane range: a lane value 0..255 times a factor
 * 0..255 peaks at 255*255=65025). Replaces the per-lane division in the FX loops
 * below; the product 65025*0x8081 fits in 32 bits so no widening is needed. */
static inline UINT32 ui_div255(UINT32 x) { return (x * 0x8081u) >> 23; }

/* Multiply every lane of a region by (255-amt)/255 (darken). No mark. */
static void fx_darken(int x, int y, int w, int h, int amt)
{
    if (!g_back || amt <= 0) return;
    if (x < g_clx0) { w -= g_clx0 - x; x = g_clx0; }
    if (y < g_cly0) { h -= g_cly0 - y; y = g_cly0; }
    if (x + w > g_clx1) w = g_clx1 - x;
    if (y + h > g_cly1) h = g_cly1 - y;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((UINT32)(x + w) > g_w) w = (int)g_w - x;
    if ((UINT32)(y + h) > g_h) h = (int)g_h - y;
    if (w <= 0 || h <= 0) return;
    int keep = 255 - (amt > 255 ? 255 : amt);
    UINT32 pw = g_back_pitch / 4u;
    for (int row = 0; row < h; row++) {
        UINT32 *line = (UINT32 *)g_back + (UINTN)(y + row) * pw + x;
        for (int i = 0; i < w; i++) {
            UINT32 p = line[i];
            UINT32 b0 = ui_div255((p & 0xFF) * keep), b1 = ui_div255(((p >> 8) & 0xFF) * keep), b2 = ui_div255(((p >> 16) & 0xFF) * keep);
            line[i] = (b2 << 16) | (b1 << 8) | b0;
        }
    }
}

/* Alpha-blend a solid logical color over a region (alpha 0..255). */
void ui_blend_rect(int x, int y, int w, int h, UINT32 color, int alpha)
{
    if (!g_fb || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    if (x < g_clx0) { w -= g_clx0 - x; x = g_clx0; }
    if (y < g_cly0) { h -= g_cly0 - y; y = g_cly0; }
    if (x + w > g_clx1) w = g_clx1 - x;
    if (y + h > g_cly1) h = g_cly1 - y;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((UINT32)(x + w) > g_w) w = (int)g_w - x;
    if ((UINT32)(y + h) > g_h) h = (int)g_h - y;
    if (w <= 0 || h <= 0) return;
    UINT32 s = ui_pack(color);
    UINT32 s0 = s & 0xFF, s1 = (s >> 8) & 0xFF, s2 = (s >> 16) & 0xFF;
    int ia = 255 - alpha;
    for (int row = 0; row < h; row++) {
        volatile UINT32 *line = (volatile UINT32 *)(g_fb + (UINTN)(y + row) * g_pitch + (UINTN)x * 4u);
        for (int i = 0; i < w; i++) {
            UINT32 d = line[i];
            UINT32 o0 = ui_div255(s0 * alpha + (d & 0xFF) * ia);
            UINT32 o1 = ui_div255(s1 * alpha + ((d >> 8) & 0xFF) * ia);
            UINT32 o2 = ui_div255(s2 * alpha + ((d >> 16) & 0xFF) * ia);
            line[i] = (o2 << 16) | (o1 << 8) | o0;
        }
    }
    ui_mark_dirty(x, y, w, h);
}

/* Frosted-glass backdrop: blur + darken what is behind a panel/window. */
void ui_backdrop(int x, int y, int w, int h)
{
    if (!g_fx.glass) return;
    ui_blur_rect(x, y, w, h, g_fx.blur ? g_fx.blur : 8);
    fx_darken(x, y, w, h, g_fx.opacity);
    ui_mark_dirty(x, y, w, h);
}

/* Whole-screen edge darken. Forces a full flip (call only on redraw frames). */
void ui_vignette(int strength)
{
    if (!g_back || strength <= 0) return;
    if (strength > 255) strength = 255;
    UINT32 pw = g_back_pitch / 4u;
    int cx = (int)g_w / 2, cy = (int)g_h / 2;
    int maxd = cx * cx + cy * cy; if (maxd < 1) maxd = 1;
    for (UINT32 yy = 0; yy < g_h; yy++) {
        int dy = (int)yy - cy;
        int dy2 = dy * dy;                 /* x-invariant: hoisted out of the row */
        UINT32 *line = (UINT32 *)g_back + (UINTN)yy * pw;
        for (UINT32 xx = 0; xx < g_w; xx++) {
            int dx = (int)xx - cx;
            int amt = (int)(((UINT64)(dx * dx + dy2) * (UINT64)strength) / (UINT64)maxd);
            int keep = 255 - amt;
            UINT32 p = line[xx];
            UINT32 b0 = ui_div255((p & 0xFF) * keep), b1 = ui_div255(((p >> 8) & 0xFF) * keep), b2 = ui_div255(((p >> 16) & 0xFF) * keep);
            line[xx] = (b2 << 16) | (b1 << 8) | b0;
        }
    }
    ui_mark_all();
}

/* Dim every other scanline (CRT look). Forces a full flip. */
void ui_scanlines(int strength)
{
    if (!g_back || strength <= 0) return;
    if (strength > 255) strength = 255;
    int keep = 255 - strength;
    UINT32 pw = g_back_pitch / 4u;
    for (UINT32 yy = 1; yy < g_h; yy += 2) {
        UINT32 *line = (UINT32 *)g_back + (UINTN)yy * pw;
        for (UINT32 xx = 0; xx < g_w; xx++) {
            UINT32 p = line[xx];
            UINT32 b0 = ui_div255((p & 0xFF) * keep), b1 = ui_div255(((p >> 8) & 0xFF) * keep), b2 = ui_div255(((p >> 16) & 0xFF) * keep);
            line[xx] = (b2 << 16) | (b1 << 8) | b0;
        }
    }
    ui_mark_all();
}

/* =============================================================================
 * Widget renderer: configurable buttons + checkbox + slider.
 * -----------------------------------------------------------------------------
 * Resolved from a concrete WIDGET_BASE + the config's struct forebo_widget
 * (fields that are -1 / FOREB_COLOR_UNSET inherit). The Settings dialog + window
 * chrome draw through these so every control obeys forebo.cfg. State drives the
 * fill/text color; style drives the shape (flat/raised/pill/outline/ghost/glass).
 * ========================================================================== */
static struct forebo_widget g_wid;

static const struct forebo_widget WIDGET_BASE = {
    FBTN_RAISED, FCN_ROUND, 1, 12, 5, 1, 1, 0,
    FOREB_COLOR_UNSET, FOREB_COLOR_UNSET, FOREB_COLOR_UNSET, FOREB_COLOR_UNSET,
    FOREB_COLOR_UNSET, FOREB_COLOR_UNSET, FOREB_COLOR_UNSET, FOREB_COLOR_UNSET,
    FOREB_COLOR_UNSET, FOREB_COLOR_UNSET,
    FCN_SQUARE, 1, 255, FOREB_DEF_UI_SEPARATOR, 8, FOREB_DEF_UI_SCROLLBAR,
    FOREB_DEF_UI_FOCUS, 2, 100
};

void ui_apply_widgets(const struct forebo_widget *cfg)
{
    g_wid = WIDGET_BASE;
    if (!cfg) return;
    #define WI(f) do { if (cfg->f != -1) g_wid.f = cfg->f; } while (0)
    #define WC(f) do { if (cfg->f != FOREB_COLOR_UNSET) g_wid.f = cfg->f; } while (0)
    WI(style); WI(corner); WI(border_w); WI(pad_x); WI(pad_y);
    WI(gradient); WI(shadow); WI(glow);
    WC(face_normal); WC(face_hover); WC(face_active); WC(face_disabled);
    WC(text_normal); WC(text_hover); WC(text_active); WC(text_disabled);
    WC(border_col); WC(focus_col);
    WI(window_corner); WI(window_border_w); WI(panel_alpha);
    WC(separator); WI(scrollbar_w); WC(scrollbar_color); WC(focus_color);
    WI(focus_width); WI(font_scale);
    #undef WI
    #undef WC
}

/* Accessors used by wm.c / the menu so those obey the same skin. */
UINT32 ui_wid_separator(void)  { return g_wid.separator; }
UINT32 ui_wid_scrollbar(void)  { return g_wid.scrollbar_color; }
int    ui_wid_scrollbar_w(void){ return g_wid.scrollbar_w > 0 ? g_wid.scrollbar_w : 6; }
UINT32 ui_wid_focus(void)      { return g_wid.focus_color; }
int    ui_wid_window_corner(void) { return g_wid.window_corner; }

static UINT32 wid_or(UINT32 v, UINT32 fallback) { return v == FOREB_COLOR_UNSET ? fallback : v; }

/* Corner notch in a backing color (fakes round/cut like the menu panel). */
static void wid_notch(int x, int y, int w, int h, int corner, UINT32 c)
{
    if (corner == FCN_SQUARE) return;
    int n = (corner == FCN_ROUND) ? 3 : 5;
    for (int i = 0; i < n; i++) {
        int ww = n - i;
        fill_rect(x, y + i, ww, 1, c);
        fill_rect(x + w - ww, y + i, ww, 1, c);
        fill_rect(x, y + h - 1 - i, ww, 1, c);
        fill_rect(x + w - ww, y + h - 1 - i, ww, 1, c);
    }
}

/* Draw a button. state = FBTN_NORMAL/HOVER/ACTIVE/FOCUSED/DISABLED. */
void ui_button(int x, int y, int w, int h, const char *label, int state)
{
    int style = g_wid.style, corner = g_wid.corner;
    UINT32 base = wid_or(g_wid.face_normal, ui_lerp(g_pal.panel, g_pal.white, 22));
    UINT32 hov  = wid_or(g_wid.face_hover,  ui_lerp(base, g_pal.accent, 55));
    UINT32 act  = wid_or(g_wid.face_active, g_pal.accent);
    UINT32 dis  = wid_or(g_wid.face_disabled, ui_lerp(g_pal.panel, g_pal.shadow, 40));
    UINT32 bord = wid_or(g_wid.border_col, ui_lerp(g_pal.panel, g_pal.border, 200));
    UINT32 fill = base, txt = wid_or(g_wid.text_normal, g_pal.white);
    switch (state) {
        case FBTN_HOVER:   fill = hov; txt = wid_or(g_wid.text_hover, g_pal.white); break;
        case FBTN_ACTIVE:  fill = act; txt = wid_or(g_wid.text_active, g_pal.white); break;
        case FBTN_DISABLED:fill = dis; txt = wid_or(g_wid.text_disabled, g_pal.dim); break;
        case FBTN_FOCUSED: fill = ui_lerp(base, g_pal.accent, 25); break;
        default: break;
    }

    if (g_wid.shadow && style != FBTN_GHOST && style != FBTN_OUTLINE)
        fill_rect(x + 2, y + 3, w, h, FOREB_SHADOW);

    switch (style) {
        case FBTN_FLAT:
            fill_rect(x, y, w, h, fill);
            break;
        case FBTN_OUTLINE:
            if (state == FBTN_HOVER || state == FBTN_ACTIVE) ui_blend_rect(x, y, w, h, fill, 40);
            draw_rect_outline(x, y, w, h, g_wid.border_w > 0 ? g_wid.border_w : 1, g_pal.accent);
            txt = wid_or(g_wid.text_normal, g_pal.accent);
            break;
        case FBTN_GHOST:
            if (state == FBTN_HOVER) ui_blend_rect(x, y, w, h, hov, 45);
            else if (state == FBTN_ACTIVE) ui_blend_rect(x, y, w, h, act, 70);
            txt = wid_or(g_wid.text_normal, g_pal.text);
            break;
        case FBTN_GLASS:
            ui_backdrop(x, y, w, h);
            ui_blend_rect(x, y, w, h, fill, g_wid.panel_alpha >= 0 ? g_wid.panel_alpha : 150);
            draw_rect_outline(x, y, w, h, 1, ui_lerp(fill, g_pal.white, 40));
            break;
        case FBTN_PILL:
        case FBTN_RAISED:
        default: {
            UINT32 gt = g_wid.gradient ? ui_lerp(fill, g_pal.white, 22) : fill;
            UINT32 gb = g_wid.gradient ? ui_lerp(fill, g_pal.shadow, 26) : fill;
            /* Clip the face span once and write rows straight to the buffer with
             * one dirty mark, instead of h separate re-clipping fill_rect calls. */
            int hh = h ? h : 1;
            int rx0 = x < g_clx0 ? g_clx0 : x;
            int rx1 = (x + w > g_clx1) ? g_clx1 : x + w;
            for (int gy = 0; gy < h; gy++) {
                int ry = y + gy;
                if (ry < g_cly0 || ry >= g_cly1 || rx0 >= rx1) continue;
                ui_raw_fill(rx0, ry, rx1 - rx0, 1,
                            ui_pack(ui_lerp(gt, gb, gy * 256 / hh)));
            }
            ui_mark_dirty(x, y, w, h);
            if (g_wid.border_w != 0)
                draw_rect_outline(x, y, w, h, g_wid.border_w > 0 ? g_wid.border_w : 1, bord);
            fill_rect(x + 1, y + 1, w - 2, 1, ui_lerp(fill, g_pal.white, 40)); /* top highlight */
            break;
        }
    }
    if (style == FBTN_PILL && corner == FCN_SQUARE) corner = FCN_ROUND;
    wid_notch(x, y, w, h, corner, FOREB_SHADOW);
    if (g_wid.glow || state == FBTN_FOCUSED)
        draw_rect_outline(x - 1, y - 1, w + 2, h + 2, 1, wid_or(g_wid.focus_col, g_pal.accent));

    if (label && label[0]) {
        int tw = (int)ui_strlen(label) * ui_gw();
        int lx = x + (w - tw) / 2, ly = y + (h - FOREB_GLYPH_H * g_uiscale) / 2;
        if (lx < x + 4) lx = x + 4;
        draw_string(lx, ly, label, txt, 0, 1, 1);
    }
}

/* Hit -> state from pointer position + button-down. */
int ui_button_state(int x, int y, int w, int h, int mx, int my, int down)
{
    if (mx < x || my < y || mx >= x + w || my >= y + h) return FBTN_NORMAL;
    return down ? FBTN_ACTIVE : FBTN_HOVER;
}
int ui_hit(int x, int y, int w, int h, int mx, int my)
{
    return (mx >= x && my >= y && mx < x + w && my < y + h) ? 1 : 0;
}

/* Checkbox: a square that fills with accent when checked, label to the right. */
void ui_checkbox(int x, int y, int size, int checked, const char *label, int state)
{
    UINT32 box = ui_lerp(g_pal.panel, g_pal.white, 18);
    draw_rect_outline(x, y, size, size, 1, ui_lerp(g_pal.panel, g_pal.border, 200));
    fill_rect(x + 1, y + 1, size - 2, size - 2, box);
    if (checked) {
        fill_rect(x + 3, y + 3, size - 6, size - 6, g_pal.accent);
    }
    if (state == FBTN_HOVER) draw_rect_outline(x - 1, y - 1, size + 2, size + 2, 1, g_pal.accent);
    if (label && label[0])
        draw_string(x + size + 8, y + (size - FOREB_GLYPH_H * g_uiscale) / 2, label,
                    state == FBTN_DISABLED ? g_pal.dim : g_pal.text, 0, 1, 1);
}

/* Horizontal slider track + accent fill + knob. val/max in [0,max]. */
void ui_slider(int x, int y, int w, int h, int val, int max, int state)
{
    if (max < 1) max = 1;
    if (val < 0) val = 0; if (val > max) val = max;
    int cy = y + h / 2;
    fill_rect(x, cy - 2, w, 4, ui_lerp(g_pal.panel, g_pal.shadow, 40));      /* track */
    int fw = (int)(((UINT64)val * (UINT64)(w - 1)) / (UINT64)max);
    fill_rect(x, cy - 2, fw, 4, g_pal.accent);                              /* fill  */
    int kx = x + fw - h / 2; if (kx < x) kx = x; if (kx > x + w - h) kx = x + w - h;
    UINT32 kf = (state == FBTN_ACTIVE) ? ui_lerp(g_pal.accent, g_pal.white, 40)
                                       : ui_lerp(g_pal.panel, g_pal.white, 40);
    fill_rect(kx, y, h, h, kf);
    draw_rect_outline(kx, y, h, h, 1, ui_lerp(g_pal.panel, g_pal.border, 200));
    wid_notch(kx, y, h, h, FCN_ROUND, FOREB_SHADOW);
}

/* Map a pointer X on a slider track back to a value 0..max. */
int ui_slider_value_at(int x, int w, int max, int mx)
{
    if (w < 2) return 0;
    int v = (int)(((UINT64)(mx - x) * (UINT64)max) / (UINT64)(w - 1));
    if (v < 0) v = 0; if (v > max) v = max;
    return v;
}

void ui_menu(const char *const entries[], int count, int selected,
             int seconds_left)
{
    int px, py, pw, ph, eh, entries_top, vis;
    int gh = FOREB_GLYPH_H * g_uiscale;
    int first, row;
    int has_bar, pad = g_rs.pad < 4 ? 6 : g_rs.pad;

    ui_menu_layout(count, &px, &py, &pw, &ph, &eh, &entries_top, &vis);
    has_bar = (count > vis) && g_rs.show_scrollbar;

    ui_panel_frame(px, py, pw, ph);

    /* Optional header ("[ Boot Menu ]" + accent rule). */
    if (g_rs.title_bar) {
        int label_y = py + 8;
        draw_string(px + pad, label_y, FOREB_PANEL_LABEL, FOREB_TITLE, 0, 1, 1);
        draw_hline(px + 6, label_y + gh + 2, pw - 12, g_pal.accent);
        draw_hline(px + 6, label_y + gh + 3, pw - 12, FOREB_BORDER);
    }

    /* Content column: reserve caret, icon gutter and scrollbar space. */
    int isz    = eh - 12; if (isz < 10) isz = 10;
    int caretw = g_rs.show_caret ? (ui_gw() + 4) : 0;
    int gut    = g_rs.show_icons ? (isz + 10) : 0;
    int bar_x  = px + pad + (g_rs.icon_right ? 0 : gut);
    int bar_w  = pw - 2 * pad - gut - (has_bar ? 10 : 0);
    int cl     = bar_x + caretw;                       /* label left  */
    int cr     = bar_x + bar_w;                        /* label right */

    first = g_menu_first;
    if (first > count - vis) first = count - vis;
    if (first < 0) first = 0;
    g_menu_first = first;

    /* Slide-animation floating highlight uses the same selection renderer. */
    if (g_menu_hl_y >= 0) {
        int by = g_menu_hl_y, top = entries_top, bot = entries_top + vis * eh;
        if (by < top)            by = top;
        if (by > bot - (eh - 2)) by = bot - (eh - 2);
        ui_sel_bg(bar_x, by, bar_w, eh);
    }

    for (row = 0; row < vis && (first + row) < count; row++) {
        int idx    = first + row;
        int rowtop = entries_top + row * eh;
        int text_y = rowtop + (eh - gh) / 2;
        int is_sel = (idx == selected);
        UINT32 fg  = FOREB_TEXT;

        if (g_rs.dividers && row > 0)
            draw_hline(px + pad, rowtop, pw - 2 * pad, ui_lerp(FOREB_PANEL, FOREB_BORDER, 120));

        if (is_sel) {
            if (g_menu_hl_y < 0) fg = ui_sel_bg(bar_x, rowtop, bar_w, eh);
            else                 fg = FOREB_WHITE;
            /* Caret would collide with the bracket glyphs, so skip it there. */
            if (g_rs.show_caret && g_rs.sel_style != FSS_BRACKET)
                draw_string(bar_x + 2, text_y, ">", g_pal.accent, 0, 1, 1);
        }

        /* Label with alignment, optional [brackets] on the selected row. */
        const char *lbl = entries[idx];
        int tw = ui_strpx(lbl);
        int lx;
        if (g_rs.align == FAL_CENTER)      lx = cl + (cr - cl - tw) / 2;
        else if (g_rs.align == FAL_RIGHT)  lx = cr - tw;
        else                               lx = cl;
        if (lx < cl) lx = cl;

        if (is_sel && g_rs.sel_style == FSS_BRACKET) {
            draw_string(lx - ui_gw(), text_y, "[", g_pal.accent, 0, 1, 1);
            draw_string(lx + tw,      text_y, "]", g_pal.accent, 0, 1, 1);
        }
        draw_string(lx, text_y, lbl, fg, 0, 1, 1);
    }

    if (has_bar) {
        int tx, ty, tw, th, thy, thh;
        if (ui_menu_scrollbar(count, &tx, &ty, &tw, &th, &thy, &thh)) {
            fill_rect(tx, ty, tw, th, FOREB_SHADOW);
            draw_rect_outline(tx, ty, tw, th, 1, FOREB_BORDER);
            fill_rect(tx + 1, thy + 1, tw - 2, thh - 2, g_pal.accent);
        }
    }

    if (seconds_left >= 0 && g_rs.show_timer) {
        char num[24], msg[48];
        int n = ui_u64_dec((UINT64)seconds_left, num);
        int k = 0, j;
        const char *pre = "Auto-boot in ", *suf = " sec";
        for (j = 0; pre[j]; j++) msg[k++] = pre[j];
        for (j = 0; j < n; j++)  msg[k++] = num[j];
        for (j = 0; suf[j]; j++) msg[k++] = suf[j];
        msg[k] = 0;
        if (g_rs.gradient) {
            /* Endpoint colors are loop-invariant; hoist them (like
             * ui_panel_frame) and drop the +py/-py that cancels in the lerp
             * position. One ui_lerp per row instead of three. */
            UINT32 gtop = ui_lerp(FOREB_PANEL, FOREB_WHITE, 16);
            UINT32 gbot = ui_lerp(FOREB_PANEL, FOREB_SHADOW, 46);
            /* Clip the countdown strip span once, write each row to the buffer,
             * one dirty mark (vs gh+2 re-clipping/re-marking fill_rect calls). */
            int sx0 = px + 6, sx1 = px + pw - 6;
            int sy0 = py + ph - gh - 6, sh = gh + 2;
            int rx0 = sx0 < g_clx0 ? g_clx0 : sx0;
            int rx1 = (sx1 > g_clx1) ? g_clx1 : sx1;
            for (int gy = 0; gy < sh; gy++) {
                int ry = sy0 + gy;
                if (ry < g_cly0 || ry >= g_cly1 || rx0 >= rx1) continue;
                ui_raw_fill(rx0, ry, rx1 - rx0, 1,
                            ui_pack(ui_lerp(gtop, gbot, (sy0 - py + gy) * 256 / ph)));
            }
            ui_mark_dirty(sx0, sy0, pw - 12, sh);
        } else
            fill_rect(px + 6, py + ph - gh - 6, pw - 12, gh + 2, FOREB_PANEL);
        draw_string_center(px + pw / 2, py + ph - gh - 6, msg, FOREB_TIMER, 0, 1, 1);
    }

    if (g_rs.show_footer)
        draw_string_center((int)g_w / 2, UI_FH(UIP_FOOTER_Y),
                           FOREB_FOOTER_HINT, FOREB_DIM, 0, 1, 1);
}

/* ------------------------------------------------------------------ */
/*  In-place progress bar + status line                                */
/* ------------------------------------------------------------------ */
void ui_progress(const char *label, UINT64 cur, UINT64 total)
{
    int bx = UI_FW(UIP_PROGRESS_X);
    int by = UI_FH(UIP_PROGRESS_Y);
    int bw = UI_FW(UIP_PROGRESS_W);
    int bh = UI_FH(UIP_PROGRESS_H);
    int inner_x, inner_y, inner_w, inner_h, fillw;
    int gh = FOREB_GLYPH_H * g_uiscale;   /* actual rendered glyph height */
    unsigned pct;
    char pbuf[8];
    int n;

    if (bw < 16) bw = 16;
    if (bh < 8)  bh = 8;

    /* Percentage (clamped). */
    if (total == 0) {
        pct = 100;
    } else {
        if (cur > total) cur = total;
        pct = (unsigned)((cur * 100u) / total);
    }

    /* Label above the bar (clear its strip first, then redraw). */
    if (label) {
        int ly = by - (gh + 4);
        fill_rect(bx, ly, bw, gh, FOREB_BG);
        draw_string(bx, ly, label, FOREB_TEXT, 0, 1, 1);
    }

    /* Bar frame + track. */
    draw_rect_outline(bx - 1, by - 1, bw + 2, bh + 2, 1, FOREB_BORDER);
    inner_x = bx;
    inner_y = by;
    inner_w = bw;
    inner_h = bh;
    fill_rect(inner_x, inner_y, inner_w, inner_h, FOREB_PROGRESS_TRACK);

    /* Proportional fill. */
    fillw = (int)(((UINT64)inner_w * pct) / 100u);
    if (fillw > 0) fill_rect(inner_x, inner_y, fillw, inner_h, FOREB_PROGRESS_FILL);

    /* Centered percentage text. */
    n = ui_u64_dec((UINT64)pct, pbuf);
    pbuf[n] = '%';
    pbuf[n + 1] = 0;
    draw_string_center(bx + bw / 2, by + (bh - gh) / 2,
                       pbuf, FOREB_WHITE, 0, 1, 1);
}

void ui_status(const char *line)
{
    int bx = UI_FW(UIP_PROGRESS_X);
    int by = UI_FH(UIP_PROGRESS_Y);
    int bw = UI_FW(UIP_PROGRESS_W);
    int bh = UI_FH(UIP_PROGRESS_H);
    int sy;

    if (bw < 16) bw = 16;
    if (bh < 8)  bh = 8;
    sy = by + bh + 6;

    /* Clear the status strip then draw in place (never scrolls). */
    fill_rect(bx, sy, bw, FOREB_GLYPH_H * g_uiscale + 2, FOREB_BG);
    if (line) draw_string(bx, sy, line, FOREB_DIM, 0, 1, 1);
}
