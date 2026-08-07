/*
 * forebo_theme.h - Shared ForeB forest-theme UI constants (C, BIOS + UEFI).
 *
 * Colors are packed 0x00RRGGBB (24-bit). On a little-endian x86 32bpp GOP
 * framebuffer whose PixelFormat is PixelBlueGreenRedReserved8BitPerColor
 * (the x86 default = BGRX in memory), store the u32 value directly: the
 * bytes land as B,G,R,X and the color is correct. If PixelFormat is
 * PixelRedGreenBlueReserved8BitPerColor (RGBX), swap R and B before store
 * (see FOREB_SWAP_RB below).
 *
 * RGB values are the exact BIOS 8bpp palette (DAC 6-bit -> RGB888, rounded
 * v8 = round(v6 * 255 / 63)) so BIOS and UEFI render pixel-identical.
 *
 * Layout constants come in two forms:
 *   - Absolute pixel constants (FOREB_*_PX / MENU_X ...) matching the BIOS
 *     800x600 reference design (config.h). Use on the BIOS path.
 *   - Fractional constants (FOREB_F_*) expressed as 0..1 of screen W/H, so
 *     the UEFI renderer scales the same layout to any GOP resolution.
 */
#ifndef FOREB_THEME_H
#define FOREB_THEME_H

/* ------------------------------------------------------------------ */
/*  Forest palette - 0x00RRGGBB                                        */
/* ------------------------------------------------------------------ */
/* name            RGB888            DAC(6-bit)  role                  */
#define FOREB_BG            0x00182D18u  /* (24,45,24)    dark forest background      */
#define FOREB_PANEL         0x001C351Cu  /* (28,53,28)    menu panel fill            */
#define FOREB_BORDER        0x00285128u  /* (40,81,40)    separators / outlines      */
#define FOREB_SELECT        0x00146514u  /* (20,101,20)   selected-entry highlight   */
#define FOREB_TITLE         0x0051CA3Du  /* (81,202,61)   title / menu label (leaf)  */
#define FOREB_TEXT          0x00B6DFB6u  /* (182,223,182) normal entry text (mint)   */
#define FOREB_DIM           0x00658265u  /* (101,130,101) subtitles / hints (grey)   */
#define FOREB_TIMER         0x00DFA214u  /* (223,162,20)  countdown timer (amber)    */
#define FOREB_WHITE         0x00FFFFFFu  /* (255,255,255) selected label + '>' arrow */
#define FOREB_SHADOW        0x00040804u  /* (4,8,4)       drop shadow / near-black   */
#define FOREB_TREE1         0x003D1C08u  /* (61,28,8)     tree trunk (brown)         */
#define FOREB_TREE2         0x001C791Cu  /* (28,121,28)   foliage mid green          */
#define FOREB_TREE3         0x003DB63Du  /* (61,182,61)   foliage highlight          */

/* Progress bar (new; not in BIOS palette). Track = panel-dark, fill = leaf. */
#define FOREB_PROGRESS_TRACK  FOREB_BORDER   /* empty portion of the load bar     */
#define FOREB_PROGRESS_FILL   FOREB_TITLE    /* filled portion of the load bar    */

/* Optional gradient endpoints for the background (top darker -> bottom lit). */
#define FOREB_BG_TOP        0x00102010u  /* (16,32,16)   top of vertical gradient    */
#define FOREB_BG_BOTTOM     0x001E3A1Eu  /* (30,58,30)   bottom of vertical gradient  */

/* ------------------------------------------------------------------ */
/*  Pixel-format helper                                               */
/* ------------------------------------------------------------------ */
/* Swap R<->B of a 0x00RRGGBB value (for RGBX framebuffers). */
static inline unsigned int foreb_swap_rb(unsigned int c)
{
    return (c & 0x0000FF00u)
         | ((c & 0x00FF0000u) >> 16)
         | ((c & 0x000000FFu) << 16);
}

/* ------------------------------------------------------------------ */
/*  Absolute layout (BIOS 800x600 reference, from config.h)           */
/* ------------------------------------------------------------------ */
#define FOREB_REF_W          800
#define FOREB_REF_H          600

