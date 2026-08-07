#include "../include/graphics/gpu_accel.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/graphics_driver_v2.h"
#include "../include/debuglog.h"
#include "../include/mm.h"
#include "../include/pci.h"
#include "../include/string.h"

static gpu_accel_context_t accel_ctx = {
    .type = GPU_ACCEL_NONE,
    .available = false,
    .hw_cursor = false,
    .hw_blend = false,
    .hw_scale = false,
    .vram_size = 0,
    .max_texture_size = 0,
    .fill_rect = NULL,
    .blit = NULL,
    .blend = NULL,
    .set_cursor = NULL,
    .move_cursor = NULL,
    .swap_buffers = NULL,
};

static gpu_accel_type_t detect_gpu_type(uint16_t vendor_id, uint16_t device_id) {
    switch (vendor_id) {
        case 0x1022:
        case 0x1002:
            return GPU_ACCEL_AMD;
        case 0x10DE:
            return GPU_ACCEL_NVIDIA;
        case 0x8086:
            return GPU_ACCEL_INTEL;
        case 0x15AD:
            return GPU_ACCEL_VMWARE;
        case 0x1234:
            return GPU_ACCEL_BOCHS;
        default:
            break;
    }
    (void)device_id;
    return GPU_ACCEL_NONE;
}

static void sw_fill_rect(int x, int y, int w, int h, uint32_t color) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) return;

    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint8_t* base = (uint8_t*)(fb->double_buffered && fb->back_buffer
                               ? fb->back_buffer : fb->virtual_addr);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width) w = (int)fb->width - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        uint8_t* dst = base + (uint32_t)row * fb->pitch + (uint32_t)x * bpp;
        for (int col = 0; col < w; col++) {
            switch (bpp) {
                case 4: ((uint32_t*)dst)[col] = color; break;
                case 3:
                    dst[col * 3 + 0] = (uint8_t)(color & 0xFF);
                    dst[col * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
                    dst[col * 3 + 2] = (uint8_t)((color >> 16) & 0xFF);
                    break;
                case 2: ((uint16_t*)dst)[col] = (uint16_t)(color & 0xFFFF); break;
                case 1: dst[col] = (uint8_t)(color & 0xFF); break;
            }
        }
    }
}

static void sw_blit(void* src, int sx, int sy, int sw, int sh, int dx, int dy) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || !src) return;

    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint8_t* dst_base = (uint8_t*)(fb->double_buffered && fb->back_buffer
                                   ? fb->back_buffer : fb->virtual_addr);
    uint8_t* src_base = (uint8_t*)src;

    if (dx < 0) { sx += -dx; sw += dx; dx = 0; }
    if (dy < 0) { sy += -dy; sh += dy; dy = 0; }
    if (dx + sw > (int)fb->width) sw = (int)fb->width - dx;
    if (dy + sh > (int)fb->height) sh = (int)fb->height - dy;
    if (sw <= 0 || sh <= 0) return;

    for (int row = 0; row < sh; row++) {
        uint32_t src_off = (uint32_t)(sy + row) * (uint32_t)sw * bpp;
        uint32_t dst_off = (uint32_t)(dy + row) * fb->pitch + (uint32_t)dx * bpp;
        memcpy(dst_base + dst_off, src_base + src_off, (uint32_t)sw * bpp);
    }
}

