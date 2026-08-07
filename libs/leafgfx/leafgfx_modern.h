/**
 * LeafGFX Modern Effects
 *
 * Advanced visual effects for modern UI: blur, shadows, gradients,
 * and frosted glass effects inspired by GNOME/KDE/macOS.
 */

#ifndef LEAFGFX_MODERN_H
#define LEAFGFX_MODERN_H

#include "leafgfx.h"

// =============================================================================
// Blur Effects
// =============================================================================

/**
 * Apply box blur to a region of the framebuffer
 * @param x, y, w, h  Region to blur
 * @param radius      Blur radius (1-32 recommended)
 */
void gfx_blur_box(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius);

/**
 * Apply fast approximated Gaussian blur (3-pass box blur)
 * More visually pleasing than single box blur
 */
void gfx_blur_gaussian_fast(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius);

/**
 * Apply blur to a buffer (not the framebuffer)
 */
void gfx_blur_buffer(uint32_t* buffer, int32_t w, int32_t h, int32_t radius);

// =============================================================================
// Frosted Glass Effect (macOS-style)
// =============================================================================

/**
 * Draw a frosted glass panel with blur and tint
 * @param x, y, w, h  Panel bounds
 * @param corner_radius  Corner radius for rounded rectangle
 * @param blur_radius    Blur amount (8-16 typical)
 * @param tint_color     Color to overlay (use alpha for intensity)
 */
void gfx_glass_panel(int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t corner_radius, int32_t blur_radius, uint32_t tint_color);

/**
 * Lightweight frosted effect (simulated, faster)
 * Uses sampling and tinting instead of true blur
 */
void gfx_glass_panel_fast(int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t corner_radius, uint32_t tint_color);

// =============================================================================
// Shadow Effects
// =============================================================================

/**
 * Shadow parameters for modern UI
 */
typedef struct {
    int8_t   offset_x;      // Horizontal offset
    int8_t   offset_y;      // Vertical offset (positive = down)
    uint8_t  blur_radius;   // Blur amount
    uint8_t  spread;        // Spread (expand/contract shadow)
    uint32_t color;         // Shadow color with alpha
} gfx_shadow_params_t;

// Predefined shadow styles
extern const gfx_shadow_params_t GFX_SHADOW_NONE;
extern const gfx_shadow_params_t GFX_SHADOW_SUBTLE;    // Minimal elevation
extern const gfx_shadow_params_t GFX_SHADOW_SMALL;     // Small card/button
extern const gfx_shadow_params_t GFX_SHADOW_MEDIUM;    // Standard window
extern const gfx_shadow_params_t GFX_SHADOW_LARGE;     // Elevated modal
extern const gfx_shadow_params_t GFX_SHADOW_ELEVATED;  // Popup/dropdown

/**
 * Draw a shadow for a rectangle
 * @param rect          Rectangle to shadow
 * @param corner_radius Corner radius (0 for square)
 * @param shadow        Shadow parameters
 */
void gfx_draw_shadow(const gfx_rect_t* rect, int32_t corner_radius,
                     const gfx_shadow_params_t* shadow);

/**
 * Draw a shadow for a rounded rectangle (multi-layer for soft edges)
 */
void gfx_draw_shadow_soft(const gfx_rect_t* rect, int32_t corner_radius,
                          const gfx_shadow_params_t* shadow);

/**
 * Draw inset shadow (pressed/sunken effect)
 */
void gfx_draw_shadow_inset(const gfx_rect_t* rect, int32_t corner_radius,
                           const gfx_shadow_params_t* shadow);

// =============================================================================
// Advanced Gradients
// =============================================================================

/**
 * Radial gradient (center to edge)
 * @param cx, cy        Center point
 * @param radius        Outer radius
 * @param inner_color   Color at center
 * @param outer_color   Color at edge
 */
void gfx_gradient_radial(int32_t cx, int32_t cy, int32_t radius,
                         uint32_t inner_color, uint32_t outer_color);

