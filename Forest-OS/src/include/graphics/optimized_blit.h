#ifndef OPTIMIZED_BLIT_H
#define OPTIMIZED_BLIT_H

#include "graphics_types.h"

#define BLIT_CACHE_SIZE 64
#define BLIT_BLOCK_SIZE 64
#define BLIT_ALIGNMENT 16

typedef enum {
    BLIT_MODE_COPY,
    BLIT_MODE_BLEND,
    BLIT_MODE_ADD,
    BLIT_MODE_MULTIPLY,
    BLIT_MODE_ALPHA_MASK
} blit_mode_t;

typedef struct {
    const uint8_t* src_data;
    uint8_t* dst_data;
    uint32_t src_pitch;
    uint32_t dst_pitch;
    uint32_t bytes_per_pixel;
    pixel_format_t format;
} blit_context_t;

typedef struct {
    bool enabled;
    struct {
        uint32_t src_x, src_y, width, height;
        uint32_t checksum;
    } cache[BLIT_CACHE_SIZE];
    uint32_t cache_index;
} blit_cache_t;

typedef struct {
    bool use_simd;
    bool use_prefetch;
    bool use_non_temporal;
    uint32_t prefetch_distance;
    uint32_t unroll_factor;
} blit_config_t;

graphics_result_t optimized_blit_init(void);
graphics_result_t optimized_blit_shutdown(void);

graphics_result_t optimized_blit_surface(const graphics_surface_t* src,
                                      graphics_surface_t* dst,
                                      const graphics_rect_t* src_rect,
                                      int32_t dst_x, int32_t dst_y,
                                      blit_mode_t mode);

graphics_result_t optimized_blit_surface_alpha(const graphics_surface_t* src,
                                            graphics_surface_t* dst,
                                            const graphics_rect_t* src_rect,
                                            int32_t dst_x, int32_t dst_y,
                                            uint8_t alpha);

graphics_result_t optimized_blit_surface_scaled(const graphics_surface_t* src,
                                              graphics_surface_t* dst,
                                              const graphics_rect_t* src_rect,
                                              int32_t dst_x, int32_t dst_y,
                                              uint32_t dst_width, uint32_t dst_height);

graphics_result_t optimized_fill_rect(graphics_surface_t* surface,
                                     const graphics_rect_t* rect,
                                     graphics_color_t color);

graphics_result_t optimized_fill_pattern(graphics_surface_t* surface,
                                       const graphics_rect_t* rect,
                                       const uint32_t* pattern,
                                       uint32_t pattern_width,
                                       uint32_t pattern_height);

graphics_result_t optimized_copy_rect(const graphics_surface_t* src,
                                   graphics_surface_t* dst,
                                   const graphics_rect_t* src_rect,
                                   int32_t dst_x, int32_t dst_y);

graphics_result_t optimized_blit_scanline(const uint8_t* src, uint8_t* dst,
                                        uint32_t width, uint32_t bytes_per_pixel,
                                        blit_mode_t mode);

static inline bool is_aligned(const void* ptr, uint32_t alignment) {
    return ((uintptr_t)ptr & (alignment - 1)) == 0;
}

static inline bool is_aligned16(const void* ptr) {
    return is_aligned(ptr, 16);
}

static inline bool is_aligned64(const void* ptr) {
    return is_aligned(ptr, 64);
}

static inline void write_pixel_32(uint8_t* dst, uint32_t value) {
    *(uint32_t*)dst = value;
}

static inline uint32_t read_pixel_32(const uint8_t* src) {
    return *(const uint32_t*)src;
}

static inline void write_pixel_16(uint8_t* dst, uint16_t value) {
    *(uint16_t*)dst = value;
}

static inline uint16_t read_pixel_16(const uint8_t* src) {
    return *(const uint16_t*)src;
}

static inline void prefetch_cache_line(const void* addr) {
    __asm__ volatile ("prefetcht0 %0" : : "m" (*(const char*)addr));
}

static inline void prefetch_for_write(const void* addr) {
    __asm__ volatile ("prefetchw %0" : : "m" (*(char*)addr));
}

#endif // OPTIMIZED_BLIT_H
