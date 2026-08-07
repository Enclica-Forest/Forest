/*
 * LeafGFX Font Rendering - Implementation
 *
 * Provides text rendering with built-in bitmap fonts
 * and TrueType font support with anti-aliased rendering.
 */

#include "leafgfx_font.h"
#include "leafgfx.h"
#include "leafgfx_ttf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// UTF-8 Utility Functions
// ============================================================================

static uint32_t utf8_decode(const char* str, uint32_t* bytes_consumed) {
    if (!str || !bytes_consumed) {
        if (bytes_consumed) {
            *bytes_consumed = 0;
        }
        return 0;
    }

    uint8_t first = (uint8_t)str[0];

    if ((first & 0x80) == 0) {
        *bytes_consumed = 1;
        return first;
    } else if ((first & 0xE0) == 0xC0) {
        if ((str[1] & 0xC0) == 0x80) {
            *bytes_consumed = 2;
            return ((first & 0x1F) << 6) | (str[1] & 0x3F);
        }
    } else if ((first & 0xF0) == 0xE0) {
        if ((str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80) {
            *bytes_consumed = 3;
            return ((first & 0x0F) << 12) | ((str[1] & 0x3F) << 6) | (str[2] & 0x3F);
        }
    } else if ((first & 0xF8) == 0xF0) {
        if ((str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80 && (str[3] & 0xC0) == 0x80) {
            *bytes_consumed = 4;
            return ((first & 0x07) << 18) | ((str[1] & 0x3F) << 12) |
                   ((str[2] & 0x3F) << 6) | (str[3] & 0x3F);
        }
    }

    *bytes_consumed = 1;
    return first;
}

// ============================================================================
// Glyph Atlas Cache
// ============================================================================

#define GLYPH_CACHE_CAPACITY 512
#define GLYPH_CACHE_LOAD_FACTOR 75

typedef struct {
    uint32_t codepoint;
    uint8_t* bitmap;
    uint8_t  width;
    uint8_t  height;
    int8_t   bearing_x;
    int8_t   bearing_y;
    uint16_t advance;
    bool     occupied;
} gfx_glyph_cache_entry_t;

typedef struct {
    gfx_glyph_cache_entry_t entries[GLYPH_CACHE_CAPACITY];
    uint32_t count;
} gfx_glyph_cache_t;

// ============================================================================
// Font Structure
// ============================================================================

struct gfx_font {
    gfx_font_type_t type;
    uint32_t        height;
    uint32_t        width;
    uint32_t        line_spacing;
    bool            is_builtin;

    int32_t         ascent;
    int32_t         descent;
    int32_t         line_gap;

    const uint8_t*  bitmap_data;
    uint32_t        glyph_height;

    void*           ttf_data;
    size_t          ttf_size;
    leafgfx_ttf_font_t* ttf_font;

    gfx_font_t*     fallback;
    bool            subpixel;
    bool            is_monospace;
    uint32_t        monospace_width;
    const gfx_font_t* emoji_font;

    gfx_glyph_cache_t* atlas;
};

// ============================================================================
// Built-in 8x16 Bitmap Font Data
// ============================================================================

static const uint8_t font_8x16_data[128][16] = {
    // Control characters (0-31) are zero-initialized by default
    // Space (32)
    [32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'] = {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    ['"'] = {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['#'] = {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    ['$'] = {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    ['%'] = {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    ['&'] = {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    ['\'']= {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['('] = {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    [')'] = {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    ['*'] = {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    ['+'] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    [','] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    ['-'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    ['/'] = {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    ['0'] = {0x00,0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00},
    ['1'] = {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    ['2'] = {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    ['3'] = {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['4'] = {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    ['5'] = {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['6'] = {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['7'] = {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    ['8'] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['9'] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    [':'] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    [';'] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    ['<'] = {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    ['='] = {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['>'] = {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    ['?'] = {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    ['@'] = {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    ['A'] = {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    ['B'] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    ['C'] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    ['D'] = {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    ['E'] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    ['F'] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    ['G'] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    ['H'] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    ['I'] = {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    ['J'] = {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    ['K'] = {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    ['L'] = {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    ['M'] = {0x00,0x00,0xC3,0xE7,0xFF,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00},
    ['N'] = {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    ['O'] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['P'] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    ['Q'] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    ['R'] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    ['S'] = {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['T'] = {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    ['U'] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['V'] = {0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00},
    ['W'] = {0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0x00,0x00,0x00,0x00},
    ['X'] = {0x00,0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3,0xC3,0x00,0x00,0x00,0x00},
    ['Y'] = {0x00,0x00,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    ['Z'] = {0x00,0x00,0xFF,0xC3,0x86,0x0C,0x18,0x30,0x60,0xC1,0xC3,0xFF,0x00,0x00,0x00,0x00},
    ['['] = {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    ['\\']= {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    [']'] = {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    ['^'] = {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    ['`'] = {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['a'] = {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    ['b'] = {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    ['c'] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['d'] = {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    ['e'] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['f'] = {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    ['g'] = {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    ['h'] = {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    ['i'] = {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    ['j'] = {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    ['k'] = {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    ['l'] = {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    ['m'] = {0x00,0x00,0x00,0x00,0x00,0xE6,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0x00,0x00,0x00,0x00},
    ['n'] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    ['o'] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['p'] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    ['q'] = {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    ['r'] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    ['s'] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    ['t'] = {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    ['u'] = {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    ['v'] = {0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00},
    ['w'] = {0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x00,0x00,0x00,0x00},
    ['x'] = {0x00,0x00,0x00,0x00,0x00,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0x00,0x00,0x00,0x00},
    ['y'] = {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    ['z'] = {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    ['{'] = {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    ['|'] = {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    ['}'] = {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    ['~'] = {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [127] = {0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
};

// ============================================================================
// Built-in 8x8 Bitmap Font Data (simplified)
// ============================================================================

static const uint8_t font_8x8_data[128][8] = {
    // Control characters (0-31) are zero-initialized by default
    [32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'] = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    ['"'] = {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},
    ['#'] = {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00},
    ['$'] = {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00},
    ['%'] = {0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00,0x00},
    ['&'] = {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    ['\'']= {0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00},
    ['('] = {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    [')'] = {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    ['*'] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    ['+'] = {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    [','] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    ['-'] = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    ['/'] = {0x06,0x0C,0x18,0x30,0x60,0xC0,0x00,0x00},
    ['0'] = {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00},
    ['1'] = {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    ['2'] = {0x7C,0xC6,0x0C,0x18,0x30,0x60,0xFE,0x00},
    ['3'] = {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    ['4'] = {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00},
    ['5'] = {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
    ['6'] = {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    ['7'] = {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    ['8'] = {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    ['9'] = {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    [':'] = {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    [';'] = {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    ['<'] = {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    ['='] = {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    ['>'] = {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    ['?'] = {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    ['@'] = {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7C,0x00},
    ['A'] = {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00},
    ['B'] = {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    ['C'] = {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    ['D'] = {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    ['E'] = {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    ['F'] = {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    ['G'] = {0x3C,0x66,0xC0,0xCE,0xC6,0x66,0x3A,0x00},
    ['H'] = {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    ['I'] = {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['J'] = {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    ['K'] = {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    ['L'] = {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    ['M'] = {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00},
    ['N'] = {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    ['O'] = {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    ['P'] = {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    ['Q'] = {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    ['R'] = {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    ['S'] = {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
    ['T'] = {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['U'] = {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    ['V'] = {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    ['W'] = {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    ['X'] = {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00},
    ['Y'] = {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    ['Z'] = {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    ['['] = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    ['\\']= {0xC0,0x60,0x30,0x18,0x0C,0x06,0x00,0x00},
    [']'] = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    ['^'] = {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    ['`'] = {0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    ['a'] = {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    ['b'] = {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    ['c'] = {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    ['d'] = {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    ['e'] = {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    ['f'] = {0x38,0x6C,0x60,0xF8,0x60,0x60,0xF0,0x00},
    ['g'] = {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x78},
    ['h'] = {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    ['i'] = {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    ['j'] = {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C},
    ['k'] = {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    ['l'] = {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['m'] = {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00},
    ['n'] = {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    ['o'] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    ['p'] = {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    ['q'] = {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    ['r'] = {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    ['s'] = {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00},
    ['t'] = {0x30,0x30,0xFC,0x30,0x30,0x34,0x18,0x00},
    ['u'] = {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    ['v'] = {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    ['w'] = {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    ['x'] = {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    ['y'] = {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C},
    ['z'] = {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00},
    ['{'] = {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    ['|'] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    ['}'] = {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    ['~'] = {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
    [127] = {0x00,0x10,0x38,0x6C,0xC6,0xFE,0x00,0x00},
};

// ============================================================================
// Built-in Font Objects
// ============================================================================

static gfx_font_t font_builtin_8x16 = {
    .type = GFX_FONT_BUILTIN_8X16,
    .height = 16,
    .width = 8,
    .line_spacing = 18,
    .is_builtin = true,
    .ascent = 14,
    .descent = -2,
    .line_gap = 2,
    .bitmap_data = (const uint8_t*)font_8x16_data,
    .glyph_height = 16,
    .ttf_data = NULL,
    .ttf_size = 0,
    .ttf_font = NULL,
    .fallback = NULL,
    .subpixel = false,
    .is_monospace = true,
    .monospace_width = 8,
    .emoji_font = NULL,
    .atlas = NULL
};

static gfx_font_t font_builtin_8x8 = {
    .type = GFX_FONT_BUILTIN_8X8,
    .height = 8,
    .width = 8,
    .line_spacing = 10,
    .is_builtin = true,
    .ascent = 7,
    .descent = -1,
    .line_gap = 1,
    .bitmap_data = (const uint8_t*)font_8x8_data,
    .glyph_height = 8,
    .ttf_data = NULL,
    .ttf_size = 0,
    .ttf_font = NULL,
    .fallback = NULL,
    .subpixel = false,
    .is_monospace = true,
    .monospace_width = 8,
    .emoji_font = NULL,
    .atlas = NULL
};

static gfx_font_t* g_tracked_fonts[256] = {0};
static size_t g_tracked_font_count = 0;

static void track_font(gfx_font_t* font) {
    if (!font || font->is_builtin) {
        return;
    }

    for (size_t i = 0; i < g_tracked_font_count; i++) {
        if (g_tracked_fonts[i] == font) {
            return;
        }
    }

    if (g_tracked_font_count < (sizeof(g_tracked_fonts) / sizeof(g_tracked_fonts[0]))) {
        g_tracked_fonts[g_tracked_font_count++] = font;
        return;
    }

    printf("[LeafGFX] WARNING: font tracker full; font will not be tracked\n");
}

static void untrack_font(gfx_font_t* font) {
    if (!font) {
        return;
    }

    for (size_t i = 0; i < g_tracked_font_count; i++) {
        if (g_tracked_fonts[i] != font) {
            continue;
        }

        g_tracked_font_count--;
        g_tracked_fonts[i] = g_tracked_fonts[g_tracked_font_count];
        g_tracked_fonts[g_tracked_font_count] = NULL;
        return;
    }
}

static void free_font_internal(gfx_font_t* font) {
    if (!font || font->is_builtin) {
        return;
    }

    if (font->ttf_font) {
        leafgfx_ttf_unload(font->ttf_font);
    }
    if (font->ttf_data) {
        free(font->ttf_data);
    }
    if (font->atlas) {
        for (uint32_t i = 0; i < GLYPH_CACHE_CAPACITY; i++) {
            if (font->atlas->entries[i].occupied && font->atlas->entries[i].bitmap) {
                free(font->atlas->entries[i].bitmap);
            }
        }
        free(font->atlas);
    }
    free(font);
}

// ============================================================================
// Font Loading Functions
// ============================================================================

const gfx_font_t* gfx_font_get_default(void) {
    return &font_builtin_8x16;
}

const gfx_font_t* gfx_font_get_builtin(gfx_font_type_t type) {
    switch (type) {
        case GFX_FONT_BUILTIN_8X8:
            return &font_builtin_8x8;
        case GFX_FONT_BUILTIN_8X16:
            return &font_builtin_8x16;
        default:
            return NULL;
    }
}

gfx_font_result_t gfx_font_load_ttf(const char* path, uint32_t size, gfx_font_t** font) {
    if (!path || !font || size == 0) {
        return GFX_FONT_ERROR_INVALID_PARAMETER;
    }

    // Open and read file (try common path variants)
    FILE* fp = fopen(path, "rb");
    if (!fp && path[0] == '/') {
        fp = fopen(path + 1, "rb");
    } else if (!fp) {
        char with_slash[512];
        snprintf(with_slash, sizeof(with_slash), "/%s", path);
        fp = fopen(with_slash, "rb");
    }
    if (!fp) {
        return GFX_FONT_ERROR_FILE_NOT_FOUND;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 16 * 1024 * 1024) {
        fclose(fp);
        return GFX_FONT_ERROR_INVALID_FILE;
    }

    uint8_t* data = (uint8_t*)malloc(file_size);
    if (!data) {
        fclose(fp);
        return GFX_FONT_ERROR_OUT_OF_MEMORY;
    }

    if (fread(data, 1, file_size, fp) != (size_t)file_size) {
        free(data);
        fclose(fp);
        return GFX_FONT_ERROR_INVALID_FILE;
    }
    fclose(fp);

    gfx_font_result_t result = gfx_font_load_ttf_memory(data, file_size, size, font);
    if (result != GFX_FONT_SUCCESS) {
        free(data);
    }
    return result;
}

gfx_font_result_t gfx_font_load_ttf_memory(const uint8_t* data, size_t data_size,
                                           uint32_t size, gfx_font_t** font) {
    if (!data || !font || data_size < 12 || size == 0) {
        return GFX_FONT_ERROR_INVALID_PARAMETER;
    }

    // Validate TTF data first
    if (!leafgfx_ttf_is_valid(data, data_size)) {
        return GFX_FONT_ERROR_INVALID_FILE;
    }

    // Allocate font structure
    gfx_font_t* f = (gfx_font_t*)malloc(sizeof(gfx_font_t));
    if (!f) {
        return GFX_FONT_ERROR_OUT_OF_MEMORY;
    }
    memset(f, 0, sizeof(gfx_font_t));

    // Copy TTF data (we own it)
    f->ttf_data = malloc(data_size);
    if (!f->ttf_data) {
        free(f);
        return GFX_FONT_ERROR_OUT_OF_MEMORY;
    }
    memcpy(f->ttf_data, data, data_size);
    f->ttf_size = data_size;

    // Parse the TTF font
    leafgfx_ttf_font_t* ttf = NULL;
    leafgfx_ttf_result_t result = leafgfx_ttf_load_memory(f->ttf_data, f->ttf_size, &ttf);
    if (result != LEAFGFX_TTF_SUCCESS || !ttf) {
        free(f->ttf_data);
        free(f);
        return GFX_FONT_ERROR_INVALID_FILE;
    }

    // Initialize the glyph cache at the requested size
    leafgfx_ttf_set_cache_ppem(ttf, size);

    // Set up font structure
    f->type = GFX_FONT_TTF;
    f->ttf_font = ttf;
    f->height = size;
    f->is_builtin = false;

    // Calculate metrics from TTF
    int16_t ascender = leafgfx_ttf_get_ascender(ttf, size);
    int16_t descender = leafgfx_ttf_get_descender(ttf, size);
    int16_t line_gap = leafgfx_ttf_get_line_gap(ttf, size);

    // Width is approximate for proportional fonts, use 'M' as reference
    uint16_t m_glyph = leafgfx_ttf_get_glyph_index(ttf, 'M');
    int16_t m_advance = leafgfx_ttf_get_glyph_advance(ttf, m_glyph, size);
    f->width = m_advance > 0 ? (uint32_t)m_advance : size / 2;

    f->line_spacing = ascender - descender + line_gap;
    if (f->line_spacing < (int32_t)size) {
        f->line_spacing = size + 2;
    }

    f->ascent = ascender;
    f->descent = descender;
    f->line_gap = line_gap;

    f->bitmap_data = (const uint8_t*)font_8x16_data;
    f->glyph_height = 16;
    f->fallback = NULL;
    f->subpixel = false;
    f->is_monospace = false;
    f->monospace_width = 0;
    f->emoji_font = NULL;
    f->atlas = NULL;

    *font = f;
    track_font(f);
    return GFX_FONT_SUCCESS;
}

void gfx_font_free(gfx_font_t* font) {
    if (!font || font->is_builtin) {
        return;
    }

    untrack_font(font);
    free_font_internal(font);
}

void gfx_font_release_all_tracked(void) {
    while (g_tracked_font_count > 0) {
        gfx_font_t* font = g_tracked_fonts[g_tracked_font_count - 1];
        g_tracked_fonts[g_tracked_font_count - 1] = NULL;
        g_tracked_font_count--;
        free_font_internal(font);
    }
}

// ============================================================================
// Font Properties
// ============================================================================

gfx_font_type_t gfx_font_get_type(const gfx_font_t* font) {
    return font ? font->type : GFX_FONT_BUILTIN_8X16;
}

uint32_t gfx_font_get_height(const gfx_font_t* font) {
    return font ? font->height : 16;
}

uint32_t gfx_font_get_line_spacing(const gfx_font_t* font) {
    return font ? font->line_spacing : 18;
}

uint32_t gfx_font_get_char_width(const gfx_font_t* font, char c) {
    if (!font) return 8;

    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        uint16_t glyph_idx = leafgfx_ttf_get_glyph_index(font->ttf_font, (uint32_t)(unsigned char)c);
        if (glyph_idx == 0 && c != 0 && font->fallback) {
            return gfx_font_get_char_width(font->fallback, c);
        }
        int16_t advance = leafgfx_ttf_get_glyph_advance(font->ttf_font, glyph_idx, font->height);
        if (font->is_monospace && font->monospace_width > 0) {
            return font->monospace_width;
        }
        return advance > 0 ? (uint32_t)advance : font->width;
    }

    if (font->is_monospace && font->monospace_width > 0) {
        return font->monospace_width;
    }
    return font->width;
}

uint32_t gfx_font_get_text_width(const gfx_font_t* font, const char* text) {
    if (!text) return 0;
    if (!font) return (uint32_t)strlen(text) * 8;

    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        uint32_t total = 0;
        const char* p = text;
        while (*p) {
            uint32_t bytes_consumed;
            uint32_t codepoint = utf8_decode(p, &bytes_consumed);
            uint16_t glyph_idx = leafgfx_ttf_get_glyph_index(font->ttf_font, codepoint);
            
            if (glyph_idx == 0 && codepoint != 0) {
                if (font->fallback) {
                    total += gfx_font_get_text_width(font->fallback, p);
                    break;
                }
                glyph_idx = 0;
            }
            
            int16_t advance = leafgfx_ttf_get_glyph_advance(font->ttf_font, glyph_idx, font->height);
            
            if (advance <= 0) {
                advance = font->width;
            }
            if (font->is_monospace && font->monospace_width > 0) {
                advance = font->monospace_width;
            }
            total += (uint32_t)advance;
            p += bytes_consumed;
        }
        return total;
    }

    if (font->is_monospace && font->monospace_width > 0) {
        return (uint32_t)strlen(text) * font->monospace_width;
    }
    return (uint32_t)strlen(text) * font->width;
}

void gfx_font_get_text_bounds(const gfx_font_t* font, const char* text,
                              uint32_t* width, uint32_t* height) {
    if (width) *width = gfx_font_get_text_width(font, text);
    if (height) *height = font ? font->height : 16;
}

// ============================================================================
// Detailed Font Metrics
// ============================================================================

int32_t gfx_font_get_ascent(const gfx_font_t* font) {
    if (!font) return 14;
    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        return font->ascent;
    }
    return font->ascent;
}

int32_t gfx_font_get_descent(const gfx_font_t* font) {
    if (!font) return -2;
    return font->descent;
}

int32_t gfx_font_get_line_gap(const gfx_font_t* font) {
    if (!font) return 2;
    return font->line_gap;
}

// ============================================================================
// Font Fallback Chain
// ============================================================================

void gfx_font_set_fallback(gfx_font_t* font, gfx_font_t* fallback) {
    if (!font) return;
    font->fallback = fallback;
}

const gfx_font_t* gfx_font_get_fallback(const gfx_font_t* font) {
    return font ? font->fallback : NULL;
}

// ============================================================================
// Font Configuration
// ============================================================================

void gfx_font_set_subpixel(gfx_font_t* font, bool enabled) {
    if (!font) return;
    font->subpixel = enabled;
}

bool gfx_font_get_subpixel(const gfx_font_t* font) {
    return font ? font->subpixel : false;
}

void gfx_font_set_monospace(gfx_font_t* font, bool is_mono, uint32_t width) {
    if (!font) return;
    font->is_monospace = is_mono;
    font->monospace_width = width;
}

bool gfx_font_is_monospace(const gfx_font_t* font) {
    return font ? font->is_monospace : false;
}

void gfx_font_set_emoji(gfx_font_t* font, const gfx_font_t* emoji_font) {
    if (!font) return;
    font->emoji_font = emoji_font;
}

const gfx_font_t* gfx_font_get_emoji(const gfx_font_t* font) {
    return font ? font->emoji_font : NULL;
}

// ============================================================================
// Glyph Atlas Cache
// ============================================================================

static uint32_t glyph_cache_hash(uint32_t codepoint) {
    codepoint = ((codepoint >> 16) ^ codepoint) * 0x45d9f3b;
    codepoint = ((codepoint >> 16) ^ codepoint) * 0x45d9f3b;
    codepoint = (codepoint >> 16) ^ codepoint;
    return codepoint % GLYPH_CACHE_CAPACITY;
}

static gfx_glyph_cache_entry_t* glyph_cache_lookup(gfx_glyph_cache_t* cache, uint32_t codepoint) {
    if (!cache) return NULL;
    uint32_t idx = glyph_cache_hash(codepoint);
    for (uint32_t probe = 0; probe < GLYPH_CACHE_CAPACITY; probe++) {
        uint32_t slot = (idx + probe) % GLYPH_CACHE_CAPACITY;
        if (!cache->entries[slot].occupied) {
            return NULL;
        }
        if (cache->entries[slot].codepoint == codepoint) {
            return &cache->entries[slot];
        }
    }
    return NULL;
}

static gfx_glyph_cache_entry_t* glyph_cache_insert(gfx_glyph_cache_t* cache, uint32_t codepoint) {
    if (!cache) return NULL;
    if (cache->count >= GLYPH_CACHE_LOAD_FACTOR) {
        return NULL;
    }
    uint32_t idx = glyph_cache_hash(codepoint);
    for (uint32_t probe = 0; probe < GLYPH_CACHE_CAPACITY; probe++) {
        uint32_t slot = (idx + probe) % GLYPH_CACHE_CAPACITY;
        if (!cache->entries[slot].occupied) {
            cache->entries[slot].codepoint = codepoint;
            cache->entries[slot].occupied = true;
            cache->count++;
            return &cache->entries[slot];
        }
    }
    return NULL;
}

void gfx_font_clear_glyph_cache(gfx_font_t* font) {
    if (!font || !font->atlas) return;
    for (uint32_t i = 0; i < GLYPH_CACHE_CAPACITY; i++) {
        if (font->atlas->entries[i].occupied && font->atlas->entries[i].bitmap) {
            free(font->atlas->entries[i].bitmap);
            font->atlas->entries[i].bitmap = NULL;
        }
        font->atlas->entries[i].occupied = false;
    }
    font->atlas->count = 0;
}

uint32_t gfx_font_get_glyph_cache_size(const gfx_font_t* font) {
    return font && font->atlas ? font->atlas->count : 0;
}

// ============================================================================
// Bidirectional Text Support
// ============================================================================

bool gfx_unicode_is_rtl(uint32_t codepoint) {
    if (codepoint >= 0x0590 && codepoint <= 0x05FF) return true;
    if (codepoint >= 0x0600 && codepoint <= 0x06FF) return true;
    if (codepoint >= 0x0700 && codepoint <= 0x074F) return true;
    if (codepoint >= 0x0750 && codepoint <= 0x077F) return true;
    if (codepoint >= 0x0780 && codepoint <= 0x07BF) return true;
    if (codepoint >= 0x07C0 && codepoint <= 0x07FF) return true;
    if (codepoint >= 0x08A0 && codepoint <= 0x08FF) return true;
    if (codepoint >= 0xFB1D && codepoint <= 0xFB4F) return true;
    if (codepoint >= 0xFB50 && codepoint <= 0xFDFF) return true;
    if (codepoint >= 0xFE70 && codepoint <= 0xFEFF) return true;
    if (codepoint >= 0x10800 && codepoint <= 0x10FFF) return true;
    if (codepoint >= 0x1EE00 && codepoint <= 0x1EEFF) return true;
    return false;
}

static bool is_emoji_codepoint(uint32_t cp) {
    if (cp >= 0x1F600 && cp <= 0x1F64F) return true;
    if (cp >= 0x1F300 && cp <= 0x1F5FF) return true;
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return true;
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;
    if (cp >= 0x1FA00 && cp <= 0x1FA6F) return true;
    if (cp >= 0x1FA70 && cp <= 0x1FAFF) return true;
    if (cp >= 0x2600 && cp <= 0x26FF) return true;
    if (cp >= 0x2700 && cp <= 0x27BF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
    if (cp == 0x200D) return true;
    return false;
}

// ============================================================================
// Text Rendering
// ============================================================================

// Helper to draw TTF glyph with anti-aliasing
static uint32_t gfx_draw_char_ttf(const gfx_font_t* font, int32_t x, int32_t y,
                                   uint32_t codepoint, uint32_t color) {
    if (!font->ttf_font) return font->width;

    uint16_t glyph_idx = leafgfx_ttf_get_glyph_index(font->ttf_font, codepoint);
    
    if (glyph_idx == 0 && codepoint != 0) {
        if (font->fallback) {
            return gfx_draw_char(font->fallback, x, y, (char)(codepoint & 0x7F), color);
        }
        if (font->emoji_font && is_emoji_codepoint(codepoint)) {
            return gfx_draw_char(font->emoji_font, x, y, (char)(codepoint & 0x7F), color);
        }
        glyph_idx = 0;
    }
    
    uint16_t advance = 0;
    leafgfx_ttf_get_glyph_metrics(font->ttf_font, glyph_idx, font->height,
                                   &advance, NULL, NULL, NULL, NULL, NULL);

    /* Whitespace legitimately rasterizes to an empty (0x0) bitmap - that is
     * not a missing-glyph condition, so it must never fall into the notdef/
     * tofu-box fallback below. Advance the cursor and draw nothing. */
    if (codepoint == ' ' || codepoint == '\t') {
        return advance > 0 ? advance : font->width;
    }

    gfx_glyph_cache_entry_t* cached = NULL;
    gfx_glyph_cache_t* atlas = ((gfx_font_t*)font)->atlas;
    if (atlas) {
        cached = glyph_cache_lookup(atlas, codepoint);
    }

    const uint8_t* bitmap = NULL;
    uint8_t bitmap_width = 0;
    uint8_t bitmap_height = 0;
    int8_t bearing_x = 0;
    int8_t bearing_y = 0;

    if (cached && cached->bitmap) {
        bitmap = cached->bitmap;
        bitmap_width = cached->width;
        bitmap_height = cached->height;
        bearing_x = cached->bearing_x;
        bearing_y = cached->bearing_y;
        if (advance == 0) advance = cached->advance;
    } else {
        leafgfx_ttf_result_t result = leafgfx_ttf_get_cached_glyph(
            font->ttf_font, glyph_idx, font->height,
            &bitmap, &bitmap_width, &bitmap_height, &bearing_x, &bearing_y
        );

        if ((result != LEAFGFX_TTF_SUCCESS || !bitmap || bitmap_width == 0 || bitmap_height == 0) && codepoint != 0) {
            if (glyph_idx != 0) {
                result = leafgfx_ttf_get_cached_glyph(font->ttf_font, 0, font->height,
                    &bitmap, &bitmap_width, &bitmap_height, &bearing_x, &bearing_y);
            }
            
            if (result != LEAFGFX_TTF_SUCCESS || !bitmap || bitmap_width == 0 || bitmap_height == 0) {
                int32_t char_width = (int32_t)(advance > 0 ? advance : font->width);
                int32_t char_height = (int32_t)font->height;
                
                uint8_t base_a = gfx_alpha_from_color(color);
                if (base_a == 0) {
                    return advance > 0 ? advance : font->width;
                }
                uint32_t rgb = color & 0x00FFFFFF;
                
                for (int32_t py = 0; py < char_height && (y + py) < (int32_t)gfx_screen_height(); py++) {
                    for (int32_t px = 0; px < char_width && (x + px) < (int32_t)gfx_screen_width(); px++) {
                        if (py == 0 || py == char_height - 1 || px == 0 || px == char_width - 1) {
                            uint8_t alpha = base_a;
                            uint32_t pixel_color = (alpha << 24) | rgb;
                            gfx_pixel_blend(x + px, y + py, pixel_color);
                        }
                    }
                }
                return advance > 0 ? advance : font->width;
            }
        }

        if (atlas && bitmap && bitmap_width > 0 && bitmap_height > 0) {
            gfx_glyph_cache_entry_t* entry = glyph_cache_insert(atlas, codepoint);
            if (entry) {
                uint32_t bsize = (uint32_t)bitmap_width * bitmap_height;
                entry->bitmap = (uint8_t*)malloc(bsize);
                if (entry->bitmap) {
                    memcpy(entry->bitmap, bitmap, bsize);
                    entry->width = bitmap_width;
                    entry->height = bitmap_height;
                    entry->bearing_x = bearing_x;
                    entry->bearing_y = bearing_y;
                    entry->advance = advance;
                    bitmap = entry->bitmap;
                }
            }
        }
    }

    int16_t ascender = leafgfx_ttf_get_ascender(font->ttf_font, font->height);

    int32_t glyph_x = x + bearing_x;
    int32_t glyph_y = y + ascender - bearing_y;

    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) {
        return advance > 0 ? advance : font->width;
    }
    uint32_t rgb = color & 0x00FFFFFF;

    if (font->subpixel) {
        for (uint32_t py = 0; py < bitmap_height; py++) {
            for (uint32_t px = 0; px < bitmap_width; px++) {
                uint8_t coverage = bitmap[py * bitmap_width + px];
                if (coverage > 0) {
                    uint8_t alpha_r = (uint8_t)(((uint32_t)base_a * coverage) / 255);
                    uint8_t alpha_g = alpha_r;
                    uint8_t alpha_b = alpha_r;

                    if (px > 0) {
                        uint8_t left = bitmap[py * bitmap_width + (px - 1)];
                        uint8_t diff = coverage > left ? coverage - left : 0;
                        alpha_r = (uint8_t)(alpha_r - (diff / 3));
                    }
                    if (px < bitmap_width - 1) {
                        uint8_t right = bitmap[py * bitmap_width + (px + 1)];
                        uint8_t diff = coverage > right ? coverage - right : 0;
                        alpha_b = (uint8_t)(alpha_b - (diff / 3));
                    }

                    uint8_t dr = (uint8_t)((rgb >> 16) & 0xFF);
                    uint8_t dg = (uint8_t)((rgb >> 8) & 0xFF);
                    uint8_t db = (uint8_t)(rgb & 0xFF);

                    uint32_t spx = (uint32_t)gfx_screen_width();
                    int32_t dx = glyph_x + (int32_t)px;
                    int32_t dy = glyph_y + (int32_t)py;
                    if (dx < 0 || dy < 0 || dx >= (int32_t)spx || dy >= (int32_t)gfx_screen_height()) continue;

                    uint32_t dst = gfx_read_pixel(dx, dy);
                    uint8_t da = (uint8_t)((dst >> 24) & 0xFF);
                    uint8_t dd_r = (uint8_t)((dst >> 16) & 0xFF);
                    uint8_t dd_g = (uint8_t)((dst >> 8) & 0xFF);
                    uint8_t dd_b = (uint8_t)(dst & 0xFF);

                    uint32_t inv_r = 255 - alpha_r;
                    uint32_t inv_g = 255 - alpha_g;
                    uint32_t inv_b = 255 - alpha_b;

                    uint32_t r_acc = (uint32_t)dr * alpha_r + (uint32_t)dd_r * inv_r;
                    uint32_t g_acc = (uint32_t)dg * alpha_g + (uint32_t)dd_g * inv_g;
                    uint32_t b_acc = (uint32_t)db * alpha_b + (uint32_t)dd_b * inv_b;
                    uint8_t max_a = alpha_r > alpha_g ? alpha_r : alpha_g;
                    if (alpha_b > max_a) max_a = alpha_b;
                    uint8_t out_a = da > max_a ? da : max_a;

                    uint8_t final_r = (uint8_t)((r_acc + 127 + ((r_acc + 127) >> 8)) >> 8);
                    uint8_t final_g = (uint8_t)((g_acc + 127 + ((g_acc + 127) >> 8)) >> 8);
                    uint8_t final_b = (uint8_t)((b_acc + 127 + ((b_acc + 127) >> 8)) >> 8);

                    gfx_pixel_blend(dx, dy, ((uint32_t)out_a << 24) | (final_r << 16) | (final_g << 8) | final_b);
                }
            }
        }
    } else {
        for (uint32_t py = 0; py < bitmap_height; py++) {
            for (uint32_t px = 0; px < bitmap_width; px++) {
                uint8_t coverage = bitmap[py * bitmap_width + px];
                if (coverage > 0) {
                    uint8_t alpha = (uint8_t)((base_a * coverage) / 255);
                    uint32_t pixel_color = (alpha << 24) | rgb;
                    gfx_pixel_blend(glyph_x + (int32_t)px, glyph_y + (int32_t)py, pixel_color);
                }
            }
        }
    }

    if (font->is_monospace && font->monospace_width > 0) {
        return font->monospace_width;
    }
    return advance > 0 ? advance : font->width;
}


// ============================================================================
// Font Size Scaling
// ============================================================================

gfx_font_result_t gfx_font_create_scaled(const gfx_font_t* source, uint32_t size, gfx_font_t** out) {
    if (!source || !out || size == 0) {
        return GFX_FONT_ERROR_INVALID_PARAMETER;
    }

    if (source->type == GFX_FONT_TTF && source->ttf_font) {
        return gfx_font_load_ttf_memory((const uint8_t*)source->ttf_data, source->ttf_size, size, out);
    }

    gfx_font_t* f = (gfx_font_t*)malloc(sizeof(gfx_font_t));
    if (!f) return GFX_FONT_ERROR_OUT_OF_MEMORY;
    memset(f, 0, sizeof(gfx_font_t));

    f->type = source->type;
    f->is_builtin = false;
    f->bitmap_data = source->bitmap_data;
    f->glyph_height = source->glyph_height;

    if (source->height > 0) {
        f->width = (source->width * size + source->height / 2) / source->height;
        f->height = size;
        f->line_spacing = (source->line_spacing * size + source->height / 2) / source->height;
        f->ascent = (source->ascent * (int32_t)size) / (int32_t)source->height;
        f->descent = (source->descent * (int32_t)size) / (int32_t)source->height;
        f->line_gap = (source->line_gap * (int32_t)size) / (int32_t)source->height;
    } else {
        f->width = 8;
        f->height = size;
        f->line_spacing = size + 2;
        f->ascent = (int32_t)size - 2;
        f->descent = -2;
        f->line_gap = 2;
    }

    f->fallback = (gfx_font_t*)source->fallback;
    f->subpixel = source->subpixel;
    f->is_monospace = source->is_monospace;
    f->monospace_width = f->width;
    f->emoji_font = source->emoji_font;
    f->atlas = NULL;

    *out = f;
    track_font(f);
    return GFX_FONT_SUCCESS;
}

// ============================================================================
// Font Loading from Initrd
// ============================================================================

gfx_font_result_t gfx_font_load_ttf_initrd(const char* path, uint32_t size, gfx_font_t** font) {
    if (!path || !font || size == 0) {
        return GFX_FONT_ERROR_INVALID_PARAMETER;
    }

    char resolved[512];
    const char* tries[2];
    tries[0] = path;
    tries[1] = NULL;

    if (path[0] != '/') {
        snprintf(resolved, sizeof(resolved), "/initrd/%s", path);
        tries[1] = resolved;
    }

    for (int i = 0; i < 2 && tries[i]; i++) {
        gfx_font_result_t result = gfx_font_load_ttf(tries[i], size, font);
        if (result == GFX_FONT_SUCCESS) {
            return GFX_FONT_SUCCESS;
        }
    }

    return GFX_FONT_ERROR_FILE_NOT_FOUND;
}

// ============================================================================
// Bidirectional Text
// ============================================================================

void gfx_draw_text_bidi(const gfx_font_t* font, int32_t x, int32_t y,
                        const char* text, uint32_t max_width, uint32_t color) {
    if (!text || !font) return;

    uint32_t len = (uint32_t)strlen(text);
    if (len == 0) return;

    typedef struct {
        const char* start;
        uint32_t byte_len;
        bool rtl;
    } bidi_run_t;

    bidi_run_t runs[128];
    uint32_t run_count = 0;

    const char* p = text;
    bool current_rtl = false;
    const char* run_start = p;
    uint32_t run_bytes = 0;

    while (*p) {
        uint32_t bc;
        uint32_t cp = utf8_decode(p, &bc);
        bool cp_rtl = gfx_unicode_is_rtl(cp);

        if (cp_rtl != current_rtl && run_bytes > 0) {
            if (run_count < 128) {
                runs[run_count].start = run_start;
                runs[run_count].byte_len = run_bytes;
                runs[run_count].rtl = current_rtl;
                run_count++;
            }
            run_start = p;
            run_bytes = 0;
            current_rtl = cp_rtl;
        }
        run_bytes += bc;
        p += bc;
    }

    if (run_bytes > 0 && run_count < 128) {
        runs[run_count].start = run_start;
        runs[run_count].byte_len = run_bytes;
        runs[run_count].rtl = current_rtl;
        run_count++;
    }

    int32_t cx = x;
    for (uint32_t i = 0; i < run_count; i++) {
        if (runs[i].rtl) {
            uint32_t codes[256];
            uint32_t widths[256];
            uint32_t count = 0;
            const char* q = runs[i].start;
            const char* end = runs[i].start + runs[i].byte_len;

            while (q < end && count < 256) {
                uint32_t bc;
                uint32_t cp = utf8_decode(q, &bc);
                widths[count] = gfx_font_get_char_width(font, (char)(cp & 0x7F));
                codes[count] = cp;
                count++;
                q += bc;
            }

            int32_t run_width = 0;
            for (uint32_t j = 0; j < count; j++) {
                run_width += widths[j];
            }

            int32_t rx = cx + run_width;
            for (uint32_t j = count; j > 0; j--) {
                rx -= widths[j - 1];
                gfx_draw_char(font, rx, y, (char)(codes[j - 1] & 0x7F), color);
            }
            cx += run_width;
        } else {
            const char* ep = runs[i].start + runs[i].byte_len;
            const char* q = runs[i].start;
            while (q < ep) {
                uint32_t bc;
                uint32_t cp = utf8_decode(q, &bc);
                uint32_t adv = gfx_draw_char(font, cx, y, (char)(cp & 0x7F), color);
                cx += adv;
                q += bc;
            }
        }
        (void)max_width;
    }
}

// ============================================================================
// Bitmap Glyph Helper
// ============================================================================

static uint32_t gfx_draw_char_bitmap(const gfx_font_t* font, int32_t x, int32_t y,
                                      char c, uint32_t color) {
    if (c < 0) c = '?';
    unsigned char uc = (unsigned char)c;
    if (uc > 127) uc = '?';

    const uint8_t* glyph;
    uint32_t glyph_height;

    if (font->glyph_height == 8) {
        glyph = &font_8x8_data[(int)uc][0];
        glyph_height = 8;
    } else {
        glyph = &font_8x16_data[(int)uc][0];
        glyph_height = 16;
    }

    for (uint32_t row = 0; row < glyph_height; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfx_pixel_blend(x + col, y + row, color);
            }
        }
    }

    return font->width;
}

uint32_t gfx_draw_char(const gfx_font_t* font, int32_t x, int32_t y, char c, uint32_t color) {
    if (!font) font = &font_builtin_8x16;

    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        return gfx_draw_char_ttf(font, x, y, (uint32_t)(unsigned char)c, color);
    }

    uint32_t w = gfx_draw_char_bitmap(font, x, y, c, color);
    if (font->is_monospace && font->monospace_width > 0) {
        return font->monospace_width;
    }
    return w;
}

void gfx_draw_text(const gfx_font_t* font, int32_t x, int32_t y,
                   const char* text, uint32_t color) {
    if (!text) return;
    if (!font) font = &font_builtin_8x16;

    int32_t start_x = x;

    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        const char* p = text;
        while (*p) {
            if (*p == '\n') {
                y += font->line_spacing;
                x = start_x;
                p++;
                continue;
            }
            uint32_t bytes_consumed;
            uint32_t codepoint = utf8_decode(p, &bytes_consumed);

            if (is_emoji_codepoint(codepoint) && font->emoji_font) {
                uint32_t adv = gfx_draw_char(font->emoji_font, x, y, (char)(codepoint & 0x7F), color);
                x += adv;
            } else {
                uint32_t advance = gfx_draw_char_ttf(font, x, y, codepoint, color);
                x += advance;
            }
            p += bytes_consumed;
        }
        return;
    }

    while (*text) {
        if (*text == '\n') {
            y += font->line_spacing;
            x = start_x;
        } else {
            uint32_t advance = gfx_draw_char(font, x, y, *text, color);
            x += advance;
        }
        text++;
    }
}

void gfx_draw_text_centered(const gfx_font_t* font, int32_t x, int32_t y,
                            uint32_t width, const char* text, uint32_t color) {
    if (!text) return;
    uint32_t text_w = gfx_font_get_text_width(font, text);
    int32_t offset = (width > text_w) ? (width - text_w) / 2 : 0;
    gfx_draw_text(font, x + offset, y, text, color);
}

void gfx_draw_text_right(const gfx_font_t* font, int32_t x, int32_t y,
                         uint32_t width, const char* text, uint32_t color) {
    if (!text) return;
    uint32_t text_w = gfx_font_get_text_width(font, text);
    int32_t offset = (width > text_w) ? (width - text_w) : 0;
    gfx_draw_text(font, x + offset, y, text, color);
}

void gfx_draw_text_centered_rect(const gfx_font_t* font, const gfx_rect_t* rect,
                                 const char* text, uint32_t color) {
    if (!text || !rect) return;
    if (!font) font = &font_builtin_8x16;

    uint32_t text_w = gfx_font_get_text_width(font, text);
    int32_t offset_x = (rect->width > text_w) ? (rect->width - text_w) / 2 : 0;
    int32_t offset_y = (rect->height > font->height) ? (rect->height - font->height) / 2 : 0;
    if (font->type == GFX_FONT_TTF && offset_y > 0) {
        /* TTF glyphs look visually low with geometric centering; nudge upward slightly. */
        int32_t optical_shift = (int32_t)(font->height / 6);
        if (optical_shift > offset_y) optical_shift = offset_y;
        offset_y -= optical_shift;
    }

    gfx_draw_text(font, rect->x + offset_x, rect->y + offset_y, text, color);
}

uint32_t gfx_draw_text_wrapped(const gfx_font_t* font, int32_t x, int32_t y,
                               uint32_t max_width, const char* text, uint32_t color) {
    if (!text) return 0;
    if (!font) font = &font_builtin_8x16;

    int32_t start_x = x;
    int32_t start_y = y;

    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        const char* p = text;
        while (*p) {
            if (*p == '\n') {
                x = start_x;
                y += font->line_spacing;
                p++;
                continue;
            } else if (*p == ' ') {
                const char* word_end = p + 1;
                uint32_t word_width = 0;
                while (*word_end && *word_end != ' ' && *word_end != '\n') {
                    uint32_t bc;
                    uint32_t cp = utf8_decode(word_end, &bc);
                    uint32_t cw = gfx_font_get_char_width(font, (char)(cp & 0x7F));
                    word_width += cw;
                    word_end += bc;
                }

                uint32_t space_width = gfx_font_get_char_width(font, ' ');

                if ((uint32_t)(x - start_x) + space_width + word_width > max_width && x > start_x) {
                    x = start_x;
                    y += font->line_spacing;
                } else {
                    uint32_t adv = gfx_draw_char_ttf(font, x, y, ' ', color);
                    x += adv;
                }
                p++;
            } else {
                uint32_t bytes_consumed;
                uint32_t codepoint = utf8_decode(p, &bytes_consumed);
                uint32_t char_width = gfx_font_get_char_width(font, (char)(codepoint & 0x7F));

                if ((uint32_t)(x - start_x) + char_width > max_width && x > start_x) {
                    x = start_x;
                    y += font->line_spacing;
                }
                uint32_t draw_adv = gfx_draw_char_ttf(font, x, y, codepoint, color);
                x += draw_adv;
                p += bytes_consumed;
            }
        }
        return y - start_y + font->height;
    }

    while (*text) {
        if (*text == '\n') {
            x = start_x;
            y += font->line_spacing;
        } else if (*text == ' ') {
            const char* word_end = text + 1;
            uint32_t word_width = 0;
            while (*word_end && *word_end != ' ' && *word_end != '\n') {
                word_width += gfx_font_get_char_width(font, *word_end);
                word_end++;
            }

            uint32_t space_width = gfx_font_get_char_width(font, ' ');
            if ((uint32_t)(x - start_x) + space_width + word_width > max_width && x > start_x) {
                x = start_x;
                y += font->line_spacing;
            } else {
                uint32_t advance = gfx_draw_char(font, x, y, ' ', color);
                x += advance;
            }
        } else {
            uint32_t char_width = gfx_font_get_char_width(font, *text);
            if ((uint32_t)(x - start_x) + char_width > max_width && x > start_x) {
                x = start_x;
                y += font->line_spacing;
            }
            uint32_t advance = gfx_draw_char(font, x, y, *text, color);
            x += advance;
        }
        text++;
    }

    return y - start_y + font->height;
}

void gfx_draw_text_shadow(const gfx_font_t* font, int32_t x, int32_t y,
                          const char* text, uint32_t color,
                          uint32_t shadow_color, int32_t shadow_offset) {
    gfx_draw_text(font, x + shadow_offset, y + shadow_offset, text, shadow_color);
    gfx_draw_text(font, x, y, text, color);
}

void gfx_draw_text_outline(const gfx_font_t* font, int32_t x, int32_t y,
                           const char* text, uint32_t color, uint32_t outline_color) {
    // Draw outline in 8 directions
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dy != 0) {
                gfx_draw_text(font, x + dx, y + dy, text, outline_color);
            }
        }
    }
    // Draw main text
    gfx_draw_text(font, x, y, text, color);
}

// ============================================================================
// Convenience Functions
// ============================================================================

void gfx_text(int32_t x, int32_t y, const char* text, uint32_t color) {
    gfx_draw_text(&font_builtin_8x16, x, y, text, color);
}

void gfx_text_centered(int32_t cx, int32_t y, const char* text, uint32_t color) {
    if (!text) return;
    uint32_t w = gfx_font_get_text_width(&font_builtin_8x16, text);
    gfx_draw_text(&font_builtin_8x16, cx - w / 2, y, text, color);
}

uint32_t gfx_text_width(const char* text) {
    return gfx_font_get_text_width(&font_builtin_8x16, text);
}

// ============================================================================
// Kerning
// ============================================================================

int32_t gfx_font_get_kerning(const gfx_font_t* font, uint32_t left_cp, uint32_t right_cp) {
    if (!font) return 0;
    if (left_cp == 0 || right_cp == 0) return 0;

    if (font->type == GFX_FONT_TTF && font->ttf_font) {
        uint16_t left_gi = leafgfx_ttf_get_glyph_index(font->ttf_font, left_cp);
        uint16_t right_gi = leafgfx_ttf_get_glyph_index(font->ttf_font, right_cp);
        if (left_gi == 0 || right_gi == 0) return 0;

        int16_t left_adv = leafgfx_ttf_get_glyph_advance(font->ttf_font, left_gi, font->height);
        int16_t right_adv = leafgfx_ttf_get_glyph_advance(font->ttf_font, right_gi, font->height);
        (void)left_adv;
        (void)right_adv;

        return 0;
    }

    static const struct { char l; char r; int8_t k; } kern_table[] = {
        {'A', 'V', -1}, {'A', 'W', -1}, {'A', 'T', -1},
        {'L', 'T', -1}, {'L', 'V', -1}, {'L', 'W', -1},
        {'T', 'o', -1}, {'T', 'a', -1}, {'T', 'e', -1},
        {'V', 'o', -1}, {'V', 'a', -1}, {'V', 'e', -1},
        {'W', 'o', -1}, {'W', 'a', -1}, {'W', 'e', -1},
        {'Y', 'o', -1}, {'Y', 'a', -1}, {'Y', 'e', -1},
        {'F', 'o', -1}, {'F', 'a', -1}, {'F', 'e', -1},
        {'P', 'o', -1}, {'P', 'a', -1}, {'P', 'e', -1},
        {'.', '.', -1}, {'!', '.', -1}, {'?', '.', -1},
        {'f', 'i', -1}, {'f', 'l', -1}, {'f', 'f', -1},
        { 0,   0,   0}
    };

    if (left_cp > 127 || right_cp > 127) return 0;
    char lc = (char)left_cp;
    char rc = (char)right_cp;

    for (int i = 0; kern_table[i].l != 0; i++) {
        if (kern_table[i].l == lc && kern_table[i].r == rc) {
            return (int32_t)kern_table[i].k;
        }
    }

    return 0;
}

// ============================================================================
// Text Justify
// ============================================================================

void gfx_draw_text_justify(const gfx_font_t* font, int32_t x, int32_t y,
                            uint32_t width, const char* text, uint32_t color) {
    if (!text || !font) return;
    if (!*text) return;

    uint32_t total_width = gfx_font_get_text_width(font, text);
    if (total_width >= width) {
        gfx_draw_text(font, x, y, text, color);
        return;
    }

    int32_t num_spaces = 0;
    for (const char* p = text; *p; p++) {
        if (*p == ' ') num_spaces++;
    }
    if (num_spaces <= 0) {
        gfx_draw_text(font, x, y, text, color);
        return;
    }

    int32_t extra = (int32_t)width - (int32_t)total_width;
    int32_t space_gap = extra / num_spaces;
    int32_t remainder = extra - space_gap * num_spaces;

    const char* p = text;
    int32_t cx = x;
    int32_t space_idx = 0;
    while (*p) {
        if (*p == ' ') {
            int32_t gap = space_gap + (space_idx < remainder ? 1 : 0);
            cx += gap;
            space_idx++;
            p++;
        } else {
            const char* word_start = p;
            while (*p && *p != ' ') p++;
            uint32_t word_len = p - word_start;
            char word[256];
            if (word_len >= sizeof(word)) word_len = sizeof(word) - 1;
            memcpy(word, word_start, word_len);
            word[word_len] = '\0';
            gfx_draw_text(font, cx, y, word, color);
            cx += gfx_font_get_text_width(font, word);
        }
    }
}
