/*
 * LeafGFX Font Rendering
 *
 * Provides text rendering with both built-in bitmap fonts
 * and TrueType font loading support.
 */

#ifndef LEAFGFX_FONT_H
#define LEAFGFX_FONT_H

#include "leafgfx.h"

// ============================================================================
// Font Types
// ============================================================================

typedef enum {
    GFX_FONT_BUILTIN_8X8,     // 8x8 pixel bitmap font (small)
    GFX_FONT_BUILTIN_8X16,    // 8x16 pixel bitmap font (default)
    GFX_FONT_TTF              // TrueType font
} gfx_font_type_t;

typedef enum {
    GFX_FONT_SUCCESS = 0,
    GFX_FONT_ERROR_INVALID_FILE,
    GFX_FONT_ERROR_UNSUPPORTED_FORMAT,
    GFX_FONT_ERROR_OUT_OF_MEMORY,
    GFX_FONT_ERROR_FILE_NOT_FOUND,
    GFX_FONT_ERROR_INVALID_PARAMETER,
    GFX_FONT_ERROR_GLYPH_NOT_FOUND
} gfx_font_result_t;

// Forward declaration
typedef struct gfx_font gfx_font_t;

// ============================================================================
// Font Loading
// ============================================================================

/**
 * Get the default built-in font (8x16 bitmap)
 *
 * @return      Pointer to the default font (do not free)
 */
const gfx_font_t* gfx_font_get_default(void);

/**
 * Get a built-in font by type
 *
 * @param type  Font type (GFX_FONT_BUILTIN_8X8 or GFX_FONT_BUILTIN_8X16)
 * @return      Pointer to the font (do not free), or NULL if invalid type
 */
const gfx_font_t* gfx_font_get_builtin(gfx_font_type_t type);

/**
 * Load a TrueType font from a file
 *
 * @param path      Path to the TTF file
 * @param size      Font size in pixels (height)
 * @param font      Output: pointer to loaded font (caller must free with gfx_font_free)
 * @return          GFX_FONT_SUCCESS on success, error code otherwise
 */
gfx_font_result_t gfx_font_load_ttf(const char* path, uint32_t size, gfx_font_t** font);

/**
 * Load a TrueType font from memory
 *
 * @param data      Pointer to TTF file data
 * @param data_size Size of the data in bytes
 * @param size      Font size in pixels (height)
 * @param font      Output: pointer to loaded font
 * @return          GFX_FONT_SUCCESS on success, error code otherwise
 */
gfx_font_result_t gfx_font_load_ttf_memory(const uint8_t* data, size_t data_size,
                                           uint32_t size, gfx_font_t** font);

/**
 * Free a loaded font
 *
 * @param font      Font to free (may be NULL, must not be a built-in font)
 */
void gfx_font_free(gfx_font_t* font);

/**
 * Free all dynamically loaded fonts currently tracked by LeafGFX.
 * Useful during global app shutdown paths.
 */
void gfx_font_release_all_tracked(void);

// ============================================================================
// Font Properties
// ============================================================================

/**
 * Get font type
 */
gfx_font_type_t gfx_font_get_type(const gfx_font_t* font);

/**
 * Get font height in pixels
 */
uint32_t gfx_font_get_height(const gfx_font_t* font);

/**
 * Get font line spacing (distance between baselines)
 */
uint32_t gfx_font_get_line_spacing(const gfx_font_t* font);

/**
 * Get the width of a character
 */
uint32_t gfx_font_get_char_width(const gfx_font_t* font, char c);

/**
 * Get the width of a string in pixels
 */
uint32_t gfx_font_get_text_width(const gfx_font_t* font, const char* text);

/**
 * Get text bounds (width and height)
 */
void gfx_font_get_text_bounds(const gfx_font_t* font, const char* text,
                              uint32_t* width, uint32_t* height);

// ============================================================================
// Text Rendering
// ============================================================================

/**
 * Draw a single character
 *
 * @param font      Font to use
 * @param x         X position
 * @param y         Y position
 * @param c         Character to draw
 * @param color     Text color (ARGB)
 * @return          Width of the character drawn
 */
uint32_t gfx_draw_char(const gfx_font_t* font, int32_t x, int32_t y, char c, uint32_t color);

/**
 * Draw a string of text
 *
 * @param font      Font to use
 * @param x         X position
 * @param y         Y position
 * @param text      Text string to draw
 * @param color     Text color (ARGB)
 */
void gfx_draw_text(const gfx_font_t* font, int32_t x, int32_t y,
                   const char* text, uint32_t color);

/**
 * Draw text centered horizontally within a width
 *
 * @param font      Font to use
 * @param x         X position (left edge of centering area)
 * @param y         Y position
 * @param width     Width of centering area
 * @param text      Text string to draw
 * @param color     Text color (ARGB)
 */
void gfx_draw_text_centered(const gfx_font_t* font, int32_t x, int32_t y,
                            uint32_t width, const char* text, uint32_t color);

