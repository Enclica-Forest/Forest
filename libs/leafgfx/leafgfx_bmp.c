/*
 * LeafGFX BMP Image Loading - Implementation
 *
 * Provides BMP image loading and rendering for userspace applications.
 */

#include "leafgfx_bmp.h"
#include "leafgfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// BMP File Format Structures
// ============================================================================

#define BMP_SIGNATURE 0x4D42  // "BM" in little-endian

// BMP file header (14 bytes)
typedef struct __attribute__((packed)) {
    uint16_t signature;      // "BM" (0x4D42)
    uint32_t file_size;      // Size of the BMP file
    uint16_t reserved1;      // Reserved
    uint16_t reserved2;      // Reserved
    uint32_t data_offset;    // Offset to start of pixel data
} bmp_file_header_t;

// BMP info header (40 bytes for BITMAPINFOHEADER)
typedef struct __attribute__((packed)) {
    uint32_t header_size;    // Size of this header (40 bytes)
    int32_t  width;          // Image width in pixels
    int32_t  height;         // Image height (positive=bottom-up, negative=top-down)
    uint16_t planes;         // Number of color planes (must be 1)
    uint16_t bits_per_pixel; // Bits per pixel (24 or 32)
    uint32_t compression;    // Compression method (0 = none)
    uint32_t image_size;     // Image size in bytes (0 for uncompressed)
    int32_t  x_resolution;   // Horizontal resolution (pixels per meter)
    int32_t  y_resolution;   // Vertical resolution (pixels per meter)
    uint32_t colors_used;    // Number of colors in palette (0 = all)
    uint32_t important_colors; // Number of important colors (0 = all)
} bmp_info_header_t;

// ============================================================================
// Error Handling
// ============================================================================

static char g_error_msg[256] = {0};
static gfx_image_t* g_tracked_images[1024] = {0};
static size_t g_tracked_image_count = 0;

static void set_error(const char* msg) {
    strncpy(g_error_msg, msg, sizeof(g_error_msg) - 1);
    g_error_msg[sizeof(g_error_msg) - 1] = '\0';
}

const char* gfx_bmp_get_error(void) {
    return g_error_msg[0] ? g_error_msg : NULL;
}

static void track_image(gfx_image_t* image) {
    if (!image) {
        return;
    }

    for (size_t i = 0; i < g_tracked_image_count; i++) {
        if (g_tracked_images[i] == image) {
            return;
        }
    }

    if (g_tracked_image_count < (sizeof(g_tracked_images) / sizeof(g_tracked_images[0]))) {
        g_tracked_images[g_tracked_image_count++] = image;
        return;
    }

    printf("[GFX_BMP] WARNING: image tracker full; image will not be tracked\n");
}

static void untrack_image(gfx_image_t* image) {
    if (!image) {
        return;
    }

    for (size_t i = 0; i < g_tracked_image_count; i++) {
        if (g_tracked_images[i] != image) {
            continue;
        }

        g_tracked_image_count--;
        g_tracked_images[i] = g_tracked_images[g_tracked_image_count];
        g_tracked_images[g_tracked_image_count] = NULL;
        return;
    }
}

static void free_image_internal(gfx_image_t* image) {
    if (!image) {
        return;
    }
    if (image->pixels) {
        free(image->pixels);
    }
    free(image);
}

// ============================================================================
// BMP Loading from Memory
// ============================================================================

