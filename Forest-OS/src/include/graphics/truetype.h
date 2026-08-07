/**
 * TrueType/OpenType Font Parser for Forest-OS
 *
 * Provides TrueType (.ttf) and OpenType (.otf with TrueType outlines) font
 * loading, parsing, and glyph rasterization capabilities.
 *
 * Based on:
 * - Apple TrueType Reference Manual
 * - Microsoft OpenType Specification 1.9.1
 */

#ifndef TRUETYPE_H
#define TRUETYPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// Error Codes
// =============================================================================

typedef enum {
    TTF_SUCCESS = 0,
    TTF_ERROR_INVALID_FILE,
    TTF_ERROR_UNSUPPORTED_FORMAT,
    TTF_ERROR_TABLE_NOT_FOUND,
    TTF_ERROR_INVALID_TABLE,
    TTF_ERROR_OUT_OF_MEMORY,
    TTF_ERROR_GLYPH_NOT_FOUND,
    TTF_ERROR_INVALID_PARAMETER
} ttf_result_t;

// =============================================================================
// SFNT Container Structures (Big-Endian)
// =============================================================================

// TTF/OTF file signatures (big-endian)
#define TTF_SFNT_VERSION_1_0    0x00010000  // TrueType outlines
#define TTF_SFNT_VERSION_OTTO   0x4F54544F  // OpenType with CFF ("OTTO")
#define TTF_SFNT_VERSION_TRUE   0x74727565  // "true" - Apple TrueType

// SFNT offset table (file header) - 12 bytes
typedef struct __attribute__((packed)) {
    uint32_t sfnt_version;      // 0x00010000 for TTF, "OTTO" for OTF
    uint16_t num_tables;        // Number of tables
    uint16_t search_range;      // (max power of 2 <= numTables) * 16
    uint16_t entry_selector;    // log2(max power of 2 <= numTables)
    uint16_t range_shift;       // numTables * 16 - searchRange
} ttf_offset_table_t;

// Table directory entry - 16 bytes
typedef struct __attribute__((packed)) {
    uint32_t tag;               // 4-byte table identifier
    uint32_t checksum;          // Table checksum
    uint32_t offset;            // Offset from beginning of file
    uint32_t length;            // Length of table
} ttf_table_record_t;

// Table tag constants (as big-endian uint32_t)
#define TTF_TAG_HEAD    0x68656164  // 'head' - font header
#define TTF_TAG_HHEA    0x68686561  // 'hhea' - horizontal header
#define TTF_TAG_MAXP    0x6D617870  // 'maxp' - maximum profile
#define TTF_TAG_CMAP    0x636D6170  // 'cmap' - character to glyph mapping
#define TTF_TAG_LOCA    0x6C6F6361  // 'loca' - index to location
#define TTF_TAG_GLYF    0x676C7966  // 'glyf' - glyph data
#define TTF_TAG_HMTX    0x686D7478  // 'hmtx' - horizontal metrics
#define TTF_TAG_NAME    0x6E616D65  // 'name' - naming table (optional)
#define TTF_TAG_OS2     0x4F532F32  // 'OS/2' - OS/2 and Windows metrics
#define TTF_TAG_POST    0x706F7374  // 'post' - PostScript information

// =============================================================================
// Required Table Structures
// =============================================================================

// 'head' table - Font header (54 bytes)
typedef struct __attribute__((packed)) {
    uint16_t major_version;     // Should be 1
    uint16_t minor_version;     // Should be 0
    int32_t  font_revision;     // Fixed-point 16.16
    uint32_t checksum_adjustment;
    uint32_t magic_number;      // Should be 0x5F0F3CF5
    uint16_t flags;
    uint16_t units_per_em;      // Typically 1000-2048
    int64_t  created;           // Date created
    int64_t  modified;          // Date modified
    int16_t  x_min;             // Bounding box
    int16_t  y_min;
    int16_t  x_max;
    int16_t  y_max;
    uint16_t mac_style;
    uint16_t lowest_rec_ppem;
    int16_t  font_direction_hint;
    int16_t  index_to_loc_format; // 0 = short (16-bit), 1 = long (32-bit)
    int16_t  glyph_data_format;
} ttf_head_table_t;

