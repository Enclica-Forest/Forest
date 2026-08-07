/**
 * LeafGFX TrueType Font Renderer
 *
 * Userspace TrueType font parser and rasterizer with anti-aliasing support.
 * Ported from Forest-OS kernel implementation.
 */

#ifndef LEAFGFX_TTF_H
#define LEAFGFX_TTF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// Error Codes
// =============================================================================

typedef enum {
    LEAFGFX_TTF_SUCCESS = 0,
    LEAFGFX_TTF_ERROR_INVALID_FILE,
    LEAFGFX_TTF_ERROR_UNSUPPORTED_FORMAT,
    LEAFGFX_TTF_ERROR_TABLE_NOT_FOUND,
    LEAFGFX_TTF_ERROR_INVALID_TABLE,
    LEAFGFX_TTF_ERROR_OUT_OF_MEMORY,
    LEAFGFX_TTF_ERROR_GLYPH_NOT_FOUND,
    LEAFGFX_TTF_ERROR_INVALID_PARAMETER
} leafgfx_ttf_result_t;

// =============================================================================
// SFNT Container Constants
// =============================================================================

#define LEAFGFX_TTF_SFNT_VERSION_1_0    0x00010000
#define LEAFGFX_TTF_SFNT_VERSION_OTTO   0x4F54544F
#define LEAFGFX_TTF_SFNT_VERSION_TRUE   0x74727565

// Table tags
#define LEAFGFX_TTF_TAG_HEAD    0x68656164
#define LEAFGFX_TTF_TAG_HHEA    0x68686561
#define LEAFGFX_TTF_TAG_MAXP    0x6D617870
#define LEAFGFX_TTF_TAG_CMAP    0x636D6170
#define LEAFGFX_TTF_TAG_LOCA    0x6C6F6361
#define LEAFGFX_TTF_TAG_GLYF    0x676C7966
#define LEAFGFX_TTF_TAG_HMTX    0x686D7478

// =============================================================================
// Glyph Structures
// =============================================================================

// Glyph point
typedef struct {
    int32_t x;
    int32_t y;
    bool    on_curve;
} leafgfx_ttf_point_t;

// Glyph contour
typedef struct {
    uint16_t            num_points;
    leafgfx_ttf_point_t* points;
} leafgfx_ttf_contour_t;

// Glyph outline
typedef struct {
    uint16_t             glyph_index;
    int16_t              x_min, y_min, x_max, y_max;
    uint16_t             num_contours;
    leafgfx_ttf_contour_t* contours;
    uint16_t             advance_width;
    int16_t              left_side_bearing;
} leafgfx_ttf_outline_t;

// Cached glyph bitmap
typedef struct {
    uint8_t* bitmap;
    uint8_t  width;
    uint8_t  height;
    int8_t   bearing_x;
    int8_t   bearing_y;
    uint8_t  advance;
    bool     cached;
} leafgfx_ttf_cached_glyph_t;

// Glyph cache
typedef struct {
    uint32_t                   ppem;
    uint16_t                   num_glyphs;
    leafgfx_ttf_cached_glyph_t* glyphs;
} leafgfx_ttf_cache_t;

// =============================================================================
// Font Context
// =============================================================================

typedef struct leafgfx_ttf_font {
    // Raw file data
    const uint8_t* data;
    size_t         data_size;
    bool           owns_data;

    // Table pointers
    const uint8_t* head_table;
    const uint8_t* hhea_table;
    const uint8_t* maxp_table;
    const uint8_t* cmap_table;
    const uint8_t* loca_table;
    const uint8_t* glyf_table;
    const uint8_t* hmtx_table;

    // Table sizes
    uint32_t head_size;
    uint32_t hhea_size;
    uint32_t maxp_size;
    uint32_t cmap_size;
    uint32_t loca_size;
    uint32_t glyf_size;
    uint32_t hmtx_size;

    // Parsed values
    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_h_metrics;
    int16_t  ascender;
    int16_t  descender;
    int16_t  line_gap;
    uint16_t advance_width_max;
    bool     loca_format_long;

    // cmap lookup
    const uint8_t* cmap_subtable;
    uint16_t       cmap_format;
    uint16_t       cmap_seg_count;

    // Glyph cache
    leafgfx_ttf_cache_t cache;
} leafgfx_ttf_font_t;

