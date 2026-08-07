/**
 * Fern - VGA Graphics Mode Driver V2
 * 
 * Complete VGA graphics mode driver with double-buffering support.
 * Based on standard VGA documentation.
 * 
 * Features:
 * - 640x480x16 (VGA Mode 12h)
 * - 320x200x256 (VGA Mode 13h)
 * - 320x200x256 (Mode X/Planar)
 * - Double-buffering support
 * - V-Sync support
 * - Software rendering fallback
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "../../include/graphics/graphics_driver_v2.h"
#include "../../include/graphics/graphics_hw_regs.h"
#include "../../include/graphics/graphics_types.h"
#include "../../include/memory.h"
#include "../../include/debug.h"

/* VGA graphics mode dimensions */
#define VGA_MODE_13H_WIDTH    320
#define VGA_MODE_13H_HEIGHT   200
#define VGA_MODE_12H_WIDTH    640
#define VGA_MODE_12H_HEIGHT   480
#define VGA_MODE_X_WIDTH      320
#define VGA_MODE_X_HEIGHT     240

/* VGA memory */
#define VGA_GRAPHICS_MEM      0xA0000
#define VGA_GRAPHICS_SIZE     (64 * 1024)

/* Driver-private data structure */
typedef struct {
    /* Framebuffer */
    uint8_t* framebuffer;           /* Mapped framebuffer */
    uint32_t width;                 /* Width in pixels */
    uint32_t height;                /* Height in pixels */
    uint32_t pitch;                 /* Bytes per row */
    uint8_t bpp;                    /* Bits per pixel */
    
    /* Double buffering */
    uint8_t* back_buffer;           /* Software back buffer */
    size_t back_buffer_size;
    bool double_buffered;
    bool vsync_enabled;
    
    /* Current mode */
    gfx_framebuffer_t gfx_framebuffer;
    
    /* Dirty rectangle tracking for partial updates */
    bool dirty;
    uint32_t dirty_x1, dirty_y1, dirty_x2, dirty_y2;
    
    /* DAC palette (for 256-color modes) */
    uint8_t palette[256][3];
} vga_graphics_private_t;

/* VGA mode definitions */
typedef struct {
    uint16_t width, height;
    uint8_t bpp;
    uint8_t mode_type;  /* 0 = planar 16-color, 1 = packed 256-color, 2 = mode X */
    const uint8_t* regs;
    size_t reg_count;
} vga_mode_info_t;

/* Forward declarations */
static gfx_result_t vga_gfx_probe(gfx_device_t* dev);
static gfx_result_t vga_gfx_init(gfx_device_t* dev);
static gfx_result_t vga_gfx_shutdown(gfx_device_t* dev);
static gfx_result_t vga_gfx_reset(gfx_device_t* dev);
static gfx_result_t vga_gfx_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t vga_gfx_set_mode(gfx_device_t* dev, const gfx_mode_t* mode);
static gfx_result_t vga_gfx_get_mode(gfx_device_t* dev, gfx_mode_t* mode);
static gfx_result_t vga_gfx_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t vga_gfx_clear(gfx_device_t* dev, gfx_color_t color);
static gfx_result_t vga_gfx_draw_pixel(gfx_device_t* dev, int32_t x, int32_t y, gfx_color_t color);
static gfx_result_t vga_gfx_flip(gfx_device_t* dev);
static gfx_result_t vga_gfx_wait_vsync(gfx_device_t* dev);

/* Driver operations table */
static const gfx_driver_ops_t vga_gfx_driver_ops = {
    .name = "vga-graphics",
    .version = 0x00020000,
    
    .probe = vga_gfx_probe,
    .init = vga_gfx_init,
    .shutdown = vga_gfx_shutdown,
    .reset = vga_gfx_reset,
    
    .get_modes = vga_gfx_get_modes,
    .set_mode = vga_gfx_set_mode,
    .get_mode = vga_gfx_get_mode,
    
    .map_fb = vga_gfx_map_fb,
    .unmap_fb = NULL,
    .set_fb_offset = NULL,
    
    .clear = vga_gfx_clear,
    .draw_pixel = vga_gfx_draw_pixel,
    .draw_rect = NULL,
    .blit = NULL,
    
    .set_cursor = NULL,
    .move_cursor = NULL,
    .show_cursor = NULL,
    
    .write_char = NULL,
    .set_text_cursor = NULL,
    .scroll = NULL,
    
    .wait_vsync = vga_gfx_wait_vsync,
    .flip = vga_gfx_flip,
    
    .read_edid = NULL,
    .detect_displays = NULL,
    
    .set_dpms = NULL,
    .ioctl = NULL,
};

