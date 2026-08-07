/**
 * TrueType/OpenType Font Parser for Forest-OS
 *
 * Implements TrueType (.ttf) and OpenType (.otf with TrueType outlines) font
 * loading, parsing, and glyph outline extraction.
 */

#include "../include/graphics/truetype.h"
#include "../include/graphics/truetype_raster.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/mm.h"
#include "../include/vfs.h"

// =============================================================================
// Byte Swapping Utilities (Big-Endian to Little-Endian)
// =============================================================================

static inline uint16_t ttf_swap16(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

static inline uint32_t ttf_swap32(uint32_t val) {
    return ((val & 0xFF) << 24) |
           ((val & 0xFF00) << 8) |
           ((val >> 8) & 0xFF00) |
           ((val >> 24) & 0xFF);
}

__attribute__((unused)) static inline int16_t ttf_swap16s(int16_t val) {
    return (int16_t)ttf_swap16((uint16_t)val);
}

__attribute__((unused)) static inline int32_t ttf_swap32s(int32_t val) {
    return (int32_t)ttf_swap32((uint32_t)val);
}

// Read big-endian values from byte array
static inline uint16_t ttf_read16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t ttf_read32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline int16_t ttf_read16s(const uint8_t* p) {
    return (int16_t)ttf_read16(p);
}

// =============================================================================
// Scaling Utilities
// =============================================================================

// Scale from FUnits to pixels: pixel = funits * ppem / upem
static inline int32_t ttf_scale(int32_t funits, uint32_t ppem, uint16_t upem) {
    if (upem == 0) return 0;
    return (int32_t)(((int64_t)funits * ppem + upem / 2) / upem);
}

// =============================================================================
// Table Lookup
// =============================================================================

static const uint8_t* ttf_find_table(const uint8_t* data, size_t size,
                                      uint32_t tag, uint32_t* out_size) {
    if (size < sizeof(ttf_offset_table_t)) {
        return NULL;
    }

    uint16_t num_tables = ttf_read16(data + 4);

    // Bounds check
    size_t table_dir_end = 12 + num_tables * 16;
    if (table_dir_end > size) {
        return NULL;
    }

    const uint8_t* table_dir = data + 12;
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t* entry = table_dir + i * 16;
        uint32_t entry_tag = ttf_read32(entry);

        if (entry_tag == tag) {
            uint32_t offset = ttf_read32(entry + 8);
            uint32_t length = ttf_read32(entry + 12);

            // Bounds check
            if (offset + length > size) {
                return NULL;
            }

            if (out_size) {
                *out_size = length;
            }
            return data + offset;
        }
    }

    return NULL;
}

// =============================================================================
// Format Detection
// =============================================================================

bool ttf_is_valid_font(const void* data, size_t size) {
    if (!data || size < 12) {
        return false;
    }

    uint32_t magic = ttf_read32((const uint8_t*)data);

    // TrueType or OpenType with TrueType outlines
    if (magic == TTF_SFNT_VERSION_1_0 || magic == TTF_SFNT_VERSION_TRUE) {
        return true;
    }

    // OpenType with CFF outlines (we support glyf table only for now)
    if (magic == TTF_SFNT_VERSION_OTTO) {
        // Check if it has glyf table (TrueType outlines)
        uint32_t glyf_size;
        return ttf_find_table((const uint8_t*)data, size, TTF_TAG_GLYF, &glyf_size) != NULL;
    }

    return false;
}

// =============================================================================
// Font Loading
// =============================================================================

