/**
 * LeafGFX Modern Effects Implementation
 *
 * Advanced visual effects for modern UI design.
 */

#include "leafgfx_modern.h"
#include "leafgfx.h"
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Predefined Shadow Styles
// =============================================================================

const gfx_shadow_params_t GFX_SHADOW_NONE = {0, 0, 0, 0, 0x00000000};
const gfx_shadow_params_t GFX_SHADOW_SUBTLE = {0, 1, 2, 0, 0x1A000000};
const gfx_shadow_params_t GFX_SHADOW_SMALL = {0, 2, 4, 0, 0x26000000};
const gfx_shadow_params_t GFX_SHADOW_MEDIUM = {0, 4, 8, 0, 0x33000000};
const gfx_shadow_params_t GFX_SHADOW_LARGE = {0, 8, 16, 0, 0x40000000};
const gfx_shadow_params_t GFX_SHADOW_ELEVATED = {0, 12, 24, 4, 0x4D000000};

// =============================================================================
// Helper Functions
// =============================================================================

static inline int32_t min3(int32_t a, int32_t b, int32_t c) {
    return (a < b) ? (a < c ? a : c) : (b < c ? b : c);
}

static inline int32_t max3(int32_t a, int32_t b, int32_t c) {
    return (a > b) ? (a > c ? a : c) : (b > c ? b : c);
}

static inline int32_t abs_val(int32_t x) {
    return x < 0 ? -x : x;
}

