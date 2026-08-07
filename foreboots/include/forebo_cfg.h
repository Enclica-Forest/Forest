/*
 * forebo_cfg.h - Parsed-config data model for ForeB (grub.cfg-like).
 *
 * Shared header describing the in-memory result of parsing forebo.cfg from
 * the ESP. It is intentionally POD (plain C structs, fixed-size buffers, no
 * pointers, no allocation) so the same struct can be:
 *   - filled by the parser (uefi/config.c),
 *   - read by the boot menu / handoff (uefi/bootx64.c),
 *   - inspected/edited by the interactive shell (uefi/shell.c),
 * all without a heap and while remaining valid after ExitBootServices.
 *
 * ---------------------------------------------------------------------------
 *  Config file format (see forebo.cfg for a worked example)
 * ---------------------------------------------------------------------------
 *  Global keys (one per line, before/outside any menuentry block):
 *      timeout=<seconds>        auto-boot countdown (0 = boot default now)
 *      default=<N|path>         pre-selected / auto-booted entry (see below)
 *      remember_last=0|1        persist the last booted entry in a UEFI
 *                               variable and pre-select it on next boot
 *      background=<path>        default menu background image (BMP/TGA)
 *
 *  Per-entry block:
 *      menuentry "Title" {
 *          type=<kind>          forest|linux|chainload|shell|recovery|reboot|
 *                               tools|setup (aliases: firmware->setup)
 *                               (optional; defaults to 'forest' == legacy)
 *          kernel=<path>        multiboot1 ELF to load (forest entries)
 *          module=<path>        a boot module (initrd, etc.); repeatable
 *          module2=<path>       accepted ALIAS of 'module' (grub compat)
 *          cmdline="<string>"   kernel/Linux command line
 *          vmlinuz=<path>       EFI-stub Linux kernel (type=linux)
 *          initrd=<path>        Linux initramfs delivered via LoadFile2
 *          chain=<path>         EFI app to chainload (type=chainload), e.g.
 *                               \EFI\BOOT\BOOTX64.EFI or \EFI\<distro>\grubx64.efi
 *          background=<path>    per-entry background override (optional)
 *          icon=<path>          per-entry icon (TGA/BMP w/ alpha, optional)
 *      }
 *
 *  Submenu block (groups entries under a collapsible menu level, Limine-style):
 *      submenu "Title" {
 *          icon=<path|name>     optional icon for the submenu row itself
 *          menuentry "Child" { ... }
 *          submenu "Deeper" { ... }     (nesting capped at 8 levels)
 *      }
 *  Rows are stored flat in file order; each row's 'parent' field holds the
 *  flat index of its enclosing submenu row, or -1 at top level. The menu
 *  renders one level at a time; Enter/Right descends, Esc/Left goes back.
 *  A '}' closes the innermost open block (menuentry OR submenu).
 *
 *  The 'default=' key selects the pre-selected/auto-booted entry:
 *      default=2                    0-based index counting ONLY top-level
 *                                   rows (parent == -1; submenu rows count)
 *      default=CachyOS/linux-rt     path of titles, matched case-sensitively
 *                                   level by level from the top; intermediate
 *                                   segments must name submenu rows
 *  In both cases a resolved default that lands on a submenu row descends to
 *  its first child (repeated). On any resolution failure the default falls
 *  back to the first top-level non-submenu row (0 if none) and is clamped
 *  into [0, count-1]. The raw non-numeric default= string is kept in
 *  cfg->default_path ("" when default= was numeric or absent).
 *
 *  remember_last=1 makes the UEFI loader persist the flat index of every
 *  booted forest/linux/chainload entry into the UEFI variable
 *  "ForeBLastEntry" (vendor GUID {46524542-4F4F-5442-8001-466F72654231},
 *  attributes NV|BS) and use it as the default on the next boot (with the
 *  same descend/fallback rules; any variable error keeps the config default).
 *
 *  Entry TYPES (the 'type' key selects the boot method):
 *      forest    - Multiboot1 ELF handoff to the Forest kernel (x86_64 only).
 *                  This is the default and preserves all legacy behaviour.
 *      linux     - Boot vmlinuz as an EFI-stub PE via LoadImage/StartImage,
 *                  cmdline -> LoadOptions, initrd -> LoadFile2 media protocol.
 *      chainload - LoadImage/StartImage another EFI bootloader (GRUB on USB,
 *                  Windows Boot Manager, ...). Uses 'chain='; if empty, ForeB
 *                  auto-scans all volumes for \EFI\BOOT\BOOTX64.EFI etc.
 *      shell     - Open the interactive ForeB shell window from the menu.
 *      recovery  - Open the Recovery/disk-tools window from the menu.
 *      reboot    - Firmware reset (RuntimeServices->ResetSystem). Legacy alias:
 *                  kernel=reboot still works and maps to this type.
 *      tools     - Open the windowed GUI Tools launcher (Disk Info, GPT Viewer,
 *                  File Browser, Hex Viewer, Memory Map, EFI Variables, ...).
 *      setup     - Reboot into the firmware/UEFI setup screen by setting the
 *                  OsIndications runtime variable (EFI_OS_INDICATIONS_BOOT_TO_
 *                  FW_UI) then ResetSystem(EfiResetCold). Requires the firmware
 *                  to advertise the bit in OsIndicationsSupported; if absent the
 *                  loader reports it and stays in the menu. Alias: firmware.
 *
 *  Global THEME / customization keys (outside any menuentry block):
 *      color_bg / color_fg / color_accent / color_sel_bg / color_sel_fg /
 *      color_titlebar / color_window / color_cursor   = 0xRRGGBB
 *      cursor=<path>            cursor sprite (TGA w/ alpha)
 *      cursor_enabled=0|1       show the mouse cursor
 *      mouse_enabled=0|1        poll pointer protocols
 *      animations=0|1           fades/particles/spinner on/off
 *      double_buffer=0|1        off-screen back buffer + ui_present (default 1)
 *      window_skin=<name>       compositor window style (flat|beveled|glass)
 *
 *  Paths are ESP-absolute using either '/' or '\' separators, e.g.
 *  /forebo/kernel.elf. Quotes around title/cmdline are stripped by the
 *  parser. Lines beginning with '#' are comments.
 * ---------------------------------------------------------------------------
 */