// =============================================================================
// Font Loading/Unloading
// =============================================================================

/**
 * Load a TrueType font from memory
 */
leafgfx_ttf_result_t leafgfx_ttf_load_memory(const void* data, size_t size,
                                              leafgfx_ttf_font_t** font);

/**
 * Load a TrueType font from a file path
 */
leafgfx_ttf_result_t leafgfx_ttf_load_file(const char* path,
                                            leafgfx_ttf_font_t** font);

/**
 * Unload a font and free resources
 */
void leafgfx_ttf_unload(leafgfx_ttf_font_t* font);

/**
 * Check if data is a valid TrueType font
 */
bool leafgfx_ttf_is_valid(const void* data, size_t size);

// =============================================================================
// Font Information
// =============================================================================

uint16_t leafgfx_ttf_get_units_per_em(leafgfx_ttf_font_t* font);
uint16_t leafgfx_ttf_get_num_glyphs(leafgfx_ttf_font_t* font);
int16_t  leafgfx_ttf_get_ascender(leafgfx_ttf_font_t* font, uint32_t ppem);
int16_t  leafgfx_ttf_get_descender(leafgfx_ttf_font_t* font, uint32_t ppem);
int16_t  leafgfx_ttf_get_line_gap(leafgfx_ttf_font_t* font, uint32_t ppem);
int16_t  leafgfx_ttf_get_line_height(leafgfx_ttf_font_t* font, uint32_t ppem);

// =============================================================================
// Character Mapping
// =============================================================================

/**
 * Get glyph index for a Unicode codepoint
 * Returns 0 (.notdef) if not found
 */
uint16_t leafgfx_ttf_get_glyph_index(leafgfx_ttf_font_t* font, uint32_t codepoint);

// =============================================================================
// Glyph Metrics
// =============================================================================

leafgfx_ttf_result_t leafgfx_ttf_get_glyph_metrics(
    leafgfx_ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    uint16_t* advance_width,
    int16_t* left_side_bearing,
    int16_t* x_min, int16_t* y_min,
    int16_t* x_max, int16_t* y_max
);

int16_t leafgfx_ttf_get_glyph_advance(leafgfx_ttf_font_t* font,
                                       uint16_t glyph_index, uint32_t ppem);

// =============================================================================
// Glyph Rasterization
// =============================================================================

/**
 * Rasterize a glyph to an 8-bit grayscale bitmap
 * Caller must free() the returned bitmap
 */
uint8_t* leafgfx_ttf_rasterize_glyph(
    leafgfx_ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    uint8_t* out_width,
    uint8_t* out_height,
    int8_t* out_bearing_x,
    int8_t* out_bearing_y,
    bool antialias
);

/**
 * Get a cached glyph (auto-rasterizes if not cached)
 * Do NOT free the returned bitmap
 */
leafgfx_ttf_result_t leafgfx_ttf_get_cached_glyph(
    leafgfx_ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    const uint8_t** bitmap,
    uint8_t* width,
    uint8_t* height,
    int8_t* bearing_x,
    int8_t* bearing_y
);

// =============================================================================
// Cache Management
// =============================================================================

void leafgfx_ttf_clear_cache(leafgfx_ttf_font_t* font);
leafgfx_ttf_result_t leafgfx_ttf_set_cache_ppem(leafgfx_ttf_font_t* font, uint32_t ppem);

// =============================================================================
// Outline Extraction (for advanced use)
// =============================================================================

leafgfx_ttf_result_t leafgfx_ttf_get_outline(
    leafgfx_ttf_font_t* font,
    uint16_t glyph_index,
    leafgfx_ttf_outline_t** outline
);

void leafgfx_ttf_free_outline(leafgfx_ttf_outline_t* outline);

#endif // LEAFGFX_TTF_H
