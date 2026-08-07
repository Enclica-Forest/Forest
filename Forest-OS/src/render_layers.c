/*
 * render_layers.c - Z-ordered layer compositing for Fern
 *
 * Provides a simple software compositor that blits layers bottom-to-top
 * into the master framebuffer. Each layer (splash, TTY, GUI, overlays)
 * renders to its own off-screen buffer; this module composites them.
 */

#include "include/render_layers.h"
#include "include/graphics/graphics_types.h"
#include "include/gfx_config.h"
#include "include/debuglog.h"
#include <string.h>

#if HAS_FRAMEBUFFER

/* External kernel heap allocator */
extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

/* ---------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */
static bool         g_rl_initialized = false;
static rl_layer_t   g_layers[RL_MAX_LAYERS];
static uint32_t     g_fb_width  = 0;
static uint32_t     g_fb_height = 0;
static uint32_t     g_fb_pitch  = 0;
static uint32_t     g_fb_bpp    = 0;
static pixel_format_t g_fb_format = PIXEL_FORMAT_BGR_888;
static rl_stats_t   g_stats = {0};

/* ---------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */
bool rl_init(uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
             uint32_t fb_bpp, pixel_format_t fb_format)
{
    if (g_rl_initialized) return true;
    if (fb_width == 0 || fb_height == 0) return false;

    g_fb_width  = fb_width;
    g_fb_height = fb_height;
    g_fb_pitch  = fb_pitch;
    g_fb_bpp    = fb_bpp;
    g_fb_format = fb_format;

    memset(g_layers, 0, sizeof(g_layers));

    /* Set default z-order properties for each layer */
    for (int i = 0; i < RL_MAX_LAYERS; i++) {
        g_layers[i].visible    = false;
        g_layers[i].dirty      = false;
        g_layers[i].full_dirty = false;
        g_layers[i].opacity    = 255;
        g_layers[i].blend_mode = RL_BLEND_OPAQUE;
    }

    /* Allocate buffers for TTY and SPLASH layers */
    if (!rl_allocate_layer_buffer(RL_LAYER_TTY, fb_width, fb_height,
                                  fb_pitch, fb_bpp, fb_format)) {
        debuglog(DEBUG_ERROR, "rl_init: failed to allocate TTY layer buffer\n");
        return false;
    }

    if (!rl_allocate_layer_buffer(RL_LAYER_SPLASH, fb_width, fb_height,
                                  fb_pitch, fb_bpp, fb_format)) {
        debuglog(DEBUG_ERROR, "rl_init: failed to allocate SPLASH layer buffer\n");
        return false;
    }

    /* TTY starts hidden (splash is active first), SPLASH starts visible */
    g_layers[RL_LAYER_TTY].visible    = false;
    g_layers[RL_LAYER_SPLASH].visible = true;

    g_rl_initialized = true;
    debuglog(DEBUG_INFO, "rl_init: %dx%d %dbpp compositor ready (%d layers)\n",
             fb_width, fb_height, fb_bpp, RL_LAYER_COUNT);
    return true;
}

void rl_shutdown(void)
{
    for (int i = 0; i < RL_MAX_LAYERS; i++) {
        rl_free_layer_buffer(i);
    }
    g_rl_initialized = false;
}

/* ---------------------------------------------------------------------------
 * Layer management
 * ------------------------------------------------------------------------- */
rl_layer_t* rl_get_layer(rl_layer_id_t id)
{
    if (id >= RL_MAX_LAYERS) return NULL;
    return &g_layers[id];
}

bool rl_allocate_layer_buffer(rl_layer_id_t id, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t bpp, pixel_format_t format)
{
    if (id >= RL_MAX_LAYERS) return false;

    rl_layer_t* layer = &g_layers[id];

    /* Free existing buffer if any */
    if (layer->buffer) {
        kfree(layer->buffer);
        layer->buffer = NULL;
    }

    size_t buf_size = (size_t)pitch * height;
    layer->buffer = (uint8_t*)kmalloc(buf_size);
    if (!layer->buffer) {
        return false;
    }
    memset(layer->buffer, 0, buf_size);

    layer->width   = width;
    layer->height  = height;
    layer->pitch   = pitch;
    layer->bpp     = bpp;
    layer->format  = format;
    layer->full_dirty = true;

    return true;
}

void rl_free_layer_buffer(rl_layer_id_t id)
{
    if (id >= RL_MAX_LAYERS) return;
    rl_layer_t* layer = &g_layers[id];
    if (layer->buffer) {
        kfree(layer->buffer);
        layer->buffer = NULL;
    }
    layer->visible = false;
}