#ifndef FOREB_CFG_H
#define FOREB_CFG_H

/* ------------------------------------------------------------------ */
/*  Capacity limits (fixed-size, no allocation)                       */
/* ------------------------------------------------------------------ */
#define FOREB_CFG_MAX_ENTRIES     64   /* max menuentry+submenu blocks     */
#define FOREB_CFG_MAX_MODULES      8   /* max modules per entry          */
#define FOREB_CFG_TITLE_LEN       64   /* incl. NUL                      */
#define FOREB_CFG_NAME_LEN        24   /* theme preset name, incl. NUL   */
#define FOREB_CFG_PATH_LEN       256   /* kernel/module/bg/icon, incl NUL*/
#define FOREB_CFG_CMDLINE_LEN    256   /* kernel command line, incl NUL  */

/* Maximum submenu nesting depth accepted by the parser (fixed-size stack). */
#define FOREB_CFG_MAX_SUBMENU_DEPTH 8

/* Default timeout used when the config omits 'timeout='. */
#ifndef FOREB_CFG_DEFAULT_TIMEOUT
#define FOREB_CFG_DEFAULT_TIMEOUT  10  /* seconds */
#endif

/* Path the loader reads the config from on the ESP (UEFI). */
#define FOREB_CFG_ESP_PATH        "\\forebo\\forebo.cfg"

