/**
 * TTY Font Renderer
 * Dedicated font renderer for TTY/console output using only 8x8 bitmap fonts
 */

#ifndef TTY_FONT_RENDERER_H
#define TTY_FONT_RENDERER_H

#include "graphics_types.h"
#include "font8x8.h"

// TTY font renderer result
typedef enum {
    TTY_FONT_SUCCESS = 0,
    TTY_FONT_ERROR_INVALID_PARAMETER,
    TTY_FONT_ERROR_NOT_FOUND,
    TTY_FONT_ERROR_OUT_OF_MEMORY
} tty_font_result_t;

// Simplified TTY font structure
typedef struct {
    char name[32];
    uint8_t size;
    uint8_t width;
    uint8_t height;
} tty_font_t;

// Initialize TTY font renderer
tty_font_result_t tty_font_renderer_init(void);

// Load built-in 8x8 font
tty_font_result_t tty_font_load_builtin(const char* name, uint8_t size, tty_font_t** font);

// Render a single character
tty_font_result_t tty_font_render_char(tty_font_t* font, graphics_surface_t* surface,
                                     uint32_t x, uint32_t y, uint32_t codepoint,
                                     graphics_color_t fg_color, graphics_color_t bg_color);

// Measure text
void tty_font_measure_text(tty_font_t* font, const char* text, uint32_t* width, uint32_t* height);

#endif // TTY_FONT_RENDERER_H