void rl_mark_dirty(rl_layer_id_t id)
{
    if (id >= RL_MAX_LAYERS) return;
    g_layers[id].dirty      = true;
    g_layers[id].full_dirty = true;
}

void rl_mark_dirty_rect(rl_layer_id_t id, const graphics_rect_t* rect)
{
    if (id >= RL_MAX_LAYERS || !rect) return;
    rl_layer_t* layer = &g_layers[id];
    layer->dirty = true;

    if (layer->full_dirty) return; /* Already a full redraw */

    /* Expand dirty rect to include new region */
    if (layer->dirty_rect.width == 0) {
        layer->dirty_rect = *rect;
    } else {
        int32_t x1 = layer->dirty_rect.x;
        int32_t y1 = layer->dirty_rect.y;
        int32_t x2 = x1 + (int32_t)layer->dirty_rect.width;
        int32_t y2 = y1 + (int32_t)layer->dirty_rect.height;

        if (rect->x < x1) x1 = rect->x;
        if (rect->y < y1) y1 = rect->y;
        int32_t rx2 = rect->x + (int32_t)rect->width;
        int32_t ry2 = rect->y + (int32_t)rect->height;
        if (rx2 > x2) x2 = rx2;
        if (ry2 > y2) y2 = ry2;

        layer->dirty_rect.x      = x1;
        layer->dirty_rect.y      = y1;
        layer->dirty_rect.width  = (uint32_t)(x2 - x1);
        layer->dirty_rect.height = (uint32_t)(y2 - y1);
    }
}

void rl_set_visible(rl_layer_id_t id, bool visible)
{
    if (id >= RL_MAX_LAYERS) return;
    if (g_layers[id].visible != visible) {
        g_layers[id].visible = visible;
        g_layers[id].dirty   = true;
        g_layers[id].full_dirty = true;
    }
}

void rl_set_opacity(rl_layer_id_t id, uint8_t opacity)
{
    if (id >= RL_MAX_LAYERS) return;
    g_layers[id].opacity = opacity;
    if (opacity < 255) {
        g_layers[id].blend_mode = RL_BLEND_ALPHA;
    }
    g_layers[id].dirty = true;
    g_layers[id].full_dirty = true;
}

void rl_set_blend_mode(rl_layer_id_t id, rl_blend_mode_t mode)
{
    if (id >= RL_MAX_LAYERS) return;
    g_layers[id].blend_mode = mode;
}

/* ---------------------------------------------------------------------------
 * Pixel helpers
 * ------------------------------------------------------------------------- */
void rl_put_pixel(rl_layer_t* layer, uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!layer || !layer->buffer) return;
    if (x >= layer->width || y >= layer->height) return;

    uint32_t bytes_pp = (layer->bpp + 7) / 8;
    uint8_t* p = layer->buffer + y * layer->pitch + x * bytes_pp;

    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >>  8) & 0xFF;
    uint8_t b =  rgb        & 0xFF;

    /* Write in BGR byte order (standard for x86 framebuffers) */
    p[0] = b;
    p[1] = g;
    p[2] = r;
    if (bytes_pp == 4) p[3] = 0xFF;
}

void rl_fill_rect(rl_layer_t* layer, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!layer || !layer->buffer) return;

    /* Clip to layer bounds */
    if (x >= layer->width || y >= layer->height) return;
    if (x + w > layer->width)  w = layer->width - x;
    if (y + h > layer->height) h = layer->height - y;

    uint32_t bytes_pp = (layer->bpp + 7) / 8;
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g_val = (rgb >>  8) & 0xFF;
    uint8_t b =  rgb        & 0xFF;

    for (uint32_t row = 0; row < h; row++) {
        uint8_t* p = layer->buffer + (y + row) * layer->pitch + x * bytes_pp;
        for (uint32_t col = 0; col < w; col++) {
            p[0] = b;
            p[1] = g_val;
            p[2] = r;
            if (bytes_pp == 4) p[3] = 0xFF;
            p += bytes_pp;
        }
    }
}

void rl_clear_layer(rl_layer_id_t id, uint32_t rgb)
{
    if (id >= RL_MAX_LAYERS) return;
    rl_layer_t* layer = &g_layers[id];
    if (!layer->buffer) return;
    rl_fill_rect(layer, 0, 0, layer->width, layer->height, rgb);
}

/* ---------------------------------------------------------------------------
 * Layer blitting (copy region from one layer to another)
 * ------------------------------------------------------------------------- */
