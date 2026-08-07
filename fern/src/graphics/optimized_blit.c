#include "../include/graphics/optimized_blit.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

static blit_cache_t g_blit_cache = {0};
static blit_config_t g_blit_config = {
    .use_simd = false,
    .use_prefetch = true,
    .use_non_temporal = false,
    .prefetch_distance = 64,
    .unroll_factor = 4
};

graphics_result_t optimized_blit_init(void) {
    memset(&g_blit_cache, 0, sizeof(g_blit_cache));
    g_blit_cache.enabled = true;
    
    debuglog(DEBUG_INFO, "Optimized blit subsystem initialized\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t optimized_blit_shutdown(void) {
    g_blit_cache.enabled = false;
    return GRAPHICS_SUCCESS;
}

static inline uint32_t calc_checksum(const uint8_t* data, uint32_t size) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; i += 4) {
        sum += *((const uint32_t*)(data + i));
    }
    return sum;
}

graphics_result_t optimized_blit_surface(const graphics_surface_t* src,
                                      graphics_surface_t* dst,
                                      const graphics_rect_t* src_rect,
                                      int32_t dst_x, int32_t dst_y,
                                      blit_mode_t mode) {
    if (!src || !dst || !src->pixels || !dst->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    graphics_rect_t r = src_rect ? *src_rect :
        (graphics_rect_t){0, 0, src->width, src->height};
    
    if (r.x >= src->width || r.y >= src->height ||
        r.x + r.width > src->width ||
        r.y + r.height > src->height) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    int32_t clipped_dst_x = dst_x < 0 ? -dst_x : 0;
    int32_t clipped_dst_y = dst_y < 0 ? -dst_y : 0;
    
    if (clipped_dst_x >= (int32_t)r.width ||
        clipped_dst_y >= (int32_t)r.height) {
        return GRAPHICS_SUCCESS;
    }
    
    r.x += clipped_dst_x;
    r.y += clipped_dst_y;
    r.width -= clipped_dst_x;
    r.height -= clipped_dst_y;
    
    if (dst_x + (int32_t)r.width > (int32_t)dst->width) {
        r.width = dst->width - dst_x;
    }
    if (dst_y + (int32_t)r.height > (int32_t)dst->height) {
        r.height = dst->height - dst_y;
    }
    
    if (r.width == 0 || r.height == 0) {
        return GRAPHICS_SUCCESS;
    }
    
    uint32_t bytes_per_pixel = src->bpp / 8;
    
    if (mode == BLIT_MODE_COPY && bytes_per_pixel == 4) {
        for (uint32_t y = 0; y < r.height; y++) {
            const uint32_t* src_row = (const uint32_t*)(
                (const uint8_t*)src->pixels + (r.y + y) * src->pitch + r.x * 4);
            uint32_t* dst_row = (uint32_t*)(
                (uint8_t*)dst->pixels + (dst_y + y) * dst->pitch + dst_x * 4);
            
            if (g_blit_config.use_prefetch && r.width >= 16) {
                for (uint32_t x = 0; x < r.width; x += 16) {
                    prefetch_cache_line(src_row + x);
                }
            }
            
            for (uint32_t x = 0; x < r.width; x++) {
                dst_row[x] = src_row[x];
            }
        }
    } else {
        for (uint32_t y = 0; y < r.height; y++) {
            const uint8_t* src_row = (const uint8_t*)src->pixels +
                (r.y + y) * src->pitch + r.x * bytes_per_pixel;
            uint8_t* dst_row = (uint8_t*)dst->pixels +
                (dst_y + y) * dst->pitch + dst_x * bytes_per_pixel;
            memcpy(dst_row, src_row, r.width * bytes_per_pixel);
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t optimized_blit_surface_alpha(const graphics_surface_t* src,
                                            graphics_surface_t* dst,
                                            const graphics_rect_t* src_rect,
                                            int32_t dst_x, int32_t dst_y,
                                            uint8_t alpha) {
    if (!src || !dst || alpha == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    if (alpha == 255) {
        return optimized_blit_surface(src, dst, src_rect, dst_x, dst_y, BLIT_MODE_COPY);
    }
    
    graphics_rect_t r = src_rect ? *src_rect :
        (graphics_rect_t){0, 0, src->width, src->height};
    
    if (dst_x < 0) { int32_t off = -dst_x; r.x += off; r.width -= off; dst_x = 0; }
    if (dst_y < 0) { int32_t off = -dst_y; r.y += off; r.height -= off; dst_y = 0; }
    
    uint32_t bytes_per_pixel = dst->bpp / 8;
    if (bytes_per_pixel != 4) return GRAPHICS_ERROR_NOT_SUPPORTED;
    
    for (uint32_t y = 0; y < r.height; y++) {
        const uint32_t* src_row = (const uint32_t*)(
            (const uint8_t*)src->pixels + (r.y + y) * src->pitch + r.x * 4);
        uint32_t* dst_row = (uint32_t*)(
            (uint8_t*)dst->pixels + (dst_y + y) * dst->pitch + dst_x * 4);
        
        for (uint32_t x = 0; x < r.width; x++) {
            uint32_t src_pix = src_row[x];
            uint32_t dst_pix = dst_row[x];
            
            uint8_t src_r = (src_pix >> 16) & 0xFF;
            uint8_t src_g = (src_pix >> 8) & 0xFF;
            uint8_t src_b = src_pix & 0xFF;
            
            uint8_t dst_r = (dst_pix >> 16) & 0xFF;
            uint8_t dst_g = (dst_pix >> 8) & 0xFF;
            uint8_t dst_b = dst_pix & 0xFF;
            
            uint8_t out_r = (uint8_t)((src_r * alpha + dst_r * (255 - alpha)) / 255);
            uint8_t out_g = (uint8_t)((src_g * alpha + dst_g * (255 - alpha)) / 255);
            uint8_t out_b = (uint8_t)((src_b * alpha + dst_b * (255 - alpha)) / 255);
            
            dst_row[x] = (out_r << 16) | (out_g << 8) | out_b;
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t optimized_blit_surface_scaled(const graphics_surface_t* src,
                                              graphics_surface_t* dst,
                                              const graphics_rect_t* src_rect,
                                              int32_t dst_x, int32_t dst_y,
                                              uint32_t dst_width, uint32_t dst_height) {
    if (!src || !dst || !src->pixels || !dst->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    graphics_rect_t r = src_rect ? *src_rect :
        (graphics_rect_t){0, 0, src->width, src->height};
    
    if (r.width == 0 || r.height == 0 || dst_width == 0 || dst_height == 0) {
        return GRAPHICS_SUCCESS;
    }
    
    uint32_t x_ratio = (r.width << 16) / dst_width;
    uint32_t y_ratio = (r.height << 16) / dst_height;
    uint32_t bytes_per_pixel = src->bpp / 8;
    
    for (uint32_t dst_y_pos = 0; dst_y_pos < dst_height; dst_y_pos++) {
        uint32_t src_y = (dst_y_pos * y_ratio) >> 16;
        const uint8_t* src_row = (const uint8_t*)src->pixels +
            (r.y + src_y) * src->pitch + r.x * bytes_per_pixel;
        uint8_t* dst_row = (uint8_t*)dst->pixels +
            (dst_y + dst_y_pos) * dst->pitch + dst_x * bytes_per_pixel;
        
        for (uint32_t dst_x_pos = 0; dst_x_pos < dst_width; dst_x_pos++) {
            uint32_t src_x = (dst_x_pos * x_ratio) >> 16;
            
            for (uint32_t byte = 0; byte < bytes_per_pixel; byte++) {
                dst_row[dst_x_pos * bytes_per_pixel + byte] =
                    src_row[src_x * bytes_per_pixel + byte];
            }
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t optimized_fill_rect(graphics_surface_t* surface,
                                     const graphics_rect_t* rect,
                                     graphics_color_t color) {
    if (!surface || !rect || !surface->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    graphics_rect_t r = {
        rect->x >= 0 ? rect->x : 0,
        rect->y >= 0 ? rect->y : 0,
        rect->width,
        rect->height
    };
    
    if (r.x + r.width > surface->width) {
        r.width = surface->width - r.x;
    }
    if (r.y + r.height > surface->height) {
        r.height = surface->height - r.y;
    }
    
    if (r.width == 0 || r.height == 0) {
        return GRAPHICS_SUCCESS;
    }
    
    uint32_t bytes_per_pixel = surface->bpp / 8;
    
    if (bytes_per_pixel == 4) {
        uint32_t pixel_value = (color.a << 24) | (color.r << 16) |
                            (color.g << 8) | color.b;
        
        for (uint32_t y = 0; y < r.height; y++) {
            uint32_t* row = (uint32_t*)(
                (uint8_t*)surface->pixels + (r.y + y) * surface->pitch + r.x * 4);
            for (uint32_t x = 0; x < r.width; x++) {
                row[x] = pixel_value;
            }
        }
    } else if (bytes_per_pixel == 2) {
        uint16_t pixel_value = 0;
        switch (surface->format) {
            case PIXEL_FORMAT_RGB_565:
                pixel_value = ((color.r >> 3) << 11) |
                             ((color.g >> 2) << 5) |
                             (color.b >> 3);
                break;
            case PIXEL_FORMAT_RGB_555:
                pixel_value = ((color.r >> 3) << 10) |
                             ((color.g >> 3) << 5) |
                             (color.b >> 3);
                break;
            default:
                return GRAPHICS_ERROR_NOT_SUPPORTED;
        }
        
        for (uint32_t y = 0; y < r.height; y++) {
            uint16_t* row = (uint16_t*)(
                (uint8_t*)surface->pixels + (r.y + y) * surface->pitch + r.x * 2);
            for (uint32_t x = 0; x < r.width; x++) {
                row[x] = pixel_value;
            }
        }
    } else {
        for (uint32_t y = 0; y < r.height; y++) {
            for (uint32_t x = 0; x < r.width; x++) {
                uint8_t* pixel = (uint8_t*)surface->pixels +
                    (r.y + y) * surface->pitch + (r.x + x) * bytes_per_pixel;
                
                switch (bytes_per_pixel) {
                    case 1:
                        *pixel = color.r;
                        break;
                    case 3:
                        pixel[0] = color.b;
                        pixel[1] = color.g;
                        pixel[2] = color.r;
                        break;
                    default:
                        return GRAPHICS_ERROR_NOT_SUPPORTED;
                }
            }
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t optimized_fill_pattern(graphics_surface_t* surface,
                                       const graphics_rect_t* rect,
                                       const uint32_t* pattern,
                                       uint32_t pattern_width,
                                       uint32_t pattern_height) {
    if (!surface || !rect || !pattern || pattern_width == 0 || pattern_height == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t bytes_per_pixel = surface->bpp / 8;
    if (bytes_per_pixel != 4) {
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }
    
    for (uint32_t y = 0; y < rect->height; y++) {
        uint32_t pattern_y = (rect->y + y) % pattern_height;
        uint32_t* dst_row = (uint32_t*)(
            (uint8_t*)surface->pixels + (rect->y + y) * surface->pitch + rect->x * 4);
        
        for (uint32_t x = 0; x < rect->width; x++) {
            uint32_t pattern_x = (rect->x + x) % pattern_width;
            dst_row[x] = pattern[pattern_y * pattern_width + pattern_x];
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t optimized_copy_rect(const graphics_surface_t* src,
                                   graphics_surface_t* dst,
                                   const graphics_rect_t* src_rect,
                                   int32_t dst_x, int32_t dst_y) {
    return optimized_blit_surface(src, dst, src_rect, dst_x, dst_y, BLIT_MODE_COPY);
}

graphics_result_t optimized_blit_scanline(const uint8_t* src, uint8_t* dst,
                                        uint32_t width, uint32_t bytes_per_pixel,
                                        blit_mode_t mode) {
    if (!src || !dst || width == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t byte_width = width * bytes_per_pixel;
    
    if (mode == BLIT_MODE_COPY) {
        if (is_aligned16(src) && is_aligned16(dst) && (byte_width % 2 == 0)) {
            uint64_t* src64 = (uint64_t*)src;
            uint64_t* dst64 = (uint64_t*)dst;
            uint32_t count = byte_width / 8;
            
            for (uint32_t i = 0; i < count; i++) {
                dst64[i] = src64[i];
            }
            
            uint8_t* src8 = (uint8_t*)src + count * 8;
            uint8_t* dst8 = (uint8_t*)dst + count * 8;
            uint32_t remaining = byte_width % 8;
            
            for (uint32_t i = 0; i < remaining; i++) {
                dst8[i] = src8[i];
            }
        } else {
            memcpy(dst, src, byte_width);
        }
    } else if (mode == BLIT_MODE_BLEND && bytes_per_pixel == 4) {
        const uint32_t* src32 = (const uint32_t*)src;
        uint32_t* dst32 = (uint32_t*)dst;
        
        for (uint32_t x = 0; x < width; x++) {
            uint32_t src_pix = src32[x];
            uint32_t dst_pix = dst32[x];
            
            uint8_t alpha = (src_pix >> 24) & 0xFF;
            if (alpha == 0) continue;
            if (alpha == 255) {
                dst32[x] = src_pix;
                continue;
            }
            
            uint8_t src_r = (src_pix >> 16) & 0xFF;
            uint8_t src_g = (src_pix >> 8) & 0xFF;
            uint8_t src_b = src_pix & 0xFF;
            
            uint8_t dst_r = (dst_pix >> 16) & 0xFF;
            uint8_t dst_g = (dst_pix >> 8) & 0xFF;
            uint8_t dst_b = dst_pix & 0xFF;
            
            uint8_t out_r = (uint8_t)((src_r * alpha + dst_r * (255 - alpha)) / 255);
            uint8_t out_g = (uint8_t)((src_g * alpha + dst_g * (255 - alpha)) / 255);
            uint8_t out_b = (uint8_t)((src_b * alpha + dst_b * (255 - alpha)) / 255);
            
            dst32[x] = (out_r << 16) | (out_g << 8) | out_b;
        }
    } else {
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }
    
    return GRAPHICS_SUCCESS;
}