// 'hhea' table - Horizontal header (36 bytes)
typedef struct __attribute__((packed)) {
    uint16_t major_version;
    uint16_t minor_version;
    int16_t  ascender;          // Typographic ascent
    int16_t  descender;         // Typographic descent (negative)
    int16_t  line_gap;          // Typographic line gap
    uint16_t advance_width_max; // Maximum advance width
    int16_t  min_left_side_bearing;
    int16_t  min_right_side_bearing;
    int16_t  x_max_extent;
    int16_t  caret_slope_rise;
    int16_t  caret_slope_run;
    int16_t  caret_offset;
    int16_t  reserved1;
    int16_t  reserved2;
    int16_t  reserved3;
    int16_t  reserved4;
    int16_t  metric_data_format;
    uint16_t number_of_h_metrics;
} ttf_hhea_table_t;

// 'maxp' table - Maximum profile (6 bytes for version 0.5, 32 for 1.0)
typedef struct __attribute__((packed)) {
    uint32_t version;           // 0x00010000 for TrueType
    uint16_t num_glyphs;        // Number of glyphs in font
    // Additional fields for version 1.0 (TrueType outlines):
    uint16_t max_points;
    uint16_t max_contours;
    uint16_t max_composite_points;
    uint16_t max_composite_contours;
    uint16_t max_zones;
    uint16_t max_twilight_points;
    uint16_t max_storage;
    uint16_t max_function_defs;
    uint16_t max_instruction_defs;
    uint16_t max_stack_elements;
    uint16_t max_size_of_instructions;
    uint16_t max_component_elements;
    uint16_t max_component_depth;
} ttf_maxp_table_t;

// 'hmtx' table entry - Horizontal metrics (4 bytes each)
typedef struct __attribute__((packed)) {
    uint16_t advance_width;
    int16_t  left_side_bearing;
} ttf_long_hor_metric_t;

// =============================================================================
// Character Mapping (cmap) Structures
// =============================================================================

// cmap table header
typedef struct __attribute__((packed)) {
    uint16_t version;           // Should be 0
    uint16_t num_tables;        // Number of encoding subtables
} ttf_cmap_header_t;

// cmap encoding record
typedef struct __attribute__((packed)) {
    uint16_t platform_id;       // 0=Unicode, 1=Mac, 3=Windows
    uint16_t encoding_id;       // Platform-specific
    uint32_t subtable_offset;   // Offset from cmap table start
} ttf_cmap_encoding_t;

// cmap format 4 header (segment mapping to delta values)
typedef struct __attribute__((packed)) {
    uint16_t format;            // 4
    uint16_t length;            // Subtable length in bytes
    uint16_t language;          // Language code
    uint16_t seg_count_x2;      // 2 * segment count
    uint16_t search_range;
    uint16_t entry_selector;
    uint16_t range_shift;
    // Followed by variable-length arrays:
    // uint16_t end_code[seg_count]
    // uint16_t reserved_pad (always 0)
    // uint16_t start_code[seg_count]
    // int16_t  id_delta[seg_count]
    // uint16_t id_range_offset[seg_count]
    // uint16_t glyph_id_array[]
} ttf_cmap_format4_header_t;

// cmap format 12 header (segmented coverage for full 32-bit)
typedef struct __attribute__((packed)) {
    uint16_t format;            // 12
    uint16_t reserved;
    uint32_t length;
    uint32_t language;
    uint32_t num_groups;
    // Followed by sequential map groups
} ttf_cmap_format12_header_t;

// cmap format 12 sequential map group
typedef struct __attribute__((packed)) {
    uint32_t start_char_code;
    uint32_t end_char_code;
    uint32_t start_glyph_id;
} ttf_cmap_format12_group_t;

// =============================================================================
// Glyph Outline Structures
// =============================================================================

// Glyph header (in glyf table)
typedef struct __attribute__((packed)) {
    int16_t  number_of_contours; // -1 for composite glyphs
    int16_t  x_min;
    int16_t  y_min;
    int16_t  x_max;
    int16_t  y_max;
} ttf_glyph_header_t;

// Simple glyph flags
#define TTF_GLYPH_ON_CURVE_POINT    0x01
#define TTF_GLYPH_X_SHORT_VECTOR    0x02
#define TTF_GLYPH_Y_SHORT_VECTOR    0x04
#define TTF_GLYPH_REPEAT_FLAG       0x08
#define TTF_GLYPH_X_IS_SAME         0x10  // Or X_IS_POSITIVE_SHORT if X_SHORT_VECTOR
#define TTF_GLYPH_Y_IS_SAME         0x20  // Or Y_IS_POSITIVE_SHORT if Y_SHORT_VECTOR
#define TTF_GLYPH_OVERLAP_SIMPLE    0x40