gfx_bmp_result_t gfx_image_load_bmp_memory(const uint8_t* data, size_t size, gfx_image_t** image) {
    if (!data || !image) {
        set_error("Invalid parameter");
        return GFX_BMP_ERROR_INVALID_PARAMETER;
    }

    *image = NULL;

    // Check minimum file size
    if (size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
        set_error("File too small to be a valid BMP");
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    // Parse file header
    const bmp_file_header_t* file_hdr = (const bmp_file_header_t*)data;
    if (file_hdr->signature != BMP_SIGNATURE) {
        set_error("Invalid BMP signature");
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    // Parse info header
    const bmp_info_header_t* info_hdr = (const bmp_info_header_t*)(data + sizeof(bmp_file_header_t));

    // Validate header
    if (info_hdr->header_size < 40) {
        set_error("Unsupported BMP header size");
        return GFX_BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    if (info_hdr->planes != 1) {
        set_error("Invalid number of planes");
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    if (info_hdr->compression != 0) {
        set_error("Compressed BMPs not supported");
        return GFX_BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    // Only support 24-bit and 32-bit BMPs
    if (info_hdr->bits_per_pixel != 24 && info_hdr->bits_per_pixel != 32) {
        set_error("Only 24-bit and 32-bit BMPs supported");
        return GFX_BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    // Get dimensions (height can be negative for top-down)
    int64_t width_i = (int64_t)info_hdr->width;
    int64_t height_i = (int64_t)info_hdr->height;
    if (width_i < 0) width_i = -width_i;
    if (height_i < 0) height_i = -height_i;
    uint32_t width = (uint32_t)width_i;
    uint32_t height = (uint32_t)height_i;
    bool top_down = (info_hdr->height < 0);

    // Sanity check dimensions
    if (width_i <= 0 || height_i <= 0 || width_i > 16384 || height_i > 16384) {
        set_error("Invalid or unsupported image dimensions");
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    // Calculate row size with padding (rows are aligned to 4 bytes)
    uint32_t bytes_per_pixel = info_hdr->bits_per_pixel / 8;
    size_t row_size = (size_t)width * (size_t)bytes_per_pixel;
    size_t row_padding = (4 - (row_size % 4)) % 4;
    size_t row_stride = row_size + row_padding;
    size_t pixel_bytes = row_stride * (size_t)height;

    // Verify we have enough data
    if ((size_t)file_hdr->data_offset > size || pixel_bytes > (size - (size_t)file_hdr->data_offset)) {
        set_error("File truncated");
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    // Allocate image structure
    gfx_image_t* img = (gfx_image_t*)malloc(sizeof(gfx_image_t));
    if (!img) {
        set_error("Out of memory");
        return GFX_BMP_ERROR_OUT_OF_MEMORY;
    }

    img->width = width;
    img->height = height;
    img->bpp = 32; // We always convert to 32-bit ARGB
    img->pixel_count = width * height;
    if ((size_t)img->pixel_count > (((size_t)-1) / sizeof(uint32_t))) {
        free(img);
        set_error("Image too large");
        return GFX_BMP_ERROR_OUT_OF_MEMORY;
    }
    img->pixels = (uint32_t*)malloc((size_t)img->pixel_count * sizeof(uint32_t));

    if (!img->pixels) {
        free(img);
        set_error("Out of memory for pixel data");
        return GFX_BMP_ERROR_OUT_OF_MEMORY;
    }

    // Parse pixel data
    const uint8_t* pixel_data = data + file_hdr->data_offset;

    for (uint32_t y = 0; y < height; y++) {
        // BMP rows are stored bottom-up by default, top-down if height is negative
        uint32_t src_y = top_down ? y : (height - 1 - y);
        const uint8_t* row = pixel_data + src_y * row_stride;

        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* pixel = row + x * bytes_per_pixel;

            // BMP stores pixels in BGR(A) order
            uint8_t b = pixel[0];
            uint8_t g = pixel[1];
            uint8_t r = pixel[2];
            uint8_t a = (bytes_per_pixel == 4) ? pixel[3] : 255;

            // Store as ARGB
            img->pixels[y * width + x] = ((uint32_t)a << 24) |
                                         ((uint32_t)r << 16) |
                                         ((uint32_t)g << 8) |
                                         (uint32_t)b;
        }
    }

    *image = img;
    track_image(img);
    g_error_msg[0] = '\0';
    return GFX_BMP_SUCCESS;
}

// ============================================================================
// Path Normalization
// ============================================================================

// Ensure path starts with / for proper VFS resolution
static const char* normalize_path(const char* path, char* buffer, size_t buffer_size) {
    if (!path || !buffer || buffer_size < 2) return path;
    
    // Skip any leading whitespace
    while (*path == ' ' || *path == '\t') path++;
    
    // If path already starts with /, return as-is
    if (path[0] == '/') {
        return path;
    }
    
    // Prepend / to the path
    size_t len = strlen(path);
    if (len + 2 > buffer_size) {
        return path;  // Buffer too small, return original
    }
    
    buffer[0] = '/';
    memcpy(buffer + 1, path, len + 1);  // Include null terminator
    return buffer;
}

// ============================================================================
// BMP Loading from File
// ============================================================================

gfx_bmp_result_t gfx_image_load_bmp(const char* path, gfx_image_t** image) {
    if (!path || !image) {
        set_error("Invalid parameter");
        return GFX_BMP_ERROR_INVALID_PARAMETER;
    }

    *image = NULL;
    
    // Normalize path to ensure it starts with /
    char normalized_path[512];
    const char* final_path = normalize_path(path, normalized_path, sizeof(normalized_path));
    
    printf("[GFX_BMP] Loading: %s\n", final_path);

    // Open file
    FILE* fp = fopen(final_path, "rb");
    if (!fp) {
        printf("[GFX_BMP] fopen failed for: %s\n", final_path);
        
        // Try the original path if normalization changed it
        if (final_path != path) {
            printf("[GFX_BMP] Trying original path: %s\n", path);
            fp = fopen(path, "rb");
        }
        
        // Try without leading slash if it had one
        if (!fp && path[0] == '/') {
            printf("[GFX_BMP] Trying without leading slash: %s\n", path + 1);
            fp = fopen(path + 1, "rb");
        }
        
        if (!fp) {
            printf("[GFX_BMP] All path attempts failed for: %s\n", path);
            set_error("Could not open file");
            return GFX_BMP_ERROR_FILE_NOT_FOUND;
        }
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 128 * 1024 * 1024) { // Max 128MB
        fclose(fp);
        set_error("Invalid file size");
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    // Allocate buffer and read file
    uint8_t* data = (uint8_t*)malloc(file_size);
    if (!data) {
        fclose(fp);
        set_error("Out of memory");
        return GFX_BMP_ERROR_OUT_OF_MEMORY;
    }

    size_t bytes_read = fread(data, 1, file_size, fp);
    fclose(fp);

    if (bytes_read != (size_t)file_size) {
        free(data);
        set_error("Failed to read entire file");
        return GFX_BMP_ERROR_READ_ERROR;
    }

    // Load from memory
    gfx_bmp_result_t result = gfx_image_load_bmp_memory(data, file_size, image);
    free(data);

    if (result == GFX_BMP_SUCCESS && *image) {
        printf("[GFX_BMP] Loaded successfully: %ux%u %ubpp\n", 
               (*image)->width, (*image)->height, (*image)->bpp);
    } else {
        printf("[GFX_BMP] Load failed with error: %d\n", result);
    }

    return result;
}

// ============================================================================
// Image Creation and Destruction
// ============================================================================

gfx_image_t* gfx_image_create(uint32_t width, uint32_t height, uint32_t color) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        return NULL;
    }

    gfx_image_t* img = (gfx_image_t*)malloc(sizeof(gfx_image_t));
    if (!img) return NULL;

    img->width = width;
    img->height = height;
    img->bpp = 32;
    img->pixel_count = width * height;
    if ((size_t)img->pixel_count > (((size_t)-1) / sizeof(uint32_t))) {
        free(img);
        return NULL;
    }
    img->pixels = (uint32_t*)malloc((size_t)img->pixel_count * sizeof(uint32_t));

    if (!img->pixels) {
        free(img);
        return NULL;
    }

    // Fill with initial color
    for (uint32_t i = 0; i < img->pixel_count; i++) {
        img->pixels[i] = color;
    }

    track_image(img);
    return img;
}

gfx_image_t* gfx_image_clone(const gfx_image_t* src) {
    if (!src || !src->pixels) return NULL;

    gfx_image_t* img = (gfx_image_t*)malloc(sizeof(gfx_image_t));
    if (!img) return NULL;

    img->width = src->width;
    img->height = src->height;
    img->bpp = src->bpp;
    img->pixel_count = src->pixel_count;
    if ((size_t)img->pixel_count > (((size_t)-1) / sizeof(uint32_t))) {
        free(img);
        return NULL;
    }
    img->pixels = (uint32_t*)malloc((size_t)img->pixel_count * sizeof(uint32_t));

    if (!img->pixels) {
        free(img);
        return NULL;
    }

    memcpy(img->pixels, src->pixels, (size_t)img->pixel_count * sizeof(uint32_t));
    track_image(img);
    return img;
}

void gfx_image_free(gfx_image_t* image) {
    if (!image) {
        return;
    }

    untrack_image(image);
    free_image_internal(image);
}

void gfx_image_release_all_tracked(void) {
    while (g_tracked_image_count > 0) {
        gfx_image_t* image = g_tracked_images[g_tracked_image_count - 1];
        g_tracked_images[g_tracked_image_count - 1] = NULL;
        g_tracked_image_count--;
        free_image_internal(image);
    }
}

// ============================================================================
// Image Drawing Functions
// ============================================================================

void gfx_image_draw(const gfx_image_t* image, int32_t x, int32_t y) {
    if (!image || !image->pixels) return;

    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) return;

    // Clamp drawing to framebuffer bounds
    int32_t start_x = x < 0 ? 0 : x;
    int32_t start_y = y < 0 ? 0 : y;
    int32_t end_x = x + image->width > (int32_t)fb->width ? fb->width : x + image->width;
    int32_t end_y = y + image->height > (int32_t)fb->height ? fb->height : y + image->height;

    for (int32_t py = start_y; py < end_y; py++) {
        uint32_t src_y = py - y;
        for (int32_t px = start_x; px < end_x; px++) {
            uint32_t src_x = px - x;
            uint32_t color = image->pixels[src_y * image->width + src_x];
            gfx_pixel(px, py, color);
        }
    }
}

void gfx_image_draw_blend(const gfx_image_t* image, int32_t x, int32_t y) {
    if (!image || !image->pixels) return;

    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) return;

    // Clamp drawing to framebuffer bounds
    int32_t start_x = x < 0 ? 0 : x;
    int32_t start_y = y < 0 ? 0 : y;
    int32_t end_x = x + image->width > (int32_t)fb->width ? fb->width : x + image->width;
    int32_t end_y = y + image->height > (int32_t)fb->height ? fb->height : y + image->height;

    for (int32_t py = start_y; py < end_y; py++) {
        uint32_t src_y = py - y;
        for (int32_t px = start_x; px < end_x; px++) {
            uint32_t src_x = px - x;
            uint32_t color = image->pixels[src_y * image->width + src_x];
            gfx_pixel_blend(px, py, color);
        }
    }
}

void gfx_image_draw_scaled(const gfx_image_t* image, int32_t x, int32_t y,
                           uint32_t target_width, uint32_t target_height) {
    if (!image || !image->pixels || target_width == 0 || target_height == 0) return;

    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) return;

    // Clamp drawing to framebuffer bounds
    int32_t start_x = x < 0 ? 0 : x;
    int32_t start_y = y < 0 ? 0 : y;
    int32_t end_x = x + target_width > (int32_t)fb->width ? fb->width : x + target_width;
    int32_t end_y = y + target_height > (int32_t)fb->height ? fb->height : y + target_height;

    for (int32_t ty = start_y; ty < end_y; ty++) {
        uint32_t sy = ((uint32_t)(ty - y) * image->height) / target_height;
        if (sy >= image->height) sy = image->height - 1;

        for (int32_t tx = start_x; tx < end_x; tx++) {
            uint32_t sx = ((uint32_t)(tx - x) * image->width) / target_width;
            if (sx >= image->width) sx = image->width - 1;

            uint32_t color = image->pixels[sy * image->width + sx];
            gfx_pixel_blend(tx, ty, color);
        }
    }
}

void gfx_image_draw_region(const gfx_image_t* image,
                           int32_t src_x, int32_t src_y,
                           int32_t src_w, int32_t src_h,
                           int32_t dst_x, int32_t dst_y) {
    if (!image || !image->pixels) return;

    for (int32_t py = 0; py < src_h; py++) {
        int32_t sy = src_y + py;
        if (sy < 0 || sy >= (int32_t)image->height) continue;

        for (int32_t px = 0; px < src_w; px++) {
            int32_t sx = src_x + px;
            if (sx < 0 || sx >= (int32_t)image->width) continue;

            uint32_t color = image->pixels[sy * image->width + sx];
            gfx_pixel_blend(dst_x + px, dst_y + py, color);
        }
    }
}

void gfx_image_draw_opacity(const gfx_image_t* image, int32_t x, int32_t y, uint8_t opacity) {
    if (!image || !image->pixels || opacity == 0) return;

    for (uint32_t py = 0; py < image->height; py++) {
        for (uint32_t px = 0; px < image->width; px++) {
            uint32_t color = image->pixels[py * image->width + px];
            uint8_t a = (color >> 24) & 0xFF;

            // Multiply alpha by opacity
            a = (uint8_t)((a * opacity) / 255);
            color = (color & 0x00FFFFFF) | ((uint32_t)a << 24);

            gfx_pixel_blend(x + px, y + py, color);
        }
    }
}

void gfx_image_draw_tinted(const gfx_image_t* image, int32_t x, int32_t y, uint32_t tint) {
    if (!image || !image->pixels) return;

    uint8_t tr = (tint >> 16) & 0xFF;
    uint8_t tg = (tint >> 8) & 0xFF;
    uint8_t tb = tint & 0xFF;

    for (uint32_t py = 0; py < image->height; py++) {
        for (uint32_t px = 0; px < image->width; px++) {
            uint32_t color = image->pixels[py * image->width + px];
            uint8_t a = (color >> 24) & 0xFF;
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            // Multiply by tint
            r = (uint8_t)((r * tr) / 255);
            g = (uint8_t)((g * tg) / 255);
            b = (uint8_t)((b * tb) / 255);

            color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            gfx_pixel_blend(x + px, y + py, color);
        }
    }
}

// ============================================================================
// Image Manipulation Functions
// ============================================================================

void gfx_image_fill(gfx_image_t* image, uint32_t color) {
    if (!image || !image->pixels) return;

    for (uint32_t i = 0; i < image->pixel_count; i++) {
        image->pixels[i] = color;
    }
}

void gfx_image_blur(gfx_image_t* image, int32_t radius) {
    if (!image || !image->pixels || radius <= 0) return;

    // Clamp radius
    if (radius > 10) radius = 10;

    // Create temporary buffer
    uint32_t* temp = (uint32_t*)malloc(image->pixel_count * sizeof(uint32_t));
    if (!temp) return;

    // Simple box blur (horizontal pass)
    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int32_t count = 0;

            for (int32_t dx = -radius; dx <= radius; dx++) {
                int32_t sx = (int32_t)x + dx;
                if (sx >= 0 && sx < (int32_t)image->width) {
                    uint32_t pixel = image->pixels[y * image->width + sx];
                    a += (pixel >> 24) & 0xFF;
                    r += (pixel >> 16) & 0xFF;
                    g += (pixel >> 8) & 0xFF;
                    b += pixel & 0xFF;
                    count++;
                }
            }

            if (count > 0) {
                temp[y * image->width + x] = ((a / count) << 24) |
                                             ((r / count) << 16) |
                                             ((g / count) << 8) |
                                             (b / count);
            }
        }
    }

    // Vertical pass
    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int32_t count = 0;

            for (int32_t dy = -radius; dy <= radius; dy++) {
                int32_t sy = (int32_t)y + dy;
                if (sy >= 0 && sy < (int32_t)image->height) {
                    uint32_t pixel = temp[sy * image->width + x];
                    a += (pixel >> 24) & 0xFF;
                    r += (pixel >> 16) & 0xFF;
                    g += (pixel >> 8) & 0xFF;
                    b += pixel & 0xFF;
                    count++;
                }
            }

            if (count > 0) {
                image->pixels[y * image->width + x] = ((a / count) << 24) |
                                                      ((r / count) << 16) |
                                                      ((g / count) << 8) |
                                                      (b / count);
            }
        }
    }

    free(temp);
}