// Integer square root
static int32_t isqrt(int32_t n) {
    if (n < 2) return n;
    int32_t x = n, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// =============================================================================
// Color Utilities
// =============================================================================

uint32_t gfx_color_brightness(uint32_t color, int32_t amount) {
    int32_t a = (color >> 24) & 0xFF;
    int32_t r = (color >> 16) & 0xFF;
    int32_t g = (color >> 8) & 0xFF;
    int32_t b = color & 0xFF;

    r += amount; if (r < 0) r = 0; if (r > 255) r = 255;
    g += amount; if (g < 0) g = 0; if (g > 255) g = 255;
    b += amount; if (b < 0) b = 0; if (b > 255) b = 255;

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t gfx_color_saturate(uint32_t color, int32_t amount) {
    int32_t a = (color >> 24) & 0xFF;
    int32_t r = (color >> 16) & 0xFF;
    int32_t g = (color >> 8) & 0xFF;
    int32_t b = color & 0xFF;

    int32_t gray = (r + g + b) / 3;

    r = gray + ((r - gray) * (256 + amount)) / 256;
    g = gray + ((g - gray) * (256 + amount)) / 256;
    b = gray + ((b - gray) * (256 + amount)) / 256;

    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t gfx_color_opacity(uint32_t color, uint8_t opacity) {
    return (color & 0x00FFFFFF) | ((uint32_t)opacity << 24);
}

uint32_t gfx_color_mix(uint32_t c1, uint32_t c2, uint8_t ratio) {
    if (ratio == 0) return c1;
    if (ratio == 255) return c2;

    uint32_t inv = 255 - ratio;

    uint8_t a = (((c1 >> 24) & 0xFF) * inv + ((c2 >> 24) & 0xFF) * ratio) / 255;
    uint8_t r = (((c1 >> 16) & 0xFF) * inv + ((c2 >> 16) & 0xFF) * ratio) / 255;
    uint8_t g = (((c1 >> 8) & 0xFF) * inv + ((c2 >> 8) & 0xFF) * ratio) / 255;
    uint8_t b = ((c1 & 0xFF) * inv + (c2 & 0xFF) * ratio) / 255;

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// =============================================================================
// Blur Implementation
// =============================================================================

void gfx_blur_buffer(uint32_t* buffer, int32_t w, int32_t h, int32_t radius) {
    if (!buffer || w <= 0 || h <= 0 || radius <= 0) return;
    if (radius > 32) radius = 32;

    // Temporary buffer for horizontal pass
    uint32_t* temp = (uint32_t*)malloc(w * h * sizeof(uint32_t));
    if (!temp) return;

    int32_t div = radius * 2 + 1;

    // Horizontal pass
    for (int32_t y = 0; y < h; y++) {
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;

        // Initialize sum with first pixel repeated for edge handling
        uint32_t first = buffer[y * w];
        for (int32_t i = -radius; i <= radius; i++) {
            int32_t x = i < 0 ? 0 : (i >= w ? w - 1 : i);
            uint32_t c = buffer[y * w + x];
            sum_a += (c >> 24) & 0xFF;
            sum_r += (c >> 16) & 0xFF;
            sum_g += (c >> 8) & 0xFF;
            sum_b += c & 0xFF;
        }

        for (int32_t x = 0; x < w; x++) {
            temp[y * w + x] = ((sum_a / div) << 24) | ((sum_r / div) << 16) |
                              ((sum_g / div) << 8) | (sum_b / div);

            // Slide the window
            int32_t left_x = x - radius - 1;
            int32_t right_x = x + radius + 1;
            if (left_x < 0) left_x = 0;
            if (right_x >= w) right_x = w - 1;

            uint32_t left_c = buffer[y * w + left_x];
            uint32_t right_c = buffer[y * w + right_x];

            sum_a += ((right_c >> 24) & 0xFF) - ((left_c >> 24) & 0xFF);
            sum_r += ((right_c >> 16) & 0xFF) - ((left_c >> 16) & 0xFF);
            sum_g += ((right_c >> 8) & 0xFF) - ((left_c >> 8) & 0xFF);
            sum_b += (right_c & 0xFF) - (left_c & 0xFF);
        }
    }

    // Vertical pass
    for (int32_t x = 0; x < w; x++) {
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;

        // Initialize sum
        for (int32_t i = -radius; i <= radius; i++) {
            int32_t y = i < 0 ? 0 : (i >= h ? h - 1 : i);
            uint32_t c = temp[y * w + x];
            sum_a += (c >> 24) & 0xFF;
            sum_r += (c >> 16) & 0xFF;
            sum_g += (c >> 8) & 0xFF;
            sum_b += c & 0xFF;
        }

        for (int32_t y = 0; y < h; y++) {
            buffer[y * w + x] = ((sum_a / div) << 24) | ((sum_r / div) << 16) |
                                ((sum_g / div) << 8) | (sum_b / div);

            int32_t top_y = y - radius - 1;
            int32_t bot_y = y + radius + 1;
            if (top_y < 0) top_y = 0;
            if (bot_y >= h) bot_y = h - 1;

            uint32_t top_c = temp[top_y * w + x];
            uint32_t bot_c = temp[bot_y * w + x];

            sum_a += ((bot_c >> 24) & 0xFF) - ((top_c >> 24) & 0xFF);
            sum_r += ((bot_c >> 16) & 0xFF) - ((top_c >> 16) & 0xFF);
            sum_g += ((bot_c >> 8) & 0xFF) - ((top_c >> 8) & 0xFF);
            sum_b += (bot_c & 0xFF) - (top_c & 0xFF);
        }
    }

    free(temp);
}

void gfx_blur_box(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius) {
    if (w <= 0 || h <= 0 || radius <= 0) return;

    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) return;

    // Clamp to screen bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)fb->width) w = fb->width - x;
    if (y + h > (int32_t)fb->height) h = fb->height - y;
    if (w <= 0 || h <= 0) return;

    // Copy region to buffer - always use ARGB internally
    uint32_t* buffer = (uint32_t*)malloc(w * h * sizeof(uint32_t));
    if (!buffer) return;

    // Read through LeafGFX so backbuffer/frontbuffer selection stays coherent.
    for (int32_t py = 0; py < h; py++) {
        for (int32_t px = 0; px < w; px++) {
            buffer[py * w + px] = gfx_read_pixel(x + px, y + py);
        }
    }

    // Apply blur
    gfx_blur_buffer(buffer, w, h, radius);

    // Write through LeafGFX so active render target stays coherent.
    for (int32_t py = 0; py < h; py++) {
        for (int32_t px = 0; px < w; px++) {
            gfx_pixel(x + px, y + py, buffer[py * w + px]);
        }
    }

    free(buffer);
}

void gfx_blur_gaussian_fast(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius) {
    // Approximated Gaussian blur using 3 box blur passes
    // Box blur radius for approximation: r_box ≈ sqrt((12*σ²-1)/3)
    // For simplicity, use radius/3 three times

    int32_t pass_radius = (radius + 2) / 3;
    if (pass_radius < 1) pass_radius = 1;

    gfx_blur_box(x, y, w, h, pass_radius);
    gfx_blur_box(x, y, w, h, pass_radius);
    gfx_blur_box(x, y, w, h, pass_radius);
}

// =============================================================================
// Frosted Glass
// =============================================================================

void gfx_glass_panel(int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t corner_radius, int32_t blur_radius, uint32_t tint_color) {
    // Apply blur to the region
    gfx_blur_gaussian_fast(x, y, w, h, blur_radius);

    // Apply tint overlay
    gfx_fill_rounded_rect_aa(x, y, w, h, corner_radius, tint_color);
}

void gfx_glass_panel_fast(int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t corner_radius, uint32_t tint_color) {
    // Simulated frosted glass without actual blur
    // Sample and average background, then apply tint

    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) return;

    // Sample a few pixels to get average background color
    uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
    int32_t samples = 0;

    for (int32_t sy = y; sy < y + h; sy += 8) {
        for (int32_t sx = x; sx < x + w; sx += 8) {
            if (sx >= 0 && sx < (int32_t)fb->width && sy >= 0 && sy < (int32_t)fb->height) {
                uint32_t c = gfx_read_pixel(sx, sy);
                sum_r += (c >> 16) & 0xFF;
                sum_g += (c >> 8) & 0xFF;
                sum_b += c & 0xFF;
                samples++;
            }
        }
    }

    if (samples > 0) {
        uint8_t avg_r = sum_r / samples;
        uint8_t avg_g = sum_g / samples;
        uint8_t avg_b = sum_b / samples;

        // Blend average with tint
        uint8_t tint_a = (tint_color >> 24) & 0xFF;
        uint8_t tint_r = (tint_color >> 16) & 0xFF;
        uint8_t tint_g = (tint_color >> 8) & 0xFF;
        uint8_t tint_b = tint_color & 0xFF;

        uint8_t r = avg_r + ((tint_r - avg_r) * tint_a) / 255;
        uint8_t g = avg_g + ((tint_g - avg_g) * tint_a) / 255;
        uint8_t b = avg_b + ((tint_b - avg_b) * tint_a) / 255;

        uint32_t blend_color = 0xE0000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        gfx_fill_rounded_rect_aa(x, y, w, h, corner_radius, blend_color);
    } else {
        gfx_fill_rounded_rect_aa(x, y, w, h, corner_radius, tint_color);
    }
}

// =============================================================================
// Shadow Implementation
// =============================================================================

void gfx_draw_shadow(const gfx_rect_t* rect, int32_t corner_radius,
                     const gfx_shadow_params_t* shadow) {
    if (!rect || !shadow) return;
    if (shadow->color == 0) return;

    int32_t blur = shadow->blur_radius;
    int32_t spread = shadow->spread;

    // Shadow rectangle (expanded by spread, offset, and blur)
    gfx_rect_t shadow_rect = {
        .x = rect->x + shadow->offset_x - blur - spread,
        .y = rect->y + shadow->offset_y - blur - spread,
        .width = rect->width + 2 * (blur + spread),
        .height = rect->height + 2 * (blur + spread)
    };

    uint8_t base_a = (shadow->color >> 24) & 0xFF;
    uint32_t rgb = shadow->color & 0x00FFFFFF;

    // Draw multiple layers with decreasing opacity for soft shadow
    int32_t layers = blur > 0 ? blur / 2 + 1 : 1;
    if (layers > 8) layers = 8;

    for (int32_t layer = layers - 1; layer >= 0; layer--) {
        int32_t layer_expand = (layer * blur) / layers;
        int32_t layer_radius = corner_radius + layer_expand;

        uint8_t layer_alpha = base_a * (layers - layer) / (layers * 2);

        gfx_rect_t layer_rect = {
            .x = rect->x + shadow->offset_x - layer_expand - spread,
            .y = rect->y + shadow->offset_y - layer_expand - spread,
            .width = rect->width + 2 * (layer_expand + spread),
            .height = rect->height + 2 * (layer_expand + spread)
        };

        gfx_fill_rounded_rect_aa(layer_rect.x, layer_rect.y,
                                  layer_rect.width, layer_rect.height,
                                  layer_radius, (layer_alpha << 24) | rgb);
    }
}

void gfx_draw_shadow_soft(const gfx_rect_t* rect, int32_t corner_radius,
                          const gfx_shadow_params_t* shadow) {
    // Same as gfx_draw_shadow but with more layers for softer edges
    if (!rect || !shadow) return;
    if (shadow->color == 0) return;

    int32_t blur = shadow->blur_radius;
    int32_t spread = shadow->spread;

    uint8_t base_a = (shadow->color >> 24) & 0xFF;
    uint32_t rgb = shadow->color & 0x00FFFFFF;

    int32_t layers = blur > 0 ? blur : 1;
    if (layers > 16) layers = 16;

    for (int32_t layer = layers - 1; layer >= 0; layer--) {
        int32_t layer_expand = layer;
        int32_t layer_radius = corner_radius + layer_expand;

        // Gaussian-like falloff
        int32_t dist = layer;
        uint8_t layer_alpha = (uint8_t)((base_a * (layers - dist)) / (layers * 3));

        gfx_rect_t layer_rect = {
            .x = rect->x + shadow->offset_x - layer_expand - spread,
            .y = rect->y + shadow->offset_y - layer_expand - spread,
            .width = rect->width + 2 * (layer_expand + spread),
            .height = rect->height + 2 * (layer_expand + spread)
        };

        gfx_fill_rounded_rect_aa(layer_rect.x, layer_rect.y,
                                  layer_rect.width, layer_rect.height,
                                  layer_radius, (layer_alpha << 24) | rgb);
    }
}

void gfx_draw_shadow_inset(const gfx_rect_t* rect, int32_t corner_radius,
                           const gfx_shadow_params_t* shadow) {
    // Inset shadow - draw inside the rectangle
    if (!rect || !shadow) return;

    uint8_t base_a = (shadow->color >> 24) & 0xFF;
    uint32_t rgb = shadow->color & 0x00FFFFFF;

    int32_t blur = shadow->blur_radius;

    // Draw gradient from edges inward
    for (int32_t layer = 0; layer < blur; layer++) {
        uint8_t layer_alpha = (uint8_t)((base_a * (blur - layer)) / (blur * 2));

        gfx_rect_t inner = {
            .x = rect->x + layer + shadow->offset_x,
            .y = rect->y + layer + shadow->offset_y,
            .width = rect->width - 2 * layer,
            .height = rect->height - 2 * layer
        };

        if (inner.width > 0 && inner.height > 0) {
            int32_t r = corner_radius - layer;
            if (r < 0) r = 0;
            gfx_draw_rounded_rect_aa(inner.x, inner.y, inner.width, inner.height,
                                      r, 1, (layer_alpha << 24) | rgb);
        }
    }
}

// =============================================================================
// Advanced Gradients
// =============================================================================

void gfx_gradient_radial(int32_t cx, int32_t cy, int32_t radius,
                         uint32_t inner_color, uint32_t outer_color) {
    gfx_gradient_radial_ring(cx, cy, 0, radius, inner_color, outer_color);
}

void gfx_gradient_radial_ring(int32_t cx, int32_t cy,
                              int32_t inner_radius, int32_t outer_radius,
                              uint32_t inner_color, uint32_t outer_color) {
    if (outer_radius <= 0) return;
    if (inner_radius < 0) inner_radius = 0;
    if (inner_radius >= outer_radius) inner_radius = outer_radius - 1;

    int32_t range = outer_radius - inner_radius;
    if (range <= 0) range = 1;

    for (int32_t dy = -outer_radius; dy <= outer_radius; dy++) {
        for (int32_t dx = -outer_radius; dx <= outer_radius; dx++) {
            int32_t dist = isqrt(dx * dx + dy * dy);

            if (dist > outer_radius) continue;

            uint8_t t;
            if (dist <= inner_radius) {
                t = 0;
            } else {
                t = ((dist - inner_radius) * 255) / range;
            }

            uint32_t color = gfx_color_mix(inner_color, outer_color, t);
            gfx_pixel_blend(cx + dx, cy + dy, color);
        }
    }
}

void gfx_gradient_angular(int32_t cx, int32_t cy, int32_t radius,
                          int32_t start_angle, const uint32_t* colors, int32_t num_colors) {
    if (!colors || num_colors < 2 || radius <= 0) return;

    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dx = -radius; dx <= radius; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            if (dist_sq > radius * radius) continue;

            // Calculate angle (atan2 approximation)
            int32_t angle;
            if (dx == 0 && dy == 0) {
                angle = 0;
            } else {
                // Approximate angle in 0-65536 range
                int32_t abs_dx = dx < 0 ? -dx : dx;
                int32_t abs_dy = dy < 0 ? -dy : dy;

                int32_t ratio;
                if (abs_dx >= abs_dy) {
                    ratio = (abs_dy * 16384) / abs_dx;  // 0.25 of quadrant
                } else {
                    ratio = 16384 - (abs_dx * 16384) / abs_dy;
                }

                if (dx >= 0 && dy >= 0) angle = ratio;
                else if (dx < 0 && dy >= 0) angle = 32768 - ratio;
                else if (dx < 0 && dy < 0) angle = 32768 + ratio;
                else angle = 65536 - ratio;
            }

            // Apply start angle offset
            angle = (angle + start_angle) & 0xFFFF;

            // Map to color
            int32_t segment = (angle * (num_colors - 1)) / 65536;
            int32_t seg_t = ((angle * (num_colors - 1)) % 65536) * 255 / 65536;

            if (segment >= num_colors - 1) {
                segment = num_colors - 2;
                seg_t = 255;
            }

            uint32_t color = gfx_color_mix(colors[segment], colors[segment + 1], seg_t);
            gfx_pixel_blend(cx + dx, cy + dy, color);
        }
    }
}