ttf_result_t ttf_load_from_memory(const void* data, size_t size, ttf_font_t** out_font) {
    if (!data || !out_font || size < 12) {
        return TTF_ERROR_INVALID_PARAMETER;
    }

    const uint8_t* font_data = (const uint8_t*)data;

    // Validate magic number
    uint32_t magic = ttf_read32(font_data);
    if (magic != TTF_SFNT_VERSION_1_0 && magic != TTF_SFNT_VERSION_TRUE &&
        magic != TTF_SFNT_VERSION_OTTO) {
        debuglog(DEBUG_ERROR, "[TTF] Invalid font magic: 0x%08X\n", magic);
        return TTF_ERROR_INVALID_FILE;
    }

    // Allocate font structure
    ttf_font_t* font = kmalloc(sizeof(ttf_font_t));
    if (!font) {
        return TTF_ERROR_OUT_OF_MEMORY;
    }
    memset(font, 0, sizeof(ttf_font_t));

    font->data = font_data;
    font->data_size = size;
    font->owns_data = false;

    // Find required tables
    font->head_table = ttf_find_table(font_data, size, TTF_TAG_HEAD, &font->head_size);
    font->hhea_table = ttf_find_table(font_data, size, TTF_TAG_HHEA, &font->hhea_size);
    font->maxp_table = ttf_find_table(font_data, size, TTF_TAG_MAXP, &font->maxp_size);
    font->cmap_table = ttf_find_table(font_data, size, TTF_TAG_CMAP, &font->cmap_size);
    font->loca_table = ttf_find_table(font_data, size, TTF_TAG_LOCA, &font->loca_size);
    font->glyf_table = ttf_find_table(font_data, size, TTF_TAG_GLYF, &font->glyf_size);
    font->hmtx_table = ttf_find_table(font_data, size, TTF_TAG_HMTX, &font->hmtx_size);

    // Check required tables
    if (!font->head_table || !font->hhea_table || !font->maxp_table ||
        !font->cmap_table || !font->loca_table || !font->glyf_table ||
        !font->hmtx_table) {
        debuglog(DEBUG_ERROR, "[TTF] Missing required table(s)\n");
        kfree(font);
        return TTF_ERROR_TABLE_NOT_FOUND;
    }

    // Parse head table
    if (font->head_size < 54) {
        kfree(font);
        return TTF_ERROR_INVALID_TABLE;
    }

    uint32_t head_magic = ttf_read32(font->head_table + 12);
    if (head_magic != 0x5F0F3CF5) {
        debuglog(DEBUG_WARN, "[TTF] Invalid head table magic: 0x%08X\n", head_magic);
    }

    font->units_per_em = ttf_read16(font->head_table + 18);
    font->loca_format_long = ttf_read16s(font->head_table + 50) != 0;

    if (font->units_per_em == 0 || font->units_per_em > 16384) {
        debuglog(DEBUG_ERROR, "[TTF] Invalid units_per_em: %u\n", font->units_per_em);
        kfree(font);
        return TTF_ERROR_INVALID_TABLE;
    }

    // Parse maxp table
    if (font->maxp_size < 6) {
        kfree(font);
        return TTF_ERROR_INVALID_TABLE;
    }
    font->num_glyphs = ttf_read16(font->maxp_table + 4);

    // Parse hhea table
    if (font->hhea_size < 36) {
        kfree(font);
        return TTF_ERROR_INVALID_TABLE;
    }
    font->ascender = ttf_read16s(font->hhea_table + 4);
    font->descender = ttf_read16s(font->hhea_table + 6);
    font->line_gap = ttf_read16s(font->hhea_table + 8);
    font->advance_width_max = ttf_read16(font->hhea_table + 10);
    font->num_h_metrics = ttf_read16(font->hhea_table + 34);

    // Parse cmap table - find suitable subtable
    if (font->cmap_size < 4) {
        kfree(font);
        return TTF_ERROR_INVALID_TABLE;
    }

    uint16_t cmap_num_tables = ttf_read16(font->cmap_table + 2);
    const uint8_t* best_subtable = NULL;
    uint16_t best_format = 0;
    int best_priority = -1;

    for (uint16_t i = 0; i < cmap_num_tables; i++) {
        size_t enc_offset = 4 + i * 8;
        if (enc_offset + 8 > font->cmap_size) break;

        uint16_t platform_id = ttf_read16(font->cmap_table + enc_offset);
        uint16_t encoding_id = ttf_read16(font->cmap_table + enc_offset + 2);
        uint32_t subtable_offset = ttf_read32(font->cmap_table + enc_offset + 4);

        if (subtable_offset + 2 > font->cmap_size) continue;

        const uint8_t* subtable = font->cmap_table + subtable_offset;
        uint16_t format = ttf_read16(subtable);

        // Priority: prefer format 12 > format 4, prefer Unicode/Windows platforms
        int priority = 0;
        if (format == 12) priority += 100;
        else if (format == 4) priority += 50;
        else continue;  // Skip unsupported formats

        // Unicode platform (0) or Windows Unicode BMP (3,1)
        if (platform_id == 0) priority += 20;
        else if (platform_id == 3 && encoding_id == 1) priority += 15;
        else if (platform_id == 3 && encoding_id == 10) priority += 25;  // Windows UCS-4

        if (priority > best_priority) {
            best_priority = priority;
            best_subtable = subtable;
            best_format = format;
        }
    }

    if (!best_subtable) {
        debuglog(DEBUG_ERROR, "[TTF] No suitable cmap subtable found\n");
        kfree(font);
        return TTF_ERROR_INVALID_TABLE;
    }

    font->cmap_subtable = best_subtable;
    font->cmap_format = best_format;

    if (best_format == 4) {
        font->cmap_seg_count = ttf_read16(best_subtable + 6) / 2;
    }

    debuglog(DEBUG_INFO, "[TTF] Loaded font: %u glyphs, %u upem, cmap format %u\n",
             font->num_glyphs, font->units_per_em, font->cmap_format);

    *out_font = font;
    return TTF_SUCCESS;
}

