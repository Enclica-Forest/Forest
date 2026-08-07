# LeafGFX - Forest OS Graphics Library

LeafGFX is the core userspace graphics library for Forest OS. It provides everything you need to build graphical applications: framebuffer access, drawing primitives, TrueType font rendering, image loading, animations, input handling, and modern visual effects like blur, shadows, and frosted glass.

All drawing is done in ARGB color format internally, with automatic conversion to whatever pixel format the framebuffer uses (BGRA, RGB565, RGB888, etc.).

## Getting Started

Every LeafGFX application starts with initialization and ends with cleanup:

```c
#include "leafgfx.h"

int main(void) {
    if (gfx_init() != 0) {
        printf("Failed to initialize graphics\n");
        return 1;
    }

    // Your app goes here
    gfx_clear(GFX_COLOR_BLACK);
    gfx_text(10, 10, "Hello, Forest OS!", GFX_COLOR_WHITE);
    gfx_flip();

    gfx_cleanup();
    return 0;
}
```

`gfx_init()` maps the kernel framebuffer into your process, sets up double buffering, and detects the screen resolution and pixel format. `gfx_flip()` copies the back buffer to the screen and tells the kernel to flush the display.

## Core Drawing Primitives

LeafGFX provides all the basic shapes you'd expect:

```c
// Fill the screen with a color
gfx_clear(GFX_COLOR_SURFACE_0);

// Single pixel (no blending)
gfx_pixel(100, 100, GFX_COLOR_RED);

// Pixel with alpha blending
gfx_pixel_blend(100, 100, 0x80FF0000); // 50% transparent red

// Horizontal and vertical lines
gfx_hline(10, 50, 200, GFX_COLOR_WHITE);
gfx_vline(10, 50, 100, GFX_COLOR_WHITE);

// Arbitrary line (Bresenham's algorithm)
gfx_line(10, 10, 200, 150, GFX_COLOR_GREEN);

// Filled rectangle
gfx_fill_rect(50, 50, 200, 100, GFX_COLOR_BLUE);

// Rectangle outline
gfx_draw_rect(50, 50, 200, 100, GFX_COLOR_WHITE);
```

### Circles

```c
// Filled circle
gfx_fill_circle(400, 300, 50, GFX_COLOR_CYAN);

// Circle outline (midpoint algorithm)
gfx_draw_circle(400, 300, 50, GFX_COLOR_WHITE);

// Anti-aliased filled circle (SDF-based, smooth edges)
gfx_fill_circle_sdf(400, 300, 50, GFX_COLOR_CYAN);

// Ring (thick circle outline)
gfx_draw_ring(400, 300, 50, 4, GFX_COLOR_WHITE);
```

### Rounded Rectangles

```c
// Filled rounded rectangle
gfx_fill_rounded_rect(50, 50, 200, 100, 12, GFX_COLOR_SURFACE_1);

// Rounded rectangle outline
gfx_draw_rounded_rect(50, 50, 200, 100, 12, GFX_COLOR_WHITE);

// Anti-aliased versions (for small radius / UI elements)
gfx_fill_rounded_rect_aa(50, 50, 200, 100, 12, GFX_COLOR_SURFACE_1);
gfx_draw_rounded_rect_aa(50, 50, 200, 100, 12, 2, GFX_COLOR_WHITE);
```

### Modern Shapes

```c
// Capsule (pill button, macOS/iOS style)
gfx_fill_capsule(100, 100, 160, 40, GFX_COLOR_ACCENT_BLUE);

// Squircle (superellipse, iOS app icon shape)
// smoothness: 0 = square, GFX_FP_ONE = circle
gfx_fill_squircle(100, 100, 80, GFX_FP_ONE * 6 / 10, GFX_COLOR_ACCENT_PURPLE);

// Stadium shape (horizontal capsule)
gfx_fill_stadium(100, 100, 200, 40, GFX_COLOR_ACCENT_TEAL);
```

### Anti-Aliased Drawing

For smooth UI elements, use the anti-aliased variants:

