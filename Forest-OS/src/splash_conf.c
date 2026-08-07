/*
 * splash_conf.c - Splash configuration and layout parser
 *
 * Parses .splconf and .spllayout files from the initrd VFS.
 * No dynamic memory allocation - modifies input buffers in-place.
 */

#include "include/splash_conf.h"
#include "include/debuglog.h"
#include <string.h>

extern int vfs_read_file(const char* path, void* buf, uint32_t max_len, uint32_t* out_len);
extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

static int32_t atoi_simple(const char* s)
{
    int32_t r = 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; }
    return neg ? -r : r;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static char* skip_whitespace(char* p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char* trim_end(char* p, char* end)
{
    while (end > p && (*(end-1) == ' ' || *(end-1) == '\t' || *(end-1) == '\r' || *(end-1) == '\n'))
        end--;
    *end = '\0';
    return p;
}

static bool starts_with(const char* s, const char* prefix)
{
    while (*prefix) { if (*s != *prefix) return false; s++; prefix++; }
    return true;
}

/* Parse "#RRGGBB" to 0xRRGGBB */
uint32_t spl_parse_color(const char* str, uint32_t def)
{
    if (!str || str[0] != '#') return def;
    str++;
    uint32_t val = 0;
    while (*str) {
        char c = *str;
        val <<= 4;
        if (c >= '0' && c <= '9')      val |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') val |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (uint32_t)(c - 'A' + 10);
        else return def;
        str++;
    }
    return val;
}

/* Parse "50%" -> {value=5000, is_percent=true} or "100" -> {value=100, is_percent=false} */
spl_pos_t spl_parse_position(const char* str)
{
    spl_pos_t pos;
    memset(&pos, 0, sizeof(pos));

    if (!str || !*str) return pos;

    if (starts_with(str, "center")) {
        pos.value = 5000;
        pos.is_percent = true;
        return pos;
    }

    if (starts_with(str, "below:")) {
        pos.is_relative = true;
        str += 6;
        /* Parse "below:element_name:offset" */
        const char* colon = str;
        while (*colon && *colon != ':') colon++;
        uint32_t name_len = (uint32_t)(colon - str);
        if (name_len >= sizeof(pos.ref_elem)) name_len = sizeof(pos.ref_elem) - 1;
        memcpy(pos.ref_elem, str, name_len);
        pos.ref_elem[name_len] = '\0';
        strcpy(pos.ref_edge, "bottom");

        if (*colon == ':') {
            colon++;
            pos.ref_offset = 0;
            bool neg = false;
            if (*colon == '-') { neg = true; colon++; }
            while (*colon >= '0' && *colon <= '9') {
                pos.ref_offset = pos.ref_offset * 10 + (*colon - '0');
                colon++;
            }
            if (neg) pos.ref_offset = -pos.ref_offset;
        }
        return pos;
    }

    if (starts_with(str, "above:")) {
        pos.is_relative = true;
        str += 6;
        const char* colon = str;
        while (*colon && *colon != ':') colon++;
        uint32_t name_len = (uint32_t)(colon - str);
        if (name_len >= sizeof(pos.ref_elem)) name_len = sizeof(pos.ref_elem) - 1;
        memcpy(pos.ref_elem, str, name_len);
        pos.ref_elem[name_len] = '\0';
        strcpy(pos.ref_edge, "top");
        if (*colon == ':') {
            colon++;
            pos.ref_offset = 0;
            while (*colon >= '0' && *colon <= '9')
                pos.ref_offset = pos.ref_offset * 10 + (*colon - '0');
        }
        return pos;
    }

    /* Check for percent */
    const char* p = str;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    int32_t val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    if (neg) val = -val;

    if (*p == '%') {
        pos.value = val * 100;
        pos.is_percent = true;
    } else {
        pos.value = val;
        pos.is_percent = false;
    }
    return pos;
}

/* ---------------------------------------------------------------------------
 * .splconf Parser
 * ------------------------------------------------------------------------- */

splconf_err_t splconf_parse(splconf_t* cfg, char* buf, uint32_t len)
{
    memset(cfg, 0, sizeof(*cfg));

    char* end = buf + len;
    char* line = buf;
    int current_section = -1;

    while (line < end) {
        /* Find end of line */
        char* eol = line;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;

        /* Null-terminate the line */
        char saved = *eol;
        *eol = '\0';

        char* p = skip_whitespace(line);

        /* Skip comments and empty lines */
        if (*p == '#' || *p == ';' || *p == '\0') {
            *eol = saved;
            line = eol + 1;
            if (saved == '\r' && *(line) == '\n') line++;
            continue;
        }

        /* Section header */
        if (*p == '[') {
            p++;
            char* name_end = p;
            while (*name_end && *name_end != ']') name_end++;
            *name_end = '\0';

            if (cfg->count < SPLCONF_MAX_SECTIONS) {
                current_section = cfg->count++;
                memset(&cfg->sections[current_section], 0, sizeof(splconf_section_t));
                strncpy(cfg->sections[current_section].name, p, SPLCONF_MAX_KEY_LEN - 1);
            }
            *eol = saved;
            line = eol + 1;
            if (saved == '\r' && *(line) == '\n') line++;
            continue;
        }

        /* Key=Value */
        char* eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            char* key = p;
            char* val = eq + 1;
            key = trim_end(key, eq);
            val = skip_whitespace(val);
            val = trim_end(val, eol);

            if (current_section >= 0 && cfg->sections[current_section].count < SPLCONF_MAX_ENTRIES) {
                splconf_entry_t* entry = &cfg->sections[current_section].entries[
                    cfg->sections[current_section].count++];
                strncpy(entry->key, key, SPLCONF_MAX_KEY_LEN - 1);
                strncpy(entry->val, val, SPLCONF_MAX_VAL_LEN - 1);
            }
        }

        *eol = saved;
        line = eol + 1;
        if (saved == '\r' && *(line) == '\n') line++;
    }

    cfg->loaded = (cfg->count > 0);
    return SPLCONF_OK;
}