ttf_result_t ttf_load_from_vfs(const char* path, ttf_font_t** font) {
    if (!path || !font) {
        return TTF_ERROR_INVALID_PARAMETER;
    }

    const uint8_t* file_data = NULL;
    uint32_t file_size = 0;

    if (!vfs_read_file(path, &file_data, &file_size)) {
        debuglog(DEBUG_ERROR, "[TTF] Failed to read file: %s\n", path);
        return TTF_ERROR_INVALID_FILE;
    }

    ttf_result_t result = ttf_load_from_memory(file_data, file_size, font);
    if (result == TTF_SUCCESS) {
        // VFS data is managed by ramdisk, so we don't own it
        (*font)->owns_data = false;
    }

    return result;
}

void ttf_unload(ttf_font_t* font) {
    if (!font) return;

    // Clear glyph cache
    ttf_clear_cache(font);

    // Free font data if owned
    if (font->owns_data && font->data) {
        kfree((void*)font->data);
    }

    kfree(font);
}

// =============================================================================
// Font Information
// =============================================================================

uint16_t ttf_get_units_per_em(ttf_font_t* font) {
    return font ? font->units_per_em : 0;
}

uint16_t ttf_get_num_glyphs(ttf_font_t* font) {
    return font ? font->num_glyphs : 0;
}

int16_t ttf_get_ascender(ttf_font_t* font, uint32_t ppem) {
    if (!font) return 0;
    return (int16_t)ttf_scale(font->ascender, ppem, font->units_per_em);
}

int16_t ttf_get_descender(ttf_font_t* font, uint32_t ppem) {
    if (!font) return 0;
    return (int16_t)ttf_scale(font->descender, ppem, font->units_per_em);
}

int16_t ttf_get_line_gap(ttf_font_t* font, uint32_t ppem) {
    if (!font) return 0;
    return (int16_t)ttf_scale(font->line_gap, ppem, font->units_per_em);
}

// =============================================================================
// Character Mapping (cmap)
// =============================================================================