```c
// Smooth line (Wu's algorithm)
gfx_line_aa(10, 10, 300, 200, GFX_COLOR_WHITE);

// Thick AA line
gfx_line_aa_thick(10, 10, 300, 200, 3, GFX_COLOR_WHITE);

// AA circle outline
gfx_draw_circle_aa(400, 300, 50, GFX_COLOR_WHITE);

// AA ring
gfx_draw_ring_aa(400, 300, 50, 4, GFX_COLOR_WHITE);
```

## Gradients

LeafGFX supports several gradient types for modern UI design:

```c
// Vertical gradient (top to bottom)
gfx_gradient_vertical(0, 0, 800, 600,
    GFX_COLOR_ACCENT_BLUE, GFX_COLOR_ACCENT_PURPLE);

// Horizontal gradient (left to right)
gfx_gradient_horizontal(0, 0, 800, 600,
    GFX_COLOR_RED, GFX_COLOR_BLUE);

// 3-stop vertical gradient
gfx_gradient_vertical_3(0, 0, 800, 600,
    GFX_COLOR_BLUE, GFX_COLOR_GREEN, GFX_COLOR_RED);

// Linear gradient at any angle
gfx_gradient_linear(0, 0, 800, 600,
    0, 0, 800, 600,
    GFX_COLOR_BLUE, GFX_COLOR_RED);

// Radial gradient
gfx_gradient_radial_rect(0, 0, 800, 600,
    400, 300, 300,
    GFX_COLOR_WHITE, GFX_COLOR_TRANSPARENT);
```

The `leafgfx_modern.h` header adds even more gradient types:

```c
// Radial gradient (center to edge)
gfx_gradient_radial(400, 300, 200, inner_color, outer_color);

// Angular/conic gradient
uint32_t colors[] = { GFX_COLOR_RED, GFX_COLOR_GREEN, GFX_COLOR_BLUE, GFX_COLOR_RED };
gfx_gradient_angular(400, 300, 200, 0, colors, 4);

// Mesh gradient (4-corner bilinear interpolation)
gfx_rect_t rect = {100, 100, 300, 200};
gfx_gradient_mesh(&rect, tl_color, tr_color, bl_color, br_color);

// Multi-stop gradient
int32_t stops[] = {0, 32768, 65536};
uint32_t cols[] = { GFX_COLOR_RED, GFX_COLOR_GREEN, GFX_COLOR_BLUE };
gfx_gradient_multi(&rect, true, cols, stops, 3);
```

## Color Utilities

LeafGFX defines a rich set of color constants inspired by GNOME Adwaita, KDE, and macOS:

```c
// Basic colors
gfx_clear(GFX_COLOR_BLACK);
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_RED);

// Modern accent colors
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_ACCENT_BLUE);   // GNOME blue
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_ACCENT_PURPLE); // KDE purple
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_ACCENT_ORANGE);  // macOS orange

// Semantic colors
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_SUCCESS);
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_WARNING);
gfx_fill_rect(0, 0, 100, 100, GFX_COLOR_ERROR);

// Surface colors (dark theme)
gfx_clear(GFX_COLOR_SURFACE_0);
gfx_fill_rect(10, 10, 200, 100, GFX_COLOR_SURFACE_1);

// Text colors
gfx_text(10, 10, "Primary", GFX_COLOR_TEXT_PRIMARY);
gfx_text(10, 30, "Secondary", GFX_COLOR_TEXT_SECONDARY);
```

You can also manipulate colors with utility functions:

```c
// Create colors
gfx_color_t c = gfx_rgba(255, 128, 0, 200);
uint32_t pixel = gfx_color_to_pixel(c);

// Blend two colors (source over destination)
uint32_t blended = gfx_blend(dst_color, src_color);

// Interpolate between colors
uint32_t mid = gfx_lerp_color(color1, color2, 128); // 50% mix

// Adjust opacity
uint32_t dim = gfx_with_alpha(GFX_COLOR_WHITE, 128);

// Brightness/saturation (from leafgfx_modern.h)
uint32_t bright = gfx_color_brightness(color, 30);
uint32_t dull = gfx_color_saturate(color, -50);
uint32_t mixed = gfx_color_mix(color1, color2, 128);
```

## TrueType Font Rendering