// Composite glyph flags
#define TTF_COMPOSITE_ARG_1_AND_2_ARE_WORDS     0x0001
#define TTF_COMPOSITE_ARGS_ARE_XY_VALUES        0x0002
#define TTF_COMPOSITE_ROUND_XY_TO_GRID          0x0004
#define TTF_COMPOSITE_WE_HAVE_A_SCALE           0x0008
#define TTF_COMPOSITE_MORE_COMPONENTS           0x0020
#define TTF_COMPOSITE_WE_HAVE_AN_X_AND_Y_SCALE  0x0040
#define TTF_COMPOSITE_WE_HAVE_A_TWO_BY_TWO      0x0080
#define TTF_COMPOSITE_WE_HAVE_INSTRUCTIONS      0x0100
#define TTF_COMPOSITE_USE_MY_METRICS            0x0200

// Parsed glyph point
typedef struct {
    int32_t x;                  // X coordinate in FUnits
    int32_t y;                  // Y coordinate in FUnits
    bool    on_curve;           // True if on-curve control point
} ttf_glyph_point_t;

// Parsed glyph contour
typedef struct {
    uint16_t           num_points;
    ttf_glyph_point_t* points;   // Array of points
} ttf_glyph_contour_t;

// Parsed glyph outline
typedef struct {
    uint16_t            glyph_index;
    int16_t             x_min, y_min, x_max, y_max;
    uint16_t            num_contours;
    ttf_glyph_contour_t* contours;   // Array of contours
    uint16_t            advance_width;
    int16_t             left_side_bearing;
} ttf_glyph_outline_t;

// =============================================================================
// Main TTF Font Context
// =============================================================================

// Cached glyph bitmap
typedef struct {
    uint8_t* bitmap;            // 8-bit grayscale coverage
    uint8_t  width;
    uint8_t  height;
    int8_t   bearing_x;
    int8_t   bearing_y;
    uint8_t  advance;
    bool     cached;
} ttf_cached_glyph_t;

// Font glyph cache
typedef struct {
    uint32_t           ppem;              // Pixels per EM for this cache
    uint16_t           num_glyphs;
    ttf_cached_glyph_t* glyphs;           // Array indexed by glyph_index
} ttf_glyph_cache_t;

// Main TTF font context
typedef struct ttf_font {
    // Raw file data (may be owned or borrowed)
    const uint8_t* data;
    size_t         data_size;
    bool           owns_data;

    // Table pointers (offsets into data)
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

    // Parsed header values
    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_h_metrics;
    int16_t  ascender;
    int16_t  descender;
    int16_t  line_gap;
    uint16_t advance_width_max;
    bool     loca_format_long;   // True = 32-bit offsets, False = 16-bit

    // cmap lookup
    const uint8_t* cmap_subtable;
    uint16_t       cmap_format;
    uint16_t       cmap_seg_count;  // For format 4

    // Glyph cache (for a single ppem)
    ttf_glyph_cache_t cache;

} ttf_font_t;

// =============================================================================
// Public API - Font Loading/Unloading
// =============================================================================

/**
 * Load a TrueType font from memory
 * @param data      Pointer to font file data
 * @param size      Size of font data in bytes
 * @param font      Output pointer to created font
 * @return          TTF_SUCCESS or error code
 */
ttf_result_t ttf_load_from_memory(const void* data, size_t size, ttf_font_t** font);

/**
 * Load a TrueType font from VFS
 * @param path      VFS path to font file
 * @param font      Output pointer to created font
 * @return          TTF_SUCCESS or error code
 */
ttf_result_t ttf_load_from_vfs(const char* path, ttf_font_t** font);

/**
 * Unload a TrueType font and free resources
 * @param font      Font to unload
 */
void ttf_unload(ttf_font_t* font);

// =============================================================================
// Public API - Font Information
// =============================================================================

/**
 * Get units per EM for the font
 */
uint16_t ttf_get_units_per_em(ttf_font_t* font);

/**
 * Get number of glyphs in the font
 */
uint16_t ttf_get_num_glyphs(ttf_font_t* font);

/**
 * Get scaled ascender value
 * @param font      Font
 * @param ppem      Pixels per EM (target size)
 * @return          Ascender in pixels
 */
int16_t ttf_get_ascender(ttf_font_t* font, uint32_t ppem);

/**
 * Get scaled descender value (negative)
 * @param font      Font
 * @param ppem      Pixels per EM (target size)
 * @return          Descender in pixels (negative)
 */
int16_t ttf_get_descender(ttf_font_t* font, uint32_t ppem);

