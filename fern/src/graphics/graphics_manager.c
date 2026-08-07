/**
 * Fern - Graphics Manager (V2 Bridge)
 * 
 * This file provides the legacy graphics API while using the new V2
 * driver system underneath. All functions maintain backward compatibility
 * with existing code.
 */

#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/display_driver.h"
#include "../include/graphics/hardware_detect.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/libc/stdio.h"
#include "../include/gfx_config.h"

#if HAS_GRAPHICS

/* External V2 functions */
extern gfx_result_t gfx_init(void);
extern gfx_result_t gfx_shutdown(void);
extern gfx_result_t gfx_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
extern gfx_result_t gfx_get_modes(gfx_mode_t** modes, uint32_t* count);
extern gfx_result_t gfx_get_framebuffer(gfx_framebuffer_t** fb);
extern gfx_result_t gfx_clear_screen(gfx_color_t color);
extern gfx_result_t gfx_draw_pixel(int32_t x, int32_t y, gfx_color_t color);
extern gfx_result_t gfx_draw_rect(const gfx_rect_t* rect, gfx_color_t color, bool filled);
extern gfx_result_t gfx_swap_buffers(void);
extern gfx_result_t gfx_get_device(uint32_t index, gfx_device_t** dev);
extern gfx_result_t gfx_get_primary_device(gfx_device_t** dev);
extern uint32_t gfx_get_device_count(void);
extern bool gfx_is_initialized(void);
extern void* gfx_get_fb_addr(void);
extern uint32_t gfx_get_fb_width(void);
extern uint32_t gfx_get_fb_height(void);
extern uint32_t gfx_get_fb_pitch(void);
extern uint32_t gfx_get_fb_bpp(void);

/* Multiboot framebuffer globals (set by kernel during early boot) */
extern void*    g_multiboot_framebuffer;   /* virtual address, or NULL */
extern uintptr_t g_multiboot_fb_addr;      /* physical address, or 0  */
extern uint32_t g_multiboot_fb_width;
extern uint32_t g_multiboot_fb_height;
extern uint32_t g_multiboot_fb_pitch;
extern uint32_t g_multiboot_fb_bpp;

/* QEMU/Bochs VBE identity-maps MMIO at this fixed virtual window */
#define GFXMGR_FALLBACK_FB_VIRT  ((uintptr_t)0xF0000000U)

/* Global state */
static struct {
    bool initialized;
    framebuffer_t framebuffer;
    graphics_device_t device;
    video_mode_t current_mode;
    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;
    void* compat_back_buffer_alloc;
    size_t compat_back_buffer_size;
} graphics_state = {0};

/* Built-in 8x8 font for text rendering */
extern const uint8_t font8x8_basic[128][8];

/* Convert legacy color to V2 color */
static inline gfx_color_t to_v2_color(graphics_color_t c) {
    gfx_color_t v2 = {c.r, c.g, c.b, c.a};
    return v2;
}

/* Convert legacy rect to V2 rect */
static inline gfx_rect_t to_v2_rect(const graphics_rect_t* r) {
    gfx_rect_t v2 = {r->x, r->y, r->width, r->height};
    return v2;
}

static inline uint32_t fb_bytes_per_pixel_from_stride(const framebuffer_t* fb) {
    if (!fb) {
        return 0;
    }

    uint32_t bytes_pp = (fb->bpp + 7) / 8;
    if (fb->width != 0 && fb->pitch >= fb->width && (fb->pitch % fb->width) == 0) {
        uint32_t stride_bpp = fb->pitch / fb->width;
        if (stride_bpp >= 1 && stride_bpp <= 4) {
            bytes_pp = stride_bpp;
        }
    }

    return bytes_pp;
}