LeafGFX includes a full userspace TrueType font parser and rasterizer. No kernel support needed.

### Loading Fonts

```c
#include "leafgfx_font.h"

// Use the built-in 8x16 bitmap font (no loading needed)
const gfx_font_t* font = gfx_font_get_default();

// Load a TrueType font from file
gfx_font_t* ttf_font = NULL;
gfx_font_result_t result = gfx_font_load_ttf("/usr/share/fonts/DejaVuSans.ttf",
                                              16, &ttf_font);

// Load from memory (e.g., embedded in binary)
extern const uint8_t font_data[];
extern size_t font_data_size;
gfx_font_load_ttf_memory(font_data, font_data_size, 16, &ttf_font);
```

### Drawing Text

```c
// Basic text rendering
gfx_draw_text(font, 10, 10, "Hello, World!", GFX_COLOR_WHITE);

// Single character (returns width)
uint32_t w = gfx_draw_char(font, 10, 10, 'A', GFX_COLOR_WHITE);

// Centered text
gfx_draw_text_centered(font, 0, 10, 800, "Centered!", GFX_COLOR_WHITE);

// Right-aligned text
gfx_draw_text_right(font, 0, 10, 800, "Right aligned", GFX_COLOR_WHITE);

// Centered both horizontally and vertically in a rectangle
gfx_rect_t rect = {100, 100, 200, 40};
gfx_draw_text_centered_rect(font, &rect, "Centered", GFX_COLOR_WHITE);

// Word-wrapped text
uint32_t total_height = gfx_draw_text_wrapped(font, 10, 10, 300,
    "This is a long paragraph that will wrap to the next line.",
    GFX_COLOR_WHITE);

// Text with shadow (great for readability on busy backgrounds)
gfx_draw_text_shadow(font, 10, 10, "Shadow text",
    GFX_COLOR_WHITE, 0x80000000, 2);

// Text with outline
gfx_draw_text_outline(font, 10, 10, "Outlined",
    GFX_COLOR_WHITE, GFX_COLOR_BLACK);

// Justified text
gfx_draw_text_justify(font, 10, 10, 400,
    "This text will be justified to fill the width.",
    GFX_COLOR_WHITE);
```

### Convenience Functions

For quick text rendering with the default font:

```c
// These use gfx_font_get_default() automatically
gfx_text(10, 10, "Quick text!", GFX_COLOR_WHITE);
gfx_text_centered(400, 10, "Centered quick text", GFX_COLOR_WHITE);
uint32_t w = gfx_text_width("Measure me");
```

### Font Properties and Metrics

```c
uint32_t height = gfx_font_get_height(font);
uint32_t line_spacing = gfx_font_get_line_spacing(font);
uint32_t char_w = gfx_font_get_char_width(font, 'W');
uint32_t text_w = gfx_font_get_text_width(font, "Hello");

uint32_t tw, th;
gfx_font_get_text_bounds(font, "Hello", &tw, &th);

// Detailed metrics
int32_t ascent = gfx_font_get_ascent(font);
int32_t descent = gfx_font_get_descent(font);
int32_t line_gap = gfx_font_get_line_gap(font);
```

### Font Configuration

```c
// Font fallback chain (for missing glyphs)
gfx_font_set_fallback(ttf_font, fallback_font);

// Emoji support
gfx_font_set_emoji(ttf_font, emoji_font);

// Monospace mode
gfx_font_set_monospace(ttf_font, true, 8);

// Subpixel rendering
gfx_font_set_subpixel(ttf_font, true);
```

### Bidirectional Text

```c
// Check if a character is RTL
bool rtl = gfx_unicode_is_rtl(0x0627); // Arabic Alef

// Draw with automatic bidi reordering
gfx_draw_text_bidi(font, 10, 10, "Hello مرحبا World",
    800, GFX_COLOR_WHITE);
```

## BMP Image Loading

LeafGFX can load and display BMP images, plus other formats through auto-detection:

