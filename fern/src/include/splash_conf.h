#ifndef SPLASH_CONF_H
#define SPLASH_CONF_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Splash Configuration and Layout Parser
 *
 * Parses .splconf (key=value config) and .spllayout (element positioning)
 * files from the initrd VFS. No dynamic memory allocation - uses the
 * VFS buffer in-place with null-termination over delimiters.
 */

/* ---------------------------------------------------------------------------
 * .splconf Parser
 * ------------------------------------------------------------------------- */

#define SPLCONF_MAX_SECTIONS    16
#define SPLCONF_MAX_ENTRIES     48
#define SPLCONF_MAX_KEY_LEN     32
#define SPLCONF_MAX_VAL_LEN     80

typedef enum {
    SPLCONF_OK = 0,
    SPLCONF_ERR_NOT_FOUND,
    SPLCONF_ERR_INVALID_VALUE,
    SPLCONF_ERR_BUFFER_FULL,
    SPLCONF_ERR_IO
} splconf_err_t;

typedef struct {
    char     key[SPLCONF_MAX_KEY_LEN];
    char     val[SPLCONF_MAX_VAL_LEN];
} splconf_entry_t;

typedef struct {
    char              name[SPLCONF_MAX_KEY_LEN];
    splconf_entry_t   entries[SPLCONF_MAX_ENTRIES];
    uint8_t           count;
} splconf_section_t;

typedef struct {
    splconf_section_t sections[SPLCONF_MAX_SECTIONS];
    uint8_t           count;
    bool              loaded;
} splconf_t;

/* Parse a .splconf buffer in-place. Returns SPLCONF_OK on success. */
splconf_err_t splconf_parse(splconf_t* cfg, char* buf, uint32_t len);

/* Lookup helpers. Return def if key not found. */
bool        splconf_get_bool  (const splconf_t* cfg, const char* section, const char* key, bool def);
int32_t     splconf_get_int   (const splconf_t* cfg, const char* section, const char* key, int32_t def);
const char* splconf_get_string(const splconf_t* cfg, const char* section, const char* key, const char* def);
uint32_t    splconf_get_color (const splconf_t* cfg, const char* section, const char* key, uint32_t def);

/* ---------------------------------------------------------------------------
 * .spllayout Element Types
 * ------------------------------------------------------------------------- */

typedef enum {
    SPL_ELEMTYPE_BACKGROUND = 0,
    SPL_ELEMTYPE_LOGO,
    SPL_ELEMTYPE_VERSION,
    SPL_ELEMTYPE_PROGRESS_BAR,
    SPL_ELEMTYPE_STATUS_TEXT,
    SPL_ELEMTYPE_CUSTOM_TEXT,
    SPL_ELEMTYPE_IMAGE,
    SPL_ELEMTYPE_RECTANGLE,
    SPL_ELEMTYPE_COUNT
} spl_elem_type_t;

typedef enum {
    SPL_BAR_STYLE_SOLID = 0,
    SPL_BAR_STYLE_SEGMENTED,
    SPL_BAR_STYLE_MARQUEE,
    SPL_BAR_STYLE_DOTS,
    SPL_BAR_STYLE_PULSE
} spl_bar_style_t;

typedef enum {
    SPL_BG_SOLID = 0,
    SPL_BG_GRADIENT,
    SPL_BG_IMAGE
} spl_bg_type_t;

typedef enum {
    SPL_ANCHOR_TOP_LEFT = 0,
    SPL_ANCHOR_TOP_CENTER,
    SPL_ANCHOR_TOP_RIGHT,
    SPL_ANCHOR_CENTER_LEFT,
    SPL_ANCHOR_CENTER,
    SPL_ANCHOR_CENTER_RIGHT,
    SPL_ANCHOR_BOTTOM_LEFT,
    SPL_ANCHOR_BOTTOM_CENTER,
    SPL_ANCHOR_BOTTOM_RIGHT
} spl_anchor_t;

/* Position: can be absolute pixels, percent of screen, or relative */
typedef struct {
    int32_t  value;        /* pixels or percent*100 */
    bool     is_percent;   /* true = value is percent*100 */
    bool     is_relative;  /* true = relative to another element */
    char     ref_elem[32]; /* name of reference element */
    char     ref_edge[16]; /* "bottom", "top", "right", "left" */
    int32_t  ref_offset;   /* offset from reference edge */
} spl_pos_t;

#define SPL_MAX_ELEMENTS    16
#define SPL_MAX_TEXT_LEN    64

typedef struct {
    spl_elem_type_t  type;
    char             name[32];
    spl_pos_t        x, y;
    spl_pos_t        width, height;
    spl_anchor_t     anchor;
    uint8_t          z_order;
    uint8_t          opacity;
    bool             visible;

    /* Type-specific data */
    union {
        struct {
            spl_bg_type_t bg_type;
            uint32_t      color;         /* solid color */
            uint32_t      gradient_top;  /* gradient top color */
            uint32_t      gradient_bot;  /* gradient bottom color */
        } background;

        struct {
            char     text[SPL_MAX_TEXT_LEN];
            uint32_t color;
            uint32_t scale;
            bool     shadow;
            uint32_t shadow_color;
        } text;

        struct {
            spl_bar_style_t style;
            uint32_t        trough_color;
            uint32_t        fill_color;
            uint32_t        highlight_color;
            uint32_t        border_color;
            uint32_t        border_width;
            uint32_t        corner_radius;
            uint32_t        marquee_seg_width;
            uint32_t        marquee_speed;
            uint32_t        segment_gap;
        } bar;

        struct {
            char     path[64];
            uint32_t key_color;  /* transparent key color, 0xFFFFFFFF = none */
        } image;
    } data;
} spl_element_t;

/* ---------------------------------------------------------------------------
 * .spllayout Parser
 * ------------------------------------------------------------------------- */

typedef struct {
    spl_element_t elements[SPL_MAX_ELEMENTS];
    uint8_t       count;
    bool          loaded;
} spllayout_t;

/* Parse a .spllayout buffer in-place. Returns SPLCONF_OK on success. */
splconf_err_t spllayout_parse(spllayout_t* layout, const splconf_t* conf,
                              char* buf, uint32_t len);

/* Resolve relative positions to absolute pixel coordinates. */
void spllayout_resolve(spllayout_t* layout, uint32_t screen_w, uint32_t screen_h);

/* Find an element by name. Returns NULL if not found. */
spl_element_t* spllayout_find(spllayout_t* layout, const char* name);

/* ---------------------------------------------------------------------------
 * Config File Loading
 * ------------------------------------------------------------------------- */

/* Load .splconf from VFS path. Returns SPLCONF_OK on success. */
splconf_err_t splconf_load(splconf_t* cfg, const char* path);

/* Load .spllayout from VFS path. Returns SPLCONF_OK on success. */
splconf_err_t spllayout_load(spllayout_t* layout, const splconf_t* cfg, const char* path);

/* Parse a hex color string "#RRGGBB" to uint32_t 0xRRGGBB. */
uint32_t spl_parse_color(const char* str, uint32_t def);

/* Parse a position value: "50%", "100", "center", "below:logo:10" */
spl_pos_t spl_parse_position(const char* str);

#endif /* SPLASH_CONF_H */
