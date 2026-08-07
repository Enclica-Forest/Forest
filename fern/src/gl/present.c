#include "present.h"
#include "framebuffer.h"
#include "rasterizer.h"
#include "pixelformat.h"
#include "../arch/framebuffer.h"
#include "../include/framebuffer.h"
#include "../include/debuglog.h"
#include "../include/string.h"

extern gl_framebuffer_t* g_gl_framebuffer;

void gl_present(void) {
    if (!g_gl_framebuffer || !g_gl_framebuffer->color_buffer) return;

    struct fb_info info;
    if (fb_get_dbuf_info(&info) != FB_SUCCESS) {
        if (!framebuffer_is_available()) return;
        uint32_t w, h, pitch, bpp;
        framebuffer_get_info(&w, &h, &pitch, &bpp);
        info.width = w;
        info.height = h;
        info.pitch = pitch;
        info.bpp = bpp;
        info.front = framebuffer_get_buffer();
        if (!info.front) return;
        info.double_buffered = 0;
    }

    void* screen = info.double_buffered ? info.back : info.front;
    if (!screen) return;

    uint32_t src_w = (uint32_t)g_gl_framebuffer->width;
    uint32_t src_h = (uint32_t)g_gl_framebuffer->height;
    uint32_t dst_w = info.width;
    uint32_t dst_h = info.height;

    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;

    uint32_t bpp_bytes = (info.bpp + 7) / 8;
    uint32_t* src = g_gl_framebuffer->color_buffer;
    uint8_t* dst = (uint8_t*)screen;

    if (src_w == dst_w && src_h == dst_h) {
        for (uint32_t y = 0; y < src_h; y++) {
            uint32_t* src_row = src + y * g_gl_framebuffer->stride;
            uint8_t* dst_row = dst + y * info.pitch;

            switch (bpp_bytes) {
            case 4:
                gl_convert_rgba_to_bgra((uint32_t*)dst_row, src_row, (int)src_w);
                break;
            case 3:
                gl_convert_rgba_to_rgb888(dst_row, src_row, (int)src_w);
                break;
            case 2:
                gl_convert_rgba_to_rgb565((uint16_t*)dst_row, src_row, (int)src_w);
                break;
            default:
                break;
            }
        }
    } else {
        uint32_t fx = (src_w << 16) / dst_w;
        uint32_t fy = (src_h << 16) / dst_h;

        for (uint32_t y = 0; y < dst_h; y++) {
            uint32_t sy = (y * fy) >> 16;
            if (sy >= src_h) sy = src_h - 1;
            uint32_t* src_row = src + sy * g_gl_framebuffer->stride;
            uint8_t* dst_row = dst + y * info.pitch;

            switch (bpp_bytes) {
            case 4:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx = (x * fx) >> 16;
                    if (sx >= src_w) sx = src_w - 1;
                    gl_convert_rgba_to_bgra(((uint32_t*)dst_row) + x, src_row + sx, 1);
                }
                break;
            case 3:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx = (x * fx) >> 16;
                    if (sx >= src_w) sx = src_w - 1;
                    gl_convert_rgba_to_rgb888(dst_row + x * 3, src_row + sx, 1);
                }
                break;
            case 2:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx = (x * fx) >> 16;
                    if (sx >= src_w) sx = src_w - 1;
                    gl_convert_rgba_to_rgb565(((uint16_t*)dst_row) + x, src_row + sx, 1);
                }
                break;
            default:
                break;
            }
        }
    }

    if (info.double_buffered) {
        fb_rect_t rect;
        rect.x = 0;
        rect.y = 0;
        rect.w = (int)dst_w;
        rect.h = (int)dst_h;
        fb_invalidate_rect(&rect);
        fb_present(FB_PRESENT_FULL);
    }
}