// Format 4: Segment mapping to delta values
static uint16_t ttf_cmap_lookup_format4(ttf_font_t* font, uint32_t codepoint) {
    if (codepoint > 0xFFFF) {
        return 0;  // Format 4 only supports BMP
    }

    const uint8_t* subtable = font->cmap_subtable;
    uint16_t seg_count = font->cmap_seg_count;

    // Calculate array offsets
    // Header is 14 bytes, then:
    // endCode[segCount], reservedPad (2 bytes), startCode[segCount],
    // idDelta[segCount], idRangeOffset[segCount], glyphIdArray[]
    const uint8_t* end_codes = subtable + 14;
    const uint8_t* start_codes = end_codes + seg_count * 2 + 2;  // +2 for reserved pad
    const uint8_t* id_deltas = start_codes + seg_count * 2;
    const uint8_t* id_range_offsets = id_deltas + seg_count * 2;

    // Binary search for segment
    uint16_t lo = 0, hi = seg_count;
    while (lo < hi) {
        uint16_t mid = (lo + hi) / 2;
        uint16_t end_code = ttf_read16(end_codes + mid * 2);
        if (codepoint > end_code) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo >= seg_count) {
        return 0;
    }

    uint16_t end_code = ttf_read16(end_codes + lo * 2);
    uint16_t start_code = ttf_read16(start_codes + lo * 2);
    int16_t id_delta = ttf_read16s(id_deltas + lo * 2);
    uint16_t id_range_offset = ttf_read16(id_range_offsets + lo * 2);

    if (codepoint < start_code || codepoint > end_code) {
        return 0;
    }

    uint16_t glyph_index;
    if (id_range_offset == 0) {
        glyph_index = (uint16_t)((codepoint + id_delta) & 0xFFFF);
    } else {
        // Calculate offset into glyphIdArray
        (void)(id_range_offset / 2);
        const uint8_t* glyph_id_ptr = id_range_offsets + lo * 2 + id_range_offset +
                                       (codepoint - start_code) * 2;
        glyph_index = ttf_read16(glyph_id_ptr);
        if (glyph_index != 0) {
            glyph_index = (uint16_t)((glyph_index + id_delta) & 0xFFFF);
        }
    }

    return glyph_index;
}

// Format 12: Segmented coverage
static uint16_t ttf_cmap_lookup_format12(ttf_font_t* font, uint32_t codepoint) {
    const uint8_t* subtable = font->cmap_subtable;
    uint32_t num_groups = ttf_read32(subtable + 12);

    // Binary search for group
    const uint8_t* groups = subtable + 16;
    uint32_t lo = 0, hi = num_groups;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const uint8_t* group = groups + mid * 12;
        uint32_t start_code = ttf_read32(group);
        uint32_t end_code = ttf_read32(group + 4);

        if (codepoint < start_code) {
            hi = mid;
        } else if (codepoint > end_code) {
            lo = mid + 1;
        } else {
            // Found the segment
            uint32_t start_glyph_id = ttf_read32(group + 8);
            return (uint16_t)(start_glyph_id + (codepoint - start_code));
        }
    }

    return 0;
}

uint16_t ttf_get_glyph_index(ttf_font_t* font, uint32_t codepoint) {
    if (!font || !font->cmap_subtable) {
        return 0;
    }

    switch (font->cmap_format) {
        case 4:
            return ttf_cmap_lookup_format4(font, codepoint);
        case 12:
            return ttf_cmap_lookup_format12(font, codepoint);
        default:
            return 0;
    }
}

// =============================================================================
// Glyph Metrics
// =============================================================================