void gfx_image_draw_scaled_bilinear(const gfx_image_t* image, int32_t x, int32_t y,
                                     uint32_t target_width, uint32_t target_height) {
    if (!image || !image->pixels || target_width == 0 || target_height == 0) return;

    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) return;

    int32_t start_x = x < 0 ? 0 : x;
    int32_t start_y = y < 0 ? 0 : y;
    int32_t end_x = x + (int32_t)target_width > (int32_t)fb->width
                  ? (int32_t)fb->width : x + (int32_t)target_width;
    int32_t end_y = y + (int32_t)target_height > (int32_t)fb->height
                  ? (int32_t)fb->height : y + (int32_t)target_height;

    for (int32_t ty = start_y; ty < end_y; ty++) {
        int64_t src_y_fp = (int64_t)(ty - y) * image->height / target_height;
        int32_t sy0 = (int32_t)(src_y_fp >> 16);
        int32_t fy = (int32_t)(src_y_fp & 0xFFFF);

        if (sy0 < 0) sy0 = 0;
        if (sy0 >= (int32_t)image->height - 1) {
            if (image->height > 1) {
                sy0 = (int32_t)image->height - 2;
            } else {
                sy0 = 0;
                fy = 0;
            }
        }

        int32_t sy1 = sy0 + 1;
        if (sy1 >= (int32_t)image->height) sy1 = sy0;

        for (int32_t tx = start_x; tx < end_x; tx++) {
            int64_t src_x_fp = (int64_t)(tx - x) * image->width / target_width;
            int32_t sx0 = (int32_t)(src_x_fp >> 16);
            int32_t fx = (int32_t)(src_x_fp & 0xFFFF);

            if (sx0 < 0) sx0 = 0;
            if (sx0 >= (int32_t)image->width - 1) {
                if (image->width > 1) {
                    sx0 = (int32_t)image->width - 2;
                } else {
                    sx0 = 0;
                    fx = 0;
                }
            }

            int32_t sx1 = sx0 + 1;
            if (sx1 >= (int32_t)image->width) sx1 = sx0;

            uint32_t c00 = image->pixels[sy0 * image->width + sx0];
            uint32_t c10 = image->pixels[sy0 * image->width + sx1];
            uint32_t c01 = image->pixels[sy1 * image->width + sx0];
            uint32_t c11 = image->pixels[sy1 * image->width + sx1];

            uint32_t inv_fx = 0x10000 - fx;
            uint32_t inv_fy = 0x10000 - fy;

            uint32_t a00 = (c00 >> 24) & 0xFF;
            uint32_t r00 = (c00 >> 16) & 0xFF;
            uint32_t g00 = (c00 >> 8) & 0xFF;
            uint32_t b00 = c00 & 0xFF;

            uint32_t a10 = (c10 >> 24) & 0xFF;
            uint32_t r10 = (c10 >> 16) & 0xFF;
            uint32_t g10 = (c10 >> 8) & 0xFF;
            uint32_t b10 = c10 & 0xFF;

            uint32_t a01 = (c01 >> 24) & 0xFF;
            uint32_t r01 = (c01 >> 16) & 0xFF;
            uint32_t g01 = (c01 >> 8) & 0xFF;
            uint32_t b01 = c01 & 0xFF;

            uint32_t a11 = (c11 >> 24) & 0xFF;
            uint32_t r11 = (c11 >> 16) & 0xFF;
            uint32_t g11 = (c11 >> 8) & 0xFF;
            uint32_t b11 = c11 & 0xFF;

            uint32_t top_r = (r00 * inv_fx + r10 * fx) >> 16;
            uint32_t top_g = (g00 * inv_fx + g10 * fx) >> 16;
            uint32_t top_b = (b00 * inv_fx + b10 * fx) >> 16;
            uint32_t top_a = (a00 * inv_fx + a10 * fx) >> 16;

            uint32_t bot_r = (r01 * inv_fx + r11 * fx) >> 16;
            uint32_t bot_g = (g01 * inv_fx + g11 * fx) >> 16;
            uint32_t bot_b = (b01 * inv_fx + b11 * fx) >> 16;
            uint32_t bot_a = (a01 * inv_fx + a11 * fx) >> 16;

            uint32_t r = (top_r * inv_fy + bot_r * fy) >> 16;
            uint32_t g = (top_g * inv_fy + bot_g * fy) >> 16;
            uint32_t b = (top_b * inv_fy + bot_b * fy) >> 16;
            uint32_t a = (top_a * inv_fy + bot_a * fy) >> 16;

            uint32_t color = (a << 24) | (r << 16) | (g << 8) | b;
            gfx_pixel_blend(tx, ty, color);
        }
    }
}