/* ------------------------------------------------------------------ */
/*  Boot method for one menu entry (the 'type=' key)                  */
/*  FOREST == 0 so a zeroed struct == legacy Forest behaviour.        */
/* ------------------------------------------------------------------ */
enum forebo_entry_type {
    FOREB_ENTRY_FOREST    = 0,   /* Multiboot1 Forest kernel (x86_64 only)   */
    FOREB_ENTRY_LINUX     = 1,   /* EFI-stub vmlinuz + initrd (LoadFile2)    */
    FOREB_ENTRY_CHAINLOAD = 2,   /* LoadImage/StartImage another EFI app     */
    FOREB_ENTRY_SHELL     = 3,   /* open the interactive shell window        */
    FOREB_ENTRY_RECOVERY  = 4,   /* open the recovery / disk-tools window    */
    FOREB_ENTRY_REBOOT    = 5,   /* RuntimeServices->ResetSystem             */
    /* --- appended (values kept stable; do NOT renumber the above) --- */
    FOREB_ENTRY_TOOLS     = 6,   /* open the GUI Tools launcher window       */
    FOREB_ENTRY_FWSETUP   = 7,   /* reboot into firmware/BIOS setup via the  */
                                 /* OsIndications runtime variable           */
    FOREB_ENTRY_SUBMENU   = 8,   /* submenu row: groups child rows (parent   */
                                 /* links); not bootable, Enter descends     */
    FOREB_ENTRY_SETTINGS  = 9,   /* open the windowed Settings / theme editor */
    FOREB_ENTRY_UEFI_SETTINGS = 10 /* UEFI firmware settings panel (view/edit) */
};

/* ------------------------------------------------------------------ */
/*  Window-skin style for the compositor (the 'window_skin=' key)     */
/* ------------------------------------------------------------------ */
enum forebo_window_skin {
    FOREB_SKIN_FLAT    = 0,      /* solid title bar + 1px border             */
    FOREB_SKIN_BEVELED = 1,      /* raised bevel highlight/shadow edges      */
    FOREB_SKIN_GLASS   = 2       /* translucent alpha-blended title bar      */
};

/* ------------------------------------------------------------------ */
/*  Boot-menu STYLE system (menu_style= preset + granular overrides).  */
/*  Every field is an int; -1 means "inherit from the chosen preset".  */
/* ------------------------------------------------------------------ */
enum foreb_menu_pos {                 /* where the menu panel sits          */
    FMP_CENTER = 0, FMP_LEFT, FMP_RIGHT, FMP_TOP, FMP_BOTTOM,
    FMP_FULL, FMP_CUSTOM
};
enum foreb_sel_style {                /* how the selected row is marked     */
    FSS_BAR = 0,      /* solid/gradient highlight bar (classic)             */
    FSS_BOX,          /* outlined box around the row                       */
    FSS_OUTLINE,      /* thin accent outline only                          */
    FSS_UNDERLINE,    /* accent rule under the label                       */
    FSS_ARROW,        /* just the ">" caret, no fill                       */
    FSS_BRACKET,      /* [ label ] brackets                                */
    FSS_INVERT,       /* swap fg/bg (text color block)                     */
    FSS_PILL,         /* rounded-cap accent pill                           */
    FSS_GRADIENT,     /* horizontal accent->panel gradient bar             */
    FSS_GLOW,         /* accent bar + soft edge glow                       */
    FSS_NONE,         /* color the text only                               */
    FSS_DOUBLEBAR     /* bar + left accent stripe (default forest look)    */
};
enum foreb_border_style {             /* panel frame                        */
    FBD_NONE = 0, FBD_THIN, FBD_THICK, FBD_DOUBLE, FBD_SHADOW,
    FBD_GLOW, FBD_DASHED
};
enum foreb_corner_style { FCN_SQUARE = 0, FCN_ROUND, FCN_CUT };
enum foreb_align        { FAL_LEFT = 0, FAL_CENTER, FAL_RIGHT };

