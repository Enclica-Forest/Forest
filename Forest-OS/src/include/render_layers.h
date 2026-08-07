#ifndef RENDER_LAYERS_H
#define RENDER_LAYERS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "graphics/graphics_types.h"

/*
 * Render Layer Compositing System
 *
 * Provides z-ordered layer compositing so that the splash screen, TTY,
 * GUI apps, and overlays each render to their own off-screen buffer.
 * A compositor blits layers bottom-to-top (painter's algorithm) into
 * the master framebuffer, avoiding cross-layer corruption.
 *
 * Layer IDs are fixed so callers don't need to manage dynamic allocation:
 *   0  BACKGROUND   - desktop wallpaper / solid fill
 *   1  TTY_CONSOLE  - kernel framebuffer console text
 *   2  GUI_APP      - display manager / Forest desktop / fullscreen app
 *   3  OVERLAYS     - notifications, control center, etc.
 *   4  SPLASH       - boot splash (highest: always on top when visible)
 *   5  PANIC        - kernel panic screen (absolute top)
 */

#define RL_MAX_LAYERS 8

/* Fixed layer IDs */
typedef enum {
    RL_LAYER_BACKGROUND = 0,
    RL_LAYER_TTY        = 1,
    RL_LAYER_GUI_APP    = 2,
    RL_LAYER_OVERLAYS   = 3,
    RL_LAYER_SPLASH     = 4,
    RL_LAYER_PANIC      = 5,
    RL_LAYER_COUNT      = 6
} rl_layer_id_t;

/* Blend modes for compositing */
typedef enum {
    RL_BLEND_OPAQUE = 0,    /* Source overwrites destination completely */
    RL_BLEND_ALPHA,         /* Source alpha-blends over destination */
    RL_BLEND_KEY_COLOR      /* Pixels matching key_color are transparent */
} rl_blend_mode_t;

/* Per-layer state */
typedef struct {
    uint8_t*        buffer;         /* Off-screen pixel buffer (NULL = no own buffer) */
    uint32_t        width;          /* Buffer width in pixels */
    uint32_t        height;         /* Buffer height in pixels */
    uint32_t        pitch;          /* Bytes per scanline */
    uint32_t        bpp;            /* Bits per pixel */
    pixel_format_t  format;         /* Pixel format */
    bool            visible;        /* Whether this layer is composited */
    bool            dirty;          /* Whether this layer needs recompositing */
    uint8_t         opacity;        /* 0 = fully transparent, 255 = fully opaque */
    rl_blend_mode_t blend_mode;     /* How this layer composites onto lower layers */
    uint32_t        key_color;      /* Transparent color for KEY_COLOR blend mode */
    graphics_rect_t dirty_rect;     /* Bounding box of dirty region (optimization) */
    bool            full_dirty;     /* True = entire buffer needs recompositing */
} rl_layer_t;

/* Compositor statistics */
typedef struct {
    uint32_t frames_composited;
    uint32_t layers_rendered;
    uint32_t total_blit_time_us;
} rl_stats_t;

/* Initialize the render layer system with the given framebuffer dimensions.
 * Allocates layer buffers for TTY and SPLASH layers automatically. */
bool rl_init(uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
             uint32_t fb_bpp, pixel_format_t fb_format);

/* Shut down the render layer system and free all buffers. */
void rl_shutdown(void);

/* Get a pointer to a layer's state (for direct buffer writes). */
rl_layer_t* rl_get_layer(rl_layer_id_t id);

/* Allocate an off-screen buffer for a layer. Returns false on OOM. */
bool rl_allocate_layer_buffer(rl_layer_id_t id, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t bpp, pixel_format_t format);

/* Free a layer's buffer (layer becomes transparent passthrough). */
void rl_free_layer_buffer(rl_layer_id_t id);

/* Mark a layer as needing recompositing (full redraw). */
void rl_mark_dirty(rl_layer_id_t id);

/* Mark a rectangular region of a layer as dirty (partial redraw). */
void rl_mark_dirty_rect(rl_layer_id_t id, const graphics_rect_t* rect);

/* Show/hide a layer. Hidden layers are skipped during compositing. */
void rl_set_visible(rl_layer_id_t id, bool visible);

/* Set a layer's opacity (0-255). */
void rl_set_opacity(rl_layer_id_t id, uint8_t opacity);

/* Set a layer's blend mode. */
void rl_set_blend_mode(rl_layer_id_t id, rl_blend_mode_t mode);

/* Compose all visible dirty layers into the master framebuffer.
 * The master_fb pointer is the live hardware framebuffer.
 * Returns the number of layers that were recomposited. */
uint32_t rl_composite(framebuffer_t* master_fb);

/* Force a full recomposite of all visible layers. */
uint32_t rl_composite_full(framebuffer_t* master_fb);

/* Get compositor statistics. */
void rl_get_stats(rl_stats_t* out_stats);

/* Helper: write a pixel to a layer's buffer in the correct format. */
void rl_put_pixel(rl_layer_t* layer, uint32_t x, uint32_t y, uint32_t rgb);

/* Helper: fill a rectangle in a layer's buffer. */
void rl_fill_rect(rl_layer_t* layer, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t rgb);

/* Helper: clear a layer's buffer to a solid color. */
void rl_clear_layer(rl_layer_id_t id, uint32_t rgb);

/* Helper: copy a rectangular region from one layer to another.
 * Performs alpha blending if the source layer has RL_BLEND_ALPHA. */
void rl_blit_layer(rl_layer_id_t dst_id, rl_layer_id_t src_id,
                   const graphics_rect_t* region);

/* Check if the layer system is initialized. */
bool rl_is_initialized(void);

/* Get the master framebuffer dimensions used during init. */
void rl_get_dimensions(uint32_t* width, uint32_t* height);

#endif /* RENDER_LAYERS_H */