ttf_result_t ttf_get_glyph_metrics(
    ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    uint16_t* advance_width,
    int16_t* left_side_bearing,
    int16_t* x_min, int16_t* y_min,
    int16_t* x_max, int16_t* y_max
) {
    if (!font) {
        return TTF_ERROR_INVALID_PARAMETER;
    }

    if (glyph_index >= font->num_glyphs) {
        return TTF_ERROR_GLYPH_NOT_FOUND;
    }

    // Get horizontal metrics from hmtx table
    uint16_t aw_funits;
    int16_t lsb_funits;

    if (glyph_index < font->num_h_metrics) {
        // Full longHorMetric entry
        const uint8_t* metric = font->hmtx_table + glyph_index * 4;
        aw_funits = ttf_read16(metric);
        lsb_funits = ttf_read16s(metric + 2);
    } else {
        // Advance width from last longHorMetric, LSB from leftSideBearing array
        const uint8_t* last_metric = font->hmtx_table + (font->num_h_metrics - 1) * 4;
        aw_funits = ttf_read16(last_metric);

        uint16_t lsb_index = glyph_index - font->num_h_metrics;
        const uint8_t* lsb_array = font->hmtx_table + font->num_h_metrics * 4;
        lsb_funits = ttf_read16s(lsb_array + lsb_index * 2);
    }

    // Get glyph bounding box from glyf table
    int16_t xmin_funits = 0, ymin_funits = 0, xmax_funits = 0, ymax_funits = 0;

    // Get glyph offset from loca table
    uint32_t glyph_offset, next_offset;
    if (font->loca_format_long) {
        glyph_offset = ttf_read32(font->loca_table + glyph_index * 4);
        next_offset = ttf_read32(font->loca_table + (glyph_index + 1) * 4);
    } else {
        glyph_offset = ttf_read16(font->loca_table + glyph_index * 2) * 2;
        next_offset = ttf_read16(font->loca_table + (glyph_index + 1) * 2) * 2;
    }

    // Check if glyph has outline data (non-empty)
    if (glyph_offset < next_offset && glyph_offset + 10 <= font->glyf_size) {
        const uint8_t* glyph_data = font->glyf_table + glyph_offset;
        // Skip numberOfContours (2 bytes)
        xmin_funits = ttf_read16s(glyph_data + 2);
        ymin_funits = ttf_read16s(glyph_data + 4);
        xmax_funits = ttf_read16s(glyph_data + 6);
        ymax_funits = ttf_read16s(glyph_data + 8);
    }

    // Scale to pixels
    if (advance_width) {
        *advance_width = (uint16_t)ttf_scale(aw_funits, ppem, font->units_per_em);
    }
    if (left_side_bearing) {
        *left_side_bearing = (int16_t)ttf_scale(lsb_funits, ppem, font->units_per_em);
    }
    if (x_min) {
        *x_min = (int16_t)ttf_scale(xmin_funits, ppem, font->units_per_em);
    }
    if (y_min) {
        *y_min = (int16_t)ttf_scale(ymin_funits, ppem, font->units_per_em);
    }
    if (x_max) {
        *x_max = (int16_t)ttf_scale(xmax_funits, ppem, font->units_per_em);
    }
    if (y_max) {
        *y_max = (int16_t)ttf_scale(ymax_funits, ppem, font->units_per_em);
    }

    return TTF_SUCCESS;
}

// Convenience function to get glyph advance width
int16_t ttf_get_glyph_advance(ttf_font_t* font, uint16_t glyph_index, uint32_t ppem) {
    uint16_t advance = 0;
    ttf_result_t result = ttf_get_glyph_metrics(font, glyph_index, ppem, &advance, NULL, NULL, NULL, NULL, NULL);
    if (result != TTF_SUCCESS) {
        return 0;
    }
    return (int16_t)advance;
}

// =============================================================================
// Glyph Outline Extraction
// =============================================================================