/* ------------------------------------------------------------------ */
/*  Widget (button / generic UI element) appearance.                   */
/*  -1 (ints) / FOREB_COLOR_UNSET (colors) = inherit the built-in look. */
/* ------------------------------------------------------------------ */
enum foreb_btn_style {                /* btn_style= */
    FBTN_FLAT = 0, FBTN_RAISED, FBTN_PILL, FBTN_OUTLINE, FBTN_GHOST, FBTN_GLASS
};
enum foreb_btn_state {                /* runtime draw argument */
    FBTN_NORMAL = 0, FBTN_HOVER, FBTN_ACTIVE, FBTN_FOCUSED, FBTN_DISABLED
};

struct forebo_widget {
    /* button appearance (btn_*) */
    int style;                         /* enum foreb_btn_style              */
    int corner;                        /* enum foreb_corner_style           */
    int border_w;                      /* outline px (-1 auto)              */
    int pad_x, pad_y;                  /* label padding px (-1 auto)        */
    int gradient, shadow, glow;        /* 0/1/-1                            */
    unsigned int face_normal, face_hover, face_active, face_disabled;
    unsigned int text_normal, text_hover, text_active, text_disabled;
    unsigned int border_col;           /* UNSET = derive from face          */
    unsigned int focus_col;            /* UNSET = accent                    */
    /* generic UI knobs (ui_*) */
    int window_corner;                 /* enum foreb_corner_style           */
    int window_border_w;
    int panel_alpha;                   /* 0..255 (255 = opaque)             */
    unsigned int separator;
    int scrollbar_w;
    unsigned int scrollbar_color;
    unsigned int focus_color;
    int focus_width;
    int font_scale;                    /* percent, 100 = 1x                 */
};

struct forebo_style {
    char preset[FOREB_CFG_NAME_LEN]; /* menu_style= name ("" = built-in)    */
    /* geometry (permille of screen where noted; -1 = auto from `pos`) */
    int pos;              /* enum foreb_menu_pos                            */
    int panel_x, panel_y; /* permille override of top-left                 */
    int panel_w, panel_h; /* permille override of size                     */
    int entry_h;          /* per-row height, permille of H                 */
    int pad;              /* inner padding, px                             */
    int align;            /* enum foreb_align (label alignment)            */
    /* appearance toggles: -1 inherit, else 0/1 or enum */
    int sel_style;        /* enum foreb_sel_style                          */
    int border;           /* enum foreb_border_style                       */
    int corner;           /* enum foreb_corner_style                       */
    int accent_strip;     /* colored strip along the panel top             */
    int dividers;         /* thin rule between rows                         */
    int gradient;         /* vertical gradient panel body                  */
    int shadow;           /* drop shadow under the panel                   */
    int title_bar;        /* draw the "[ Boot Menu ]" header               */
    int show_title;       /* draw the big centered scene title             */
    int show_footer;      /* draw the key-hint footer                      */
    int show_timer;       /* draw the auto-boot countdown                  */
    int show_icons;       /* draw per-entry icons                          */
    int icon_right;       /* 1 = icons on the right gutter, 0 = left       */
    int show_scrollbar;   /* draw the scrollbar when overflowing           */
    int show_caret;       /* draw the ">" caret on the selected row        */
};

/* ------------------------------------------------------------------ */
/*  One boot menu entry (menuentry block)                             */
/* ------------------------------------------------------------------ */
struct forebo_menuentry {
    int  type;                                       /* enum forebo_entry_type */
    char title[FOREB_CFG_TITLE_LEN];                 /* display label        */
    char kernel[FOREB_CFG_PATH_LEN];                 /* multiboot ELF path   */
    char modules[FOREB_CFG_MAX_MODULES][FOREB_CFG_PATH_LEN]; /* module paths */
    int  module_count;                               /* # of valid modules   */
    char cmdline[FOREB_CFG_CMDLINE_LEN];             /* kernel/Linux cmdline */
    char vmlinuz[FOREB_CFG_PATH_LEN];                /* type=linux kernel    */
    char initrd[FOREB_CFG_PATH_LEN];                 /* type=linux initramfs */
    char chain[FOREB_CFG_PATH_LEN];                  /* type=chainload target */
    char background[FOREB_CFG_PATH_LEN];             /* per-entry bg (or "")  */
    char icon[FOREB_CFG_PATH_LEN];                   /* per-entry icon (or "") */
    int  parent;                     /* flat index of the enclosing submenu   */
                                     /* row, or -1 for a top-level row        */
};

