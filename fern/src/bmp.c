#include "include/bmp.h"
#include "include/vfs.h"
#include "include/graphics/graphics_manager.h"
#include "include/graphics/graphics_types.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/mm.h"
#include "include/memory.h"

extern void* kmalloc(size_t size);
extern void* krealloc(void* ptr, size_t new_size);
extern void  kfree(void* ptr);

// Enable stb_image implementation
#define STBI_MALLOC(sz)        kmalloc(sz)
#define STBI_REALLOC(p, newsz) krealloc(p, newsz)
#define STBI_FREE(p)           kfree(p)
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// BMP signature: "BM" in little-endian
#define BMP_SIGNATURE 0x4D42

// PNG signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A
#define PNG_SIGNATURE_0 0x89
#define PNG_SIGNATURE_1 0x50

// Check if data is a PNG file
static bool is_png_file(const uint8* data, uint32 size) {
    return size >= 8 && 
           data[0] == PNG_SIGNATURE_0 && 
           data[1] == PNG_SIGNATURE_1;
}

// Load PNG using stb_image
static bmp_result_t load_png_from_memory(const uint8* data, uint32 size, bmp_image_t** image) {
    if (!data || size < 8 || !image) {
        return BMP_ERROR_INVALID_PARAMETER;
    }

    // stb_image functions are already declared in stb_image.h
    int width, height, channels;
    stbi_uc* pixels = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
    
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        debuglog(DEBUG_ERROR, "[BMP] stbi_load_from_memory failed for PNG (%s)\n",
                 reason ? reason : "unknown");
        return BMP_ERROR_DECODER_UNAVAILABLE;
    }

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        stbi_image_free(pixels);
        debuglog(DEBUG_ERROR, "[BMP] PNG dimensions invalid: %dx%d\n", width, height);
        return BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    uint64 pixel_count_64 = (uint64)(uint32)width * (uint64)(uint32)height;
    if (pixel_count_64 > (128ULL * 1024ULL * 1024ULL) / 4ULL) {
        stbi_image_free(pixels);
        debuglog(DEBUG_ERROR, "[BMP] PNG image too large: %ux%u\n", (uint32)width, (uint32)height);
        return BMP_ERROR_OUT_OF_MEMORY;
    }
    
    // Allocate image structure
    bmp_image_t* img = (bmp_image_t*)kmalloc(sizeof(bmp_image_t));
    if (!img) {
        stbi_image_free(pixels);
        return BMP_ERROR_OUT_OF_MEMORY;
    }
    
    // Convert RGBA to BGRA for consistency with BMP format
    uint32 pixel_count = (uint32)pixel_count_64;
    uint8* bgra_pixels = (uint8*)kmalloc(pixel_count * 4);
    if (!bgra_pixels) {
        stbi_image_free(pixels);
        kfree(img);
        return BMP_ERROR_OUT_OF_MEMORY;
    }
    
    // Swap R and B channels
    for (uint32 i = 0; i < pixel_count; i++) {
        bgra_pixels[i * 4 + 0] = pixels[i * 4 + 2]; // B from R
        bgra_pixels[i * 4 + 1] = pixels[i * 4 + 1]; // G
        bgra_pixels[i * 4 + 2] = pixels[i * 4 + 0]; // R from B
        bgra_pixels[i * 4 + 3] = pixels[i * 4 + 3]; // A
    }
    
    stbi_image_free(pixels);
    
    img->width = (uint32)width;
    img->height = (uint32)height;
    img->bpp = 32;
    img->row_stride = (uint32)(width * 4);
    img->top_down = true; // PNG is always top-down
    img->format = 2; // PNG format
    img->pixel_data = bgra_pixels;
    img->pixel_data_size = pixel_count * 4;
    img->owns_data = true;
    
    *image = img;
    return BMP_SUCCESS;
}

// Compression types
#define BMP_COMPRESSION_NONE 0
#define BMP_COMPRESSION_RLE8 1
#define BMP_COMPRESSION_RLE4 2
#define BMP_COMPRESSION_BITFIELDS 3