void gl_present_region(int x, int y, int w, int h) {
    if (!g_gl_framebuffer || !g_gl_framebuffer->color_buffer) return;
    if (w <= 0 || h <= 0) return;

    struct fb_info info;
    if (fb_get_dbuf_info(&info) != FB_SUCCESS) {
        if (!framebuffer_is_available()) return;
        uint32_t fw, fh, fpitch, fbpp;
        framebuffer_get_info(&fw, &fh, &fpitch, &fbpp);
        info.width = fw;
        info.height = fh;
        info.pitch = fpitch;
        info.bpp = fbpp;
        info.front = framebuffer_get_buffer();
        if (!info.front) return;
        info.double_buffered = 0;
    }

    void* screen = info.double_buffered ? info.back : info.front;
    if (!screen) return;

    int32_t src_w = (int32_t)g_gl_framebuffer->width;
    int32_t src_h = (int32_t)g_gl_framebuffer->height;
    int32_t dst_w = (int32_t)info.width;
    int32_t dst_h = (int32_t)info.height;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst_w) w = dst_w - x;
    if (y + h > dst_h) h = dst_h - y;
    if (x + w > src_w) w = src_w - x;
    if (y + h > src_h) h = src_h - y;
    if (w <= 0 || h <= 0) return;

    uint32_t bpp_bytes = (info.bpp + 7) / 8;
    uint32_t* src = g_gl_framebuffer->color_buffer;
    uint8_t* dst = (uint8_t*)screen;

    if (src_w == dst_w && src_h == dst_h) {
        for (int32_t row = 0; row < h; row++) {
            uint32_t* src_row = src + (y + row) * g_gl_framebuffer->stride + x;
            uint8_t* dst_row = dst + (y + row) * info.pitch + x * bpp_bytes;

            switch (bpp_bytes) {
            case 4:
                gl_convert_rgba_to_bgra((uint32_t*)dst_row, src_row, w);
                break;
            case 3:
                gl_convert_rgba_to_rgb888(dst_row, src_row, w);
                break;
            case 2:
                gl_convert_rgba_to_rgb565((uint16_t*)dst_row, src_row, w);
                break;
            default:
                break;
            }
        }
    } else {
        uint32_t fx = (src_w << 16) / dst_w;
        uint32_t fy = (src_h << 16) / dst_h;

        for (int32_t row = 0; row < h; row++) {
            uint32_t sy = (uint32_t)(((y + row) * fy) >> 16);
            if (sy >= (uint32_t)src_h) sy = (uint32_t)src_h - 1;
            uint32_t* src_row = src + sy * g_gl_framebuffer->stride;
            uint8_t* dst_row = dst + (y + row) * info.pitch;

            switch (bpp_bytes) {
            case 4:
                for (int32_t col = 0; col < w; col++) {
                    uint32_t sx = ((x + col) * fx) >> 16;
                    if (sx >= (uint32_t)src_w) sx = (uint32_t)src_w - 1;
                    gl_convert_rgba_to_bgra(((uint32_t*)dst_row) + (x + col), src_row + sx, 1);
                }
                break;
            case 3:
                for (int32_t col = 0; col < w; col++) {
                    uint32_t sx = ((x + col) * fx) >> 16;
                    if (sx >= (uint32_t)src_w) sx = (uint32_t)src_w - 1;
                    gl_convert_rgba_to_rgb888(dst_row + (x + col) * 3, src_row + sx, 1);
                }
                break;
            case 2:
                for (int32_t col = 0; col < w; col++) {
                    uint32_t sx = ((x + col) * fx) >> 16;
                    if (sx >= (uint32_t)src_w) sx = (uint32_t)src_w - 1;
                    gl_convert_rgba_to_rgb565(((uint16_t*)dst_row) + (x + col), src_row + sx, 1);
                }
                break;
            default:
                break;
            }
        }
    }

    if (info.double_buffered) {
        fb_rect_t rect;
        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        fb_invalidate_rect(&rect);
        fb_present(FB_PRESENT_DIRTY);
    }
}