/* Driver instance */
DECLARE_GFX_DRIVER(vga_graphics, &vga_gfx_driver_ops, GFX_DEVICE_VGA);

/* ============================================================================
 * VGA Hardware Detection
 * ============================================================================ */

/**
 * Check if VGA hardware is present and supports graphics modes
 */
static bool vga_gfx_hw_present(void) {
    /* Check for VGA by testing CRTC registers */
    gfx_outb(VGA_CRTC_INDEX_COLOR, 0x00);
    uint8_t val1 = gfx_inb(VGA_CRTC_DATA_COLOR);
    
    gfx_outb(VGA_CRTC_INDEX_COLOR, 0x00);
    gfx_outb(VGA_CRTC_DATA_COLOR, val1 ^ 0xFF);
    
    gfx_outb(VGA_CRTC_INDEX_COLOR, 0x00);
    uint8_t val2 = gfx_inb(VGA_CRTC_DATA_COLOR);
    
    /* Restore original value */
    gfx_outb(VGA_CRTC_INDEX_COLOR, 0x00);
    gfx_outb(VGA_CRTC_DATA_COLOR, val1);
    
    /* If we can write and read back, VGA is present */
    if (val2 != (val1 ^ 0xFF)) {
        return false;
    }
    
    /* Check for VGA feature control register */
    gfx_outb(0x3DA, 0x00);
    uint8_t status = gfx_inb(0x3DA);
    
    /* VGA should have display enable and vertical retrace bits */
    return true;
}

/**
 * Get VGA memory size
 */
static uint32_t vga_get_memory_size(void) {
    /* Read sequencer register to get memory size info */
    gfx_outb(VGA_SEQ_INDEX, 0x07);
    uint8_t mem_size = gfx_inb(VGA_SEQ_DATA);
    
    /* Bits 2-3 indicate memory size:
     * 00 = 64KB, 01 = 128KB, 10 = 192KB, 11 = 256KB */
    uint8_t size_bits = (mem_size >> 2) & 0x03;
    
    switch (size_bits) {
        case 0: return 64 * 1024;
        case 1: return 128 * 1024;
        case 2: return 192 * 1024;
        case 3: return 256 * 1024;
        default: return 64 * 1024;
    }
}

/* ============================================================================
 * VGA Mode Setting
 * ============================================================================ */

/**
 * VGA Mode 13h (320x200x256) register values
 */