void rl_blit_layer(rl_layer_id_t dst_id, rl_layer_id_t src_id,
                   const graphics_rect_t* region)
{
    if (dst_id >= RL_MAX_LAYERS || src_id >= RL_MAX_LAYERS) return;

    rl_layer_t* dst = &g_layers[dst_id];
    rl_layer_t* src = &g_layers[src_id];
    if (!dst->buffer || !src->buffer) return;

    /* Default to full source bounds if no region specified */
    uint32_t sx = 0, sy = 0, sw = src->width, sh = src->height;
    if (region) {
        sx = (uint32_t)region->x;
        sy = (uint32_t)region->y;
        sw = region->width;
        sh = region->height;
    }

    /* Clip to both buffers */
    if (sx >= src->width || sy >= src->height) return;
    if (sx + sw > src->width)  sw = src->width - sx;
    if (sy + sh > src->height) sh = src->height - sy;
    if (sw > dst->width)  sw = dst->width;
    if (sh > dst->height) sh = dst->height;

    uint32_t bytes_pp = (src->bpp + 7) / 8;
    bool do_alpha = (src->blend_mode == RL_BLEND_ALPHA && src->opacity < 255);
    bool do_key   = (src->blend_mode == RL_BLEND_KEY_COLOR);

    for (uint32_t row = 0; row < sh; row++) {
        uint8_t* sp = src->buffer + (sy + row) * src->pitch + sx * bytes_pp;
        uint8_t* dp = dst->buffer + (sy + row) * dst->pitch + sx * bytes_pp;

        if (do_alpha) {
            /* Alpha blend: src over dst */
            uint8_t a = src->opacity;
            uint8_t inv_a = 255 - a;
            for (uint32_t col = 0; col < sw; col++) {
                dp[0] = (uint8_t)((sp[0] * a + dp[0] * inv_a) / 255);
                dp[1] = (uint8_t)((sp[1] * a + dp[1] * inv_a) / 255);
                dp[2] = (uint8_t)((sp[2] * a + dp[2] * inv_a) / 255);
                sp += bytes_pp;
                dp += bytes_pp;
            }
        } else if (do_key) {
            /* Key color transparency */
            uint8_t kr = (src->key_color >> 16) & 0xFF;
            uint8_t kg = (src->key_color >>  8) & 0xFF;
            uint8_t kb =  src->key_color        & 0xFF;
            for (uint32_t col = 0; col < sw; col++) {
                if (sp[0] != kb || sp[1] != kg || sp[2] != kr) {
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                    if (bytes_pp == 4) dp[3] = sp[3];
                }
                sp += bytes_pp;
                dp += bytes_pp;
            }
        } else {
            /* Opaque copy */
            memcpy(dp, sp, sw * bytes_pp);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Compositor - composites layers into the master framebuffer
 * ------------------------------------------------------------------------- */
static void composite_region(framebuffer_t* master_fb, rl_layer_t* layer,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!layer->buffer || !master_fb) return;

    uint32_t bytes_pp = (layer->bpp + 7) / 8;
    bool do_alpha = (layer->blend_mode == RL_BLEND_ALPHA && layer->opacity < 255);
    bool do_key   = (layer->blend_mode == RL_BLEND_KEY_COLOR);

    for (uint32_t row = 0; row < h; row++) {
        uint8_t* sp = layer->buffer + (y + row) * layer->pitch + x * bytes_pp;
        uint8_t* dp = (uint8_t*)master_fb->virtual_addr
                      + (y + row) * master_fb->pitch
                      + x * ((master_fb->bpp + 7) / 8);
        uint32_t fb_bpp = (master_fb->bpp + 7) / 8;

        if (do_alpha) {
            uint8_t a = layer->opacity;
            uint8_t inv_a = 255 - a;
            for (uint32_t col = 0; col < w; col++) {
                dp[0] = (uint8_t)((sp[0] * a + dp[0] * inv_a) / 255);
                dp[1] = (uint8_t)((sp[1] * a + dp[1] * inv_a) / 255);
                dp[2] = (uint8_t)((sp[2] * a + dp[2] * inv_a) / 255);
                sp += bytes_pp;
                dp += fb_bpp;
            }
        } else if (do_key) {
            uint8_t kr = (layer->key_color >> 16) & 0xFF;
            uint8_t kg = (layer->key_color >>  8) & 0xFF;
            uint8_t kb =  layer->key_color        & 0xFF;
            for (uint32_t col = 0; col < w; col++) {
                if (sp[0] != kb || sp[1] != kg || sp[2] != kr) {
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                }
                sp += bytes_pp;
                dp += fb_bpp;
            }
        } else {
            /* Opaque blit - use memcpy for the common case */
            memcpy(dp, sp, w * bytes_pp);
        }
    }
}

uint32_t rl_composite(framebuffer_t* master_fb)
{
    if (!g_rl_initialized || !master_fb) return 0;

    uint32_t composited = 0;

    /* Iterate layers bottom-to-top (painter's algorithm) */
    for (int i = 0; i < RL_MAX_LAYERS; i++) {
        rl_layer_t* layer = &g_layers[i];
        if (!layer->visible || !layer->dirty || !layer->buffer) continue;

        if (layer->full_dirty) {
            /* Composite entire layer */
            composite_region(master_fb, layer, 0, 0,
                             layer->width, layer->height);
            layer->full_dirty = false;
        } else if (layer->dirty_rect.width > 0 && layer->dirty_rect.height > 0) {
            /* Composite only dirty region */
            composite_region(master_fb, layer,
                             (uint32_t)layer->dirty_rect.x,
                             (uint32_t)layer->dirty_rect.y,
                             layer->dirty_rect.width,
                             layer->dirty_rect.height);
        }

        layer->dirty = false;
        memset(&layer->dirty_rect, 0, sizeof(layer->dirty_rect));
        composited++;
        g_stats.layers_rendered++;
    }

    g_stats.frames_composited++;
    return composited;
}

uint32_t rl_composite_full(framebuffer_t* master_fb)
{
    if (!g_rl_initialized || !master_fb) return 0;

    /* Mark all visible layers as fully dirty */
    for (int i = 0; i < RL_MAX_LAYERS; i++) {
        if (g_layers[i].visible && g_layers[i].buffer) {
            g_layers[i].dirty      = true;
            g_layers[i].full_dirty = true;
        }
    }

    return rl_composite(master_fb);
}

void rl_get_stats(rl_stats_t* out_stats)
{
    if (out_stats) *out_stats = g_stats;
}

bool rl_is_initialized(void)
{
    return g_rl_initialized;
}

void rl_get_dimensions(uint32_t* width, uint32_t* height)
{
    if (width)  *width  = g_fb_width;
    if (height) *height = g_fb_height;
}

#else /* !HAS_FRAMEBUFFER */

/* No-framebuffer stubs: layer compositing is meaningless without a fb.
 * Initialisers succeed vacuously so boot can proceed; everything else
 * returns failure/NULL/no-op. */

bool rl_init(uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
             uint32_t fb_bpp, pixel_format_t fb_format) {
    (void)fb_width; (void)fb_height; (void)fb_pitch; (void)fb_bpp; (void)fb_format;
    return true;
}
void rl_shutdown(void) {}
rl_layer_t* rl_get_layer(rl_layer_id_t id) { (void)id; return NULL; }
bool rl_allocate_layer_buffer(rl_layer_id_t id, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t bpp, pixel_format_t format) {
    (void)id; (void)width; (void)height; (void)pitch; (void)bpp; (void)format;
    return false;
}
void rl_free_layer_buffer(rl_layer_id_t id) { (void)id; }
void rl_mark_dirty(rl_layer_id_t id) { (void)id; }
void rl_mark_dirty_rect(rl_layer_id_t id, const graphics_rect_t* rect) {
    (void)id; (void)rect;
}
void rl_set_visible(rl_layer_id_t id, bool visible) { (void)id; (void)visible; }
void rl_set_opacity(rl_layer_id_t id, uint8_t opacity) { (void)id; (void)opacity; }
void rl_set_blend_mode(rl_layer_id_t id, rl_blend_mode_t mode) { (void)id; (void)mode; }
uint32_t rl_composite(framebuffer_t* master_fb) { (void)master_fb; return 0; }
uint32_t rl_composite_full(framebuffer_t* master_fb) { (void)master_fb; return 0; }
void rl_get_stats(rl_stats_t* out_stats) { if (out_stats) memset(out_stats, 0, sizeof(*out_stats)); }
void rl_put_pixel(rl_layer_t* layer, uint32_t x, uint32_t y, uint32_t rgb) {
    (void)layer; (void)x; (void)y; (void)rgb;
}
void rl_fill_rect(rl_layer_t* layer, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t rgb) {
    (void)layer; (void)x; (void)y; (void)w; (void)h; (void)rgb;
}
void rl_clear_layer(rl_layer_id_t id, uint32_t rgb) { (void)id; (void)rgb; }
void rl_blit_layer(rl_layer_id_t dst_id, rl_layer_id_t src_id,
                   const graphics_rect_t* region) {
    (void)dst_id; (void)src_id; (void)region;
}
bool rl_is_initialized(void) { return false; }
void rl_get_dimensions(uint32_t* width, uint32_t* height) {
    if (width)  *width  = 0;
    if (height) *height = 0;
}

#endif /* HAS_FRAMEBUFFER */