ttf_result_t ttf_get_glyph_outline(
    ttf_font_t* font,
    uint16_t glyph_index,
    ttf_glyph_outline_t** out_outline
) {
    if (!font || !out_outline) {
        return TTF_ERROR_INVALID_PARAMETER;
    }

    if (glyph_index >= font->num_glyphs) {
        return TTF_ERROR_GLYPH_NOT_FOUND;
    }

    // Get glyph offset from loca table
    uint32_t glyph_offset, next_offset;
    if (font->loca_format_long) {
        glyph_offset = ttf_read32(font->loca_table + glyph_index * 4);
        next_offset = ttf_read32(font->loca_table + (glyph_index + 1) * 4);
    } else {
        glyph_offset = ttf_read16(font->loca_table + glyph_index * 2) * 2;
        next_offset = ttf_read16(font->loca_table + (glyph_index + 1) * 2) * 2;
    }

    // Allocate outline structure
    ttf_glyph_outline_t* outline = kmalloc(sizeof(ttf_glyph_outline_t));
    if (!outline) {
        return TTF_ERROR_OUT_OF_MEMORY;
    }
    memset(outline, 0, sizeof(ttf_glyph_outline_t));

    outline->glyph_index = glyph_index;

    // Get horizontal metrics
    if (glyph_index < font->num_h_metrics) {
        const uint8_t* metric = font->hmtx_table + glyph_index * 4;
        outline->advance_width = ttf_read16(metric);
        outline->left_side_bearing = ttf_read16s(metric + 2);
    } else {
        const uint8_t* last_metric = font->hmtx_table + (font->num_h_metrics - 1) * 4;
        outline->advance_width = ttf_read16(last_metric);
        uint16_t lsb_index = glyph_index - font->num_h_metrics;
        const uint8_t* lsb_array = font->hmtx_table + font->num_h_metrics * 4;
        outline->left_side_bearing = ttf_read16s(lsb_array + lsb_index * 2);
    }

    // Check for empty glyph
    if (glyph_offset >= next_offset) {
        // Empty glyph (e.g., space)
        outline->num_contours = 0;
        outline->contours = NULL;
        *out_outline = outline;
        return TTF_SUCCESS;
    }

    if (glyph_offset + 10 > font->glyf_size) {
        kfree(outline);
        return TTF_ERROR_INVALID_TABLE;
    }

    const uint8_t* glyph_data = font->glyf_table + glyph_offset;
    uint32_t glyph_size = next_offset - glyph_offset;

    int16_t num_contours = ttf_read16s(glyph_data);
    outline->x_min = ttf_read16s(glyph_data + 2);
    outline->y_min = ttf_read16s(glyph_data + 4);
    outline->x_max = ttf_read16s(glyph_data + 6);
    outline->y_max = ttf_read16s(glyph_data + 8);

    // Check for composite glyph
    if (num_contours < 0) {
        // Composite glyph - not supported in initial implementation
        // Return as empty for now
        outline->num_contours = 0;
        outline->contours = NULL;
        *out_outline = outline;
        return TTF_SUCCESS;
    }

    outline->num_contours = (uint16_t)num_contours;

    if (num_contours == 0) {
        outline->contours = NULL;
        *out_outline = outline;
        return TTF_SUCCESS;
    }

    // Parse simple glyph
    const uint8_t* ptr = glyph_data + 10;

    // Read endPtsOfContours array
    if ((uint32_t)(10 + num_contours * 2) > glyph_size) {
        kfree(outline);
        return TTF_ERROR_INVALID_TABLE;
    }

    uint16_t* end_pts = kmalloc(num_contours * sizeof(uint16_t));
    if (!end_pts) {
        kfree(outline);
        return TTF_ERROR_OUT_OF_MEMORY;
    }

    uint16_t num_points = 0;
    for (int i = 0; i < num_contours; i++) {
        end_pts[i] = ttf_read16(ptr + i * 2);
        if (end_pts[i] >= num_points) {
            num_points = end_pts[i] + 1;
        }
    }
    ptr += num_contours * 2;

    // Skip instructions
    uint16_t instruction_length = ttf_read16(ptr);
    ptr += 2 + instruction_length;

    // Allocate points array
    ttf_glyph_point_t* points = kmalloc(num_points * sizeof(ttf_glyph_point_t));
    if (!points) {
        kfree(end_pts);
        kfree(outline);
        return TTF_ERROR_OUT_OF_MEMORY;
    }
    memset(points, 0, num_points * sizeof(ttf_glyph_point_t));

    // Parse flags
    uint8_t* flags = kmalloc(num_points);
    if (!flags) {
        kfree(points);
        kfree(end_pts);
        kfree(outline);
        return TTF_ERROR_OUT_OF_MEMORY;
    }

    uint16_t flag_idx = 0;
    while (flag_idx < num_points) {
        uint8_t flag = *ptr++;
        flags[flag_idx++] = flag;

        if (flag & TTF_GLYPH_REPEAT_FLAG) {
            uint8_t repeat_count = *ptr++;
            for (uint8_t j = 0; j < repeat_count && flag_idx < num_points; j++) {
                flags[flag_idx++] = flag;
            }
        }
    }

    // Parse X coordinates
    int32_t x = 0;
    for (uint16_t i = 0; i < num_points; i++) {
        uint8_t flag = flags[i];
        if (flag & TTF_GLYPH_X_SHORT_VECTOR) {
            int16_t dx = *ptr++;
            if (!(flag & TTF_GLYPH_X_IS_SAME)) {
                dx = -dx;
            }
            x += dx;
        } else if (!(flag & TTF_GLYPH_X_IS_SAME)) {
            x += ttf_read16s(ptr);
            ptr += 2;
        }
        // else: X_IS_SAME means x unchanged
        points[i].x = x;
        points[i].on_curve = (flag & TTF_GLYPH_ON_CURVE_POINT) != 0;
    }

    // Parse Y coordinates
    int32_t y = 0;
    for (uint16_t i = 0; i < num_points; i++) {
        uint8_t flag = flags[i];
        if (flag & TTF_GLYPH_Y_SHORT_VECTOR) {
            int16_t dy = *ptr++;
            if (!(flag & TTF_GLYPH_Y_IS_SAME)) {
                dy = -dy;
            }
            y += dy;
        } else if (!(flag & TTF_GLYPH_Y_IS_SAME)) {
            y += ttf_read16s(ptr);
            ptr += 2;
        }
        points[i].y = y;
    }

    kfree(flags);

    // Build contours
    outline->contours = kmalloc(num_contours * sizeof(ttf_glyph_contour_t));
    if (!outline->contours) {
        kfree(points);
        kfree(end_pts);
        kfree(outline);
        return TTF_ERROR_OUT_OF_MEMORY;
    }

    uint16_t start_pt = 0;
    for (int i = 0; i < num_contours; i++) {
        uint16_t end_pt = end_pts[i];
        uint16_t contour_points = end_pt - start_pt + 1;

        outline->contours[i].num_points = contour_points;
        outline->contours[i].points = kmalloc(contour_points * sizeof(ttf_glyph_point_t));
        if (!outline->contours[i].points) {
            // Cleanup
            for (int j = 0; j < i; j++) {
                kfree(outline->contours[j].points);
            }
            kfree(outline->contours);
            kfree(points);
            kfree(end_pts);
            kfree(outline);
            return TTF_ERROR_OUT_OF_MEMORY;
        }

        memcpy(outline->contours[i].points, &points[start_pt],
               contour_points * sizeof(ttf_glyph_point_t));

        start_pt = end_pt + 1;
    }

    kfree(points);
    kfree(end_pts);

    *out_outline = outline;
    return TTF_SUCCESS;
}