static const char* splconf_find(const splconf_t* cfg, const char* section, const char* key)
{
    for (uint8_t s = 0; s < cfg->count; s++) {
        if (strcmp(cfg->sections[s].name, section) == 0) {
            for (uint8_t e = 0; e < cfg->sections[s].count; e++) {
                if (strcmp(cfg->sections[s].entries[e].key, key) == 0) {
                    return cfg->sections[s].entries[e].val;
                }
            }
        }
    }
    return NULL;
}

bool splconf_get_bool(const splconf_t* cfg, const char* section, const char* key, bool def)
{
    const char* val = splconf_find(cfg, section, key);
    if (!val) return def;
    return (val[0] == 't' || val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
}

int32_t splconf_get_int(const splconf_t* cfg, const char* section, const char* key, int32_t def)
{
    const char* val = splconf_find(cfg, section, key);
    if (!val) return def;
    int32_t result = 0;
    bool neg = false;
    if (*val == '-') { neg = true; val++; }
    while (*val >= '0' && *val <= '9') {
        result = result * 10 + (*val - '0');
        val++;
    }
    return neg ? -result : result;
}

const char* splconf_get_string(const splconf_t* cfg, const char* section, const char* key, const char* def)
{
    const char* val = splconf_find(cfg, section, key);
    return val ? val : def;
}

uint32_t splconf_get_color(const splconf_t* cfg, const char* section, const char* key, uint32_t def)
{
    const char* val = splconf_find(cfg, section, key);
    if (!val) return def;
    return spl_parse_color(val, def);
}

/* ---------------------------------------------------------------------------
 * .spllayout Parser
 * ------------------------------------------------------------------------- */

static spl_anchor_t parse_anchor(const char* str)
{
    if (!str) return SPL_ANCHOR_TOP_LEFT;
    if (starts_with(str, "center"))      return SPL_ANCHOR_CENTER;
    if (starts_with(str, "top-center"))  return SPL_ANCHOR_TOP_CENTER;
    if (starts_with(str, "top-right"))   return SPL_ANCHOR_TOP_RIGHT;
    if (starts_with(str, "top-left"))    return SPL_ANCHOR_TOP_LEFT;
    if (starts_with(str, "top"))         return SPL_ANCHOR_TOP_LEFT;
    if (starts_with(str, "bottom-center")) return SPL_ANCHOR_BOTTOM_CENTER;
    if (starts_with(str, "bottom-right")) return SPL_ANCHOR_BOTTOM_RIGHT;
    if (starts_with(str, "bottom-left")) return SPL_ANCHOR_BOTTOM_LEFT;
    if (starts_with(str, "center-left")) return SPL_ANCHOR_CENTER_LEFT;
    if (starts_with(str, "center-right")) return SPL_ANCHOR_CENTER_RIGHT;
    return SPL_ANCHOR_TOP_LEFT;
}

static spl_bar_style_t parse_bar_style(const char* str)
{
    if (!str) return SPL_BAR_STYLE_MARQUEE;
    if (starts_with(str, "solid"))     return SPL_BAR_STYLE_SOLID;
    if (starts_with(str, "segmented")) return SPL_BAR_STYLE_SEGMENTED;
    if (starts_with(str, "marquee"))   return SPL_BAR_STYLE_MARQUEE;
    if (starts_with(str, "dots"))      return SPL_BAR_STYLE_DOTS;
    if (starts_with(str, "pulse"))     return SPL_BAR_STYLE_PULSE;
    return SPL_BAR_STYLE_MARQUEE;
}

splconf_err_t spllayout_parse(spllayout_t* layout, const splconf_t* conf,
                              char* buf, uint32_t len)
{
    memset(layout, 0, sizeof(*layout));

    char* end = buf + len;
    char* line = buf;
    int current_elem = -1;

    while (line < end) {
        char* eol = line;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;
        char saved = *eol;
        *eol = '\0';

        char* p = skip_whitespace(line);

        if (*p == '#' || *p == ';' || *p == '\0') {
            *eol = saved;
            line = eol + 1;
            if (saved == '\r' && *(line) == '\n') line++;
            continue;
        }

        if (*p == '[') {
            p++;
            char* name_end = p;
            while (*name_end && *name_end != ']') name_end++;
            *name_end = '\0';

            if (layout->count < SPL_MAX_ELEMENTS) {
                current_elem = layout->count++;
                spl_element_t* elem = &layout->elements[current_elem];
                memset(elem, 0, sizeof(*elem));
                elem->opacity = 255;
                elem->visible = true;

                /* Parse type and optional name: "type:name" */
                char* colon = p;
                while (*colon && *colon != ':') colon++;
                if (*colon == ':') {
                    *colon = '\0';
                    strncpy(elem->name, colon + 1, sizeof(elem->name) - 1);
                }

                /* Determine element type */
                if (starts_with(p, "background"))     elem->type = SPL_ELEMTYPE_BACKGROUND;
                else if (starts_with(p, "logo"))       elem->type = SPL_ELEMTYPE_LOGO;
                else if (starts_with(p, "version"))    elem->type = SPL_ELEMTYPE_VERSION;
                else if (starts_with(p, "progress"))   elem->type = SPL_ELEMTYPE_PROGRESS_BAR;
                else if (starts_with(p, "status"))     elem->type = SPL_ELEMTYPE_STATUS_TEXT;
                else if (starts_with(p, "text"))       elem->type = SPL_ELEMTYPE_CUSTOM_TEXT;
                else if (starts_with(p, "image"))      elem->type = SPL_ELEMTYPE_IMAGE;
                else if (starts_with(p, "rectangle"))  elem->type = SPL_ELEMTYPE_RECTANGLE;
                else {
                    layout->count--;
                    current_elem = -1;
                }
            }
            *eol = saved;
            line = eol + 1;
            if (saved == '\r' && *(line) == '\n') line++;
            continue;
        }

        /* Property key=value */
        char* eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=' && current_elem >= 0) {
            *eq = '\0';
            char* key = p;
            char* val = eq + 1;
            key = trim_end(key, eq);
            val = skip_whitespace(val);
            val = trim_end(val, eol);

            spl_element_t* elem = &layout->elements[current_elem];

            /* Common properties */
            if (starts_with(key, "x"))        elem->x = spl_parse_position(val);
            else if (starts_with(key, "y"))   elem->y = spl_parse_position(val);
            else if (starts_with(key, "width")) elem->width = spl_parse_position(val);
            else if (starts_with(key, "height")) elem->height = spl_parse_position(val);
            else if (starts_with(key, "anchor")) elem->anchor = parse_anchor(val);
            else if (starts_with(key, "z"))   elem->z_order = (uint8_t)atoi_simple(val);
            else if (starts_with(key, "opacity")) elem->opacity = (uint8_t)atoi_simple(val);
            else if (starts_with(key, "visible")) elem->visible = (val[0] == 't' || val[0] == '1');
            else if (starts_with(key, "color")) {
                uint32_t c = spl_parse_color(val, 0xFFFFFF);
                if (elem->type == SPL_ELEMTYPE_CUSTOM_TEXT || elem->type == SPL_ELEMTYPE_LOGO ||
                    elem->type == SPL_ELEMTYPE_VERSION || elem->type == SPL_ELEMTYPE_STATUS_TEXT)
                    elem->data.text.color = c;
            }
            else if (starts_with(key, "text") || starts_with(key, "content")) {
                if (elem->type == SPL_ELEMTYPE_CUSTOM_TEXT || elem->type == SPL_ELEMTYPE_LOGO ||
                    elem->type == SPL_ELEMTYPE_VERSION || elem->type == SPL_ELEMTYPE_STATUS_TEXT)
                    strncpy(elem->data.text.text, val, SPL_MAX_TEXT_LEN - 1);
            }
            else if (starts_with(key, "scale") || starts_with(key, "font_scale") || starts_with(key, "font_size")) {
                if (elem->type == SPL_ELEMTYPE_CUSTOM_TEXT || elem->type == SPL_ELEMTYPE_LOGO ||
                    elem->type == SPL_ELEMTYPE_VERSION || elem->type == SPL_ELEMTYPE_STATUS_TEXT)
                    elem->data.text.scale = (uint32_t)atoi_simple(val);
            }
            else if (starts_with(key, "shadow")) {
                if (elem->type == SPL_ELEMTYPE_CUSTOM_TEXT || elem->type == SPL_ELEMTYPE_LOGO ||
                    elem->type == SPL_ELEMTYPE_VERSION || elem->type == SPL_ELEMTYPE_STATUS_TEXT)
                    elem->data.text.shadow = (val[0] == 't' || val[0] == '1');
            }
            else if (starts_with(key, "shadow_color")) {
                if (elem->type == SPL_ELEMTYPE_CUSTOM_TEXT || elem->type == SPL_ELEMTYPE_LOGO ||
                    elem->type == SPL_ELEMTYPE_VERSION || elem->type == SPL_ELEMTYPE_STATUS_TEXT)
                    elem->data.text.shadow_color = spl_parse_color(val, 0x000000);
            }
            /* Background properties */
            else if (starts_with(key, "gradient_top") || starts_with(key, "fill")) {
                if (elem->type == SPL_ELEMTYPE_BACKGROUND)
                    elem->data.background.gradient_top = spl_parse_color(val, 0x003AAE);
            }
            else if (starts_with(key, "gradient_bottom") || starts_with(key, "trough_color")) {
                if (elem->type == SPL_ELEMTYPE_BACKGROUND)
                    elem->data.background.gradient_bot = spl_parse_color(val, 0x000050);
            }
            /* Progress bar properties */
            else if (starts_with(key, "style")) {
                if (elem->type == SPL_ELEMTYPE_PROGRESS_BAR)
                    elem->data.bar.style = parse_bar_style(val);
            }
            else if (starts_with(key, "fill_color")) {
                if (elem->type == SPL_ELEMTYPE_PROGRESS_BAR)
                    elem->data.bar.fill_color = spl_parse_color(val, 0x3399FF);
            }
            else if (starts_with(key, "trough_color") && elem->type == SPL_ELEMTYPE_PROGRESS_BAR)
                elem->data.bar.trough_color = spl_parse_color(val, 0x002470);
            else if (starts_with(key, "highlight_color"))
                elem->data.bar.highlight_color = spl_parse_color(val, 0x66BBFF);
            else if (starts_with(key, "border_color"))
                elem->data.bar.border_color = spl_parse_color(val, 0x001850);
            else if (starts_with(key, "border_width"))
                elem->data.bar.border_width = (uint32_t)atoi_simple(val);
            else if (starts_with(key, "corner_radius"))
                elem->data.bar.corner_radius = (uint32_t)atoi_simple(val);
            else if (starts_with(key, "marquee_segment_width") || starts_with(key, "marquee_seg_width"))
                elem->data.bar.marquee_seg_width = (uint32_t)atoi_simple(val);
            else if (starts_with(key, "marquee_speed"))
                elem->data.bar.marquee_speed = (uint32_t)atoi_simple(val);
            else if (starts_with(key, "segment_gap"))
                elem->data.bar.segment_gap = (uint32_t)atoi_simple(val);
            /* Image properties */
            else if (starts_with(key, "src") || starts_with(key, "path") || starts_with(key, "image"))
                strncpy(elem->data.image.path, val, sizeof(elem->data.image.path) - 1);
            else if (starts_with(key, "key_color"))
                elem->data.image.key_color = spl_parse_color(val, 0xFFFFFFFF);
        }

        *eol = saved;
        line = eol + 1;
        if (saved == '\r' && *(line) == '\n') line++;
    }

    layout->loaded = (layout->count > 0);
    return SPLCONF_OK;
}

