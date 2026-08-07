/*
 * LeafGFX - Forest OS Userspace Graphics Library
 *
 * Provides framebuffer access, image loading, font rendering,
 * and input handling for userspace applications.
 */

#ifndef LEAFGFX_H
#define LEAFGFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// Core Types
// ============================================================================

typedef struct {
    void*    addr;        // Framebuffer address (mapped into userspace)
    uint32_t width;       // Screen width in pixels
    uint32_t height;      // Screen height in pixels
    uint32_t pitch;       // Bytes per row (may include padding)
    uint32_t bpp;         // Bits per pixel (typically 32)
    uint64_t phys_addr;   // Physical address (for reference)
    uint32_t size;        // Total size in bytes
    uint32_t format;      // Pixel format (FB_FORMAT_*)
    uint32_t bytes_per_pixel; // Bytes per pixel (derived from bpp)
} gfx_framebuffer_t;

typedef struct {
    uint8_t r, g, b, a;
} gfx_color_t;

typedef struct {
    int32_t x, y;
    uint32_t width, height;
} gfx_rect_t;

typedef struct {
    int8_t   offset_x;
    int8_t   offset_y;
    uint8_t  blur_radius;
    uint8_t  spread;
    uint32_t color;
} gfx_shadow_params_t;

typedef struct {
    int32_t x, y;
} gfx_point_t;

// ============================================================================
// Color Constants (ARGB format)
// ============================================================================

#define GFX_COLOR_BLACK          0xFF000000
#define GFX_COLOR_WHITE          0xFFFFFFFF
#define GFX_COLOR_RED            0xFFFF0000
#define GFX_COLOR_GREEN          0xFF00FF00
#define GFX_COLOR_BLUE           0xFF0000FF
#define GFX_COLOR_YELLOW         0xFFFFFF00
#define GFX_COLOR_CYAN           0xFF00FFFF
#define GFX_COLOR_MAGENTA        0xFFFF00FF
#define GFX_COLOR_GRAY           0xFF808080
#define GFX_COLOR_LIGHT_GRAY     0xFFC0C0C0
#define GFX_COLOR_DARK_GRAY      0xFF404040
#define GFX_COLOR_TRANSPARENT    0x00000000

// Modern accent colors (GNOME/KDE/macOS inspired)
#define GFX_COLOR_ACCENT_BLUE    0xFF3584E4  // GNOME blue
#define GFX_COLOR_ACCENT_PURPLE  0xFF9141AC  // KDE purple
#define GFX_COLOR_ACCENT_ORANGE  0xFFFF7800  // macOS orange
#define GFX_COLOR_ACCENT_TEAL    0xFF00B5AD  // Teal accent
#define GFX_COLOR_ACCENT_PINK    0xFFE91E63  // Material pink

// Semantic colors (GNOME Adwaita style)
#define GFX_COLOR_SUCCESS        0xFF2EC27E  // GNOME green
#define GFX_COLOR_WARNING        0xFFF5C211  // GNOME yellow
#define GFX_COLOR_ERROR          0xFFE01B24  // GNOME red
#define GFX_COLOR_INFO           0xFF3584E4  // GNOME blue

// Surface colors (dark theme)
#define GFX_COLOR_SURFACE_0      0xFF1E1E1E  // Background
#define GFX_COLOR_SURFACE_1      0xFF2D2D2D  // Elevated surface
#define GFX_COLOR_SURFACE_2      0xFF3D3D3D  // Higher elevation
#define GFX_COLOR_SURFACE_3      0xFF4D4D4D  // Highest elevation

// Surface colors (light theme)
#define GFX_COLOR_SURFACE_LIGHT_0  0xFFFAFAFA  // Background
#define GFX_COLOR_SURFACE_LIGHT_1  0xFFFFFFFF  // Elevated surface
#define GFX_COLOR_SURFACE_LIGHT_2  0xFFF5F5F5  // Higher elevation