/* Update local framebuffer copy from V2 system */
static void update_framebuffer_state(void) {
    /* If V2 never initialised but we already have a valid fallback framebuffer
     * (set during graphics_init()), there is nothing to update — skip the V2
     * call entirely to prevent error spam on every render-loop tick. */
    if (!gfx_is_initialized()) {
        if (graphics_state.framebuffer.virtual_addr != 0) {
            return;
        }
        /* V2 not ready and no fallback yet — fall through so the fallback
         * path below populates graphics_state from the multiboot info. */
    }

    gfx_framebuffer_t* v2_fb = NULL;

    gfx_result_t result = gfx_get_framebuffer(&v2_fb);
    if (result == GFX_OK && v2_fb) {
        /* Note: Removed excessive logging here - was spamming serial output */
        
        uintptr_t prev_phys = graphics_state.framebuffer.physical_addr;
        graphics_state.framebuffer.virtual_addr = (uintptr_t)v2_fb->virt_addr;
        graphics_state.framebuffer.physical_addr = v2_fb->phys_addr ? v2_fb->phys_addr : prev_phys;
        graphics_state.framebuffer.width = v2_fb->width;
        graphics_state.framebuffer.height = v2_fb->height;
        graphics_state.framebuffer.pitch = v2_fb->pitch;
        graphics_state.framebuffer.bpp = v2_fb->bpp;
        graphics_state.framebuffer.size = v2_fb->size;
        graphics_state.framebuffer.back_buffer = (uintptr_t)v2_fb->back_buffer;
        graphics_state.framebuffer.double_buffered = v2_fb->double_buffered;

        if (graphics_state.framebuffer.width != 0 &&
            graphics_state.framebuffer.pitch >= graphics_state.framebuffer.width &&
            (graphics_state.framebuffer.pitch % graphics_state.framebuffer.width) == 0) {
            uint32_t stride_bpp = graphics_state.framebuffer.pitch / graphics_state.framebuffer.width;
            uint32_t declared_bpp = (graphics_state.framebuffer.bpp + 7) / 8;
            if (stride_bpp >= 1 && stride_bpp <= 4 && declared_bpp != stride_bpp) {
                debuglog(DEBUG_WARN,
                         "[GFXMGR] FB bpp mismatch: reported=%u (%u Bpp), pitch/width=%u Bpp; normalizing\n",
                         graphics_state.framebuffer.bpp, declared_bpp, stride_bpp);
                graphics_state.framebuffer.bpp = stride_bpp * 8;
            }
        }
        
        switch (v2_fb->format) {
            case GFX_FORMAT_BGRX8888:
            case GFX_FORMAT_BGRA8888:
                graphics_state.framebuffer.format = PIXEL_FORMAT_BGRA_8888;
                break;
            case GFX_FORMAT_RGBX8888:
            case GFX_FORMAT_RGBA8888:
                graphics_state.framebuffer.format = PIXEL_FORMAT_RGBA_8888;
                break;
            case GFX_FORMAT_BGR888:
                graphics_state.framebuffer.format = PIXEL_FORMAT_BGR_888;
                break;
            case GFX_FORMAT_RGB888:
                graphics_state.framebuffer.format = PIXEL_FORMAT_RGB_888;
                break;
            default:
                /* Auto-detect based on bpp */
                if (graphics_state.framebuffer.bpp == 24) {
                    graphics_state.framebuffer.format = PIXEL_FORMAT_BGR_888;
                } else {
                    graphics_state.framebuffer.format = PIXEL_FORMAT_BGRA_8888;
                }
                break;
        }
        
        /* Update current mode — must include format so surface allocation uses correct bpp */
        graphics_state.current_mode.width = v2_fb->width;
        graphics_state.current_mode.height = v2_fb->height;
        graphics_state.current_mode.bpp = graphics_state.framebuffer.bpp;
        graphics_state.current_mode.pitch = v2_fb->pitch;
        graphics_state.current_mode.format = graphics_state.framebuffer.format;
        graphics_state.current_mode.is_text_mode = false;
        
        /* Validate framebuffer has valid virtual address */
        if (graphics_state.framebuffer.virtual_addr == 0 || 
            graphics_state.framebuffer.width == 0 ||
            graphics_state.framebuffer.height == 0) {
            debuglog(DEBUG_WARN, "[GFXMGR] Framebuffer has invalid dimensions or address!\n");
        }

        if (graphics_state.compat_back_buffer_alloc != NULL && !v2_fb->double_buffered) {
            graphics_state.framebuffer.back_buffer = (uintptr_t)graphics_state.compat_back_buffer_alloc;
            graphics_state.framebuffer.double_buffered = true;
        }
    } else {
        debuglog(DEBUG_ERROR, "[GFXMGR] Failed to get V2 framebuffer: result=%d, fb=%p\n",
                 result, v2_fb);
        if (graphics_state.framebuffer.virtual_addr != 0 &&
            graphics_state.framebuffer.width != 0 &&
            graphics_state.framebuffer.height != 0) {
            debuglog(DEBUG_WARN, "[GFXMGR] Preserving last-known-good framebuffer metadata\n");
        } else {
            memset(&graphics_state.framebuffer, 0, sizeof(graphics_state.framebuffer));

            /* --- Last-resort fallback when V2 produced no framebuffer at all ---
             *
             * Try the multiboot framebuffer pointer first (kernel already mapped
             * the physical MMIO there), then fall back to the fixed virtual window
             * 0xF0000000 that kernel_finalize_framebuffer_mapping() always
             * establishes.  QEMU Bochs VBE identity-maps its MMIO so this works
             * even without an explicit page-table entry.
             */
            uintptr_t fallback_virt = 0;
            if (g_multiboot_framebuffer != NULL) {
                fallback_virt = (uintptr_t)g_multiboot_framebuffer;
                debuglog(DEBUG_WARN,
                         "[GFXMGR] Fallback: using g_multiboot_framebuffer @ 0x%08x\n",
                         (uint32_t)fallback_virt);
            } else if (g_multiboot_fb_addr != 0) {
                /* Physical address known but no virtual mapping recorded — use
                 * the fixed kernel window which should cover it. */
                fallback_virt = GFXMGR_FALLBACK_FB_VIRT;
                debuglog(DEBUG_WARN,
                         "[GFXMGR] Fallback: g_multiboot_framebuffer NULL, "
                         "using fixed window 0x%08x (phys=0x%08x)\n",
                         (uint32_t)fallback_virt, (uint32_t)g_multiboot_fb_addr);
            } else {
                /* Absolute last resort: assume QEMU Bochs VBE default */
                fallback_virt = GFXMGR_FALLBACK_FB_VIRT;
                debuglog(DEBUG_WARN,
                         "[GFXMGR] Fallback: no multiboot info, "
                         "assuming Bochs VBE @ 0x%08x\n",
                         (uint32_t)fallback_virt);
            }

            if (fallback_virt != 0) {
                graphics_state.framebuffer.virtual_addr  = fallback_virt;
                graphics_state.framebuffer.physical_addr = (uintptr_t)g_multiboot_fb_addr;
                /* Use multiboot dimensions if available, else safe defaults */
                graphics_state.framebuffer.width  = g_multiboot_fb_width  ? g_multiboot_fb_width  : 1024;
                graphics_state.framebuffer.height = g_multiboot_fb_height ? g_multiboot_fb_height : 768;
                graphics_state.framebuffer.bpp    = g_multiboot_fb_bpp    ? g_multiboot_fb_bpp    : 32;
                graphics_state.framebuffer.pitch  = g_multiboot_fb_pitch  ? g_multiboot_fb_pitch
                    : graphics_state.framebuffer.width * ((graphics_state.framebuffer.bpp + 7) / 8);
                graphics_state.framebuffer.size   = graphics_state.framebuffer.pitch
                                                  * graphics_state.framebuffer.height;
                graphics_state.framebuffer.format =
                    (graphics_state.framebuffer.bpp == 24) ? PIXEL_FORMAT_BGR_888
                                                           : PIXEL_FORMAT_BGRA_8888;
                graphics_state.framebuffer.double_buffered = false;
                graphics_state.framebuffer.back_buffer     = 0;

                graphics_state.current_mode.width    = graphics_state.framebuffer.width;
                graphics_state.current_mode.height   = graphics_state.framebuffer.height;
                graphics_state.current_mode.bpp      = graphics_state.framebuffer.bpp;
                graphics_state.current_mode.pitch    = graphics_state.framebuffer.pitch;
                graphics_state.current_mode.format   = graphics_state.framebuffer.format;
                graphics_state.current_mode.is_text_mode = false;

                debuglog(DEBUG_WARN,
                         "[GFXMGR] Fallback FB: %ux%ux%u pitch=%u virt=0x%08x\n",
                         graphics_state.framebuffer.width,
                         graphics_state.framebuffer.height,
                         graphics_state.framebuffer.bpp,
                         graphics_state.framebuffer.pitch,
                         (uint32_t)graphics_state.framebuffer.virtual_addr);
            }
        }
    }
}

