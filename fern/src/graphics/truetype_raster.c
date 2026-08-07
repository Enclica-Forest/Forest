/**
 * TrueType Glyph Rasterizer for Forest-OS
 *
 * Implements quadratic Bezier curve flattening and scan-line conversion
 * for rendering TrueType glyph outlines to bitmaps.
 */

#include "../include/graphics/truetype_raster.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/mm.h"

// =============================================================================
// Configuration
// =============================================================================

// Flatness threshold for Bezier subdivision (in 1/64 of a pixel)
#define TTF_FLATNESS_THRESHOLD  (TTF_FIXED_ONE / 16)

// Maximum recursion depth for Bezier subdivision
#define TTF_MAX_BEZIER_DEPTH    16

// Supersampling factors for antialiasing (lower for crisper small text)
#define TTF_AA_SAMPLES_X        2
#define TTF_AA_SAMPLES_Y        2

// Maximum dimensions for a single glyph
#define TTF_MAX_GLYPH_SIZE      512

// =============================================================================
// Edge Pool Management
// =============================================================================

static ttf_edge_t* ttf_alloc_edge(ttf_rasterizer_t* raster) {
    // Check current pool
    if (raster->num_pools > 0) {
        ttf_edge_pool_t* pool = &raster->edge_pools[raster->num_pools - 1];
        if (pool->used < TTF_EDGE_POOL_SIZE) {
            return &pool->edges[pool->used++];
        }
    }

    // Need a new pool
    if (raster->num_pools >= raster->max_pools) {
        // Expand pools array
        uint32_t new_max = raster->max_pools == 0 ? 4 : raster->max_pools * 2;
        ttf_edge_pool_t* new_pools = kmalloc(new_max * sizeof(ttf_edge_pool_t));
        if (!new_pools) {
            return NULL;
        }

        if (raster->edge_pools) {
            memcpy(new_pools, raster->edge_pools, raster->num_pools * sizeof(ttf_edge_pool_t));
            kfree(raster->edge_pools);
        }

        raster->edge_pools = new_pools;
        raster->max_pools = new_max;
    }

    // Initialize new pool
    ttf_edge_pool_t* pool = &raster->edge_pools[raster->num_pools++];
    pool->used = 0;

    return &pool->edges[pool->used++];
}

// =============================================================================
// Rasterizer Creation/Destruction
// =============================================================================

ttf_rasterizer_t* ttf_rasterizer_create(int32_t width, int32_t height,
                                         int32_t x_offset, int32_t y_offset) {
    if (width <= 0 || height <= 0 || width > TTF_MAX_GLYPH_SIZE || height > TTF_MAX_GLYPH_SIZE) {
        return NULL;
    }

    ttf_rasterizer_t* raster = kmalloc(sizeof(ttf_rasterizer_t));
    if (!raster) {
        return NULL;
    }
    memset(raster, 0, sizeof(ttf_rasterizer_t));

    raster->width = width;
    raster->height = height;
    raster->pitch = width;
    raster->x_offset = x_offset;
    raster->y_offset = y_offset;

    // Allocate bitmap
    raster->bitmap = kmalloc(width * height);
    if (!raster->bitmap) {
        kfree(raster);
        return NULL;
    }
    memset(raster->bitmap, 0, width * height);

    // Allocate edge table (one list per scanline)
    raster->edge_table = kmalloc(height * sizeof(ttf_edge_t*));
    if (!raster->edge_table) {
        kfree(raster->bitmap);
        kfree(raster);
        return NULL;
    }
    memset(raster->edge_table, 0, height * sizeof(ttf_edge_t*));

    return raster;
}

void ttf_rasterizer_destroy(ttf_rasterizer_t* raster) {
    if (!raster) return;

    if (raster->bitmap) {
        kfree(raster->bitmap);
    }

    if (raster->edge_table) {
        kfree(raster->edge_table);
    }

    if (raster->edge_pools) {
        kfree(raster->edge_pools);
    }

    kfree(raster);
}

void ttf_rasterizer_reset(ttf_rasterizer_t* raster) {
    if (!raster) return;

    // Clear bitmap
    memset(raster->bitmap, 0, raster->width * raster->height);

    // Clear edge table
    memset(raster->edge_table, 0, raster->height * sizeof(ttf_edge_t*));

    // Reset edge pools
    for (uint32_t i = 0; i < raster->num_pools; i++) {
        raster->edge_pools[i].used = 0;
    }
    raster->num_pools = 0;

    // Reset path state
    raster->path_open = false;
}

// =============================================================================
// Edge Addition
// =============================================================================