static bmp_result_t parse_bmp_header(const uint8* data, uint32 size, 
                                     bmp_file_header_t* file_header,
                                     bmp_info_header_t* info_header,
                                     uint32* pixel_data_offset) {
    if (!data || size < 54) {  // Minimum size for file header + info header
        return BMP_ERROR_INVALID_FILE;
    }

    // Parse file header
    file_header->signature = *(uint16*)data;
    if (file_header->signature != BMP_SIGNATURE) {
        debuglog(DEBUG_ERROR, "[BMP] Invalid signature: 0x%04X\n", file_header->signature);
        return BMP_ERROR_INVALID_FILE;
    }

    file_header->file_size = *(uint32*)(data + 2);
    file_header->reserved1 = *(uint16*)(data + 6);
    file_header->reserved2 = *(uint16*)(data + 8);
    file_header->data_offset = *(uint32*)(data + 10);

    // Parse info header
    info_header->header_size = *(uint32*)(data + 14);
    if (info_header->header_size < 40) {
        debuglog(DEBUG_ERROR, "[BMP] Unsupported header size: %u\n", info_header->header_size);
        return BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    info_header->width = *(int32*)(data + 18);
    info_header->height = *(int32*)(data + 22);
    info_header->planes = *(uint16*)(data + 26);
    info_header->bits_per_pixel = *(uint16*)(data + 28);
    info_header->compression = *(uint32*)(data + 30);
    info_header->image_size = *(uint32*)(data + 34);
    info_header->x_resolution = *(int32*)(data + 38);
    info_header->y_resolution = *(int32*)(data + 42);
    info_header->colors_used = *(uint32*)(data + 46);
    info_header->important_colors = *(uint32*)(data + 50);

    // Validate
    if (info_header->planes != 1) {
        debuglog(DEBUG_ERROR, "[BMP] Invalid planes: %u\n", info_header->planes);
        return BMP_ERROR_INVALID_FILE;
    }

    if (info_header->compression != BMP_COMPRESSION_NONE) {
        debuglog(DEBUG_ERROR, "[BMP] Compression not supported: %u\n", info_header->compression);
        return BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    // Only support 24-bit and 32-bit BMPs for now
    if (info_header->bits_per_pixel != 24 && info_header->bits_per_pixel != 32) {
        debuglog(DEBUG_ERROR, "[BMP] Unsupported bits per pixel: %u\n", info_header->bits_per_pixel);
        return BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    *pixel_data_offset = file_header->data_offset;
    return BMP_SUCCESS;
}

bmp_result_t bmp_load_from_file(const char* path, bmp_image_t** image) {
    if (!path || !image) {
        return BMP_ERROR_INVALID_PARAMETER;
    }

    *image = NULL;

    // Read file from VFS
    const uint8* file_data = NULL;
    uint32 file_size = 0;
    if (!vfs_read_file(path, &file_data, &file_size)) {
        debuglog(DEBUG_ERROR, "[BMP] File not found: %s\n", path);
        return BMP_ERROR_FILE_NOT_FOUND;
    }

    // Check if it's a PNG file first
    if (is_png_file(file_data, file_size)) {
        debuglog(DEBUG_INFO, "[BMP] Detected PNG file: %s\n", path);
        bmp_result_t result = load_png_from_memory(file_data, file_size, image);
        if (result == BMP_SUCCESS) {
            debuglog(DEBUG_INFO, "[BMP] Loaded PNG: %ux%u, %ubpp\n", 
                     (*image)->width, (*image)->height, (*image)->bpp);
        }
        return result;
    }

    if (file_size < 54) {
        debuglog(DEBUG_ERROR, "[BMP] File too small: %u bytes\n", file_size);
        return BMP_ERROR_INVALID_FILE;
    }

    // Parse headers
    bmp_file_header_t file_header;
    bmp_info_header_t info_header;
    uint32 pixel_data_offset;
    
    bmp_result_t result = parse_bmp_header(file_data, file_size, &file_header, 
                                         &info_header, &pixel_data_offset);
    if (result != BMP_SUCCESS) {
        // Try to detect if it's a PNG that we couldn't parse as BMP
        if (is_png_file(file_data, file_size)) {
            debuglog(DEBUG_WARN, "[BMP] BMP parsing failed, trying PNG: %s\n", path);
            return load_png_from_memory(file_data, file_size, image);
        }
        return result;
    }

    // Determine if image is top-down (negative height) or bottom-up (positive height)
    bool top_down = (info_header.height < 0);
    uint32 width = (uint32)(info_header.width < 0 ? -info_header.width : info_header.width);
    uint32 height = (uint32)(info_header.height < 0 ? -info_header.height : info_header.height);

    // Calculate row size (must be aligned to 4-byte boundary)
    uint32 bytes_per_pixel = info_header.bits_per_pixel / 8;
    uint32 row_size = width * bytes_per_pixel;
    uint32 row_padding = (4 - (row_size % 4)) % 4;  // Padding to align to 4 bytes
    uint32 row_size_with_padding = row_size + row_padding;
    uint32 pixel_data_size = row_size_with_padding * height;

    // Check for reasonable image size limits (prevent excessive memory allocation)
    if (width > 16384 || height > 16384) {
        debuglog(DEBUG_ERROR, "[BMP] Image dimensions too large: %ux%u (max 16384x16384)\n", width, height);
        return BMP_ERROR_UNSUPPORTED_FORMAT;
    }

    if (pixel_data_size > 128 * 1024 * 1024) {
        debuglog(DEBUG_ERROR, "[BMP] Image data too large: %u bytes (max 128MB)\n", pixel_data_size);
        return BMP_ERROR_OUT_OF_MEMORY;
    }

    // Check if we have enough data
    if (pixel_data_offset + pixel_data_size > file_size) {
        debuglog(DEBUG_ERROR, "[BMP] File too small for image data\n");
        return BMP_ERROR_INVALID_FILE;
    }

    // Allocate image structure
    bmp_image_t* img = (bmp_image_t*)kmalloc(sizeof(bmp_image_t));
    if (!img) {
        debuglog(DEBUG_ERROR, "[BMP] Out of memory for image structure\n");
        return BMP_ERROR_OUT_OF_MEMORY;
    }
    img->pixel_data = NULL;
    img->pixel_data_size = 0;
    img->owns_data = false;

    // Allocate pixel data buffer
    uint8* pixel_buffer = (uint8*)kmalloc(pixel_data_size);
    if (!pixel_buffer) {
        debuglog(DEBUG_WARN, "[BMP] OOM for pixel data (%u bytes) for %s, using zero-copy view\n",
                 pixel_data_size, path);
        img->pixel_data = (uint8*)(file_data + pixel_data_offset);
        img->owns_data = false;
    } else {
        memcpy(pixel_buffer, file_data + pixel_data_offset, pixel_data_size);
        img->pixel_data = pixel_buffer;
        img->owns_data = true;
    }

    img->width = width;
    img->height = height;
    img->bpp = info_header.bits_per_pixel;
    img->top_down = top_down;
    img->pixel_data_size = pixel_data_size;

    *image = img;

    debuglog(DEBUG_INFO, "[BMP] Loaded image: %s (%ux%u, %ubpp, %s)\n", 
             path, width, height, info_header.bits_per_pixel, 
             top_down ? "top-down" : "bottom-up");

    return BMP_SUCCESS;
}

void bmp_free(bmp_image_t* image) {
    if (!image) {
        return;
    }

    if (image->pixel_data && image->owns_data) {
        kfree(image->pixel_data);
    }
    kfree(image);
}

static graphics_result_t bmp_draw_pixel_row(bmp_image_t* image, int32_t x, int32_t y, 
                                           uint32 row_index, uint32 target_width, 
                                           uint32 target_height, bool scaled) {
    if (!image || !image->pixel_data) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    uint32 bytes_per_pixel = image->bpp / 8;
    uint32 row_size = image->width * bytes_per_pixel;
    uint32 row_padding = (4 - (row_size % 4)) % 4;
    uint32 row_size_with_padding = row_size + row_padding;

    // Calculate source row (handle top-down vs bottom-up)
    uint32 src_row;
    if (scaled && target_height > 0) {
        uint32 sy = (row_index * image->height) / target_height;
        if (sy >= image->height) {
            sy = image->height - 1;
        }
        src_row = image->top_down ? sy : (image->height - 1 - sy);
    } else {
        if (row_index >= image->height) {
            return GRAPHICS_ERROR_INVALID_PARAMETER;
        }
        src_row = image->top_down ? row_index : (image->height - 1 - row_index);
    }

    const uint8* row_data = image->pixel_data + (src_row * row_size_with_padding);

    if (scaled && target_width > 0 && target_height > 0) {
        // Scaled drawing
        for (uint32 tx = 0; tx < target_width; tx++) {
            uint32 sx = (tx * image->width) / target_width;
            if (sx >= image->width) sx = image->width - 1;

            const uint8* pixel = row_data + (sx * bytes_per_pixel);
            uint8 b = pixel[0];
            uint8 g = pixel[1];
            uint8 r = pixel[2];
            uint8 a = (bytes_per_pixel == 4) ? pixel[3] : 255;

            graphics_color_t color = graphics_make_color(r, g, b, a);
            uint32 target_x = x + tx;
            uint32 target_y = y + row_index;
            graphics_draw_pixel((int32_t)target_x, (int32_t)target_y, color);
        }
    } else {
        // 1:1 drawing
        for (uint32 sx = 0; sx < image->width; sx++) {
            const uint8* pixel = row_data + (sx * bytes_per_pixel);
            uint8 b = pixel[0];
            uint8 g = pixel[1];
            uint8 r = pixel[2];
            uint8 a = (bytes_per_pixel == 4) ? pixel[3] : 255;

            graphics_color_t color = graphics_make_color(r, g, b, a);
            graphics_draw_pixel(x + (int32_t)sx, y + (int32_t)row_index, color);
        }
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t bmp_draw_image(bmp_image_t* image, int32_t x, int32_t y) {
    if (!image) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    for (uint32 row = 0; row < image->height; row++) {
        bmp_draw_pixel_row(image, x, y, row, 0, 0, false);
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t bmp_draw_image_scaled(bmp_image_t* image, int32_t x, int32_t y, 
                                        uint32_t target_width, uint32_t target_height) {
    if (!image || target_width == 0 || target_height == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    for (uint32 row = 0; row < target_height; row++) {
        bmp_draw_pixel_row(image, x, y, row, target_width, target_height, true);
    }

    return GRAPHICS_SUCCESS;
}