// Text colors (dark theme)
#define GFX_COLOR_TEXT_PRIMARY     0xFFFFFFFF  // 100% white
#define GFX_COLOR_TEXT_SECONDARY   0xB3FFFFFF  // 70% white
#define GFX_COLOR_TEXT_DISABLED    0x66FFFFFF  // 40% white
#define GFX_COLOR_TEXT_HINT        0x4DFFFFFF  // 30% white

// Text colors (light theme)
#define GFX_COLOR_TEXT_DARK_PRIMARY   0xFF000000  // 100% black
#define GFX_COLOR_TEXT_DARK_SECONDARY 0xB3000000  // 70% black
#define GFX_COLOR_TEXT_DARK_DISABLED  0x66000000  // 40% black

// Border and divider colors
#define GFX_COLOR_BORDER_DARK    0x33FFFFFF  // 20% white border
#define GFX_COLOR_BORDER_LIGHT   0x1F000000  // 12% black border
#define GFX_COLOR_DIVIDER_DARK   0x1FFFFFFF  // 12% white divider
#define GFX_COLOR_DIVIDER_LIGHT  0x14000000  // 8% black divider

// ============================================================================
// Color Utilities
// ============================================================================

// Create color from RGBA components
static inline gfx_color_t gfx_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (gfx_color_t){r, g, b, a};
}

// Create opaque color from RGB components
static inline gfx_color_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (gfx_color_t){r, g, b, 255};
}

// ============================================================================
// Pixel Format Support
// ============================================================================

// Pixel format constants (matching framebuffer.h)
#define GFX_FORMAT_TEXT_MODE        0
#define GFX_FORMAT_INDEXED_8        1
#define GFX_FORMAT_RGB_555          2
#define GFX_FORMAT_RGB_565          3
#define GFX_FORMAT_RGB_888          4
#define GFX_FORMAT_RGBA_8888        5
#define GFX_FORMAT_BGR_888          6
#define GFX_FORMAT_BGRA_8888        7

// Convert internal ARGB color to framebuffer format
// Automatically detects current framebuffer format and converts appropriately
uint32_t gfx_color_to_fb(uint32_t argb_color);

// Read pixel from framebuffer and convert to ARGB
// Handles all supported pixel formats
uint32_t gfx_fb_to_color(void* fb_addr, uint32_t x, uint32_t y);

// Write pixel to framebuffer with format conversion
// x, y: pixel coordinates
// color: ARGB color value
void gfx_fb_put_pixel(void* fb_addr, uint32_t x, uint32_t y, uint32_t color);

// Get bytes per pixel for a given format
static inline uint32_t gfx_format_bpp(uint32_t format) {
    switch (format) {
        case GFX_FORMAT_INDEXED_8:  return 1;
        case GFX_FORMAT_RGB_555:    return 2;
        case GFX_FORMAT_RGB_565:    return 2;
        case GFX_FORMAT_RGB_888:    return 3;
        case GFX_FORMAT_BGR_888:    return 3;
        case GFX_FORMAT_RGBA_8888:  return 4;
        case GFX_FORMAT_BGRA_8888:  return 4;
        default:                    return 4;
    }
}

