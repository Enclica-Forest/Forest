/**
 * TTY Font Renderer Implementation
 * Dedicated font renderer for TTY/console using only 8x8 bitmap fonts
 */

#include "../include/graphics/tty_font_renderer.h"
#include <string.h>

// Static TTY font instance
static tty_font_t tty_builtin_font = {
    .name = "tty-8x8",
    .size = 8,
    .width = 8,
    .height = 8
};

static bool tty_font_renderer_initialized = false;

tty_font_result_t tty_font_renderer_init(void) {
    if (tty_font_renderer_initialized) {
        return TTY_FONT_SUCCESS;
    }

    // Initialize font8x8 system if needed
    // Assume font8x8_get_glyph is available

    tty_font_renderer_initialized = true;
    return TTY_FONT_SUCCESS;
}

tty_font_result_t tty_font_load_builtin(const char* name, uint8_t size, tty_font_t** font) {
    if (!name || !font) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }

    if (size != 8) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }

    if (strcmp(name, "tty-8x8") != 0) {
        return TTY_FONT_ERROR_NOT_FOUND;
    }

    *font = &tty_builtin_font;
    return TTY_FONT_SUCCESS;
}

tty_font_result_t tty_font_render_char(tty_font_t* font, graphics_surface_t* surface,
                                     uint32_t x, uint32_t y, uint32_t codepoint,
                                     graphics_color_t fg_color, graphics_color_t bg_color) {
    if (!font || !surface || !surface->pixels) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }

    if (font != &tty_builtin_font) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }

    // Get the 8x8 glyph
    const char* glyph = font8x8_get_glyph(codepoint);
    if (!glyph) {
        // Use space as fallback
        glyph = font8x8_basic[32];
    }

    // Calculate bytes per pixel (support both 24bpp and 32bpp)
    uint32_t bytes_per_pixel = (surface->bpp + 7) / 8;
    if (bytes_per_pixel < 3) bytes_per_pixel = 3;  // Minimum for color
    if (bytes_per_pixel > 4) bytes_per_pixel = 4;  // Maximum supported
    if (surface->width == 0 || surface->height == 0 || surface->pitch == 0) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }

    uint64_t min_pitch = (uint64_t)surface->width * (uint64_t)bytes_per_pixel;
    if ((uint64_t)surface->pitch < min_pitch) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }
    uint64_t surface_bytes = (uint64_t)surface->pitch * (uint64_t)surface->height;
    if (surface_bytes == 0) {
        return TTY_FONT_ERROR_INVALID_PARAMETER;
    }

    // Render 8x8 bitmap
    // Note: font8x8 format uses LSB as leftmost pixel, so we use (1 << dx)
    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            uint32_t px = x + dx;
            uint32_t py = y + dy;

            if (px >= surface->width || py >= surface->height) {
                continue;
            }

            // font8x8 format: bit 0 (LSB) = leftmost pixel, bit 7 (MSB) = rightmost pixel
            bool pixel_set = (glyph[dy] & (1 << dx)) != 0;
            graphics_color_t color = pixel_set ? fg_color : bg_color;

            // Write pixel byte-by-byte (works for both 24bpp and 32bpp)
            uint64_t offset = (uint64_t)py * (uint64_t)surface->pitch +
                              (uint64_t)px * (uint64_t)bytes_per_pixel;
            if (offset + bytes_per_pixel > surface_bytes) {
                continue;
            }
            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + (size_t)offset;
            pixel_ptr[0] = color.b;  // Blue
            pixel_ptr[1] = color.g;  // Green
            pixel_ptr[2] = color.r;  // Red
            if (bytes_per_pixel == 4) {
                pixel_ptr[3] = color.a;  // Alpha (only for 32bpp)
            }
        }
    }

    return TTY_FONT_SUCCESS;
}

void tty_font_measure_text(tty_font_t* font, const char* text, uint32_t* width, uint32_t* height) {
    if (!font || !text) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }

    size_t len = strlen(text);
    if (width) *width = len * font->width;
    if (height) *height = font->height;
}