static void ttf_add_edge(ttf_rasterizer_t* raster,
                         ttf_fixed_t x0, ttf_fixed_t y0,
                         ttf_fixed_t x1, ttf_fixed_t y1) {
    // Convert to pixel space with offset
    int32_t py0 = TTF_FIXED_FLOOR(y0) + raster->y_offset;
    int32_t py1 = TTF_FIXED_FLOOR(y1) + raster->y_offset;

    // Skip horizontal edges
    if (py0 == py1) {
        return;
    }

    // Determine direction and ensure top-to-bottom
    int8_t direction;
    if (py0 > py1) {
        // Swap so y0 < y1 (top to bottom)
        ttf_fixed_t tmp;
        tmp = x0; x0 = x1; x1 = tmp;
        tmp = y0; y0 = y1; y1 = tmp;
        int32_t itmp;
        itmp = py0; py0 = py1; py1 = itmp;
        direction = -1;  // Upward edge
    } else {
        direction = 1;   // Downward edge
    }

    // Clip to bitmap bounds
    if (py1 <= 0 || py0 >= raster->height) {
        return;
    }

    int32_t y_top = py0 < 0 ? 0 : py0;
    int32_t y_bottom = py1 > raster->height ? raster->height : py1;

    if (y_top >= y_bottom) {
        return;
    }

    // Calculate edge slope
    ttf_fixed_t dy = y1 - y0;
    ttf_fixed_t dx = x1 - x0;
    ttf_fixed_t dx_per_scanline = 0;

    if (dy != 0) {
        dx_per_scanline = ttf_fixed_div(dx, dy);
    }

    // Calculate starting X for the top scanline
    // We need the X at the center of the first scanline
    ttf_fixed_t start_y = TTF_INT_TO_FIXED(y_top - raster->y_offset) + TTF_FIXED_HALF;
    ttf_fixed_t x_at_start = x0 + ttf_fixed_mul(dx_per_scanline, start_y - y0);

    // Allocate edge
    ttf_edge_t* edge = ttf_alloc_edge(raster);
    if (!edge) {
        return;
    }

    edge->y_top = y_top;
    edge->y_bottom = y_bottom;
    edge->x = x_at_start + TTF_INT_TO_FIXED(raster->x_offset);
    edge->dx = dx_per_scanline;
    edge->direction = direction;

    // Insert into edge table at y_top
    edge->next = raster->edge_table[y_top];
    raster->edge_table[y_top] = edge;
}

// =============================================================================
// Path Operations
// =============================================================================

void ttf_rasterizer_move_to(ttf_rasterizer_t* raster, ttf_fixed_t x, ttf_fixed_t y) {
    if (!raster) return;

    // Close any open path
    if (raster->path_open) {
        ttf_rasterizer_close_path(raster);
    }

    raster->path_start.x = x;
    raster->path_start.y = y;
    raster->current_point.x = x;
    raster->current_point.y = y;
    raster->path_open = true;
}

void ttf_rasterizer_line_to(ttf_rasterizer_t* raster, ttf_fixed_t x, ttf_fixed_t y) {
    if (!raster || !raster->path_open) return;

    ttf_add_edge(raster,
                 raster->current_point.x, raster->current_point.y,
                 x, y);

    raster->current_point.x = x;
    raster->current_point.y = y;
}

// Recursive Bezier flattening
static void ttf_flatten_bezier_recursive(
    ttf_rasterizer_t* raster,
    ttf_fixed_t x0, ttf_fixed_t y0,    // Start point
    ttf_fixed_t x1, ttf_fixed_t y1,    // Control point
    ttf_fixed_t x2, ttf_fixed_t y2,    // End point
    int depth
) {
    // Safety check for recursion depth
    if (depth > TTF_MAX_BEZIER_DEPTH) {
        ttf_add_edge(raster, x0, y0, x2, y2);
        return;
    }

    // Calculate distance from control point to line p0-p2
    // Using the formula: d = |cross(p2-p0, p1-p0)| / |p2-p0|
    // Simplified flatness check: if midpoint is close enough to the line

    // Midpoint of p0-p2
    ttf_fixed_t mx = (x0 + x2) / 2;
    ttf_fixed_t my = (y0 + y2) / 2;

    // Vector from midpoint to control point
    ttf_fixed_t dx = x1 - mx;
    ttf_fixed_t dy = y1 - my;

    // Distance squared (avoid square root)
    int64_t dist_sq = (int64_t)dx * dx + (int64_t)dy * dy;
    int64_t threshold_sq = (int64_t)TTF_FLATNESS_THRESHOLD * TTF_FLATNESS_THRESHOLD;

    if (dist_sq <= threshold_sq) {
        // Flat enough - emit line segment
        ttf_add_edge(raster, x0, y0, x2, y2);
        return;
    }

    // Subdivide using de Casteljau's algorithm
    ttf_fixed_t x01 = (x0 + x1) / 2;
    ttf_fixed_t y01 = (y0 + y1) / 2;
    ttf_fixed_t x12 = (x1 + x2) / 2;
    ttf_fixed_t y12 = (y1 + y2) / 2;
    ttf_fixed_t x012 = (x01 + x12) / 2;
    ttf_fixed_t y012 = (y01 + y12) / 2;

    // Recurse on both halves
    ttf_flatten_bezier_recursive(raster, x0, y0, x01, y01, x012, y012, depth + 1);
    ttf_flatten_bezier_recursive(raster, x012, y012, x12, y12, x2, y2, depth + 1);
}

