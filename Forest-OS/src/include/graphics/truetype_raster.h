/**
 * TrueType Glyph Rasterizer for Forest-OS
 *
 * Implements quadratic Bezier curve flattening and scan conversion
 * for rendering TrueType glyph outlines to bitmaps.
 */

#ifndef TRUETYPE_RASTER_H
#define TRUETYPE_RASTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "truetype.h"

// =============================================================================
// Fixed-Point Arithmetic (16.16 format)
// =============================================================================

typedef int32_t ttf_fixed_t;

#define TTF_FIXED_SHIFT     16
#define TTF_FIXED_ONE       (1 << TTF_FIXED_SHIFT)
#define TTF_FIXED_HALF      (1 << (TTF_FIXED_SHIFT - 1))
#define TTF_FIXED_MASK      (TTF_FIXED_ONE - 1)

// Conversion macros
#define TTF_INT_TO_FIXED(i)     ((ttf_fixed_t)(i) << TTF_FIXED_SHIFT)
#define TTF_FIXED_TO_INT(f)     (((f) + TTF_FIXED_HALF) >> TTF_FIXED_SHIFT)
#define TTF_FIXED_FLOOR(f)      ((f) >> TTF_FIXED_SHIFT)
#define TTF_FIXED_CEIL(f)       (((f) + TTF_FIXED_MASK) >> TTF_FIXED_SHIFT)
#define TTF_FIXED_FRAC(f)       ((f) & TTF_FIXED_MASK)

// Fixed-point multiplication: (a * b) >> 16
static inline ttf_fixed_t ttf_fixed_mul(ttf_fixed_t a, ttf_fixed_t b) {
    return (ttf_fixed_t)(((int64_t)a * b) >> TTF_FIXED_SHIFT);
}

// Fixed-point division: (a << 16) / b
static inline ttf_fixed_t ttf_fixed_div(ttf_fixed_t a, ttf_fixed_t b) {
    if (b == 0) return 0;
    return (ttf_fixed_t)(((int64_t)a << TTF_FIXED_SHIFT) / b);
}

// =============================================================================
// Point Structures
// =============================================================================

// Fixed-point 2D point
typedef struct {
    ttf_fixed_t x;
    ttf_fixed_t y;
} ttf_point_fixed_t;

// =============================================================================
// Edge Structure for Scan Conversion
// =============================================================================

typedef struct ttf_edge {
    int32_t y_top;              // Top scanline (minimum Y)
    int32_t y_bottom;           // Bottom scanline (maximum Y)
    ttf_fixed_t x;              // Current X coordinate (16.16 fixed)
    ttf_fixed_t dx;             // X increment per scanline (16.16 fixed)
    int8_t direction;           // +1 for downward, -1 for upward (winding)
    struct ttf_edge* next;      // Next edge in list
} ttf_edge_t;

// =============================================================================
// Rasterizer Context
// =============================================================================

// Edge pool for memory management
#define TTF_EDGE_POOL_SIZE  256

typedef struct {
    ttf_edge_t edges[TTF_EDGE_POOL_SIZE];
    uint32_t used;
} ttf_edge_pool_t;

// Rasterizer context
typedef struct {
    // Output bitmap
    uint8_t* bitmap;
    int32_t width;
    int32_t height;
    int32_t pitch;              // Bytes per row

    // Coordinate offsets (to handle negative coordinates)
    int32_t x_offset;
    int32_t y_offset;

    // Edge table (one linked list per scanline)
    ttf_edge_t** edge_table;    // Array of linked lists, indexed by Y

    // Edge memory pool
    ttf_edge_pool_t* edge_pools;
    uint32_t num_pools;
    uint32_t max_pools;

    // Current path state
    ttf_point_fixed_t path_start;
    ttf_point_fixed_t current_point;
    bool path_open;

} ttf_rasterizer_t;

// =============================================================================
// Rasterizer API
// =============================================================================