/**
 * Radial gradient with custom inner radius (ring gradient)
 */
void gfx_gradient_radial_ring(int32_t cx, int32_t cy,
                              int32_t inner_radius, int32_t outer_radius,
                              uint32_t inner_color, uint32_t outer_color);

/**
 * Angular/conic gradient
 * @param cx, cy        Center point
 * @param radius        Outer radius
 * @param start_angle   Starting angle (0 = right, counter-clockwise, 0-65536 for 0-360)
 * @param colors        Array of colors
 * @param num_colors    Number of colors (evenly distributed)
 */
void gfx_gradient_angular(int32_t cx, int32_t cy, int32_t radius,
                          int32_t start_angle, const uint32_t* colors, int32_t num_colors);

/**
 * Mesh gradient (4-corner interpolation)
 * Colors are interpolated bilinearly across the rectangle
 * @param rect   Rectangle to fill
 * @param tl     Top-left color
 * @param tr     Top-right color
 * @param bl     Bottom-left color
 * @param br     Bottom-right color
 */
void gfx_gradient_mesh(const gfx_rect_t* rect,
                       uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);

/**
 * Linear gradient at an angle
 * @param rect       Rectangle to fill
 * @param angle_fp   Angle in 16.16 fixed point (0 = horizontal right, counter-clockwise)
 * @param start      Start color
 * @param end        End color
 */
void gfx_gradient_linear_angle(const gfx_rect_t* rect, int32_t angle_fp,
                               uint32_t start, uint32_t end);

/**
 * Multi-stop linear gradient
 * @param rect       Rectangle to fill
 * @param vertical   True for vertical, false for horizontal
 * @param colors     Array of colors
 * @param stops      Array of stop positions (0-65536, must be sorted)
 * @param num_stops  Number of stops
 */
void gfx_gradient_multi(const gfx_rect_t* rect, bool vertical,
                        const uint32_t* colors, const int32_t* stops, int32_t num_stops);

// =============================================================================
// Surface Effects
// =============================================================================

/**
 * Draw a raised/3D button surface
 */
void gfx_surface_raised(const gfx_rect_t* rect, int32_t corner_radius,
                        uint32_t base_color, int32_t depth);

/**
 * Draw a pressed/sunken surface
 */
void gfx_surface_pressed(const gfx_rect_t* rect, int32_t corner_radius,
                         uint32_t base_color, int32_t depth);

/**
 * Draw a card/panel with subtle depth
 */
void gfx_surface_card(const gfx_rect_t* rect, int32_t corner_radius,
                      uint32_t bg_color, bool elevated);

// =============================================================================
// Glow Effects
// =============================================================================

/**
 * Draw a glow around a rectangle (for focus indicators, etc.)
 */
void gfx_glow_rect(const gfx_rect_t* rect, int32_t corner_radius,
                   uint32_t glow_color, int32_t intensity);

/**
 * Draw a glow around a circle
 */
void gfx_glow_circle(int32_t cx, int32_t cy, int32_t radius,
                     uint32_t glow_color, int32_t intensity);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Adjust color brightness
 * @param color  Input color
 * @param amount Adjustment (-255 to 255)
 */
uint32_t gfx_color_brightness(uint32_t color, int32_t amount);

/**
 * Adjust color saturation
 * @param color  Input color
 * @param amount Adjustment (-255 to 255)
 */
uint32_t gfx_color_saturate(uint32_t color, int32_t amount);

/**
 * Create a color with modified opacity
 */
uint32_t gfx_color_opacity(uint32_t color, uint8_t opacity);

/**
 * Mix two colors
 * @param c1, c2  Colors to mix
 * @param ratio   Mix ratio (0 = c1, 255 = c2)
 */
uint32_t gfx_color_mix(uint32_t c1, uint32_t c2, uint8_t ratio);

#endif // LEAFGFX_MODERN_H
