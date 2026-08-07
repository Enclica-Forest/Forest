/*
 * LeafGFX BMP Image Loading
 *
 * Provides BMP image loading and rendering for userspace applications.
 * Supports 24-bit and 32-bit uncompressed BMP files.
 */

#ifndef LEAFGFX_BMP_H
#define LEAFGFX_BMP_H

#include "leafgfx.h"

// ============================================================================
// Result Codes
// ============================================================================

typedef enum {
    GFX_BMP_SUCCESS = 0,
    GFX_BMP_ERROR_INVALID_FILE,
    GFX_BMP_ERROR_UNSUPPORTED_FORMAT,
    GFX_BMP_ERROR_OUT_OF_MEMORY,
    GFX_BMP_ERROR_FILE_NOT_FOUND,
    GFX_BMP_ERROR_INVALID_PARAMETER,
    GFX_BMP_ERROR_READ_ERROR
} gfx_bmp_result_t;

// ============================================================================
// Image Structure
// ============================================================================

typedef struct {
    uint32_t  width;           // Image width in pixels
    uint32_t  height;          // Image height in pixels
    uint32_t  bpp;             // Bits per pixel (24 or 32)
    uint32_t* pixels;          // Pixel data in ARGB format (owned by this struct)
    uint32_t  pixel_count;     // Total number of pixels
} gfx_image_t;

// ============================================================================
// Image Loading Functions
// ============================================================================

/**
 * Load a BMP image from a file path
 *
 * @param path      Path to the BMP file (e.g., "/usr/share/images/bg.bmp")
 * @param image     Output: pointer to image structure (caller must free with gfx_image_free)
 * @return          GFX_BMP_SUCCESS on success, error code otherwise
 */
gfx_bmp_result_t gfx_image_load_bmp(const char* path, gfx_image_t** image);

/**
 * Load an image from file using format auto-detection.
 *
 * Supported formats:
 *  - BMP (native loader)
 *  - PNG
 *  - GIF (first frame)
 *  - JPEG/JPG
 *
 * @param path      Path to image file
 * @param image     Output image (caller owns, free with gfx_image_free)
 * @return          GFX_BMP_SUCCESS on success, error code otherwise
 */
gfx_bmp_result_t gfx_image_load(const char* path, gfx_image_t** image);

/**
 * Load a BMP image from memory
 *
 * @param data      Pointer to BMP file data in memory
 * @param size      Size of the data in bytes
 * @param image     Output: pointer to image structure
 * @return          GFX_BMP_SUCCESS on success, error code otherwise
 */
gfx_bmp_result_t gfx_image_load_bmp_memory(const uint8_t* data, size_t size, gfx_image_t** image);

/**
 * Load an image from memory using format auto-detection.
 *
 * @param data         Encoded image bytes
 * @param size         Byte size of data
 * @param path_hint    Optional path/name hint for extension-based fast path (may be NULL)
 * @param image        Output image
 * @return             GFX_BMP_SUCCESS on success, error code otherwise
 */
gfx_bmp_result_t gfx_image_load_memory(const uint8_t* data, size_t size,
                                       const char* path_hint, gfx_image_t** image);

/**
 * Free an image and its pixel data
 *
 * @param image     Image to free (may be NULL)
 */
void gfx_image_free(gfx_image_t* image);

/**
 * Free all images currently tracked by LeafGFX.
 * Useful during global app shutdown paths.
 */
void gfx_image_release_all_tracked(void);

// ============================================================================
// Image Creation Functions
// ============================================================================

/**
 * Create a blank image with the specified dimensions
 *
 * @param width     Image width in pixels
 * @param height    Image height in pixels
 * @param color     Initial fill color (ARGB format)
 * @return          Pointer to new image, or NULL on failure
 */
gfx_image_t* gfx_image_create(uint32_t width, uint32_t height, uint32_t color);

/**
 * Create a copy of an existing image
 *
 * @param src       Source image to copy
 * @return          Pointer to new image, or NULL on failure
 */
gfx_image_t* gfx_image_clone(const gfx_image_t* src);