void gfx_gradient_mesh(const gfx_rect_t* rect,
                       uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br) {
    if (!rect || rect->width <= 0 || rect->height <= 0) return;

    for (uint32_t py = 0; py < rect->height; py++) {
        // Vertical interpolation factor
        uint8_t ty = (py * 255) / (rect->height > 1 ? rect->height - 1 : 1);

        // Interpolate left edge
        uint32_t left = gfx_color_mix(tl, bl, ty);
        // Interpolate right edge
        uint32_t right = gfx_color_mix(tr, br, ty);

        for (uint32_t px = 0; px < rect->width; px++) {
            // Horizontal interpolation
            uint8_t tx = (px * 255) / (rect->width > 1 ? rect->width - 1 : 1);
            uint32_t color = gfx_color_mix(left, right, tx);

            gfx_pixel_blend(rect->x + px, rect->y + py, color);
        }
    }
}

void gfx_gradient_linear_angle(const gfx_rect_t* rect, int32_t angle_fp,
                               uint32_t start, uint32_t end) {
    if (!rect || rect->width <= 0 || rect->height <= 0) return;

    // Simplified: for now, map angle to vertical/horizontal/diagonal
    // Full implementation would use sine/cosine projection

    int32_t norm_angle = angle_fp & 0xFFFF;

    for (uint32_t py = 0; py < rect->height; py++) {
        for (uint32_t px = 0; px < rect->width; px++) {
            int32_t t;

            if (norm_angle < 16384 || norm_angle >= 49152) {
                // Primarily horizontal
                t = (px * 255) / (rect->width > 1 ? rect->width - 1 : 1);
            } else if (norm_angle < 32768) {
                // Primarily vertical (top to bottom)
                t = (py * 255) / (rect->height > 1 ? rect->height - 1 : 1);
            } else {
                // Diagonal
                t = ((px + py) * 255) / ((rect->width + rect->height - 2) > 0 ?
                    rect->width + rect->height - 2 : 1);
            }

            uint32_t color = gfx_color_mix(start, end, t);
            gfx_pixel_blend(rect->x + px, rect->y + py, color);
        }
    }
}