/* ------------------------------------------------------------------ */
/*  Theme / customization block (global)                              */
/*  Colors are 0x00RRGGBB. 0xFFFFFFFF means "unset -> use built-in".  */
/* ------------------------------------------------------------------ */
#define FOREB_COLOR_UNSET  0xFFFFFFFFu

/* Built-in default palette (Forest greens on charcoal). */
#define FOREB_DEF_COLOR_BG        0x0E1A12u  /* deep forest charcoal        */
#define FOREB_DEF_COLOR_FG        0xDDE7DEu  /* soft off-white text         */
#define FOREB_DEF_COLOR_ACCENT    0x3FB56Bu  /* forest green accent         */
#define FOREB_DEF_COLOR_SEL_BG    0x1F5E3Au  /* selected-row background     */
#define FOREB_DEF_COLOR_SEL_FG    0xFFFFFFu  /* selected-row text           */
#define FOREB_DEF_COLOR_TITLEBAR  0x1F5E3Au  /* window title bar            */
#define FOREB_DEF_COLOR_WINDOW    0x16241Bu  /* window client background    */
#define FOREB_DEF_COLOR_CURSOR    0xFFFFFFu  /* cursor sprite fill          */
#define FOREB_DEF_UI_SEPARATOR    0x285128u  /* row / element separators    */
#define FOREB_DEF_UI_SCROLLBAR    0x3FB56Bu  /* scrollbar thumb             */
#define FOREB_DEF_UI_FOCUS        0x51CA3Du  /* keyboard focus ring         */

/* ------------------------------------------------------------------ */
/*  Per-window "CSS": fully overridable compositor window chrome.      */
/*  -1 (ints) / FOREB_COLOR_UNSET (colors) = inherit built-in/theme.   */
/*  The 3 window_skin presets remain the named starting points; these  */
/*  keys override individual aspects on top of the chosen preset.      */
/* ------------------------------------------------------------------ */
struct forebo_winskin {
    int          title_h;        /* title-bar height px (-1 = auto from font)    */
    unsigned int title_fill;     /* title bar bg   (UNSET = theme color_titlebar)*/
    unsigned int title_fg;       /* title text     (UNSET = theme color_sel_fg)  */
    unsigned int border_color;   /* window frame   (UNSET = theme color_accent)  */
    int          border_w;       /* frame px       (-1 = auto: 1)                */
    int          corner;         /* enum foreb_corner_style (-1 = square)        */
    int          shadow;         /* drop shadow 0/1 (-1 = auto: on)              */
    unsigned int close_color;    /* close-box fill (UNSET = 0x00B03030)          */
    int          button_style;   /* enum foreb_btn_style for window buttons (-1) */
};