#define FOREB_MENU_X         160
#define FOREB_MENU_Y         160
#define FOREB_MENU_W         480
#define FOREB_MENU_H         300
#define FOREB_TITLE_X        200
#define FOREB_TITLE_Y        60
#define FOREB_TIMER_X        340
#define FOREB_TIMER_Y        480
#define FOREB_ENTRY_Y_START  240
#define FOREB_ENTRY_HEIGHT   32
#define FOREB_LOGO_X         340
#define FOREB_LOGO_Y         80

#define FOREB_TITLEBAR_Y     28    /* horizontal title rule y */
#define FOREB_TITLEBAR_H     2     /* title rule thickness    */
#define FOREB_MARGIN_X       16    /* left/right screen margin for rules */
#define FOREB_FOOTER_Y_FROM_BOTTOM 42  /* footer rule = screen_h - 42 */

/* Progress bar absolute rect (inside the panel, reference resolution). */
#define FOREB_PROGRESS_X     176   /* MENU_X + 16 */
#define FOREB_PROGRESS_Y     400   /* inside panel, above footer */
#define FOREB_PROGRESS_W     448   /* MENU_W - 32 */
#define FOREB_PROGRESS_H     18

/* ------------------------------------------------------------------ */
/*  Fractional layout (UEFI - scales to any GOP resolution)           */
/*  Multiply by fb_w / fb_h and round. All origins top-left.          */
/* ------------------------------------------------------------------ */
#define FOREB_F_TITLEBAR_Y      0.047   /* title rule                 */
#define FOREB_F_TITLE_Y         0.100   /* title text baseline-ish    */
#define FOREB_F_MARGIN_X        0.020   /* side margin fraction of W  */

#define FOREB_F_LOGO_CX         0.500   /* logo center X (fraction W) */
#define FOREB_F_LOGO_CY         0.230   /* logo center Y (fraction H) */
#define FOREB_F_LOGO_W          0.140   /* logo box width  (frac W)   */
#define FOREB_F_LOGO_H          0.170   /* logo box height (frac H)   */

#define FOREB_F_PANEL_X         0.200   /* menu panel left            */
#define FOREB_F_PANEL_Y         0.360   /* menu panel top             */
#define FOREB_F_PANEL_W         0.600   /* menu panel width           */
#define FOREB_F_PANEL_H         0.420   /* menu panel height          */

#define FOREB_F_ENTRY_H         0.055   /* entry row height (frac H)  */

#define FOREB_F_PROGRESS_X      0.220   /* load bar left              */
#define FOREB_F_PROGRESS_Y      0.860   /* load bar top               */
#define FOREB_F_PROGRESS_W      0.560   /* load bar width             */
#define FOREB_F_PROGRESS_H      0.030   /* load bar height            */
#define FOREB_F_STATUS_Y        0.820   /* status line above the bar  */

#define FOREB_F_FOOTER_Y        0.940   /* footer hint line           */

/* ------------------------------------------------------------------ */
/*  Menu content (shared strings + tunables)                          */
/* ------------------------------------------------------------------ */
#define FOREB_TITLE_STR      "ForeB - Forest Bootloader"
#define FOREB_SUBTITLE_STR   "Forest OS Boot Manager"
#define FOREB_PANEL_LABEL    "[ Boot Menu ]"
#define FOREB_FOOTER_HINT    "[Up/Down] Navigate  [Enter] Boot  [Esc] Reset"

#define FOREB_BOOT_ENTRY_COUNT   4
#define FOREB_DEFAULT_ENTRY      0
#ifndef FOREB_DEFAULT_TIMEOUT
#define FOREB_DEFAULT_TIMEOUT    5   /* seconds */
#endif

/* Font cell size. UEFI renderer now uses the crisp 8x16 cell (font8x16.h);
 * the BIOS path still uses its own 8x8 ROM font independently of these. */
#define FOREB_GLYPH_W        8
#define FOREB_GLYPH_H        16

#endif /* FOREB_THEME_H */