/* ============================================================================
 * Core Graphics Functions
 * ============================================================================ */

graphics_result_t graphics_init(void) {
    if (graphics_state.initialized) {
        return GRAPHICS_SUCCESS;
    }

    debuglog(DEBUG_INFO, "[GFXMGR] Initializing graphics manager (V2 backend)...\n");

    /* Prefer V2 but do not hard-fail if it is not ready yet; the fallback
     * path inside update_framebuffer_state() will try g_multiboot_framebuffer
     * and then the fixed 0xF0000000 window so we can still draw something. */
    if (!gfx_is_initialized()) {
        debuglog(DEBUG_WARN,
                 "[GFXMGR] V2 graphics not initialized — attempting legacy fallback\n");
    }

    /* Update our state from V2 system (will apply fallback if V2 has nothing) */
    update_framebuffer_state();

    /* Set up compatibility device */
    strncpy(graphics_state.device.name, "V2 Graphics Device", sizeof(graphics_state.device.name) - 1);
    graphics_state.device.type = GRAPHICS_DEVICE_VESA;
    graphics_state.device.is_active = true;

    graphics_state.cursor_x = 0;
    graphics_state.cursor_y = 0;
    graphics_state.cursor_visible = true;
    graphics_state.initialized = true;

    debuglog(DEBUG_INFO, "[GFXMGR] Graphics manager initialized: %ux%ux%u @ 0x%08x\n",
             graphics_state.framebuffer.width, graphics_state.framebuffer.height,
             graphics_state.framebuffer.bpp,
             (uint32_t)graphics_state.framebuffer.virtual_addr);

    debuglog(DEBUG_INFO,
             "[GFXMGR] Framebuffer ready at 0x%08x\n",
             (uint32_t)graphics_state.framebuffer.virtual_addr);

    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_shutdown(void) {
    if (!graphics_state.initialized) {
        return GRAPHICS_SUCCESS;
    }
    
    debuglog(DEBUG_INFO, "[GFXMGR] Shutting down graphics manager...\n");

    if (graphics_state.compat_back_buffer_alloc) {
        kfree(graphics_state.compat_back_buffer_alloc);
        graphics_state.compat_back_buffer_alloc = NULL;
        graphics_state.compat_back_buffer_size = 0;
    }
    
    memset(&graphics_state, 0, sizeof(graphics_state));
    
    return GRAPHICS_SUCCESS;
}

bool graphics_is_initialized(void) {
    /* Consider the legacy manager initialized as long as our own state is set up
     * AND we have some usable framebuffer — V2 being down is tolerated if the
     * fallback path gave us an address. */
    return graphics_state.initialized &&
           (gfx_is_initialized() || graphics_state.framebuffer.virtual_addr != 0);
}

/* ============================================================================
 * Device Management
 * ============================================================================ */

graphics_device_t* graphics_get_primary_device(void) {
    if (!graphics_state.initialized) {
        return NULL;
    }
    return &graphics_state.device;
}

graphics_result_t graphics_set_primary_device(graphics_device_t* device) {
    (void)device;
    /* V2 system handles this automatically */
    return GRAPHICS_SUCCESS;
}

graphics_device_t* graphics_get_device(uint32_t index) {
    if (index == 0 && graphics_state.initialized) {
        return &graphics_state.device;
    }
    return NULL;
}

uint32_t graphics_get_device_count(void) {
    return gfx_get_device_count();
}

/* ============================================================================
 * Mode Management
 * ============================================================================ */

graphics_result_t graphics_set_mode(uint32_t width, uint32_t height, 
                                   uint32_t bpp, uint32_t refresh_rate) {
    (void)refresh_rate;
    
    gfx_result_t result = gfx_set_mode(width, height, bpp);
    if (result == GFX_OK) {
        update_framebuffer_state();
        return GRAPHICS_SUCCESS;
    }
    
    return GRAPHICS_ERROR_INVALID_MODE;
}

graphics_result_t graphics_set_text_mode(uint32_t cols, uint32_t rows) {
    (void)cols;
    (void)rows;
    /* V2 system doesn't switch to text mode dynamically */
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t graphics_get_current_mode(video_mode_t* mode) {
    if (!mode || !graphics_state.initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    *mode = graphics_state.current_mode;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_enumerate_modes(video_mode_t** modes, uint32_t* count) {
    if (!modes || !count) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    /* Ask the V2 layer for what a runtime gfx_set_mode() call could actually
     * apply (e.g. the BGA device's real mode list). Only a device that
     * implements get_modes (currently just the Bochs/QEMU BGA driver)
     * answers this; VESA has none, so on bare VESA-only hardware this falls
     * back to reporting just the current (fixed-for-this-boot) mode below,
     * which is an honest answer -- that really is the only "settable" mode
     * there. */
    gfx_mode_t* gfx_modes = NULL;
    uint32_t gfx_count = 0;
    if (gfx_get_modes(&gfx_modes, &gfx_count) == GFX_OK && gfx_modes && gfx_count > 0) {
        video_mode_t* mode_list = (video_mode_t*)kmalloc(gfx_count * sizeof(video_mode_t));
        if (!mode_list) {
            kfree(gfx_modes);
            return GRAPHICS_ERROR_OUT_OF_MEMORY;
        }

        for (uint32_t i = 0; i < gfx_count; i++) {
            mode_list[i].width = gfx_modes[i].width;
            mode_list[i].height = gfx_modes[i].height;
            mode_list[i].bpp = gfx_modes[i].bpp;
            mode_list[i].pitch = gfx_modes[i].pitch;
            mode_list[i].refresh_rate = gfx_modes[i].refresh_hz;
            mode_list[i].is_text_mode = gfx_modes[i].is_text_mode;
            mode_list[i].mode_number = gfx_modes[i].mode_id;
            mode_list[i].hw_data = NULL;

            switch (gfx_modes[i].format) {
                case GFX_FORMAT_BGRX8888:
                case GFX_FORMAT_BGRA8888:
                    mode_list[i].format = PIXEL_FORMAT_BGRA_8888;
                    break;
                case GFX_FORMAT_RGBX8888:
                case GFX_FORMAT_RGBA8888:
                    mode_list[i].format = PIXEL_FORMAT_RGBA_8888;
                    break;
                case GFX_FORMAT_BGR888:
                    mode_list[i].format = PIXEL_FORMAT_BGR_888;
                    break;
                case GFX_FORMAT_RGB888:
                    mode_list[i].format = PIXEL_FORMAT_RGB_888;
                    break;
                default:
                    mode_list[i].format = (gfx_modes[i].bpp == 24)
                        ? PIXEL_FORMAT_BGR_888 : PIXEL_FORMAT_BGRA_8888;
                    break;
            }
        }

        kfree(gfx_modes);
        *modes = mode_list;
        *count = gfx_count;
        return GRAPHICS_SUCCESS;
    }

    /* Fallback: no device offers a real mode list (e.g. VESA-only hardware) --
     * report just the current mode, since that is genuinely the only mode
     * available for the rest of this boot. */
    video_mode_t* mode_list = (video_mode_t*)kmalloc(sizeof(video_mode_t));
    if (!mode_list) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    *mode_list = graphics_state.current_mode;
    *modes = mode_list;
    *count = 1;

    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_get_capabilities(graphics_capabilities_t* caps) {
    if (!caps) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    memset(caps, 0, sizeof(graphics_capabilities_t));
    caps->max_resolution_x = 4096;
    caps->max_resolution_y = 4096;
    caps->supports_hw_cursor = false;
    caps->supports_page_flipping = true;
    caps->supports_vsync = true;
    caps->supports_2d_accel = false;
    caps->supports_3d_accel = false;
    
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Framebuffer Access
 * ============================================================================ */

framebuffer_t* graphics_get_framebuffer(void) {
    if (!graphics_state.initialized) {
        return NULL;
    }
    
    update_framebuffer_state();
    return &graphics_state.framebuffer;
}

graphics_result_t graphics_map_framebuffer(framebuffer_t** fb) {
    if (!fb) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    *fb = graphics_get_framebuffer();
    return (*fb) ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_GENERIC;
}

graphics_result_t graphics_unmap_framebuffer(framebuffer_t* fb) {
    (void)fb;
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Drawing Operations
 * ============================================================================ */

graphics_result_t graphics_clear_screen(graphics_color_t color) {
    gfx_result_t result = gfx_clear_screen(to_v2_color(color));
    return (result == GFX_OK) ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_GENERIC;
}

graphics_result_t graphics_draw_pixel(int32_t x, int32_t y, graphics_color_t color) {
    gfx_result_t result = gfx_draw_pixel(x, y, to_v2_color(color));
    return (result == GFX_OK) ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_GENERIC;
}

graphics_result_t graphics_draw_pixels_batch(const int32_t* x_coords, const int32_t* y_coords,
                                            graphics_color_t color, uint32_t count) {
    if (!x_coords || !y_coords || count == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    gfx_color_t v2_color = to_v2_color(color);
    for (uint32_t i = 0; i < count; i++) {
        gfx_draw_pixel(x_coords[i], y_coords[i], v2_color);
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_get_pixel(int32_t x, int32_t y, graphics_color_t* color) {
    if (!color || !graphics_state.framebuffer.virtual_addr) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    if (x < 0 || y < 0 || 
        (uint32_t)x >= graphics_state.framebuffer.width ||
        (uint32_t)y >= graphics_state.framebuffer.height) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t bytes_pp = fb_bytes_per_pixel_from_stride(&graphics_state.framebuffer);
    if (bytes_pp == 0) {
        return GRAPHICS_ERROR_GENERIC;
    }

    uint8_t* fb = (uint8_t*)graphics_state.framebuffer.virtual_addr;
    uint32_t offset = y * graphics_state.framebuffer.pitch + x * bytes_pp;
    
    if (bytes_pp == 4) {
        uint32_t pixel = *(uint32_t*)(fb + offset);
        color->b = (pixel >> 0) & 0xFF;
        color->g = (pixel >> 8) & 0xFF;
        color->r = (pixel >> 16) & 0xFF;
        color->a = 255;
    } else if (bytes_pp == 3) {
        const uint8_t* pixel = fb + offset;
        color->b = pixel[0];
        color->g = pixel[1];
        color->r = pixel[2];
        color->a = 255;
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_draw_rect(const graphics_rect_t* rect, 
                                    graphics_color_t color, bool filled) {
    if (!rect) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    gfx_rect_t v2_rect = to_v2_rect(rect);
    gfx_result_t result = gfx_draw_rect(&v2_rect, to_v2_color(color), filled);
    return (result == GFX_OK) ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_GENERIC;
}

graphics_result_t graphics_draw_line(int32_t x1, int32_t y1, 
                                    int32_t x2, int32_t y2, 
                                    graphics_color_t color) {
    /* Bresenham's line algorithm */
    int32_t dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int32_t dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    int32_t sx = (x1 < x2) ? 1 : -1;
    int32_t sy = (y1 < y2) ? 1 : -1;
    int32_t err = dx - dy;
    
    gfx_color_t v2_color = to_v2_color(color);
    
    while (1) {
        gfx_draw_pixel(x1, y1, v2_color);
        
        if (x1 == x2 && y1 == y2) break;
        
        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_blit_surface(const graphics_surface_t* surface,
                                       const graphics_rect_t* src_rect,
                                       int32_t dst_x, int32_t dst_y) {
    if (!surface || !surface->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return GRAPHICS_ERROR_GENERIC;
    }
    
    /* Determine source rectangle */
    int32_t src_x = src_rect ? src_rect->x : 0;
    int32_t src_y = src_rect ? src_rect->y : 0;
    uint32_t src_w = src_rect ? src_rect->width : surface->width;
    uint32_t src_h = src_rect ? src_rect->height : surface->height;
    
    /* Clip to framebuffer bounds */
    if (dst_x < 0) { src_x -= dst_x; src_w += dst_x; dst_x = 0; }
    if (dst_y < 0) { src_y -= dst_y; src_h += dst_y; dst_y = 0; }
    if (dst_x + (int32_t)src_w > (int32_t)fb->width) src_w = fb->width - dst_x;
    if (dst_y + (int32_t)src_h > (int32_t)fb->height) src_h = fb->height - dst_y;
    
    /* Copy pixels with format conversion if needed */
    uint8_t* src_pixels = (uint8_t*)surface->pixels;
    uint8_t* dst_pixels = (uint8_t*)fb->virtual_addr;
    uint32_t src_bytes_pp = (surface->bpp + 7) / 8;
    uint32_t dst_bytes_pp = fb_bytes_per_pixel_from_stride(fb);
    if (src_bytes_pp == 0 || dst_bytes_pp == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    for (uint32_t y = 0; y < src_h; y++) {
        uint8_t* src_row = src_pixels + (src_y + y) * surface->pitch + src_x * src_bytes_pp;
        uint8_t* dst_row = dst_pixels + (dst_y + y) * fb->pitch + dst_x * dst_bytes_pp;
        if (src_bytes_pp == dst_bytes_pp) {
            memcpy(dst_row, src_row, src_w * src_bytes_pp);
        } else {
            /* Per-pixel format conversion: 32bpp→24bpp or 24bpp→32bpp */
            for (uint32_t x = 0; x < src_w; x++) {
                uint8_t* sp = src_row + x * src_bytes_pp;
                uint8_t* dp = dst_row + x * dst_bytes_pp;
                if (src_bytes_pp == 4 && dst_bytes_pp == 3) {
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; /* strip alpha */
                } else if (src_bytes_pp == 3 && dst_bytes_pp == 4) {
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 0xFF;
                } else if (src_bytes_pp == 4 && dst_bytes_pp == 2) {
                    /* 32→16 RGB565 */
                    uint16_t px = ((sp[2] >> 3) << 11) | ((sp[1] >> 2) << 5) | (sp[0] >> 3);
                    dp[0] = px & 0xFF; dp[1] = px >> 8;
                } else {
                    /* fallback: copy min bytes */
                    uint32_t n = src_bytes_pp < dst_bytes_pp ? src_bytes_pp : dst_bytes_pp;
                    for (uint32_t b = 0; b < n; b++) dp[b] = sp[b];
                }
            }
        }
    }
    
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Text Mode Operations
 * ============================================================================ */

graphics_result_t graphics_write_char(int32_t x, int32_t y, char c, uint8_t attr) {
    if (!graphics_state.initialized || !graphics_state.framebuffer.virtual_addr) {
        return GRAPHICS_ERROR_GENERIC;
    }
    
    if ((unsigned char)c >= 128) {
        c = '?';
    }
    
    /* Get foreground and background colors from attribute */
    uint8_t fg = attr & 0x0F;
    uint8_t bg = (attr >> 4) & 0x0F;
    
    /* Standard VGA 16-color palette */
    static const uint32_t vga_palette[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
    };
    
    uint32_t fg_color = vga_palette[fg];
    uint32_t bg_color = vga_palette[bg];
    
    /* Convert character position to pixel position (8x8 font) */
    int32_t px = x * 8;
    int32_t py = y * 8;
    
    /* Get font bitmap */
    const uint8_t* bitmap = font8x8_basic[(unsigned char)c];
    
    /* Draw character */
    uint8_t* fb = (uint8_t*)graphics_state.framebuffer.virtual_addr;
    uint32_t pitch = graphics_state.framebuffer.pitch;
    uint32_t bytes_pp = fb_bytes_per_pixel_from_stride(&graphics_state.framebuffer);
    if (bytes_pp == 0) {
        return GRAPHICS_ERROR_GENERIC;
    }
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < 8; col++) {
            int32_t fx = px + col;
            int32_t fy = py + row;
            
            if (fx >= 0 && fy >= 0 && 
                (uint32_t)fx < graphics_state.framebuffer.width &&
                (uint32_t)fy < graphics_state.framebuffer.height) {
                
                uint32_t color = (bits & (1 << col)) ? fg_color : bg_color;
                uint8_t* pixel = fb + fy * pitch + fx * bytes_pp;
                
                if (bytes_pp == 4) {
                    *(uint32_t*)pixel = color;
                } else if (bytes_pp == 3) {
                    /* BGR 24bpp: color is 0x00RRGGBB packed as little-endian BGRX */
                    pixel[0] = (uint8_t)( color        & 0xFF); /* B */
                    pixel[1] = (uint8_t)((color >>  8) & 0xFF); /* G */
                    pixel[2] = (uint8_t)((color >> 16) & 0xFF); /* R */
                }
            }
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_write_string(int32_t x, int32_t y, 
                                        const char* str, uint8_t attr) {
    if (!str) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    int32_t cx = x;
    int32_t cy = y;
    
    while (*str) {
        char c = *str++;
        
        if (c == '\n') {
            cx = x;
            cy++;
        } else if (c == '\r') {
            cx = x;
        } else if (c == '\t') {
            cx = (cx + 8) & ~7;
        } else {
            graphics_write_char(cx, cy, c, attr);
            cx++;
        }
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_printf(int32_t x, int32_t y, uint8_t attr, 
                                 const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    return graphics_write_string(x, y, buffer, attr);
}

graphics_result_t graphics_scroll_screen(int32_t lines) {
    if (!graphics_state.framebuffer.virtual_addr || lines == 0) {
        return GRAPHICS_SUCCESS;
    }
    
    uint8_t* fb = (uint8_t*)graphics_state.framebuffer.virtual_addr;
    uint32_t pitch = graphics_state.framebuffer.pitch;
    uint32_t height = graphics_state.framebuffer.height;
    uint32_t line_height = 8;  /* 8 pixels per text line */
    uint32_t scroll_pixels = (uint32_t)(lines > 0 ? lines : -lines) * line_height;
    
    if (lines > 0) {
        /* Scroll up */
        memmove(fb, fb + scroll_pixels * pitch, (height - scroll_pixels) * pitch);
        memset(fb + (height - scroll_pixels) * pitch, 0, scroll_pixels * pitch);
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_set_cursor_pos(int32_t x, int32_t y) {
    graphics_state.cursor_x = x;
    graphics_state.cursor_y = y;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_get_cursor_pos(int32_t* x, int32_t* y) {
    if (x) *x = graphics_state.cursor_x;
    if (y) *y = graphics_state.cursor_y;
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Cursor Management
 * ============================================================================ */

graphics_result_t graphics_set_cursor(const graphics_surface_t* cursor_surface,
                                     int32_t hotspot_x, int32_t hotspot_y) {
    (void)cursor_surface;
    (void)hotspot_x;
    (void)hotspot_y;
    /* Software cursor - handled elsewhere */
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_move_cursor(int32_t x, int32_t y) {
    graphics_state.cursor_x = x;
    graphics_state.cursor_y = y;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_show_cursor(bool show) {
    graphics_state.cursor_visible = show;
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Surface Management
 * ============================================================================ */

graphics_result_t graphics_create_surface(uint32_t width, uint32_t height,
                                         pixel_format_t format,
                                         graphics_surface_t** surface) {
    if (!surface || width == 0 || height == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    graphics_surface_t* s = (graphics_surface_t*)kmalloc(sizeof(graphics_surface_t));
    if (!s) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    memset(s, 0, sizeof(graphics_surface_t));
    s->width = width;
    s->height = height;
    s->format = format;
    switch (format) {
        case PIXEL_FORMAT_RGB_565:  s->bpp = 16; break;
        case PIXEL_FORMAT_BGR_888:  s->bpp = 24; break;
        case PIXEL_FORMAT_RGBA_8888:
        case PIXEL_FORMAT_BGRA_8888:
        default:                    s->bpp = 32; break;
    }
    s->pitch = width * ((s->bpp + 7) / 8);
    
    size_t buffer_size = s->pitch * height;
    uint32_t free_mem = kheap_get_free_memory();

    if (buffer_size > free_mem) {
        debuglog(DEBUG_WARN, "[GFX] OOM: surface %ux%u needs %u bytes, free=%u\n",
                 width, height, (uint32_t)buffer_size, free_mem);
        kfree(s);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    debuglog(DEBUG_INFO, "[GFX] surface alloc: %ux%u pitch=%u size=%u free_before=%u\n",
             width, height, s->pitch, (uint32_t)buffer_size, free_mem);
    s->pixels = kheap_graphics_alloc((uint32_t)buffer_size);
    if (!s->pixels) {
        debuglog(DEBUG_WARN, "[GFX] graphics alloc FAILED for %u bytes\n", (uint32_t)buffer_size);
        kfree(s);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    memset(s->pixels, 0, buffer_size);
    debuglog(DEBUG_INFO, "[GFX] surface ready: %p pixels=%p free_after=%u\n",
             s, s->pixels, kheap_get_free_memory());
    *surface = s;
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_destroy_surface(graphics_surface_t* surface) {
    if (!surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    if (surface->pixels) {
        kheap_graphics_free(surface->pixels);
    }
    kfree(surface);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_copy_surface(const graphics_surface_t* src,
                                        graphics_surface_t* dst) {
    if (!src || !dst || !src->pixels || !dst->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t copy_width = (src->width < dst->width) ? src->width : dst->width;
    uint32_t copy_height = (src->height < dst->height) ? src->height : dst->height;
    uint32_t src_bytes_pp = (src->bpp + 7) / 8;
    uint32_t dst_bytes_pp = (dst->bpp + 7) / 8;
    if (src_bytes_pp == 0 || dst_bytes_pp == 0 || src_bytes_pp != dst_bytes_pp) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    for (uint32_t y = 0; y < copy_height; y++) {
        uint8_t* src_row = (uint8_t*)src->pixels + y * src->pitch;
        uint8_t* dst_row = (uint8_t*)dst->pixels + y * dst->pitch;
        memcpy(dst_row, src_row, copy_width * src_bytes_pp);
    }
    
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Color Utilities
 * ============================================================================ */

graphics_color_t graphics_make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    graphics_color_t c = {r, g, b, a};
    return c;
}

uint32_t graphics_color_to_pixel(graphics_color_t color, pixel_format_t format) {
    switch (format) {
        case PIXEL_FORMAT_BGRA_8888:
            return (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
        case PIXEL_FORMAT_RGBA_8888:
            return (color.a << 24) | (color.b << 16) | (color.g << 8) | color.r;
        case PIXEL_FORMAT_RGB_565:
            return ((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3);
        default:
            return (color.r << 16) | (color.g << 8) | color.b;
    }
}

graphics_color_t graphics_pixel_to_color(uint32_t pixel, pixel_format_t format) {
    graphics_color_t c = {0, 0, 0, 255};
    
    switch (format) {
        case PIXEL_FORMAT_BGRA_8888:
            c.b = (pixel >> 0) & 0xFF;
            c.g = (pixel >> 8) & 0xFF;
            c.r = (pixel >> 16) & 0xFF;
            c.a = (pixel >> 24) & 0xFF;
            break;
        case PIXEL_FORMAT_RGBA_8888:
            c.r = (pixel >> 0) & 0xFF;
            c.g = (pixel >> 8) & 0xFF;
            c.b = (pixel >> 16) & 0xFF;
            c.a = (pixel >> 24) & 0xFF;
            break;
        default:
            c.b = (pixel >> 0) & 0xFF;
            c.g = (pixel >> 8) & 0xFF;
            c.r = (pixel >> 16) & 0xFF;
            break;
    }
    
    return c;
}

/* ============================================================================
 * Double Buffering
 * ============================================================================ */

graphics_result_t graphics_enable_double_buffering(bool enable) {
    if (!graphics_state.initialized) {
        return GRAPHICS_ERROR_GENERIC;
    }

    update_framebuffer_state();

    if (enable) {
        if (graphics_state.framebuffer.double_buffered &&
            graphics_state.framebuffer.back_buffer != 0) {
            return GRAPHICS_SUCCESS;
        }

        if (graphics_state.framebuffer.virtual_addr == 0 ||
            graphics_state.framebuffer.pitch == 0 ||
            graphics_state.framebuffer.height == 0) {
            return GRAPHICS_ERROR_GENERIC;
        }

        uint64_t needed64 = (uint64_t)graphics_state.framebuffer.pitch *
                            (uint64_t)graphics_state.framebuffer.height;
        if (needed64 == 0 || needed64 > 0xFFFFFFFFULL) {
            return GRAPHICS_ERROR_INVALID_PARAMETER;
        }
        size_t needed = (size_t)needed64;

        if (graphics_state.compat_back_buffer_alloc == NULL ||
            graphics_state.compat_back_buffer_size < needed) {
            if (graphics_state.compat_back_buffer_alloc) {
                kfree(graphics_state.compat_back_buffer_alloc);
                graphics_state.compat_back_buffer_alloc = NULL;
                graphics_state.compat_back_buffer_size = 0;
            }

            graphics_state.compat_back_buffer_alloc = kmalloc(needed);
            if (!graphics_state.compat_back_buffer_alloc) {
                return GRAPHICS_ERROR_OUT_OF_MEMORY;
            }
            graphics_state.compat_back_buffer_size = needed;
        }

        memcpy(graphics_state.compat_back_buffer_alloc,
               (const void*)graphics_state.framebuffer.virtual_addr,
               needed);
        graphics_state.framebuffer.back_buffer = (uintptr_t)graphics_state.compat_back_buffer_alloc;
        graphics_state.framebuffer.double_buffered = true;
        return GRAPHICS_SUCCESS;
    }

    graphics_state.framebuffer.double_buffered = false;
    graphics_state.framebuffer.back_buffer = 0;

    if (graphics_state.compat_back_buffer_alloc) {
        kfree(graphics_state.compat_back_buffer_alloc);
        graphics_state.compat_back_buffer_alloc = NULL;
        graphics_state.compat_back_buffer_size = 0;
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_swap_buffers(void) {
    update_framebuffer_state();

    if (graphics_state.compat_back_buffer_alloc != NULL &&
        graphics_state.framebuffer.virtual_addr != 0 &&
        graphics_state.framebuffer.pitch != 0 &&
        graphics_state.framebuffer.height != 0) {
        uint64_t bytes64 = (uint64_t)graphics_state.framebuffer.pitch *
                           (uint64_t)graphics_state.framebuffer.height;
        if (bytes64 > 0 && bytes64 <= graphics_state.compat_back_buffer_size) {
            memcpy((void*)graphics_state.framebuffer.virtual_addr,
                   graphics_state.compat_back_buffer_alloc,
                   (size_t)bytes64);
        }
    }

    gfx_result_t result = gfx_swap_buffers();
    return (result == GFX_OK) ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_GENERIC;
}

graphics_result_t graphics_wait_for_vsync(void) {
    return gfx_swap_buffers() == GFX_OK ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_NOT_SUPPORTED;
}

/* ============================================================================
 * Font Management (stub - real implementation elsewhere)
 * ============================================================================ */

graphics_result_t graphics_load_font(const char* name, uint8_t size, font_t** font) {
    (void)name; (void)size; (void)font;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t graphics_unload_font(font_t* font) {
    (void)font;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_draw_text(int32_t x, int32_t y, const char* text,
                                    font_t* font, graphics_color_t color) {
    (void)font; (void)color;
    return graphics_write_string(x / 8, y / 8, text, TEXT_ATTR_WHITE);
}

graphics_result_t graphics_get_text_bounds(const char* text, font_t* font,
                                          uint32_t* width, uint32_t* height) {
    (void)font;
    if (!text || !width || !height) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    *width = strlen(text) * 8;
    *height = 8;
    return GRAPHICS_SUCCESS;
}

/* ============================================================================
 * Debug and Diagnostics
 * ============================================================================ */

graphics_result_t graphics_dump_device_info(graphics_device_t* device) {
    if (!device) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    debuglog(DEBUG_INFO, "Device: %s\n", device->name);
    debuglog(DEBUG_INFO, "  Type: %u\n", device->type);
    debuglog(DEBUG_INFO, "  Active: %s\n", device->is_active ? "yes" : "no");
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_run_diagnostics(void) {
    debuglog(DEBUG_INFO, "=== Graphics Diagnostics ===\n");
    debuglog(DEBUG_INFO, "Initialized: %s\n", graphics_state.initialized ? "yes" : "no");
    debuglog(DEBUG_INFO, "V2 System: %s\n", gfx_is_initialized() ? "yes" : "no");
    
    if (graphics_state.initialized) {
        debuglog(DEBUG_INFO, "Framebuffer: %ux%u %ubpp\n",
                 graphics_state.framebuffer.width,
                 graphics_state.framebuffer.height,
                 graphics_state.framebuffer.bpp);
        debuglog(DEBUG_INFO, "Pitch: %u\n", graphics_state.framebuffer.pitch);
        debuglog(DEBUG_INFO, "Address: 0x%lx\n", (unsigned long)graphics_state.framebuffer.virtual_addr);
    }
    
    return GRAPHICS_SUCCESS;
}

const char* graphics_get_error_string(graphics_result_t error) {
    switch (error) {
        case GRAPHICS_SUCCESS: return "Success";
        case GRAPHICS_ERROR_GENERIC: return "Generic error";
        case GRAPHICS_ERROR_INVALID_PARAMETER: return "Invalid parameter";
        case GRAPHICS_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case GRAPHICS_ERROR_NOT_SUPPORTED: return "Not supported";
        case GRAPHICS_ERROR_HARDWARE_FAULT: return "Hardware fault";
        case GRAPHICS_ERROR_INVALID_MODE: return "Invalid mode";
        case GRAPHICS_ERROR_DEVICE_BUSY: return "Device busy";
        default: return "Unknown error";
    }
}

/* ============================================================================
 * Stub functions for less commonly used features
 * ============================================================================ */

graphics_result_t graphics_register_input_handler(void (*handler)(const input_event_t* event)) {
    (void)handler;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_unregister_input_handler(void) {
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_inject_input_event(const input_event_t* event) {
    (void)event;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_set_config(const char* key, const char* value) {
    (void)key; (void)value;
    return GRAPHICS_SUCCESS;
}

graphics_result_t graphics_get_config(const char* key, char* value, size_t size) {
    (void)key; (void)value; (void)size;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

/* Antialiased drawing stubs */
graphics_result_t graphics_draw_circle_aa(int32_t cx, int32_t cy, int32_t radius,
                                         graphics_color_t color, bool filled) {
    (void)cx; (void)cy; (void)radius; (void)color; (void)filled;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t graphics_draw_ring_aa(int32_t cx, int32_t cy, int32_t radius,
                                       int32_t thickness, graphics_color_t color) {
    (void)cx; (void)cy; (void)radius; (void)thickness; (void)color;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t graphics_draw_rounded_rect_aa(const graphics_rect_t* rect,
                                               int32_t corner_radius,
                                               graphics_color_t color, bool filled) {
    (void)rect; (void)corner_radius; (void)color; (void)filled;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t graphics_draw_line_aa(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                       graphics_color_t color) {
    return graphics_draw_line(x1, y1, x2, y2, color);
}

graphics_result_t graphics_draw_pixel_blend(int32_t x, int32_t y, graphics_color_t color) {
    return graphics_draw_pixel(x, y, color);
}

/* Parallel graphics initialization for compatibility */
graphics_result_t graphics_init_parallel(graphics_surface_t* primary_surface) {
    (void)primary_surface;
    return graphics_init();
}

#else /* !HAS_GRAPHICS */

/* No-framebuffer graphics manager stubs. Every entry point reports that
 * graphics is unavailable so callers fall back to the text console. */

graphics_result_t graphics_init(void)                      { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_shutdown(void)                  { return GRAPHICS_SUCCESS; }
bool graphics_is_initialized(void)                         { return false; }
graphics_device_t* graphics_get_primary_device(void)       { return NULL; }
graphics_result_t graphics_set_primary_device(graphics_device_t* d) { (void)d; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_device_t* graphics_get_device(uint32_t i)         { (void)i; return NULL; }
uint32_t graphics_get_device_count(void)                   { return 0; }
graphics_result_t graphics_set_mode(uint32_t w, uint32_t h, uint32_t b, uint32_t r) { (void)w; (void)h; (void)b; (void)r; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_set_text_mode(uint32_t c, uint32_t r) { (void)c; (void)r; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_get_current_mode(video_mode_t* m) { if (m) memset(m,0,sizeof(*m)); return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_enumerate_modes(video_mode_t** m, uint32_t* c) { if (m) *m=NULL; if (c) *c=0; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_get_capabilities(graphics_capabilities_t* c) { if (c) memset(c,0,sizeof(*c)); return GRAPHICS_ERROR_NOT_SUPPORTED; }
framebuffer_t* graphics_get_framebuffer(void)              { return NULL; }
graphics_result_t graphics_map_framebuffer(framebuffer_t** fb) { if (fb) *fb=NULL; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_unmap_framebuffer(framebuffer_t* fb) { (void)fb; return GRAPHICS_SUCCESS; }
graphics_result_t graphics_clear_screen(graphics_color_t c) { (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_pixel(int32_t x, int32_t y, graphics_color_t c) { (void)x; (void)y; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_pixels_batch(const int32_t* xs, const int32_t* ys, graphics_color_t c, uint32_t n) { (void)xs; (void)ys; (void)c; (void)n; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_get_pixel(int32_t x, int32_t y, graphics_color_t* c) { (void)x; (void)y; if (c) memset(c,0,sizeof(*c)); return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_rect(const graphics_rect_t* r, graphics_color_t c, bool f) { (void)r; (void)c; (void)f; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, graphics_color_t c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_blit_surface(const graphics_surface_t* s, const graphics_rect_t* r, int32_t dx, int32_t dy) { (void)s; (void)r; (void)dx; (void)dy; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_write_char(int32_t x, int32_t y, char c, uint8_t a) { (void)x; (void)y; (void)c; (void)a; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_write_string(int32_t x, int32_t y, const char* s, uint8_t a) { (void)x; (void)y; (void)s; (void)a; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_printf(int32_t x, int32_t y, uint8_t a, const char* fmt, ...) { (void)x; (void)y; (void)a; (void)fmt; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_scroll_screen(int32_t l)        { (void)l; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_set_cursor_pos(int32_t x, int32_t y) { (void)x; (void)y; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_get_cursor_pos(int32_t* x, int32_t* y) { if (x) *x=0; if (y) *y=0; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_set_cursor(const graphics_surface_t* s, int32_t hx, int32_t hy) { (void)s; (void)hx; (void)hy; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_move_cursor(int32_t x, int32_t y) { (void)x; (void)y; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_show_cursor(bool s)             { (void)s; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_create_surface(uint32_t w, uint32_t h, pixel_format_t f, graphics_surface_t** s) { (void)w; (void)h; (void)f; if (s) *s=NULL; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_destroy_surface(graphics_surface_t* s) { (void)s; return GRAPHICS_SUCCESS; }
graphics_result_t graphics_copy_surface(const graphics_surface_t* src, graphics_surface_t* dst) { (void)src; (void)dst; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_color_t graphics_make_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { graphics_color_t c = {r,g,b,a}; return c; }
uint32_t graphics_color_to_pixel(graphics_color_t c, pixel_format_t f) { (void)f; (void)c; return 0; }
graphics_color_t graphics_pixel_to_color(uint32_t p, pixel_format_t f) { (void)p; (void)f; graphics_color_t c = {0,0,0,255}; return c; }
graphics_result_t graphics_load_font(const char* n, uint8_t s, font_t** f) { (void)n; (void)s; if (f) *f=NULL; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_unload_font(font_t* f)         { (void)f; return GRAPHICS_SUCCESS; }
graphics_result_t graphics_draw_text(int32_t x, int32_t y, const char* t, font_t* f, graphics_color_t c) { (void)x; (void)y; (void)t; (void)f; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_get_text_bounds(const char* t, font_t* f, uint32_t* w, uint32_t* h) { (void)t; (void)f; if (w) *w=0; if (h) *h=0; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_enable_double_buffering(bool e) { (void)e; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_swap_buffers(void)             { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_wait_for_vsync(void)           { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_circle_aa(int32_t cx, int32_t cy, int32_t r, graphics_color_t c, bool f) { (void)cx; (void)cy; (void)r; (void)c; (void)f; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_ring_aa(int32_t cx, int32_t cy, int32_t r, int32_t t, graphics_color_t c) { (void)cx; (void)cy; (void)r; (void)t; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_rounded_rect_aa(const graphics_rect_t* r, int32_t cr, graphics_color_t c, bool f) { (void)r; (void)cr; (void)c; (void)f; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_line_aa(int32_t x1, int32_t y1, int32_t x2, int32_t y2, graphics_color_t c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_draw_pixel_blend(int32_t x, int32_t y, graphics_color_t c) { (void)x; (void)y; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_register_input_handler(void (*h)(const input_event_t*)) { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_unregister_input_handler(void) { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_inject_input_event(const input_event_t* e) { (void)e; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_set_config(const char* k, const char* v) { (void)k; (void)v; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_get_config(const char* k, char* v, size_t s) { (void)k; (void)v; (void)s; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_dump_device_info(graphics_device_t* d) { (void)d; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t graphics_run_diagnostics(void)          { return GRAPHICS_ERROR_NOT_SUPPORTED; }
const char* graphics_get_error_string(graphics_result_t e) { (void)e; return "graphics unavailable"; }
graphics_result_t graphics_init_parallel(graphics_surface_t* s) { (void)s; return GRAPHICS_ERROR_NOT_SUPPORTED; }

#endif /* HAS_GRAPHICS */