// Convert RGB565 to ARGB
static inline uint32_t gfx_rgb565_to_argb(uint16_t rgb565) {
    uint8_t r = (uint8_t)(((rgb565 >> 11) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((rgb565 >> 5) & 0x3F) * 255 / 63);
    uint8_t b = (uint8_t)((rgb565 & 0x1F) * 255 / 31);
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Convert ARGB to RGB565
static inline uint16_t gfx_argb_to_rgb565(uint32_t argb) {
    uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(argb & 0xFF);
    return (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
}

// Convert RGB555 to ARGB
static inline uint32_t gfx_rgb555_to_argb(uint16_t rgb555) {
    uint8_t r = (uint8_t)(((rgb555 >> 10) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((rgb555 >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)((rgb555 & 0x1F) * 255 / 31);
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Convert ARGB to RGB555
static inline uint16_t gfx_argb_to_rgb555(uint32_t argb) {
    uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(argb & 0xFF);
    return (uint16_t)(((r * 31 / 255) << 10) | ((g * 31 / 255) << 5) | (b * 31 / 255));
}

// Convert BGRA to ARGB (byte swap)
static inline uint32_t gfx_bgra_to_argb(uint32_t bgra) {
    uint8_t b = (uint8_t)(bgra & 0xFF);
    uint8_t g = (uint8_t)((bgra >> 8) & 0xFF);
    uint8_t r = (uint8_t)((bgra >> 16) & 0xFF);
    uint8_t a = (uint8_t)((bgra >> 24) & 0xFF);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Convert ARGB to BGRA (byte swap)
static inline uint32_t gfx_argb_to_bgra(uint32_t argb) {
    uint8_t a = (uint8_t)((argb >> 24) & 0xFF);
    uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(argb & 0xFF);
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

// Convert BGR to ARGB
static inline uint32_t gfx_bgr_to_argb(uint8_t b, uint8_t g, uint8_t r) {
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Convert ARGB to BGR bytes
static inline void gfx_argb_to_bgr(uint32_t argb, uint8_t* b, uint8_t* g, uint8_t* r) {
    *r = (uint8_t)((argb >> 16) & 0xFF);
    *g = (uint8_t)((argb >> 8) & 0xFF);
    *b = (uint8_t)(argb & 0xFF);
}

// Convert color to 32-bit ARGB pixel value
static inline uint32_t gfx_color_to_pixel(gfx_color_t c) {
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) |
           ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

// Convert 32-bit ARGB pixel to color
static inline gfx_color_t gfx_pixel_to_color(uint32_t p) {
    return (gfx_color_t){
        .r = (uint8_t)((p >> 16) & 0xFF),
        .g = (uint8_t)((p >> 8) & 0xFF),
        .b = (uint8_t)(p & 0xFF),
        .a = (uint8_t)((p >> 24) & 0xFF)
    };
}

// Normalize alpha for legacy RGB colors (treat RGB-only as opaque)
static inline uint8_t gfx_alpha_from_color(uint32_t color) {
    uint8_t a = (uint8_t)((color >> 24) & 0xFF);
    if (a == 0 && (color & 0x00FFFFFF) != 0) {
        return 255;
    }
    return a;
}

// Blend two colors (src over dst)
static inline uint32_t gfx_blend(uint32_t dst, uint32_t src) {
    uint8_t sa = (src >> 24) & 0xFF;
    if (sa == 0) return dst;
    if (sa == 255) return src;

    uint8_t sr = (src >> 16) & 0xFF;
    uint8_t sg = (src >> 8) & 0xFF;
    uint8_t sb = src & 0xFF;

    uint8_t dr = (dst >> 16) & 0xFF;
    uint8_t dg = (dst >> 8) & 0xFF;
    uint8_t db = dst & 0xFF;

    uint32_t inv = (uint32_t)(255 - sa);
    uint32_t r_acc = (uint32_t)sr * sa + (uint32_t)dr * inv;
    uint32_t g_acc = (uint32_t)sg * sa + (uint32_t)dg * inv;
    uint32_t b_acc = (uint32_t)sb * sa + (uint32_t)db * inv;

    // Fast divide by 255 with rounding.
    uint8_t r = (uint8_t)((r_acc + 127 + ((r_acc + 127) >> 8)) >> 8);
    uint8_t g = (uint8_t)((g_acc + 127 + ((g_acc + 127) >> 8)) >> 8);
    uint8_t b = (uint8_t)((b_acc + 127 + ((b_acc + 127) >> 8)) >> 8);

    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Interpolate between two colors (t: 0-255 representing 0.0-1.0)
static inline uint32_t gfx_lerp_color(uint32_t c1, uint32_t c2, uint8_t t) {
    if (t == 0) return c1;
    if (t == 255) return c2;

    uint8_t r1 = (c1 >> 16) & 0xFF, r2 = (c2 >> 16) & 0xFF;
    uint8_t g1 = (c1 >> 8) & 0xFF, g2 = (c2 >> 8) & 0xFF;
    uint8_t b1 = c1 & 0xFF, b2 = c2 & 0xFF;
    uint8_t a1 = (c1 >> 24) & 0xFF, a2 = (c2 >> 24) & 0xFF;

    // Lerp using integer math: result = v1 + (v2 - v1) * t / 255
    uint8_t r = r1 + (((int32_t)r2 - r1) * t) / 255;
    uint8_t g = g1 + (((int32_t)g2 - g1) * t) / 255;
    uint8_t b = b1 + (((int32_t)b2 - b1) * t) / 255;
    uint8_t a = a1 + (((int32_t)a2 - a1) * t) / 255;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Adjust color opacity
static inline uint32_t gfx_with_alpha(uint32_t color, uint8_t alpha) {
    return (color & 0x00FFFFFF) | ((uint32_t)alpha << 24);
}

// ============================================================================
// Initialization and Cleanup
// ============================================================================

// Initialize the graphics library and map the framebuffer
// Returns 0 on success, negative error code on failure
int gfx_init(void);

// Clean up and unmap the framebuffer
void gfx_cleanup(void);

// Full shutdown: release all resources, unmap framebuffer, free back buffer
void gfx_shutdown(void);

// Full app shutdown helper: release input, tracked assets, and framebuffer state
void gfx_app_shutdown(void);

// Get the framebuffer info
const gfx_framebuffer_t* gfx_get_framebuffer(void);

// Get screen dimensions
uint32_t gfx_screen_width(void);
uint32_t gfx_screen_height(void);

// ============================================================================
// Basic Drawing Primitives
// ============================================================================

// Clear the entire screen with a color
void gfx_clear(uint32_t color);

// Draw a single pixel (no blending)
void gfx_pixel(int32_t x, int32_t y, uint32_t color);

// Draw a pixel with alpha blending
void gfx_pixel_blend(int32_t x, int32_t y, uint32_t color);

// Read a pixel from the framebuffer
uint32_t gfx_read_pixel(int32_t x, int32_t y);

// Draw a horizontal line
void gfx_hline(int32_t x, int32_t y, int32_t width, uint32_t color);

// Draw a vertical line
void gfx_vline(int32_t x, int32_t y, int32_t height, uint32_t color);

// Draw a line between two points
void gfx_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);

// Fill a rectangle
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

// Draw a rectangle outline
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

// ============================================================================
// Advanced Drawing (Circles, Rounded Rectangles)
// ============================================================================

// Fill a circle (solid)
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Draw a circle outline
void gfx_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Fill an anti-aliased circle
void gfx_fill_circle_aa(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Draw a ring (circle outline with thickness)
void gfx_draw_ring(int32_t cx, int32_t cy, int32_t radius, int32_t thickness, uint32_t color);

// Fill a rounded rectangle
void gfx_fill_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, uint32_t color);

// Draw a rounded rectangle outline
void gfx_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, uint32_t color);

// ============================================================================
// Anti-Aliased Drawing (Modern UI)
// ============================================================================

// Anti-aliased line using Wu's algorithm
void gfx_line_aa(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);

// Anti-aliased line with thickness
void gfx_line_aa_thick(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       int32_t thickness, uint32_t color);

// Anti-aliased circle using signed distance field
void gfx_fill_circle_sdf(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Anti-aliased circle outline
void gfx_draw_circle_aa(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Anti-aliased ring with smooth edges
void gfx_draw_ring_aa(int32_t cx, int32_t cy, int32_t radius, int32_t thickness, uint32_t color);

// Anti-aliased rounded rectangle with smooth corners
void gfx_fill_rounded_rect_aa(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t radius, uint32_t color);

// Anti-aliased rounded rectangle outline
void gfx_draw_rounded_rect_aa(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t radius, int32_t thickness, uint32_t color);

// Capsule shape (pill button, like macOS/iOS)
void gfx_fill_capsule(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

// Capsule outline
void gfx_draw_capsule(int32_t x, int32_t y, int32_t w, int32_t h, int32_t thickness, uint32_t color);

// Squircle (superellipse, iOS app icon shape)
// smoothness: 0.0 = square, 1.0 = circle (recommended: 0.6 for iOS-style)
void gfx_fill_squircle(int32_t x, int32_t y, int32_t size, int32_t smoothness_fp, uint32_t color);

// Stadium shape (horizontal capsule)
void gfx_fill_stadium(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

// ============================================================================
// Gradients
// ============================================================================

// Fill a rectangle with a vertical gradient (top to bottom)
void gfx_gradient_vertical(int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t top_color, uint32_t bottom_color);

// Fill a rectangle with a horizontal gradient (left to right)
void gfx_gradient_horizontal(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t left_color, uint32_t right_color);

// Fill a rectangle with a 3-stop vertical gradient
void gfx_gradient_vertical_3(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t top, uint32_t mid, uint32_t bottom);

void gfx_gradient_linear(int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t start_x, int32_t start_y,
                          int32_t end_x, int32_t end_y,
                          uint32_t color_start, uint32_t color_end);

void gfx_gradient_radial_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t center_x, int32_t center_y, int32_t radius,
                               uint32_t inner_color, uint32_t outer_color);

// =========================================================================
// Shadows and Glass
// =========================================================================

void gfx_draw_shadow_soft(const gfx_rect_t* rect, int32_t corner_radius,
                          const gfx_shadow_params_t* shadow);
void gfx_glass_panel_fast(int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t corner_radius, uint32_t tint_color);

// ============================================================================
// Clipping
// ============================================================================

// Set the clipping rectangle (all drawing will be clipped to this area)
void gfx_set_clip(int32_t x, int32_t y, int32_t w, int32_t h);

// Clear the clipping rectangle (allow drawing to entire screen)
void gfx_clear_clip(void);

// Get the current clipping rectangle
gfx_rect_t gfx_get_clip(void);

void gfx_push_clip(int32_t x, int32_t y, int32_t w, int32_t h);
void gfx_pop_clip(void);

// ============================================================================
// Double Buffering (optional)
// ============================================================================

// Allocate a back buffer for double buffering
int gfx_create_backbuffer(void);

// Free the back buffer
void gfx_destroy_backbuffer(void);

// Flip the back buffer to the front (copy entire buffer)
void gfx_flip(void);

// Flip only a specific region
void gfx_flip_rect(int32_t x, int32_t y, int32_t w, int32_t h);

// Set whether to draw to the back buffer (if created) or directly to screen
void gfx_set_target_backbuffer(bool use_backbuffer);

// ============================================================================
// Dirty Region Tracking
// ============================================================================

void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h);
void gfx_clear_dirty(void);
bool gfx_has_dirty(void);
void gfx_get_dirty_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h);
void gfx_flip_dirty(void);

// ============================================================================
// Offscreen App Buffers (for composited apps)
// ============================================================================

typedef struct gfx_app_buffer {
    void* data;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t size;
} gfx_app_buffer_t;

// Create an offscreen buffer for app rendering (returns NULL on failure)
gfx_app_buffer_t* gfx_app_create_buffer(uint32_t width, uint32_t height);

// Destroy an offscreen buffer
void gfx_app_destroy_buffer(gfx_app_buffer_t* buffer);

// Set rendering target to offscreen buffer (NULL = use main framebuffer)
void gfx_app_set_buffer(gfx_app_buffer_t* buffer);

// Get current offscreen buffer (NULL if rendering to main framebuffer)
gfx_app_buffer_t* gfx_app_get_buffer(void);

// Flip offscreen buffer to screen (composite app window)
void gfx_app_flip_to_screen(void);

// ============================================================================
// Mouse Cursor
// ============================================================================

// Standard cursor types
typedef enum {
    GFX_CURSOR_ARROW,
    GFX_CURSOR_HAND,
    GFX_CURSOR_TEXT,
    GFX_CURSOR_CROSSHAIR,
    GFX_CURSOR_WAIT,
    GFX_CURSOR_CUSTOM
} gfx_cursor_type_t;

// Draw the system mouse cursor at the specified position
void gfx_draw_cursor(int32_t x, int32_t y, gfx_cursor_type_t type);

// Set a custom cursor image
void gfx_set_custom_cursor(const uint32_t* pixels, int32_t w, int32_t h,
                           int32_t hotspot_x, int32_t hotspot_y);

// ============================================================================
// Utility Functions
// ============================================================================

// Check if a point is inside a rectangle
static inline bool gfx_point_in_rect(int32_t px, int32_t py,
                                     int32_t rx, int32_t ry,
                                     int32_t rw, int32_t rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

// Check if a point is inside a circle
static inline bool gfx_point_in_circle(int32_t px, int32_t py,
                                       int32_t cx, int32_t cy, int32_t r) {
    int32_t dx = px - cx;
    int32_t dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

// Clamp a value to a range
static inline int32_t gfx_clamp(int32_t value, int32_t min, int32_t max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static inline int32_t gfx_abs(int32_t v) {
    return v < 0 ? -v : v;
}

static inline int32_t gfx_map_range(int32_t value, int32_t in_min, int32_t in_max,
                                     int32_t out_min, int32_t out_max) {
    if (in_max == in_min) return out_min;
    return out_min + (int32_t)(((int64_t)(value - in_min) * (out_max - out_min)) / (in_max - in_min));
}

// Linear interpolation (integer version, t is 0-256 where 256 = 1.0)
static inline int32_t gfx_lerp(int32_t a, int32_t b, int32_t t) {
    return a + ((b - a) * t) / 256;
}

// ============================================================================
// Animation Helpers (Fixed-Point)
// ============================================================================

// Fixed-point format: 16.16 (upper 16 bits = integer, lower 16 bits = fraction)
// GFX_FP_ONE represents 1.0, GFX_FP_HALF represents 0.5
#define GFX_FP_BITS     16
#define GFX_FP_ONE      (1 << GFX_FP_BITS)      // 65536 = 1.0
#define GFX_FP_HALF     (GFX_FP_ONE / 2)        // 32768 = 0.5

// Convert integer to fixed-point
#define GFX_INT_TO_FP(x)    ((x) << GFX_FP_BITS)

// Convert fixed-point to integer (truncate)
#define GFX_FP_TO_INT(x)    ((x) >> GFX_FP_BITS)

// Multiply two fixed-point numbers
#define GFX_FP_MUL(a, b)    (((int64_t)(a) * (b)) >> GFX_FP_BITS)

// Divide two fixed-point numbers
#define GFX_FP_DIV(a, b)    (((int64_t)(a) << GFX_FP_BITS) / (b))

// Easing functions (input and output are fixed-point: 0 to GFX_FP_ONE)
int32_t gfx_ease_in_quad(int32_t t);
int32_t gfx_ease_out_quad(int32_t t);
int32_t gfx_ease_in_out_quad(int32_t t);
int32_t gfx_ease_in_cubic(int32_t t);
int32_t gfx_ease_out_cubic(int32_t t);
int32_t gfx_ease_in_out_cubic(int32_t t);
int32_t gfx_ease_out_bounce(int32_t t);

// Linear interpolation using fixed-point
// Returns: a + (b - a) * t, where t is in fixed-point [0, GFX_FP_ONE]
static inline int32_t gfx_lerp_fp(int32_t a, int32_t b, int32_t t) {
    return a + GFX_FP_TO_INT((int64_t)(b - a) * t);
}

int32_t gfx_ease_in_sine(int32_t t);
int32_t gfx_ease_out_sine(int32_t t);
int32_t gfx_ease_in_out_sine(int32_t t);
int32_t gfx_spring_interpolate(int32_t t, int32_t frequency_fp, int32_t damping_fp);

// Get current time in milliseconds (for animation timing)
uint32_t gfx_get_ticks(void);

// Sleep for a number of milliseconds
void gfx_sleep(uint32_t ms);

#endif // LEAFGFX_H