```c
#include "leafgfx_bmp.h"

// Load a BMP from file
gfx_image_t* image = NULL;
gfx_bmp_result_t result = gfx_image_load_bmp("/usr/share/images/bg.bmp", &image);

// Auto-detect format (BMP, PNG, GIF, JPEG)
gfx_image_load("/usr/share/images/photo.png", &image);

// Load from memory
gfx_image_load_bmp_memory(data, data_size, &image);

// Create a blank image
gfx_image_t* blank = gfx_image_create(100, 100, GFX_COLOR_BLUE);

// Clone an image
gfx_image_t* copy = gfx_image_clone(image);
```

### Drawing Images

```c
// Draw at position
gfx_image_draw(image, 100, 50);

// Draw with alpha blending
gfx_image_draw_blend(image, 100, 50);

// Draw scaled
gfx_image_draw_scaled(image, 100, 50, 320, 240);

// Draw with bilinear interpolation (smoother)
gfx_image_draw_scaled_bilinear(image, 100, 50, 320, 240);

// Draw a sub-region
gfx_image_draw_region(image, 32, 32, 64, 64, 100, 50);

// Draw with opacity
gfx_image_draw_opacity(image, 100, 50, 128); // 50% transparent

// Draw tinted with a color
gfx_image_draw_tinted(image, 100, 50, 0x80FF0000); // Red tint
```

### Image Manipulation

```c
// Get/set individual pixels
uint32_t pixel = gfx_image_get_pixel(image, 10, 10);
gfx_image_set_pixel(image, 10, 10, GFX_COLOR_RED);

// Fill entire image
gfx_image_fill(image, GFX_COLOR_BLUE);

// Apply Gaussian blur
gfx_image_blur(image, 3); // radius 3

// Free when done
gfx_image_free(image);
```

## Animation System

LeafGFX has a modern animation system with spring physics and easing functions.

### Spring Physics

Springs give you natural-feeling animations without manual keyframing:

```c
#include "leafgfx_anim.h"

// Initialize a spring (stiffness=170, damping=26, mass=1 by default)
gfx_spring_t spring;
gfx_spring_init(&spring, GFX_INT_TO_FP(0), GFX_INT_TO_FP(100));

// Use presets for common behaviors
gfx_spring_preset_gentle(&spring);   // Slow, smooth
gfx_spring_preset_snappy(&spring);   // Fast, responsive
gfx_spring_preset_bouncy(&spring);   // Noticeable bounce
gfx_spring_preset_stiff(&spring);    // Almost no bounce

// In your animation loop:
spring.target = GFX_INT_TO_FP(mouse_x);
gfx_spring_update(&spring, delta_ms);
int32_t position = gfx_spring_get_position(&spring);

if (gfx_spring_is_at_rest(&spring)) {
    // Animation complete
}
```

### Tween Animations

For time-based animations with easing curves:

```c
gfx_tween_t tween;
gfx_tween_init(&tween, 0, GFX_FP_ONE, 500, gfx_ease_out_cubic);
gfx_tween_start(&tween);

// In your loop:
int32_t value = gfx_tween_update(&tween, delta_ms);
// value goes from 0 to GFX_FP_ONE over 500ms

// Set looping
gfx_tween_set_loop(&tween, -1, GFX_ANIM_DIR_ALTERNATE); // infinite ping-pong
```

### Easing Functions

LeafGFX provides a full set of easing curves:

```c
// Quadratic
gfx_ease_in_quad(t);
gfx_ease_out_quad(t);
gfx_ease_in_out_quad(t);

// Cubic
gfx_ease_in_cubic(t);
gfx_ease_out_cubic(t);
gfx_ease_in_out_cubic(t);

// Quartic, Quintic, Exponential, Circular
gfx_ease_in_quart(t);
gfx_ease_out_expo(t);
gfx_ease_in_out_circ(t);

// Back (overshoot, macOS-style)
gfx_ease_out_back(t);

// Elastic (spring-like bounce)
gfx_ease_out_elastic(t);

// Bounce (ball dropping)
gfx_ease_out_bounce(t);

// Sine (gentle, natural)
gfx_ease_in_out_sine(t);

// Custom cubic bezier
gfx_ease_bezier(t, p1_y, p2_y);
```

### Color Animation