/**
 * Create a rasterizer context
 * @param width     Bitmap width in pixels
 * @param height    Bitmap height in pixels
 * @param x_offset  X offset to add to all coordinates
 * @param y_offset  Y offset to add to all coordinates
 * @return          Rasterizer context or NULL on error
 */
ttf_rasterizer_t* ttf_rasterizer_create(int32_t width, int32_t height,
                                         int32_t x_offset, int32_t y_offset);

/**
 * Destroy a rasterizer context
 */
void ttf_rasterizer_destroy(ttf_rasterizer_t* raster);

/**
 * Reset rasterizer for new path
 */
void ttf_rasterizer_reset(ttf_rasterizer_t* raster);

/**
 * Move to a new point (start a new contour)
 * @param raster    Rasterizer context
 * @param x         X coordinate (16.16 fixed point)
 * @param y         Y coordinate (16.16 fixed point)
 */
void ttf_rasterizer_move_to(ttf_rasterizer_t* raster, ttf_fixed_t x, ttf_fixed_t y);

/**
 * Draw a line to a point
 * @param raster    Rasterizer context
 * @param x         End X coordinate (16.16 fixed point)
 * @param y         End Y coordinate (16.16 fixed point)
 */
void ttf_rasterizer_line_to(ttf_rasterizer_t* raster, ttf_fixed_t x, ttf_fixed_t y);

/**
 * Draw a quadratic Bezier curve
 * @param raster    Rasterizer context
 * @param cx        Control point X (16.16 fixed point)
 * @param cy        Control point Y (16.16 fixed point)
 * @param x         End point X (16.16 fixed point)
 * @param y         End point Y (16.16 fixed point)
 */
void ttf_rasterizer_quad_to(ttf_rasterizer_t* raster,
                            ttf_fixed_t cx, ttf_fixed_t cy,
                            ttf_fixed_t x, ttf_fixed_t y);

/**
 * Close the current contour (draws line back to start)
 */
void ttf_rasterizer_close_path(ttf_rasterizer_t* raster);

/**
 * Fill the path using non-zero winding rule
 * Writes to the bitmap in the rasterizer context
 */
void ttf_rasterizer_fill(ttf_rasterizer_t* raster);

// =============================================================================
// High-Level Glyph Rasterization
// =============================================================================

/**
 * Rasterize a glyph outline to a bitmap
 *
 * @param outline       Glyph outline from ttf_get_glyph_outline()
 * @param ppem          Pixels per EM (target size)
 * @param units_per_em  Font units per EM
 * @param out_width     Output: bitmap width
 * @param out_height    Output: bitmap height
 * @param out_bearing_x Output: X bearing (offset from pen position)
 * @param out_bearing_y Output: Y bearing (offset from baseline to top)
 * @param antialias     True for antialiased rendering (4x4 supersampling)
 * @return              8-bit grayscale bitmap (caller must kfree), or NULL
 */
uint8_t* ttf_raster_glyph(
    ttf_glyph_outline_t* outline,
    uint32_t ppem,
    uint16_t units_per_em,
    uint8_t* out_width,
    uint8_t* out_height,
    int8_t* out_bearing_x,
    int8_t* out_bearing_y,
    bool antialias
);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Scale a coordinate from FUnits to fixed-point pixels
 */
static inline ttf_fixed_t ttf_scale_to_fixed(int32_t funits, uint32_t ppem, uint16_t upem) {
    if (upem == 0) return 0;
    return (ttf_fixed_t)(((int64_t)funits * ppem * TTF_FIXED_ONE + upem / 2) / upem);
}

/**
 * Scale a coordinate from FUnits to integer pixels
 */
static inline int32_t ttf_scale_to_int(int32_t funits, uint32_t ppem, uint16_t upem) {
    if (upem == 0) return 0;
    return (int32_t)(((int64_t)funits * ppem + upem / 2) / upem);
}

#endif // TRUETYPE_RASTER_H