struct forebo_theme {
    unsigned int color_bg;         /* menu/desktop background            */
    unsigned int color_fg;         /* default text                       */
    unsigned int color_accent;     /* highlights / progress / focus ring */
    unsigned int color_sel_bg;     /* selected menu row bg               */
    unsigned int color_sel_fg;     /* selected menu row fg               */
    unsigned int color_titlebar;   /* window title bar                   */
    unsigned int color_window;     /* window client area                 */
    unsigned int color_cursor;     /* cursor sprite fill                 */
    char cursor_path[FOREB_CFG_PATH_LEN]; /* optional cursor TGA ("" = builtin) */
    int  cursor_enabled;           /* draw the mouse cursor (1)          */
    int  mouse_enabled;            /* poll pointer protocols (1)         */
    int  animations_enabled;       /* fades/particles/spinner (1)        */
    int  double_buffer;            /* off-screen back buffer + present (1) */
    int  window_skin;              /* enum forebo_window_skin            */
    struct forebo_winskin winskin; /* granular window-chrome overrides (win_*) */
    char img_background[FOREB_CFG_PATH_LEN]; /* menu bg override of background= */
    char img_panel[FOREB_CFG_PATH_LEN];      /* menu-panel face (blit + tint)  */
    char img_window[FOREB_CFG_PATH_LEN];     /* wm window client face          */
    char img_titlebar[FOREB_CFG_PATH_LEN];   /* wm title-bar face              */
    char img_button[FOREB_CFG_PATH_LEN];     /* button face                    */
    char img_cursor[FOREB_CFG_PATH_LEN];     /* cursor sprite (alias of cursor=) */
    char preset[FOREB_CFG_NAME_LEN]; /* named palette (theme=), "" = forest */
    struct forebo_style style;     /* menu layout/appearance (menu_style=) */
    struct forebo_widget widget;   /* button / UI-element appearance        */
    int fx_glass;                  /* frosted-glass window backdrops (0/1)  */
    int fx_blur;                   /* backdrop blur radius px (0..32)       */
    int fx_opacity;                /* backdrop darken 0..255                */
    int fx_vignette;               /* screen-edge darken 0..255 (0=off)     */
    int fx_scanlines;              /* scanline dim 0..255 (0=off)           */
};

/* Reset a theme block to the built-in defaults. */
void forebo_theme_default(struct forebo_theme *t);

/* ------------------------------------------------------------------ */
/*  Whole parsed config                                               */
/* ------------------------------------------------------------------ */
struct forebo_config {
    struct forebo_menuentry entries[FOREB_CFG_MAX_ENTRIES];
    int  count;                                      /* # of valid entries   */
    int  default_idx;                                /* 0-based default sel  */
                                                     /* (flat, resolved +    */
                                                     /* submenu-descended)   */
    int  timeout;                                    /* auto-boot seconds    */
    char background[FOREB_CFG_PATH_LEN];             /* global default bg     */
    struct forebo_theme theme;                       /* colors + toggles      */
    int  remember_last;                              /* persist last booted   */
                                                     /* entry (UEFI var)      */
    char default_path[FOREB_CFG_PATH_LEN];           /* raw default= string   */
                                                     /* when non-numeric,     */
                                                     /* "" otherwise          */
};

/* ------------------------------------------------------------------ */
/*  Parser API (implemented in uefi/config.c)                         */
/* ------------------------------------------------------------------ */
/*
 * Reset a config to empty defaults (count=0, default_idx=0,
 * timeout=FOREB_CFG_DEFAULT_TIMEOUT, all strings "", every row's parent=-1,
 * remember_last=0, default_path=""). Always call before parsing so a
 * missing/partial config yields a sane, zeroed structure.
 */
void forebo_cfg_init(struct forebo_config *cfg);

/*
 * Parse a NUL-terminated (or length-bounded) config text buffer into cfg.
 * 'text' points at the raw bytes read from forebo.cfg; 'len' is its size.
 * Returns the number of menu entries parsed (>=0, submenu rows included).
 * On any malformed input the parser skips the offending line and continues
 * (never faults), so a partially-valid file still boots what it can.
 * default_idx is fully resolved on return (top-level index / title path ->
 * flat index, submenu rows descended to their first child) and clamped to
 * [0, count-1]; resolution failures fall back to the first top-level
 * non-submenu row.
 */
int  forebo_cfg_parse(struct forebo_config *cfg, const char *text, unsigned long len);

#endif /* FOREB_CFG_H */