```c
// Interpolate between two colors
uint32_t color = gfx_color_lerp(GFX_COLOR_RED, GFX_COLOR_BLUE, t);

// HSL interpolation (smoother for hue transitions)
uint32_t hsl_color = gfx_color_lerp_hsl(GFX_COLOR_RED, GFX_COLOR_BLUE, t);
```

## Input Handling

LeafGFX handles keyboard and mouse input through device files or syscalls:

```c
#include "leafgfx_input.h"

// Initialize input (opens /dev/kbd and /dev/mouse)
gfx_input_init();

// In your main loop:
while (running) {
    gfx_input_poll(); // Non-blocking, call once per frame

    // Mouse
    const gfx_mouse_state_t* mouse = gfx_get_mouse();
    int mx = gfx_mouse_x();
    int my = gfx_mouse_y();

    if (gfx_mouse_left_clicked()) {
        printf("Clicked at %d, %d\n", mx, my);
    }

    // Check if mouse is over a button
    if (gfx_mouse_in_rect(btn_x, btn_y, btn_w, btn_h)) {
        // Hover state
    }

    // Keyboard
    const gfx_keyboard_state_t* kb = gfx_get_keyboard();

    if (gfx_key_pressed(GFX_KEY_ESC)) {
        running = false;
    }

    if (gfx_key_down(GFX_KEY_LEFTCTRL) && gfx_key_pressed(GFX_KEY_C)) {
        running = false; // Ctrl+C
    }

    // Get typed character (handles shift, caps lock)
    char c = gfx_get_typed_char();
    if (c) {
        input_buffer[pos++] = c;
    }
}
```

### Keyboard Constants

All standard keys are defined as `GFX_KEY_*` constants:

```c
GFX_KEY_ESC, GFX_KEY_ENTER, GFX_KEY_SPACE, GFX_KEY_BACKSPACE
GFX_KEY_UP, GFX_KEY_DOWN, GFX_KEY_LEFT, GFX_KEY_RIGHT
GFX_KEY_F1 through GFX_KEY_F12
GFX_KEY_LEFTCTRL, GFX_KEY_LEFTSHIFT, GFX_KEY_LEFTALT
// Letters: GFX_KEY_A through GFX_KEY_Z
// Numbers: GFX_KEY_0 through GFX_KEY_9
```

### Mouse Helpers

```c
bool down = gfx_mouse_left_down();     // Currently held
bool clicked = gfx_mouse_left_clicked(); // Just pressed this frame
bool released = gfx_mouse_left_released(); // Just released

bool right_down = gfx_mouse_right_down();
bool right_clicked = gfx_mouse_right_clicked();

bool in_circle = gfx_mouse_in_circle(400, 300, 50);
```

## Modern Rendering Effects

The `leafgfx_modern.h` module adds advanced visual effects inspired by GNOME, KDE, and macOS.

### Blur Effects

```c
#include "leafgfx_modern.h"

// Box blur on a framebuffer region
gfx_blur_box(100, 100, 300, 200, 8);

// Fast approximated Gaussian blur (3-pass box blur, much smoother)
gfx_blur_gaussian_fast(100, 100, 300, 200, 12);

// Blur a pixel buffer (not the framebuffer)
gfx_blur_buffer(pixel_buffer, width, height, radius);
```

### Frosted Glass Effect

```c
// True frosted glass (blur + tint, macOS-style)
gfx_glass_panel(100, 100, 400, 300, 16, 12, 0xC0FFFFFF);

// Fast simulated frosted glass (no actual blur, just sampling)
gfx_glass_panel_fast(100, 100, 400, 300, 16, 0xC0FFFFFF);
```

### Shadows

Predefined shadow styles for consistent elevation:

```c
gfx_rect_t rect = {100, 100, 200, 80};

// Draw shadow with predefined style
gfx_draw_shadow_soft(&rect, 12, &GFX_SHADOW_MEDIUM);

// Custom shadow
gfx_shadow_params_t shadow = {
    .offset_x = 0,
    .offset_y = 4,
    .blur_radius = 8,
    .spread = 0,
    .color = 0x40000000
};
gfx_draw_shadow(&rect, 12, &shadow);

// Inset shadow (pressed/sunken effect)
gfx_draw_shadow_inset(&rect, 12, &GFX_SHADOW_SMALL);
```