static void sw_blend(void* src, int sx, int sy, int sw, int sh, int dx, int dy, uint8_t alpha) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || !src) return;
    if (alpha == 0) return;
    if (alpha == 255) {
        sw_blit(src, sx, sy, sw, sh, dx, dy);
        return;
    }

    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint8_t* dst_base = (uint8_t*)(fb->double_buffered && fb->back_buffer
                                   ? fb->back_buffer : fb->virtual_addr);
    uint8_t* src_base = (uint8_t*)src;

    if (dx < 0) { sx += -dx; sw += dx; dx = 0; }
    if (dy < 0) { sy += -dy; sh += dy; dy = 0; }
    if (dx + sw > (int)fb->width) sw = (int)fb->width - dx;
    if (dy + sh > (int)fb->height) sh = (int)fb->height - dy;
    if (sw <= 0 || sh <= 0) return;

    uint32_t inv = 255 - alpha;
    for (int row = 0; row < sh; row++) {
        for (int col = 0; col < sw; col++) {
            uint32_t src_off = ((uint32_t)(sy + row) * (uint32_t)sw + (uint32_t)col) * bpp;
            uint32_t dst_off = (uint32_t)(dy + row) * fb->pitch + (uint32_t)(dx + col) * bpp;

            uint32_t dst_pixel = 0;
            uint32_t src_pixel = 0;

            switch (bpp) {
                case 4:
                    dst_pixel = *(uint32_t*)(dst_base + dst_off);
                    src_pixel = *(uint32_t*)(src_base + src_off);
                    break;
                case 3:
                    dst_pixel = dst_base[dst_off] | ((uint32_t)dst_base[dst_off + 1] << 8) |
                                ((uint32_t)dst_base[dst_off + 2] << 16);
                    src_pixel = src_base[src_off] | ((uint32_t)src_base[src_off + 1] << 8) |
                                ((uint32_t)src_base[src_off + 2] << 16);
                    break;
                case 2:
                    dst_pixel = *(uint16_t*)(dst_base + dst_off);
                    src_pixel = *(uint16_t*)(src_base + src_off);
                    break;
                case 1:
                    dst_pixel = dst_base[dst_off];
                    src_pixel = src_base[src_off];
                    break;
                default:
                    continue;
            }

            uint32_t dst_b = dst_pixel & 0xFF;
            uint32_t dst_g = (dst_pixel >> 8) & 0xFF;
            uint32_t dst_r = (dst_pixel >> 16) & 0xFF;
            uint32_t src_b = src_pixel & 0xFF;
            uint32_t src_g = (src_pixel >> 8) & 0xFF;
            uint32_t src_r = (src_pixel >> 16) & 0xFF;

            uint32_t r = (src_r * alpha + dst_r * inv) / 255;
            uint32_t g = (src_g * alpha + dst_g * inv) / 255;
            uint32_t b = (src_b * alpha + dst_b * inv) / 255;
            uint32_t blended = b | (g << 8) | (r << 16) | 0xFF000000u;

            switch (bpp) {
                case 4: *(uint32_t*)(dst_base + dst_off) = blended; break;
                case 3:
                    dst_base[dst_off + 0] = blended & 0xFF;
                    dst_base[dst_off + 1] = (blended >> 8) & 0xFF;
                    dst_base[dst_off + 2] = (blended >> 16) & 0xFF;
                    break;
                case 2: *(uint16_t*)(dst_base + dst_off) = (uint16_t)(blended & 0xFFFF); break;
                case 1: dst_base[dst_off] = (uint8_t)(blended & 0xFF); break;
            }
        }
    }
}

static void sw_set_cursor(int x, int y, void* bitmap, uint32_t w, uint32_t h) {
    (void)x; (void)y; (void)bitmap; (void)w; (void)h;
}

static void sw_move_cursor(int x, int y) {
    (void)x; (void)y;
}

static void sw_swap_buffers(void) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->double_buffered || !fb->back_buffer) return;

    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    size_t size = (size_t)fb->pitch * fb->height;
    memcpy((void*)fb->virtual_addr, (void*)fb->back_buffer, size);
}

static void setup_software_fallback(void) {
    accel_ctx.type = GPU_ACCEL_SOFTWARE;
    accel_ctx.available = true;
    accel_ctx.hw_cursor = false;
    accel_ctx.hw_blend = true;
    accel_ctx.hw_scale = false;
    accel_ctx.vram_size = 0;
    accel_ctx.max_texture_size = 0;
    accel_ctx.fill_rect = sw_fill_rect;
    accel_ctx.blit = sw_blit;
    accel_ctx.blend = sw_blend;
    accel_ctx.set_cursor = sw_set_cursor;
    accel_ctx.move_cursor = sw_move_cursor;
    accel_ctx.swap_buffers = sw_swap_buffers;
}