void gfx_gradient_multi(const gfx_rect_t* rect, bool vertical,
                        const uint32_t* colors, const int32_t* stops, int32_t num_stops) {
    if (!rect || !colors || !stops || num_stops < 2) return;

    uint32_t size = vertical ? rect->height : rect->width;
    if (size == 0) return;

    for (uint32_t i = 0; i < size; i++) {
        int32_t pos = (i * 65536) / size;

        // Find the two stops this position is between
        int32_t seg = 0;
        for (int32_t s = 0; s < num_stops - 1; s++) {
            if (pos >= stops[s] && pos <= stops[s + 1]) {
                seg = s;
                break;
            }
        }

        int32_t seg_range = stops[seg + 1] - stops[seg];
        int32_t t = seg_range > 0 ? ((pos - stops[seg]) * 255) / seg_range : 0;

        uint32_t color = gfx_color_mix(colors[seg], colors[seg + 1], t);

        if (vertical) {
            gfx_hline(rect->x, rect->y + i, rect->width, color);
        } else {
            gfx_vline(rect->x + i, rect->y, rect->height, color);
        }
    }
}

// =============================================================================
// Surface Effects
// =============================================================================

void gfx_surface_raised(const gfx_rect_t* rect, int32_t corner_radius,
                        uint32_t base_color, int32_t depth) {
    if (!rect) return;

    // Top highlight
    uint32_t highlight = gfx_color_brightness(base_color, depth);
    // Bottom shadow
    uint32_t shadow = gfx_color_brightness(base_color, -depth);

    // Draw with gradient from highlight to shadow
    gfx_gradient_vertical(rect->x, rect->y, rect->width, rect->height,
                          highlight, shadow);

    // Draw rounded rect border if needed
    if (corner_radius > 0) {
        gfx_fill_rounded_rect_aa(rect->x, rect->y, rect->width, rect->height,
                                  corner_radius, gfx_color_opacity(base_color, 0));
    }
}