void ttf_rasterizer_quad_to(ttf_rasterizer_t* raster,
                            ttf_fixed_t cx, ttf_fixed_t cy,
                            ttf_fixed_t x, ttf_fixed_t y) {
    if (!raster || !raster->path_open) return;

    ttf_flatten_bezier_recursive(
        raster,
        raster->current_point.x, raster->current_point.y,
        cx, cy,
        x, y,
        0
    );

    raster->current_point.x = x;
    raster->current_point.y = y;
}

void ttf_rasterizer_close_path(ttf_rasterizer_t* raster) {
    if (!raster || !raster->path_open) return;

    // Draw line back to start
    ttf_add_edge(raster,
                 raster->current_point.x, raster->current_point.y,
                 raster->path_start.x, raster->path_start.y);

    raster->path_open = false;
}

// =============================================================================
// Scan Conversion (Non-Zero Winding Rule)
// =============================================================================

// Active edge list node
typedef struct ttf_ael_node {
    ttf_edge_t* edge;
    struct ttf_ael_node* next;
} ttf_ael_node_t;

// Insert edge into AEL sorted by X
static void ttf_ael_insert(ttf_ael_node_t** ael, ttf_edge_t* edge) {
    ttf_ael_node_t* node = kmalloc(sizeof(ttf_ael_node_t));
    if (!node) return;

    node->edge = edge;

    // Find insertion point
    ttf_ael_node_t** pp = ael;
    while (*pp && (*pp)->edge->x < edge->x) {
        pp = &(*pp)->next;
    }

    node->next = *pp;
    *pp = node;
}