void ttf_free_glyph_outline(ttf_glyph_outline_t* outline) {
    if (!outline) return;

    if (outline->contours) {
        for (uint16_t i = 0; i < outline->num_contours; i++) {
            if (outline->contours[i].points) {
                kfree(outline->contours[i].points);
            }
        }
        kfree(outline->contours);
    }

    kfree(outline);
}

// =============================================================================
// Cache Management
// =============================================================================

void ttf_clear_cache(ttf_font_t* font) {
    if (!font || !font->cache.glyphs) return;

    for (uint16_t i = 0; i < font->cache.num_glyphs; i++) {
        if (font->cache.glyphs[i].bitmap) {
            kfree(font->cache.glyphs[i].bitmap);
        }
    }

    kfree(font->cache.glyphs);
    font->cache.glyphs = NULL;
    font->cache.num_glyphs = 0;
    font->cache.ppem = 0;
}

ttf_result_t ttf_set_cache_ppem(ttf_font_t* font, uint32_t ppem) {
    if (!font) {
        return TTF_ERROR_INVALID_PARAMETER;
    }

    if (font->cache.ppem == ppem && font->cache.glyphs) {
        return TTF_SUCCESS;  // Already at this ppem
    }

    // Clear existing cache
    ttf_clear_cache(font);

    // Allocate new cache
    font->cache.ppem = ppem;
    font->cache.num_glyphs = font->num_glyphs;
    font->cache.glyphs = kmalloc(font->num_glyphs * sizeof(ttf_cached_glyph_t));
    if (!font->cache.glyphs) {
        return TTF_ERROR_OUT_OF_MEMORY;
    }

    memset(font->cache.glyphs, 0, font->num_glyphs * sizeof(ttf_cached_glyph_t));

    return TTF_SUCCESS;
}

