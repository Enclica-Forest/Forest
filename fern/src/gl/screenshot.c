#include "rasterizer.h"
#include "present.h"
#include "../arch/framebuffer.h"
#include "../include/debuglog.h"
#include <stdint.h>

static inline uint32_t gl_rgba_to_fb(uint32_t rgba) {
    uint8_t r = (rgba >> 0)  & 0xFF;
    uint8_t g = (rgba >> 8)  & 0xFF;
    uint8_t b = (rgba >> 16) & 0xFF;
    uint8_t a = (rgba >> 24) & 0xFF;

    return ((uint32_t)b) | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
}

void gl_screenshot(void) {
    gl_framebuffer_t* fb = g_gl_framebuffer;
    if (!fb || !fb->color_buffer) {
        debuglog(DEBUG_WARN, "[GL] No framebuffer for screenshot\n");
        return;
    }

    if (!framebuffer_is_available()) {
        debuglog(DEBUG_WARN, "[GL] No screen framebuffer available\n");
        return;
    }

    uint32_t screen_w, screen_h, screen_pitch, screen_bpp;
    framebuffer_get_info(&screen_w, &screen_h, &screen_pitch, &screen_bpp);

    void* screen_buf = framebuffer_get_buffer();
    if (!screen_buf) {
        debuglog(DEBUG_WARN, "[GL] Screen buffer not mapped\n");
        return;
    }

    uint32_t bpp_bytes = (screen_bpp + 7) / 8;
    uint32_t src_w = (uint32_t)fb->width;
    uint32_t src_h = (uint32_t)fb->height;
    uint32_t dst_w = screen_w;
    uint32_t dst_h = screen_h;

    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;

    uint8_t* dst = (uint8_t*)screen_buf;

    if (src_w == dst_w && src_h == dst_h) {
        for (uint32_t y = 0; y < src_h; y++) {
            uint32_t* src_row = fb->color_buffer + y * fb->stride;
            uint8_t* dst_row = dst + y * screen_pitch;

            switch (bpp_bytes) {
            case 4:
                for (uint32_t x = 0; x < src_w; x++) {
                    ((uint32_t*)dst_row)[x] = gl_rgba_to_fb(src_row[x]);
                }
                break;
            case 3:
                for (uint32_t x = 0; x < src_w; x++) {
                    uint32_t c = gl_rgba_to_fb(src_row[x]);
                    dst_row[x * 3 + 0] = (c >> 0) & 0xFF;
                    dst_row[x * 3 + 1] = (c >> 8) & 0xFF;
                    dst_row[x * 3 + 2] = (c >> 16) & 0xFF;
                }
                break;
            case 2:
                for (uint32_t x = 0; x < src_w; x++) {
                    uint32_t c = gl_rgba_to_fb(src_row[x]);
                    ((uint16_t*)dst_row)[x] = (uint16_t)(((c >> 3) << 11) | (((c >> 8) & 0xFF) >> 2 << 5) | ((c >> 16) & 0xFF) >> 3);
                }
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
            uint32_t* src_row = fb->color_buffer + sy * fb->stride;
            uint8_t* dst_row = dst + y * screen_pitch;

            switch (bpp_bytes) {
            case 4:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx = (x * fx) >> 16;
                    if (sx >= src_w) sx = src_w - 1;
                    ((uint32_t*)dst_row)[x] = gl_rgba_to_fb(src_row[sx]);
                }
                break;
            case 3:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx = (x * fx) >> 16;
                    if (sx >= src_w) sx = src_w - 1;
                    uint32_t c = gl_rgba_to_fb(src_row[sx]);
                    dst_row[x * 3 + 0] = (c >> 0) & 0xFF;
                    dst_row[x * 3 + 1] = (c >> 8) & 0xFF;
                    dst_row[x * 3 + 2] = (c >> 16) & 0xFF;
                }
                break;
            case 2:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t sx = (x * fx) >> 16;
                    if (sx >= src_w) sx = src_w - 1;
                    uint32_t c = gl_rgba_to_fb(src_row[sx]);
                    ((uint16_t*)dst_row)[x] = (uint16_t)(((c >> 3) << 11) | (((c >> 8) & 0xFF) >> 2 << 5) | ((c >> 16) & 0xFF) >> 3);
                }
                break;
            default:
                break;
            }
        }
    }

    debuglog(DEBUG_INFO, "[GL] Screenshot blitted %ux%u -> %ux%u\n",
             src_w, src_h, dst_w, dst_h);
}