// Remove edge from AEL
__attribute__((unused)) static void ttf_ael_remove(ttf_ael_node_t** ael, ttf_edge_t* edge) {
    ttf_ael_node_t** pp = ael;
    while (*pp) {
        if ((*pp)->edge == edge) {
            ttf_ael_node_t* to_free = *pp;
            *pp = (*pp)->next;
            kfree(to_free);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Re-sort AEL after X updates (bubble sort - efficient for nearly sorted)
static void ttf_ael_sort(ttf_ael_node_t** ael) {
    if (!*ael) return;

    bool swapped;
    do {
        swapped = false;
        ttf_ael_node_t** pp = ael;
        while (*pp && (*pp)->next) {
            if ((*pp)->edge->x > (*pp)->next->edge->x) {
                // Swap
                ttf_ael_node_t* a = *pp;
                ttf_ael_node_t* b = a->next;
                a->next = b->next;
                b->next = a;
                *pp = b;
                swapped = true;
            }
            pp = &(*pp)->next;
        }
    } while (swapped);
}

// Free AEL
static void ttf_ael_free(ttf_ael_node_t* ael) {
    while (ael) {
        ttf_ael_node_t* next = ael->next;
        kfree(ael);
        ael = next;
    }
}

void ttf_rasterizer_fill(ttf_rasterizer_t* raster) {
    if (!raster) return;

    ttf_ael_node_t* ael = NULL;

    for (int32_t y = 0; y < raster->height; y++) {
        // Add new edges starting at this scanline
        for (ttf_edge_t* edge = raster->edge_table[y]; edge; edge = edge->next) {
            ttf_ael_insert(&ael, edge);
        }

        // Sort AEL by X
        ttf_ael_sort(&ael);

        // Fill spans using non-zero winding rule
        int32_t winding = 0;
        ttf_fixed_t x_start = 0;

        for (ttf_ael_node_t* node = ael; node; node = node->next) {
            ttf_edge_t* edge = node->edge;

            int32_t old_winding = winding;
            winding += edge->direction;

            if (old_winding == 0 && winding != 0) {
                // Start of filled span
                x_start = edge->x;
            } else if (old_winding != 0 && winding == 0) {
                // End of filled span
                int32_t x0 = TTF_FIXED_FLOOR(x_start);
                int32_t x1 = TTF_FIXED_CEIL(edge->x);

                // Clip to bitmap bounds
                if (x0 < 0) x0 = 0;
                if (x1 > raster->width) x1 = raster->width;

                // Fill pixels
                for (int32_t x = x0; x < x1; x++) {
                    raster->bitmap[y * raster->pitch + x] = 255;
                }
            }
        }

        // Update X coordinates and remove finished edges
        ttf_ael_node_t** pp = &ael;
        while (*pp) {
            ttf_edge_t* edge = (*pp)->edge;

            // Check if edge ends at next scanline
            if (y + 1 >= edge->y_bottom) {
                // Remove edge
                ttf_ael_node_t* to_free = *pp;
                *pp = (*pp)->next;
                kfree(to_free);
            } else {
                // Update X
                edge->x += edge->dx;
                pp = &(*pp)->next;
            }
        }
    }

    ttf_ael_free(ael);
}

// =============================================================================
// High-Level Glyph Rasterization
// =============================================================================

// Process a single contour
static void ttf_process_contour(
    ttf_rasterizer_t* raster,
    ttf_glyph_contour_t* contour,
    uint32_t ppem,
    uint16_t upem,
    int32_t scale_factor  // Additional scale for supersampling
) {
    if (!contour || contour->num_points == 0) return;

    uint16_t n = contour->num_points;
    ttf_glyph_point_t* points = contour->points;

    // Find first on-curve point
    uint16_t first_on = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (points[i].on_curve) {
            first_on = i;
            break;
        }
    }

    // Handle case where there's no on-curve point (implied point)
    ttf_fixed_t start_x, start_y;
    uint16_t start_idx;

    if (points[first_on].on_curve) {
        start_x = ttf_scale_to_fixed(points[first_on].x, ppem * scale_factor, upem);
        start_y = ttf_scale_to_fixed(-points[first_on].y, ppem * scale_factor, upem);  // Flip Y
        start_idx = first_on;
    } else {
        // All points are off-curve, start with midpoint between first and last
        int32_t mx = (points[0].x + points[n-1].x) / 2;
        int32_t my = -(points[0].y + points[n-1].y) / 2;  // Flip Y
        start_x = ttf_scale_to_fixed(mx, ppem * scale_factor, upem);
        start_y = ttf_scale_to_fixed(my, ppem * scale_factor, upem);
        start_idx = 0;
    }

    ttf_rasterizer_move_to(raster, start_x, start_y);

    // Process remaining points
    uint16_t i = (start_idx + 1) % n;

    while (true) {
        ttf_fixed_t px = ttf_scale_to_fixed(points[i].x, ppem * scale_factor, upem);
        ttf_fixed_t py = ttf_scale_to_fixed(-points[i].y, ppem * scale_factor, upem);  // Flip Y

        if (points[i].on_curve) {
            // Line to on-curve point
            ttf_rasterizer_line_to(raster, px, py);
        } else {
            // Off-curve point - need to find the next point
            uint16_t next_i = (i + 1) % n;
            ttf_fixed_t nx, ny;
            bool next_on_curve;

            if (next_i == start_idx) {
                // Wrap around to start
                nx = start_x;
                ny = start_y;
                next_on_curve = true;
            } else {
                nx = ttf_scale_to_fixed(points[next_i].x, ppem * scale_factor, upem);
                ny = ttf_scale_to_fixed(-points[next_i].y, ppem * scale_factor, upem);  // Flip Y
                next_on_curve = points[next_i].on_curve;
            }

            if (next_on_curve) {
                // Quadratic curve: prev -> (px,py) -> next
                ttf_rasterizer_quad_to(raster, px, py, nx, ny);
                i = next_i;
            } else {
                // Two consecutive off-curve points - implied on-curve midpoint
                ttf_fixed_t mx = (px + nx) / 2;
                ttf_fixed_t my = (py + ny) / 2;
                ttf_rasterizer_quad_to(raster, px, py, mx, my);
            }
        }

        if (i == start_idx) break;
        i = (i + 1) % n;
    }

    ttf_rasterizer_close_path(raster);
}

uint8_t* ttf_raster_glyph(
    ttf_glyph_outline_t* outline,
    uint32_t ppem,
    uint16_t units_per_em,
    uint8_t* out_width,
    uint8_t* out_height,
    int8_t* out_bearing_x,
    int8_t* out_bearing_y,
    bool antialias
) {
    if (!outline || !out_width || !out_height || !out_bearing_x || !out_bearing_y) {
        return NULL;
    }

    // Empty glyph
    if (outline->num_contours == 0) {
        *out_width = 0;
        *out_height = 0;
        *out_bearing_x = 0;
        *out_bearing_y = 0;
        return NULL;
    }

    // Calculate scaled bounding box
    int32_t x_min = ttf_scale_to_int(outline->x_min, ppem, units_per_em);
    int32_t y_min = ttf_scale_to_int(outline->y_min, ppem, units_per_em);
    int32_t x_max = ttf_scale_to_int(outline->x_max, ppem, units_per_em);
    int32_t y_max = ttf_scale_to_int(outline->y_max, ppem, units_per_em);
    int32_t bearing_x_px = ttf_scale_to_int(outline->left_side_bearing, ppem, units_per_em);
    int32_t bearing_y_px = ttf_scale_to_int(outline->y_max, ppem, units_per_em);

    // Flip Y coordinates for correct orientation (TrueType Y increases up, raster Y increases down)
    int32_t flipped_y_min = -y_max;
    int32_t flipped_y_max = -y_min;
    y_min = flipped_y_min;
    y_max = flipped_y_max;

    // Minimal padding only when antialiasing to avoid clipping without shifting bearings
    int32_t pad = antialias ? 1 : 0;

    int32_t width = (x_max - x_min) + pad * 2;
    int32_t height = (y_max - y_min) + pad * 2;

    // Sanity checks
    if (width <= 0 || height <= 0) {
        *out_width = 0;
        *out_height = 0;
        *out_bearing_x = 0;
        *out_bearing_y = 0;
        return NULL;
    }

    if (width > 255) width = 255;
    if (height > 255) height = 255;

    int32_t scale_factor = antialias ? TTF_AA_SAMPLES_X : 1;
    int32_t ss_width = width * scale_factor;
    int32_t ss_height = height * scale_factor;

    // Create rasterizer
    ttf_rasterizer_t* raster = ttf_rasterizer_create(
        ss_width, ss_height,
        (-x_min + pad) * scale_factor,
        (-y_min + pad) * scale_factor
    );
    if (!raster) {
        *out_width = 0;
        *out_height = 0;
        *out_bearing_x = 0;
        *out_bearing_y = 0;
        return NULL;
    }

    // Process all contours
    for (uint16_t c = 0; c < outline->num_contours; c++) {
        ttf_process_contour(raster, &outline->contours[c], ppem, units_per_em, scale_factor);
    }

    // Fill the path
    ttf_rasterizer_fill(raster);

    // Allocate final bitmap
    uint8_t* bitmap = kmalloc(width * height);
    if (!bitmap) {
        ttf_rasterizer_destroy(raster);
        *out_width = 0;
        *out_height = 0;
        *out_bearing_x = 0;
        *out_bearing_y = 0;
        return NULL;
    }

    if (antialias) {
        // Downsample with box filter
        for (int32_t y = 0; y < height; y++) {
            for (int32_t x = 0; x < width; x++) {
                uint32_t sum = 0;
                for (int32_t sy = 0; sy < TTF_AA_SAMPLES_Y; sy++) {
                    for (int32_t sx = 0; sx < TTF_AA_SAMPLES_X; sx++) {
                        int32_t ss_x = x * TTF_AA_SAMPLES_X + sx;
                        int32_t ss_y = y * TTF_AA_SAMPLES_Y + sy;
                        sum += raster->bitmap[ss_y * raster->pitch + ss_x];
                    }
                }
                bitmap[y * width + x] = (uint8_t)(sum / (TTF_AA_SAMPLES_X * TTF_AA_SAMPLES_Y));
            }
        }
    } else {
        // Direct copy
        memcpy(bitmap, raster->bitmap, width * height);
    }

    ttf_rasterizer_destroy(raster);

    *out_width = (uint8_t)width;
    *out_height = (uint8_t)height;
    int32_t bearing_x_out = bearing_x_px - pad; // compensate for padding offset
    int32_t bearing_y_out = bearing_y_px + pad; // keep baseline-to-top distance correct

    if (bearing_x_out < -128) bearing_x_out = -128;
    if (bearing_x_out > 127) bearing_x_out = 127;
    if (bearing_y_out < -128) bearing_y_out = -128;
    if (bearing_y_out > 127) bearing_y_out = 127;
    *out_bearing_x = (int8_t)bearing_x_out;
    *out_bearing_y = (int8_t)bearing_y_out;  // Distance from baseline to top

    return bitmap;
}