void gfx_surface_pressed(const gfx_rect_t* rect, int32_t corner_radius,
                         uint32_t base_color, int32_t depth) {
    if (!rect) return;

    // Inverse of raised - shadow on top, highlight on bottom
    uint32_t shadow = gfx_color_brightness(base_color, -depth);
    uint32_t highlight = gfx_color_brightness(base_color, depth / 2);

    gfx_gradient_vertical(rect->x, rect->y, rect->width, rect->height,
                          shadow, highlight);
}

void gfx_surface_card(const gfx_rect_t* rect, int32_t corner_radius,
                      uint32_t bg_color, bool elevated) {
    if (!rect) return;

    // Draw shadow if elevated
    if (elevated) {
        gfx_draw_shadow(rect, corner_radius, &GFX_SHADOW_MEDIUM);
    }

    // Draw card background
    gfx_fill_rounded_rect_aa(rect->x, rect->y, rect->width, rect->height,
                              corner_radius, bg_color);
}

// =============================================================================
// Glow Effects
// =============================================================================

void gfx_glow_rect(const gfx_rect_t* rect, int32_t corner_radius,
                   uint32_t glow_color, int32_t intensity) {
    if (!rect || intensity <= 0) return;

    uint8_t base_a = gfx_alpha_from_color(glow_color);
    if (base_a == 0) return;
    uint32_t rgb = glow_color & 0x00FFFFFF;

    for (int32_t layer = intensity; layer > 0; layer--) {
        uint8_t layer_alpha = (base_a * layer) / (intensity * 3);

        gfx_rect_t glow_rect = {
            .x = rect->x - layer,
            .y = rect->y - layer,
            .width = rect->width + 2 * layer,
            .height = rect->height + 2 * layer
        };

        gfx_fill_rounded_rect_aa(glow_rect.x, glow_rect.y,
                                  glow_rect.width, glow_rect.height,
                                  corner_radius + layer,
                                  (layer_alpha << 24) | rgb);
    }
}

void gfx_glow_circle(int32_t cx, int32_t cy, int32_t radius,
                     uint32_t glow_color, int32_t intensity) {
    if (intensity <= 0 || radius <= 0) return;

    uint8_t base_a = gfx_alpha_from_color(glow_color);
    if (base_a == 0) return;
    uint32_t rgb = glow_color & 0x00FFFFFF;

    for (int32_t layer = intensity; layer > 0; layer--) {
        uint8_t layer_alpha = (base_a * layer) / (intensity * 3);
        gfx_fill_circle_sdf(cx, cy, radius + layer, (layer_alpha << 24) | rgb);
    }
}