// =============================================================================
// Glyph Rasterization (delegates to truetype_raster.c)
// =============================================================================

uint8_t* ttf_rasterize_glyph(
    ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    uint8_t* out_width,
    uint8_t* out_height,
    int8_t* out_bearing_x,
    int8_t* out_bearing_y,
    bool antialias
) {
    if (!font || !out_width || !out_height || !out_bearing_x || !out_bearing_y) {
        return NULL;
    }

    // Get glyph outline
    ttf_glyph_outline_t* outline = NULL;
    ttf_result_t result = ttf_get_glyph_outline(font, glyph_index, &outline);
    if (result != TTF_SUCCESS || !outline) {
        *out_width = 0;
        *out_height = 0;
        *out_bearing_x = 0;
        *out_bearing_y = 0;
        return NULL;
    }

    // Empty glyph (e.g., space)
    if (outline->num_contours == 0) {
        uint16_t advance;
        ttf_get_glyph_metrics(font, glyph_index, ppem, &advance, NULL, NULL, NULL, NULL, NULL);
        *out_width = 0;
        *out_height = 0;
        *out_bearing_x = 0;
        *out_bearing_y = 0;
        ttf_free_glyph_outline(outline);
        return NULL;
    }

    // Rasterize using truetype_raster
    uint8_t* bitmap = ttf_raster_glyph(
        outline,
        ppem,
        font->units_per_em,
        out_width,
        out_height,
        out_bearing_x,
        out_bearing_y,
        antialias
    );

    ttf_free_glyph_outline(outline);

    return bitmap;
}

ttf_result_t ttf_get_cached_glyph(
    ttf_font_t* font,
    uint16_t glyph_index,
    uint32_t ppem,
    const uint8_t** bitmap,
    uint8_t* width,
    uint8_t* height,
    int8_t* bearing_x,
    int8_t* bearing_y
) {
    if (!font || !bitmap || !width || !height || !bearing_x || !bearing_y) {
        return TTF_ERROR_INVALID_PARAMETER;
    }

    if (glyph_index >= font->num_glyphs) {
        return TTF_ERROR_GLYPH_NOT_FOUND;
    }

    // Ensure cache is at the right ppem
    if (font->cache.ppem != ppem || !font->cache.glyphs) {
        ttf_result_t result = ttf_set_cache_ppem(font, ppem);
        if (result != TTF_SUCCESS) {
            return result;
        }
    }

    ttf_cached_glyph_t* cached = &font->cache.glyphs[glyph_index];

    // Rasterize if not cached
    if (!cached->cached) {
        uint8_t w, h;
        int8_t bx, by;
        uint8_t* bmp = ttf_rasterize_glyph(font, glyph_index, ppem, &w, &h, &bx, &by, true);

        cached->bitmap = bmp;
        cached->width = w;
        cached->height = h;
        cached->bearing_x = bx;
        cached->bearing_y = by;

        // Get advance width
        uint16_t advance;
        ttf_get_glyph_metrics(font, glyph_index, ppem, &advance, NULL, NULL, NULL, NULL, NULL);
        cached->advance = (uint8_t)(advance > 255 ? 255 : advance);

        cached->cached = true;
    }

    *bitmap = cached->bitmap;
    *width = cached->width;
    *height = cached->height;
    *bearing_x = cached->bearing_x;
    *bearing_y = cached->bearing_y;

    return TTF_SUCCESS;
}