/**
 * Draw text right-aligned within a width
 *
 * @param font      Font to use
 * @param x         X position (left edge of alignment area)
 * @param y         Y position
 * @param width     Width of alignment area
 * @param text      Text string to draw
 * @param color     Text color (ARGB)
 */
void gfx_draw_text_right(const gfx_font_t* font, int32_t x, int32_t y,
                         uint32_t width, const char* text, uint32_t color);

/**
 * Draw text centered within a rectangle (both horizontally and vertically)
 *
 * @param font      Font to use
 * @param rect      Rectangle to center within
 * @param text      Text string to draw
 * @param color     Text color (ARGB)
 */
void gfx_draw_text_centered_rect(const gfx_font_t* font, const gfx_rect_t* rect,
                                 const char* text, uint32_t color);

/**
 * Draw text with word wrapping
 *
 * @param font      Font to use
 * @param x         X position
 * @param y         Y position
 * @param max_width Maximum width before wrapping
 * @param text      Text string to draw
 * @param color     Text color (ARGB)
 * @return          Total height of rendered text
 */
uint32_t gfx_draw_text_wrapped(const gfx_font_t* font, int32_t x, int32_t y,
                               uint32_t max_width, const char* text, uint32_t color);

/**
 * Draw text with a shadow effect
 *
 * @param font          Font to use
 * @param x             X position
 * @param y             Y position
 * @param text          Text string to draw
 * @param color         Text color (ARGB)
 * @param shadow_color  Shadow color (ARGB)
 * @param shadow_offset Shadow offset in pixels
 */
void gfx_draw_text_shadow(const gfx_font_t* font, int32_t x, int32_t y,
                          const char* text, uint32_t color,
                          uint32_t shadow_color, int32_t shadow_offset);

/**
 * Draw text with an outline
 *
 * @param font          Font to use
 * @param x             X position
 * @param y             Y position
 * @param text          Text string to draw
 * @param color         Text color (ARGB)
 * @param outline_color Outline color (ARGB)
 */
void gfx_draw_text_outline(const gfx_font_t* font, int32_t x, int32_t y,
                           const char* text, uint32_t color, uint32_t outline_color);

int32_t gfx_font_get_kerning(const gfx_font_t* font, uint32_t left_cp, uint32_t right_cp);

void gfx_draw_text_justify(const gfx_font_t* font, int32_t x, int32_t y,
                            uint32_t width, const char* text, uint32_t color);

// ============================================================================
// Detailed Font Metrics
// ============================================================================

int32_t gfx_font_get_ascent(const gfx_font_t* font);
int32_t gfx_font_get_descent(const gfx_font_t* font);
int32_t gfx_font_get_line_gap(const gfx_font_t* font);

// ============================================================================
// Font Fallback Chain
// ============================================================================

void gfx_font_set_fallback(gfx_font_t* font, gfx_font_t* fallback);
const gfx_font_t* gfx_font_get_fallback(const gfx_font_t* font);

// ============================================================================
// Font Configuration
// ============================================================================

void gfx_font_set_subpixel(gfx_font_t* font, bool enabled);
bool gfx_font_get_subpixel(const gfx_font_t* font);

void gfx_font_set_monospace(gfx_font_t* font, bool is_mono, uint32_t width);
bool gfx_font_is_monospace(const gfx_font_t* font);

void gfx_font_set_emoji(gfx_font_t* font, const gfx_font_t* emoji_font);
const gfx_font_t* gfx_font_get_emoji(const gfx_font_t* font);

// ============================================================================
// Font Size Scaling
// ============================================================================

gfx_font_result_t gfx_font_create_scaled(const gfx_font_t* source, uint32_t size, gfx_font_t** out);

// ============================================================================
// Font Loading from Initrd
// ============================================================================

gfx_font_result_t gfx_font_load_ttf_initrd(const char* path, uint32_t size, gfx_font_t** font);

// ============================================================================
// Glyph Atlas Cache
// ============================================================================

void gfx_font_clear_glyph_cache(gfx_font_t* font);
uint32_t gfx_font_get_glyph_cache_size(const gfx_font_t* font);

// ============================================================================
// Bidirectional Text
// ============================================================================

bool gfx_unicode_is_rtl(uint32_t codepoint);
void gfx_draw_text_bidi(const gfx_font_t* font, int32_t x, int32_t y,
                        const char* text, uint32_t max_width, uint32_t color);

// ============================================================================
// Convenience Functions (using default font)
// ============================================================================

/**
 * Draw text using the default font
 */
void gfx_text(int32_t x, int32_t y, const char* text, uint32_t color);

/**
 * Draw centered text using the default font
 */
void gfx_text_centered(int32_t cx, int32_t y, const char* text, uint32_t color);

/**
 * Get text width using the default font
 */
uint32_t gfx_text_width(const char* text);

#endif // LEAFGFX_FONT_H