static int32_t resolve_pos(const spl_pos_t* pos, int32_t screen_dim)
{
    if (pos->is_percent) return screen_dim * pos->value / 10000;
    return pos->value;
}

void spllayout_resolve(spllayout_t* layout, uint32_t screen_w, uint32_t screen_h)
{
    /* Two-pass: first resolve all absolute positions, then relative */
    for (uint8_t i = 0; i < layout->count; i++) {
        spl_element_t* e = &layout->elements[i];
        e->x.value = resolve_pos(&e->x, (int32_t)screen_w);
        e->y.value = resolve_pos(&e->y, (int32_t)screen_h);
        e->x.is_percent = false;
        e->y.is_percent = false;
    }
}

spl_element_t* spllayout_find(spllayout_t* layout, const char* name)
{
    for (uint8_t i = 0; i < layout->count; i++) {
        if (strcmp(layout->elements[i].name, name) == 0)
            return &layout->elements[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * File Loading from VFS
 * ------------------------------------------------------------------------- */

splconf_err_t splconf_load(splconf_t* cfg, const char* path)
{
    char* buf = (char*)kmalloc(4096);
    if (!buf) return SPLCONF_ERR_IO;

    uint32_t len = 0;
    int res = vfs_read_file(path, buf, 4095, &len);
    if (res < 0 || len == 0) {
        kfree(buf);
        return SPLCONF_ERR_IO;
    }
    buf[len] = '\0';

    splconf_err_t err = splconf_parse(cfg, buf, len);
    /* Note: buf is NOT freed because cfg entries point into it */
    /* The caller must keep buf alive. For simplicity, we leak it. */
    return err;
}

splconf_err_t spllayout_load(spllayout_t* layout, const splconf_t* cfg, const char* path)
{
    char* buf = (char*)kmalloc(4096);
    if (!buf) return SPLCONF_ERR_IO;

    uint32_t len = 0;
    int res = vfs_read_file(path, buf, 4095, &len);
    if (res < 0 || len == 0) {
        kfree(buf);
        return SPLCONF_ERR_IO;
    }
    buf[len] = '\0';

    splconf_err_t err = spllayout_parse(layout, cfg, buf, len);
    /* Same note as above - buf is leaked for pointer stability */
    return err;
}