static const uint8_t vga_mode_13h_regs[] = {
    /* Miscellaneous Output */
    0x63,
    /* Sequencer */
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    /* CRTC */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    /* Graphics Controller */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    /* Attribute Controller */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/**
 * VGA Mode 12h (640x480x16) register values
 */
static const uint8_t vga_mode_12h_regs[] = {
    /* Miscellaneous Output */
    0xE3,
    /* Sequencer */
    0x03, 0x01, 0x08, 0x00, 0x06,
    /* CRTC */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xEA, 0x0C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3,
    0xFF,
    /* Graphics Controller */
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x05, 0x0F,
    /* Attribute Controller */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x01, 0x00, 0x0F, 0x00, 0x00
};

/**
 * Mode X (320x240x256) register values
 */
static const uint8_t vga_mode_x_regs[] = {
    /* Miscellaneous Output */
    0x63,
    /* Sequencer */
    0x03, 0x01, 0x0F, 0x00, 0x06,
    /* CRTC */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x00, 0x96, 0xB9, 0xE3,
    0xFF,
    /* Graphics Controller */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    /* Attribute Controller */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/**
 * Write VGA registers for graphics mode
 */
static void vga_write_registers(const uint8_t* regs) {
    /* Write Miscellaneous Output register */
    gfx_outb(VGA_MISC_WRITE, regs[0]);
    regs++;
    
    /* Write Sequencer registers */
    for (int i = 0; i < 5; i++) {
        gfx_outb(VGA_SEQ_INDEX, i);
        gfx_outb(VGA_SEQ_DATA, regs[i]);
    }
    regs += 5;
    
    /* Unlock CRTC registers */
    gfx_outb(VGA_CRTC_INDEX_COLOR, 0x03);
    gfx_outb(VGA_CRTC_DATA_COLOR, gfx_inb(VGA_CRTC_DATA_COLOR) | 0x80);
    gfx_outb(VGA_CRTC_INDEX_COLOR, 0x11);
    gfx_outb(VGA_CRTC_DATA_COLOR, gfx_inb(VGA_CRTC_DATA_COLOR) & ~0x80);
    
    /* Write CRTC registers */
    for (int i = 0; i < 25; i++) {
        gfx_outb(VGA_CRTC_INDEX_COLOR, i);
        gfx_outb(VGA_CRTC_DATA_COLOR, regs[i]);
    }
    regs += 25;
    
    /* Write Graphics Controller registers */
    for (int i = 0; i < 9; i++) {
        gfx_outb(VGA_GC_INDEX, i);
        gfx_outb(VGA_GC_DATA, regs[i]);
    }
    regs += 9;
    
    /* Write Attribute Controller registers */
    for (int i = 0; i < 21; i++) {
        gfx_inb(VGA_INPUT_STATUS1_COLOR);  /* Reset flip-flop */
        gfx_outb(VGA_AC_INDEX, i);
        gfx_outb(VGA_AC_WRITE, regs[i]);
    }
    
    /* Lock palette and unblank display */
    gfx_inb(VGA_INPUT_STATUS1_COLOR);
    gfx_outb(VGA_AC_INDEX, 0x20);
}

/**
 * Set VGA graphics mode
 */
static void vga_set_mode_13h(void) {
    vga_write_registers(vga_mode_13h_regs);
}

static void vga_set_mode_12h(void) {
    vga_write_registers(vga_mode_12h_regs);
}

static void vga_set_mode_x(void) {
    vga_write_registers(vga_mode_x_regs);
}

/**
 * Clear VGA framebuffer
 */
static void vga_clear_framebuffer(uint8_t* fb, size_t size) {
    memset(fb, 0, size);
}

/* ============================================================================
 * DAC Palette Operations (for 256-color modes)
 * ============================================================================ */

/**
 * Set a single DAC palette entry
 */
static void vga_set_dac_entry(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    gfx_outb(VGA_DAC_WRITE_INDEX, index);
    gfx_outb(VGA_DAC_DATA, r);
    gfx_outb(VGA_DAC_DATA, g);
    gfx_outb(VGA_DAC_DATA, b);
}

/**
 * Set the entire DAC palette
 */
static void vga_set_palette(const uint8_t palette[256][3]) {
    gfx_outb(VGA_DAC_WRITE_INDEX, 0);
    for (int i = 0; i < 256; i++) {
        gfx_outb(VGA_DAC_DATA, palette[i][0]);
        gfx_outb(VGA_DAC_DATA, palette[i][1]);
        gfx_outb(VGA_DAC_DATA, palette[i][2]);
    }
}

/**
 * Initialize standard VGA palette (standard 256 colors)
 */
static void vga_init_palette(uint8_t palette[256][3]) {
    /* First 16 colors: standard VGA palette */
    static const uint8_t standard_colors[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
        {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
        {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
        {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF}
    };
    
    /* Copy standard colors */
    for (int i = 0; i < 16; i++) {
        palette[i][0] = standard_colors[i][0];
        palette[i][1] = standard_colors[i][1];
        palette[i][2] = standard_colors[i][2];
    }
    
    /* Generate 216-color cube (6x6x6) */
    int idx = 16;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                if (idx < 256) {
                    palette[idx][0] = r * 51;
                    palette[idx][1] = g * 51;
                    palette[idx][2] = b * 51;
                    idx++;
                }
            }
        }
    }
    
    /* Fill remaining with grayscale */
    for (int i = 232; i < 256; i++) {
        int gray = (i - 232) * 8 + 8;
        palette[i][0] = gray;
        palette[i][1] = gray;
        palette[i][2] = gray;
    }
}

/* ============================================================================
 * Double Buffering Support
 * ============================================================================ */

/**
 * Allocate back buffer for double buffering
 */
static bool vga_allocate_back_buffer(vga_graphics_private_t* priv) {
    size_t size = (size_t)priv->pitch * priv->height;
    
    /* Allocate back buffer in main memory */
    priv->back_buffer = (uint8_t*)kmalloc(size);
    if (!priv->back_buffer) {
        debug_print("[VGA] Failed to allocate back buffer\n");
        return false;
    }
    
    priv->back_buffer_size = size;
    memset(priv->back_buffer, 0, size);
    
    debug_print("[VGA] Allocated back buffer: %zu bytes at %p\n", 
                size, (void*)priv->back_buffer);
    
    return true;
}

/**
 * Free back buffer
 */
static void vga_free_back_buffer(vga_graphics_private_t* priv) {
    if (priv->back_buffer) {
        kfree(priv->back_buffer);
        priv->back_buffer = NULL;
        priv->back_buffer_size = 0;
    }
}

/**
 * Swap buffers (copy back buffer to front)
 */
static void vga_swap_buffers(vga_graphics_private_t* priv) {
    if (!priv->back_buffer) {
        return;
    }
    
    size_t size = (size_t)priv->pitch * priv->height;
    if (size > priv->back_buffer_size) {
        size = priv->back_buffer_size;
    }
    
    /* Copy back buffer to VGA framebuffer */
    memcpy(priv->framebuffer, priv->back_buffer, size);
    
    /* Clear dirty tracking */
    priv->dirty = false;
}

/**
 * Update dirty rectangle
 */
static void vga_mark_dirty(vga_graphics_private_t* priv, int32_t x, int32_t y) {
    if (!priv->dirty) {
        priv->dirty = true;
        priv->dirty_x1 = x;
        priv->dirty_y1 = y;
        priv->dirty_x2 = x;
        priv->dirty_y2 = y;
    } else {
        if (x < (int32_t)priv->dirty_x1) priv->dirty_x1 = x;
        if (y < (int32_t)priv->dirty_y1) priv->dirty_y1 = y;
        if (x > (int32_t)priv->dirty_x2) priv->dirty_x2 = x;
        if (y > (int32_t)priv->dirty_y2) priv->dirty_y2 = y;
    }
}

/* ============================================================================
 * Driver Implementation
 * ============================================================================ */

static gfx_result_t vga_gfx_probe(gfx_device_t* dev) {
    /* Check for VGA hardware */
    if (!vga_gfx_hw_present()) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Allocate private data */
    vga_graphics_private_t* priv = (vga_graphics_private_t*)kmalloc(sizeof(vga_graphics_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(vga_graphics_private_t));
    
    /* Map VGA graphics memory */
    priv->framebuffer = (uint8_t*)VGA_GRAPHICS_MEM;
    
    /* Get memory size */
    uint32_t mem_size = vga_get_memory_size();
    
    /* Initialize palette */
    vga_init_palette(priv->palette);
    
    /* Fill in device info */
    dev->type = GFX_DEVICE_VGA;
    dev->fb_base = VGA_GRAPHICS_MEM;
    dev->fb_size = mem_size;
    dev->vram_size = mem_size;
    dev->max_width = 640;
    dev->max_height = 480;
    dev->max_bpp = 8;  /* VGA max in native modes is 8-bit (256 colors) */
    dev->driver_data = priv;
    
    /* Set capabilities - use PAGE_FLIP for double buffering support */
    dev->caps = GFX_CAP_PAGE_FLIP | GFX_CAP_VSYNC;
    
    strncpy(dev->name, "VGA Graphics", sizeof(dev->name) - 1);
    
    debug_print("[VGA] Graphics probe successful: %uKB VRAM\n", mem_size / 1024);
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_init(gfx_device_t* dev) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Set default mode (Mode 13h - 320x200x256) */
    vga_set_mode_13h();
    
    /* Initialize mode parameters */
    priv->width = VGA_MODE_13H_WIDTH;
    priv->height = VGA_MODE_13H_HEIGHT;
    priv->pitch = VGA_MODE_13H_WIDTH;  /* 1 byte per pixel in mode 13h */
    priv->bpp = 8;
    
    /* Set palette */
    vga_set_palette(priv->palette);
    
    /* Clear framebuffer */
    vga_clear_framebuffer(priv->framebuffer, priv->width * priv->height);
    
    /* Allocate back buffer for double buffering */
    priv->double_buffered = true;
    if (!vga_allocate_back_buffer(priv)) {
        priv->double_buffered = false;
    }
    
    /* Initialize dirty tracking */
    priv->dirty = false;
    priv->dirty_x1 = priv->width;
    priv->dirty_y1 = priv->height;
    priv->dirty_x2 = 0;
    priv->dirty_y2 = 0;
    
    /* Set up framebuffer structure */
    priv->gfx_framebuffer.phys_addr = VGA_GRAPHICS_MEM;
    priv->gfx_framebuffer.virt_addr = priv->framebuffer;
    priv->gfx_framebuffer.width = priv->width;
    priv->gfx_framebuffer.height = priv->height;
    priv->gfx_framebuffer.pitch = priv->pitch;
    priv->gfx_framebuffer.bpp = priv->bpp;
    /* CRITICAL FIX: Use pitch for size calculation */
    priv->gfx_framebuffer.size = priv->pitch * priv->height;
    priv->gfx_framebuffer.format = GFX_FORMAT_INDEXED_8;
    priv->gfx_framebuffer.double_buffered = priv->double_buffered;
    priv->gfx_framebuffer.back_buffer = (void*)priv->back_buffer;
    
    /* Set current mode */
    dev->current_mode.width = priv->width;
    dev->current_mode.height = priv->height;
    dev->current_mode.bpp = priv->bpp;
    dev->current_mode.pitch = priv->pitch;
    dev->current_mode.format = GFX_FORMAT_INDEXED_8;
    dev->current_mode.is_text_mode = false;
    
    dev->fb = &priv->gfx_framebuffer;
    dev->active = true;
    
    debug_print("[VGA] Graphics mode initialized: %ux%ux%u\n", 
                priv->width, priv->height, priv->bpp);
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_shutdown(gfx_device_t* dev) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (priv) {
        /* Free back buffer */
        vga_free_back_buffer(priv);
        
        /* Clear framebuffer */
        if (priv->framebuffer) {
            vga_clear_framebuffer(priv->framebuffer, priv->width * priv->height);
        }
        
        kfree(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_reset(gfx_device_t* dev) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (priv) {
        /* Clear screen */
        gfx_color_t black = {0, 0, 0, 255};
        vga_gfx_clear(dev, black);
        
        /* Reset back buffer */
        if (priv->back_buffer) {
            memset(priv->back_buffer, 0, priv->back_buffer_size);
        }
    }
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    /* VGA graphics modes */
    static const struct { 
        uint32_t w, h, bpp; 
        const char* name;
    } gfx_modes[] = {
        {320, 200, 8, "Mode 13h (320x200x256)"},
        {640, 480, 4, "Mode 12h (640x480x16)"},
        {320, 240, 8, "Mode X (320x240x256)"},
    };
    
    uint32_t num_modes = sizeof(gfx_modes) / sizeof(gfx_modes[0]);
    
    gfx_mode_t* mode_list = (gfx_mode_t*)kmalloc(num_modes * sizeof(gfx_mode_t));
    if (!mode_list) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(mode_list, 0, num_modes * sizeof(gfx_mode_t));
    
    for (uint32_t i = 0; i < num_modes; i++) {
        mode_list[i].mode_id = i;
        mode_list[i].width = gfx_modes[i].w;
        mode_list[i].height = gfx_modes[i].h;
        mode_list[i].bpp = gfx_modes[i].bpp;
        mode_list[i].pitch = gfx_modes[i].w * (gfx_modes[i].bpp / 8);
        mode_list[i].format = (gfx_modes[i].bpp == 8) ? GFX_FORMAT_INDEXED_8 : GFX_FORMAT_RGB565;
        mode_list[i].is_text_mode = false;
    }
    
    *modes = mode_list;
    *count = num_modes;
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_set_mode(gfx_device_t* dev, const gfx_mode_t* mode) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (!mode || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Handle different VGA modes */
    if (mode->width == 320 && mode->height == 200 && mode->bpp == 8) {
        vga_set_mode_13h();
        priv->width = VGA_MODE_13H_WIDTH;
        priv->height = VGA_MODE_13H_HEIGHT;
        priv->pitch = VGA_MODE_13H_WIDTH;
        priv->bpp = 8;
    } else if (mode->width == 640 && mode->height == 480 && mode->bpp == 16) {
        vga_set_mode_12h();
        priv->width = VGA_MODE_12H_WIDTH;
        priv->height = VGA_MODE_12H_HEIGHT;
        priv->pitch = VGA_MODE_12H_WIDTH * 2;
        priv->bpp = 16;
    } else if (mode->width == 320 && mode->height == 240 && mode->bpp == 8) {
        vga_set_mode_x();
        priv->width = VGA_MODE_X_WIDTH;
        priv->height = VGA_MODE_X_HEIGHT;
        priv->pitch = VGA_MODE_X_WIDTH;
        priv->bpp = 8;
    } else {
        /* Unsupported mode */
        debug_print("[VGA] Unsupported mode: %ux%ux%u\n", mode->width, mode->height, mode->bpp);
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Reallocate back buffer if needed */
    if (priv->back_buffer) {
        vga_free_back_buffer(priv);
    }
    
    if (priv->double_buffered) {
        vga_allocate_back_buffer(priv);
    }
    
    /* Update framebuffer structure */
    priv->gfx_framebuffer.width = priv->width;
    priv->gfx_framebuffer.height = priv->height;
    priv->gfx_framebuffer.pitch = priv->pitch;
    priv->gfx_framebuffer.bpp = priv->bpp;
    /* CRITICAL FIX: Use pitch for size calculation */
    priv->gfx_framebuffer.size = priv->pitch * priv->height;
    priv->gfx_framebuffer.back_buffer = (void*)priv->back_buffer;
    
    /* Clear screen */
    vga_clear_framebuffer(priv->framebuffer, priv->gfx_framebuffer.size);
    if (priv->back_buffer) {
        memset(priv->back_buffer, 0, priv->back_buffer_size);
    }
    
    /* Update device mode */
    dev->current_mode.width = priv->width;
    dev->current_mode.height = priv->height;
    dev->current_mode.bpp = priv->bpp;
    dev->current_mode.pitch = priv->pitch;
    dev->current_mode.is_text_mode = false;
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_get_mode(gfx_device_t* dev, gfx_mode_t* mode) {
    if (!mode) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    *mode = dev->current_mode;
    return GFX_OK;
}

static gfx_result_t vga_gfx_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (!fb || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Return the framebuffer structure */
    *fb = &priv->gfx_framebuffer;
    return GFX_OK;
}

static gfx_result_t vga_gfx_clear(gfx_device_t* dev, gfx_color_t color) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    uint8_t fill_color = (color.r > 128) ? 0xFF : 0x00;  /* Simple grayscale for indexed mode */
    
    /* Clear back buffer if double buffered */
    if (priv->back_buffer) {
        memset(priv->back_buffer, fill_color, priv->back_buffer_size);
    }
    
    /* Clear front buffer */
    memset(priv->framebuffer, fill_color, priv->width * priv->height);
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_draw_pixel(gfx_device_t* dev, int32_t x, int32_t y, gfx_color_t color) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Clamp coordinates */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= priv->width || (uint32_t)y >= priv->height) {
        return GFX_OK;
    }
    
    /* Convert color to indexed color */
    uint8_t pixel_index;
    if (priv->bpp == 8) {
        /* Simple color conversion for indexed mode */
        if (color.r > 200 && color.g > 200 && color.b > 200) {
            pixel_index = 15;  /* White */
        } else if (color.r > color.g && color.r > color.b) {
            pixel_index = 12;  /* Red */
        } else if (color.g > color.r && color.g > color.b) {
            pixel_index = 10;  /* Green */
        } else if (color.b > color.r && color.b > color.g) {
            pixel_index = 9;   /* Blue */
        } else {
            pixel_index = 7;   /* Light gray */
        }
    } else {
        /* 16-bit color (RGB565) */
        uint16_t rgb565 = ((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3);
        uint16_t* fb16 = (uint16_t*)priv->framebuffer;
        uint16_t* bb16 = (uint16_t*)priv->back_buffer;
        
        size_t offset = y * (priv->pitch / 2) + x;
        
        if (priv->back_buffer) {
            bb16[offset] = rgb565;
        }
        fb16[offset] = rgb565;
        return GFX_OK;
    }
    
    size_t offset = y * priv->pitch + x;
    
    /* Draw to back buffer if double buffered */
    if (priv->back_buffer) {
        priv->back_buffer[offset] = pixel_index;
    }
    
    /* Draw to front buffer */
    priv->framebuffer[offset] = pixel_index;
    
    /* Mark dirty for partial updates */
    vga_mark_dirty(priv, x, y);
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_flip(gfx_device_t* dev) {
    vga_graphics_private_t* priv = (vga_graphics_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Swap buffers */
    vga_swap_buffers(priv);
    
    return GFX_OK;
}

static gfx_result_t vga_gfx_wait_vsync(gfx_device_t* dev) {
    (void)dev;
    
    /* Wait for vertical retrace */
    while (gfx_inb(VGA_INPUT_STATUS1_COLOR) & 0x08) {
        /* Still in retrace */
    }
    while (!(gfx_inb(VGA_INPUT_STATUS1_COLOR) & 0x08)) {
        /* Wait for retrace to start */
    }
    
    return GFX_OK;
}

/* ============================================================================
 * Module Init/Exit
 * ============================================================================ */

gfx_result_t vga_graphics_driver_init(void) {
    debug_print("[VGA] Registering VGA graphics mode driver\n");
    return gfx_register_driver(&vga_graphics_gfx_driver);
}

void vga_graphics_driver_exit(void) {
    gfx_unregister_driver(&vga_graphics_gfx_driver);
}