Available shadow presets:

| Preset | Description |
|--------|-------------|
| `GFX_SHADOW_NONE` | No shadow |
| `GFX_SHADOW_SUBTLE` | Minimal elevation |
| `GFX_SHADOW_SMALL` | Small card/button |
| `GFX_SHADOW_MEDIUM` | Standard window |
| `GFX_SHADOW_LARGE` | Elevated modal |
| `GFX_SHADOW_ELEVATED` | Popup/dropdown |

### Glow Effects

```c
// Glow around a rectangle (for focus indicators)
gfx_glow_rect(&rect, 12, GFX_COLOR_ACCENT_BLUE, 8);

// Glow around a circle
gfx_glow_circle(400, 300, 50, GFX_COLOR_ACCENT_BLUE, 8);
```

### Surface Effects

```c
// Raised/3D button surface
gfx_surface_raised(&rect, 8, GFX_COLOR_SURFACE_1, 3);

// Pressed/sunken surface
gfx_surface_pressed(&rect, 8, GFX_COLOR_SURFACE_1, 3);

// Card/panel with optional elevation shadow
gfx_surface_card(&rect, 12, GFX_COLOR_SURFACE_1, true);
```

## Clipping and Dirty Tracking

### Clipping

Clip all drawing to a specific rectangle:

```c
// Set clip region
gfx_set_clip(100, 100, 400, 300);

// All drawing is now limited to this area
gfx_fill_rect(0, 0, 800, 600, GFX_COLOR_RED); // Only draws in clip region

// Clear clipping
gfx_clear_clip();

// Push/pop clip stack (nested clipping)
gfx_push_clip(100, 100, 400, 300); // Intersects with current clip
    // Draw here...
    gfx_push_clip(150, 150, 200, 100); // Further restricted
        // Draw here...
    gfx_pop_clip();
    // Back to previous clip
gfx_pop_clip();
```

### Dirty Region Tracking

Track which areas of the screen have changed, to minimize framebuffer updates:

```c
// Mark regions as dirty
gfx_mark_dirty(100, 100, 200, 50);
gfx_mark_dirty(300, 200, 150, 80);

// Check if anything is dirty
if (gfx_has_dirty()) {
    int32_t x, y, w, h;
    gfx_get_dirty_rect(&x, &y, &w, &h);

    // Flip only the dirty region
    gfx_flip_dirty();
}

// Clear dirty state
gfx_clear_dirty();
```

## Double Buffering

LeafGFX uses double buffering by default to prevent tearing:

```c
// Allocate back buffer (done automatically by gfx_init)
gfx_create_backbuffer();

// Draw to back buffer (default behavior)
gfx_fill_rect(100, 100, 200, 100, GFX_COLOR_BLUE);

// Flip entire screen
gfx_flip();

// Flip only a region (faster)
gfx_flip_rect(100, 100, 200, 100);

// Toggle back buffer usage
gfx_set_target_backbuffer(true);  // Draw to back buffer
gfx_set_target_backbuffer(false); // Draw directly to screen
```

## Offscreen App Buffers

For compositing apps, render to an offscreen buffer first:

```c
// Create offscreen buffer
gfx_app_buffer_t* buffer = gfx_app_create_buffer(800, 600);

// Set as render target
gfx_app_set_buffer(buffer);

// All drawing goes to the offscreen buffer
gfx_clear(GFX_COLOR_SURFACE_0);
gfx_fill_rect(10, 10, 200, 100, GFX_COLOR_BLUE);
gfx_text(20, 20, "Offscreen!", GFX_COLOR_WHITE);

// Composite to screen
gfx_app_flip_to_screen();

// Clean up
gfx_app_destroy_buffer(buffer);
```

## Mouse Cursor

```c
// Draw built-in cursor
gfx_draw_cursor(mouse_x, mouse_y, GFX_CURSOR_ARROW);

// Cursor types: GFX_CURSOR_ARROW, GFX_CURSOR_HAND, GFX_CURSOR_TEXT,
//               GFX_CURSOR_CROSSHAIR, GFX_CURSOR_WAIT, GFX_CURSOR_CUSTOM

// Set custom cursor
gfx_set_custom_cursor(pixels, width, height, hotspot_x, hotspot_y);
gfx_draw_cursor(mouse_x, mouse_y, GFX_CURSOR_CUSTOM);
```