graphics_result_t gpu_accel_init(void) {
    debuglog(DEBUG_INFO, "GPU accel: detecting hardware...\n");

    graphics_device_t* dev = graphics_get_primary_device();
    if (!dev) {
        debuglog(DEBUG_INFO, "GPU accel: no device, using software\n");
        setup_software_fallback();
        return GRAPHICS_SUCCESS;
    }

    gpu_accel_type_t detected = detect_gpu_type(dev->vendor_id, dev->device_id);
    if (detected == GPU_ACCEL_NONE) {
        debuglog(DEBUG_INFO, "GPU accel: unknown device %04x:%04x, software fallback\n",
                 dev->vendor_id, dev->device_id);
        setup_software_fallback();
        return GRAPHICS_SUCCESS;
    }

    debuglog(DEBUG_INFO, "GPU accel: detected type %d (%s)\n", detected, dev->name);

    accel_ctx.type = detected;
    accel_ctx.vram_size = (uint32_t)dev->framebuffer_size;

    display_driver_ops_t* ops = dev->driver ? dev->driver->ops : NULL;

    if (ops && ops->hw_fill_rect) {
        accel_ctx.fill_rect = sw_fill_rect;
    } else {
        accel_ctx.fill_rect = sw_fill_rect;
    }

    if (ops && ops->hw_copy_rect) {
        accel_ctx.blit = sw_blit;
    } else {
        accel_ctx.blit = sw_blit;
    }

    accel_ctx.blend = sw_blend;
    accel_ctx.hw_blend = false;
    accel_ctx.hw_scale = false;

    if (ops && ops->set_cursor && ops->move_cursor) {
        accel_ctx.hw_cursor = true;
        accel_ctx.set_cursor = sw_set_cursor;
        accel_ctx.move_cursor = sw_move_cursor;
    } else {
        accel_ctx.hw_cursor = false;
        accel_ctx.set_cursor = sw_set_cursor;
        accel_ctx.move_cursor = sw_move_cursor;
    }

    if (ops && ops->page_flip) {
        accel_ctx.swap_buffers = sw_swap_buffers;
    } else {
        accel_ctx.swap_buffers = sw_swap_buffers;
    }

    accel_ctx.available = true;
    accel_ctx.max_texture_size = 4096;

    debuglog(DEBUG_INFO, "GPU accel: vram=%u cursor=%s blend=%s\n",
             accel_ctx.vram_size,
             accel_ctx.hw_cursor ? "hw" : "sw",
             accel_ctx.hw_blend ? "hw" : "sw");

    return GRAPHICS_SUCCESS;
}

gpu_accel_context_t* gpu_accel_get_context(void) {
    return &accel_ctx;
}

void gpu_accel_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (accel_ctx.fill_rect) {
        accel_ctx.fill_rect(x, y, w, h, color);
    }
}

void gpu_accel_blit(void* src, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (accel_ctx.blit) {
        accel_ctx.blit(src, sx, sy, sw, sh, dx, dy);
    }
}

void gpu_accel_blend(void* src, int sx, int sy, int sw, int sh, int dx, int dy, uint8_t alpha) {
    if (accel_ctx.blend) {
        accel_ctx.blend(src, sx, sy, sw, sh, dx, dy, alpha);
    }
}

void gpu_accel_set_cursor(int x, int y, void* bitmap, uint32_t w, uint32_t h) {
    if (accel_ctx.set_cursor) {
        accel_ctx.set_cursor(x, y, bitmap, w, h);
    }
}

void gpu_accel_move_cursor(int x, int y) {
    if (accel_ctx.move_cursor) {
        accel_ctx.move_cursor(x, y);
    }
}

void gpu_accel_swap_buffers(void) {
    if (accel_ctx.swap_buffers) {
        accel_ctx.swap_buffers();
    }
}