// ============================================================================
// Image Drawing Functions
// ============================================================================

/**
 * Draw an image at the specified position
 *
 * @param image     Image to draw
 * @param x         X position (top-left corner)
 * @param y         Y position (top-left corner)
 */
void gfx_image_draw(const gfx_image_t* image, int32_t x, int32_t y);

/**
 * Draw an image with alpha blending
 *
 * @param image     Image to draw
 * @param x         X position
 * @param y         Y position
 */
void gfx_image_draw_blend(const gfx_image_t* image, int32_t x, int32_t y);

/**
 * Draw an image scaled to a target size
 *
 * @param image         Image to draw
 * @param x             X position
 * @param y             Y position
 * @param target_width  Target width in pixels
 * @param target_height Target height in pixels
 */
void gfx_image_draw_scaled(const gfx_image_t* image, int32_t x, int32_t y,
                           uint32_t target_width, uint32_t target_height);

/**
 * Draw a portion of an image (source rectangle)
 *
 * @param image     Image to draw
 * @param src_x     Source X offset
 * @param src_y     Source Y offset
 * @param src_w     Source width
 * @param src_h     Source height
 * @param dst_x     Destination X position
 * @param dst_y     Destination Y position
 */
void gfx_image_draw_region(const gfx_image_t* image,
                           int32_t src_x, int32_t src_y,
                           int32_t src_w, int32_t src_h,
                           int32_t dst_x, int32_t dst_y);

/**
 * Draw an image with opacity adjustment
 *
 * @param image     Image to draw
 * @param x         X position
 * @param y         Y position
 * @param opacity   Opacity (0-255, 0=transparent, 255=opaque)
 */
void gfx_image_draw_opacity(const gfx_image_t* image, int32_t x, int32_t y, uint8_t opacity);

/**
 * Draw an image tinted with a color
 *
 * @param image     Image to draw
 * @param x         X position
 * @param y         Y position
 * @param tint      Tint color (multiplied with image colors)
 */
void gfx_image_draw_tinted(const gfx_image_t* image, int32_t x, int32_t y, uint32_t tint);

void gfx_image_draw_scaled_bilinear(const gfx_image_t* image, int32_t x, int32_t y,
                                     uint32_t target_width, uint32_t target_height);

// ============================================================================
// Image Manipulation Functions
// ============================================================================

/**
 * Get a pixel from an image
 *
 * @param image     Image
 * @param x         X coordinate
 * @param y         Y coordinate
 * @return          Pixel value in ARGB format, or 0 if out of bounds
 */
static inline uint32_t gfx_image_get_pixel(const gfx_image_t* image, int32_t x, int32_t y) {
    if (!image || !image->pixels ||
        x < 0 || x >= (int32_t)image->width ||
        y < 0 || y >= (int32_t)image->height) {
        return 0;
    }
    return image->pixels[y * image->width + x];
}

/**
 * Set a pixel in an image
 *
 * @param image     Image
 * @param x         X coordinate
 * @param y         Y coordinate
 * @param color     Pixel value in ARGB format
 */
static inline void gfx_image_set_pixel(gfx_image_t* image, int32_t x, int32_t y, uint32_t color) {
    if (!image || !image->pixels ||
        x < 0 || x >= (int32_t)image->width ||
        y < 0 || y >= (int32_t)image->height) {
        return;
    }
    image->pixels[y * image->width + x] = color;
}

/**
 * Fill an image with a solid color
 *
 * @param image     Image
 * @param color     Fill color in ARGB format
 */
void gfx_image_fill(gfx_image_t* image, uint32_t color);

/**
 * Apply a Gaussian blur to an image
 *
 * @param image     Image to blur (modified in place)
 * @param radius    Blur radius (1-10 recommended)
 */
void gfx_image_blur(gfx_image_t* image, int32_t radius);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Get the last error message from BMP loading
 *
 * @return          Error message string, or NULL if no error
 */
const char* gfx_bmp_get_error(void);

#endif // LEAFGFX_BMP_H