## Fixed-Point Math

LeafGFX uses 16.16 fixed-point math for animation calculations:

```c
// Constants
GFX_FP_ONE   // 1.0 (65536)
GFX_FP_HALF  // 0.5 (32768)

// Conversion
int32_t fp = GFX_INT_TO_FP(5);      // 5.0 -> 327680
int32_t i = GFX_FP_TO_INT(fp);      // 327680 -> 5

// Arithmetic
int32_t product = GFX_FP_MUL(a, b); // a * b
int32_t quotient = GFX_FP_DIV(a, b); // a / b

// Interpolation
int32_t mid = gfx_lerp_fp(100, 200, GFX_FP_HALF); // 150
```

## API Reference Overview

| Header | Purpose |
|--------|---------|
| `leafgfx.h` | Core types, primitives, framebuffer, colors, clipping, animation helpers |
| `leafgfx_font.h` | Font loading, text rendering, metrics, bidi support |
| `leafgfx_ttf.h` | Low-level TrueType parser and rasterizer |
| `leafgfx_bmp.h` | Image loading (BMP, PNG, GIF, JPEG) and drawing |
| `leafgfx_anim.h` | Spring physics, tween animations, easing functions |
| `leafgfx_input.h` | Keyboard and mouse input handling |
| `leafgfx_modern.h` | Blur, shadows, gradients, glass, glow, surface effects |

## Complete Example: Interactive Button

Here's a complete example that ties everything together:

```c
#include "leafgfx.h"
#include "leafgfx_font.h"
#include "leafgfx_input.h"
#include "leafgfx_modern.h"

int main(void) {
    gfx_init();
    gfx_input_init();

    const gfx_font_t* font = gfx_font_get_default();
    bool running = true;

    int32_t btn_x = 300, btn_y = 250, btn_w = 200, btn_h = 50;

    while (running) {
        gfx_input_poll();

        if (gfx_key_pressed(GFX_KEY_ESC)) running = false;

        bool hover = gfx_mouse_in_rect(btn_x, btn_y, btn_w, btn_h);
        bool pressed = hover && gfx_mouse_left_down();

        // Clear
        gfx_clear(GFX_COLOR_SURFACE_0);

        // Draw shadow
        gfx_rect_t btn_rect = {btn_x, btn_y + 2, btn_w, btn_h};
        gfx_draw_shadow_soft(&btn_rect, 12, &GFX_SHADOW_SMALL);

        // Draw button
        uint32_t btn_color = pressed ? GFX_COLOR_SURFACE_2 : GFX_COLOR_SURFACE_1;
        gfx_fill_rounded_rect_aa(btn_x, btn_y, btn_w, btn_h, 12, btn_color);

        if (hover) {
            gfx_draw_rounded_rect_aa(btn_x, btn_y, btn_w, btn_h, 12, 2,
                                      GFX_COLOR_ACCENT_BLUE);
        }

        // Draw label
        gfx_draw_text_centered(font, btn_x, btn_y + 15, btn_w,
                               "Click Me", GFX_COLOR_TEXT_PRIMARY);

        // Draw cursor
        gfx_draw_cursor(gfx_mouse_x(), gfx_mouse_y(), GFX_CURSOR_ARROW);

        gfx_flip();
        gfx_sleep(16); // ~60fps
    }

    gfx_input_shutdown();
    gfx_cleanup();
    return 0;
}
```

## Tips

- Always call `gfx_clear()` at the start of each frame unless you want to keep the previous frame.
- Use `gfx_flip_dirty()` instead of `gfx_flip()` when only part of the screen changed.
- The `*_aa` (anti-aliased) drawing functions are slower but look much better for UI elements.
- For text on busy backgrounds, use `gfx_draw_text_shadow()` for readability.
- Call `gfx_input_poll()` exactly once per frame, not in a loop.
- Use `gfx_app_shutdown()` at program exit to cleanly release all resources.