/**
 * Get scaled line gap
 * @param font      Font
 * @param ppem      Pixels per EM (target size)
 * @return          Line gap in pixels
 */
int16_t ttf_get_line_gap(ttf_font_t* font, uint32_t ppem);

// =============================================================================
// Public API - Character Mapping
// =============================================================================

/**
 * Get glyph index for a Unicode codepoint
 * @param font      Font
 * @param codepoint Unicode codepoint
 * @return          Glyph index (0 = .notdef if not found)
 */
uint16_t ttf_get_glyph_index(ttf_font_t* font, uint32_t codepoint);

// =============================================================================
// Public API - Glyph Metrics
// =============================================================================

/**
 * Get glyph metrics scaled to ppem
 * @param font              Font
 * @param glyph_index       Glyph index from ttf_get_glyph_index()
 * @param ppem              Pixels per EM (target size)
 * @param advance_width     Output: advance width in pixels
 * @param left_side_bearing Output: left side bearing in pixels
 * @param x_min, y_min      Output: bounding box min (may be NULL)
 * @param x_max, y_max      Output: bounding box max (may be NULL)
 * @return                  TTF_SUCCESS or error code
 */
ttf_result_t ttf_get_glyph_metrics(
    ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    uint16_t* advance_width,
    int16_t* left_side_bearing,
    int16_t* x_min, int16_t* y_min,
    int16_t* x_max, int16_t* y_max
);

/**
 * Get glyph advance width (convenience function)
 * @param font          Font
 * @param glyph_index   Glyph index from ttf_get_glyph_index()
 * @param ppem          Pixels per EM (target size)
 * @return              Advance width in pixels, or 0 on error
 */
int16_t ttf_get_glyph_advance(ttf_font_t* font, uint16_t glyph_index, uint32_t ppem);

// =============================================================================
// Public API - Glyph Rasterization
// =============================================================================

/**
 * Rasterize a glyph to an 8-bit grayscale coverage bitmap
 * Caller must kfree() the returned bitmap
 *
 * @param font          Font
 * @param glyph_index   Glyph index
 * @param ppem          Pixels per EM (target size)
 * @param out_width     Output: bitmap width
 * @param out_height    Output: bitmap height
 * @param out_bearing_x Output: X bearing (offset from pen position)
 * @param out_bearing_y Output: Y bearing (offset from baseline)
 * @param antialias     True for antialiased rendering
 * @return              Bitmap pointer (NULL for empty glyphs), caller frees
 */
uint8_t* ttf_rasterize_glyph(
    ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    uint8_t* out_width,
    uint8_t* out_height,
    int8_t* out_bearing_x,
    int8_t* out_bearing_y,
    bool antialias
);

/**
 * Get a cached glyph bitmap (auto-rasterizes if not cached)
 * Do NOT free the returned bitmap
 *
 * @param font          Font
 * @param glyph_index   Glyph index
 * @param ppem          Pixels per EM (must match cache ppem)
 * @param bitmap        Output: pointer to cached bitmap
 * @param width         Output: bitmap width
 * @param height        Output: bitmap height
 * @param bearing_x     Output: X bearing
 * @param bearing_y     Output: Y bearing
 * @return              TTF_SUCCESS or error code
 */
ttf_result_t ttf_get_cached_glyph(
    ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    const uint8_t** bitmap,
    uint8_t* width,
    uint8_t* height,
    int8_t* bearing_x,
    int8_t* bearing_y
);

// =============================================================================
// Public API - Cache Management
// =============================================================================

/**
 * Clear the glyph cache
 */
void ttf_clear_cache(ttf_font_t* font);

/**
 * Set cache ppem (clears existing cache if different)
 */
ttf_result_t ttf_set_cache_ppem(ttf_font_t* font, uint32_t ppem);

// =============================================================================
// Public API - Format Detection
// =============================================================================

/**
 * Check if data is a valid TrueType/OpenType font
 */
bool ttf_is_valid_font(const void* data, size_t size);

// =============================================================================
// Internal API - Glyph Outline Extraction
// =============================================================================

/**
 * Parse glyph outline from glyf table
 * Caller must free returned outline with ttf_free_glyph_outline()
 */
ttf_result_t ttf_get_glyph_outline(
    ttf_font_t* font,
    uint16_t glyph_index,
    ttf_glyph_outline_t** outline
);

/**
 * Free a glyph outline
 */
void ttf_free_glyph_outline(ttf_glyph_outline_t* outline);

#endif // TRUETYPE_H
