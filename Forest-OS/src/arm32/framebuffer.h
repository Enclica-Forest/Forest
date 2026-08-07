/*
 * framebuffer.h - Simple Framebuffer driver interface for Fern ARM32
 *
 * Supports QEMU -machine virt with:
 *   - virtio-gpu-pci  (simple-framebuffer exposed at a DTB-defined address)
 *   - pl110/pl111     (ARM Color LCD Controller on Versatile/RealView boards)
 *   - VESA/simple-fb  (generic linear framebuffer at a fixed physical address)
 *
 * The driver assumes a linear, identity-mapped framebuffer (physical address
 * == virtual address).  Call arm_fb_init() with a populated arm_fb_config_t
 * from your DTB parser, or call arm_fb_init_default() to use the QEMU virt
 * simple-framebuffer defaults (1024x768 32bpp at ARM_FB_DEFAULT_BASE).
 *
 * Pixel formats
 * -------------
 *  ARM_FB_FORMAT_RGB565   - 16 bpp: RRRRRGGGGGGBBBBB
 *  ARM_FB_FORMAT_RGB888   - 24 bpp: RRGGBB (packed, no padding)
 *  ARM_FB_FORMAT_XRGB8888 - 32 bpp: xxRRGGBB  (x bits ignored/undefined)
 *  ARM_FB_FORMAT_ARGB8888 - 32 bpp: AARRGGBB  (alpha channel present)
 *
 * All color arguments to the drawing functions use 0xAARRGGBB convention
 * (or 0x00RRGGBB when the format has no alpha).  arm_fb_rgb() is a helper
 * that builds a 32-bit color from separate R, G, B bytes.
 *
 * Thread safety
 * -------------
 * No locking is performed.  The caller is responsible for serialising access
 * if the framebuffer is shared between ISR and task contexts.
 */

#ifndef ARM32_FRAMEBUFFER_H
#define ARM32_FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Pixel format constants
 * ========================================================================= */

/** 16 bpp RGB565 */
#define ARM_FB_FORMAT_RGB565    0u
/** 24 bpp RGB888 (packed) */
#define ARM_FB_FORMAT_RGB888    1u
/** 32 bpp XRGB8888 – the most common QEMU virtio-gpu format */
#define ARM_FB_FORMAT_XRGB8888  2u
/** 32 bpp ARGB8888 */
#define ARM_FB_FORMAT_ARGB8888  3u

/* =========================================================================
 * QEMU virt simple-framebuffer defaults
 *
 * QEMU -machine virt exposes a simple-framebuffer device node in its DTB
 * at a machine-specific address.  The address below matches the default
 * reported by QEMU 8.x for -machine virt when no explicit video base is set.
 * Adjust ARM_FB_DEFAULT_BASE if your QEMU version or DTB reports differently.
 *
 * You can also discover the real address by reading the DTB:
 *   /chosen/framebuffer { reg = <0xSSSSSSSS 0xLLLLLLLL>; ... };
 * ========================================================================= */

/** Default framebuffer physical base address (QEMU virt simple-fb) */
#ifndef ARM_FB_DEFAULT_BASE
#define ARM_FB_DEFAULT_BASE     0x3C000000UL
#endif

/** Default width in pixels */
#ifndef ARM_FB_DEFAULT_WIDTH
#define ARM_FB_DEFAULT_WIDTH    1024u
#endif

/** Default height in pixels */
#ifndef ARM_FB_DEFAULT_HEIGHT
#define ARM_FB_DEFAULT_HEIGHT   768u
#endif

/** Default bits per pixel */
#ifndef ARM_FB_DEFAULT_BPP
#define ARM_FB_DEFAULT_BPP      32u
#endif

/** Default pixel format */
#ifndef ARM_FB_DEFAULT_FORMAT
#define ARM_FB_DEFAULT_FORMAT   ARM_FB_FORMAT_XRGB8888
#endif

/* =========================================================================
 * Framebuffer configuration structure
 *
 * Passed to arm_fb_init() by the caller.  Typically populated from the DTB
 * simple-framebuffer node, a VirtIO GPU query, or hard-coded defaults.
 * ========================================================================= */

/**
 * arm_fb_config_t - Framebuffer hardware description.
 *
 * @base:   Physical base address of the linear framebuffer memory.
 *          Must be accessible (identity-mapped or mapped by the caller before
 *          calling arm_fb_init()).
 * @width:  Horizontal resolution in pixels.
 * @height: Vertical resolution in pixels.
 * @stride: Number of bytes between the start of consecutive rows.
 *          Usually >= width * (bpp / 8); may be larger due to hardware
 *          alignment requirements.
 * @bpp:    Bits per pixel: 16, 24, or 32.
 * @format: One of the ARM_FB_FORMAT_* constants above.
 */
typedef struct arm_fb_config {
    uintptr_t base;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;   /* bytes per row */
    uint32_t  bpp;      /* bits per pixel: 16, 24, or 32 */
    uint32_t  format;   /* ARM_FB_FORMAT_* */
} arm_fb_config_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * arm_fb_init() - Initialise the framebuffer with a caller-supplied config.
 *
 * Stores the configuration, performs a basic sanity check, and clears the
 * entire framebuffer to black (0x00000000).
 *
 * @config: Pointer to a fully populated arm_fb_config_t.  Must not be NULL.
 *          The structure is copied; the caller may free it after this call.
 *
 * @return: true on success, false if the configuration is invalid (e.g. zero
 *          base address, zero dimensions, or unsupported bpp).
 */
bool arm_fb_init(const arm_fb_config_t *config);

/**
 * arm_fb_init_default() - Initialise using QEMU virt simple-fb defaults.
 *
 * Equivalent to calling arm_fb_init() with:
 *   base   = ARM_FB_DEFAULT_BASE
 *   width  = ARM_FB_DEFAULT_WIDTH
 *   height = ARM_FB_DEFAULT_HEIGHT
 *   stride = width * (bpp / 8)   (no extra padding)
 *   bpp    = ARM_FB_DEFAULT_BPP
 *   format = ARM_FB_DEFAULT_FORMAT
 *
 * Use this when no DTB is available (e.g. when QEMU is run with -kernel and
 * the simple-framebuffer address is hard-coded).
 *
 * @return: true on success (always succeeds with default values).
 */
bool arm_fb_init_default(void);

/**
 * arm_fb_set_pixel() - Write a single pixel.
 *
 * @x:     Horizontal coordinate (0 = left edge).  Out-of-bounds is silently
 *         ignored.
 * @y:     Vertical coordinate (0 = top edge).
 * @color: Pixel color as 0x00RRGGBB (alpha byte is ignored for non-ARGB
 *         formats; for ARGB8888 use 0xAARRGGBB).
 *
 * The color is converted to the framebuffer's native format before writing.
 */
void arm_fb_set_pixel(uint32_t x, uint32_t y, uint32_t color);

/**
 * arm_fb_get_pixel() - Read a single pixel.
 *
 * @x: Horizontal coordinate.
 * @y: Vertical coordinate.
 *
 * @return: Color at (x, y) as 0x00RRGGBB, or 0 if out-of-bounds.
 */
uint32_t arm_fb_get_pixel(uint32_t x, uint32_t y);

/**
 * arm_fb_fill() - Fill a rectangular region with a solid color.
 *
 * Clips the rectangle to the framebuffer boundaries.  For 32-bpp formats this
 * uses an optimised word-fill loop rather than individual set_pixel calls.
 *
 * @x, @y:  Top-left corner of the rectangle.
 * @w, @h:  Width and height in pixels.
 * @color:  Fill color (same convention as arm_fb_set_pixel).
 */
void arm_fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t color);

/**
 * arm_fb_clear() - Clear the entire framebuffer to a solid color.
 *
 * @color: Background color (same convention as arm_fb_set_pixel).
 *         Pass 0x00000000 for black.
 */
void arm_fb_clear(uint32_t color);

/**
 * arm_fb_blit() - Copy a pixel array to the framebuffer.
 *
 * Source pixels are in 0x00RRGGBB format (32-bit each), regardless of the
 * framebuffer's native format.  The driver converts each pixel on the fly.
 *
 * @src:       Pointer to the source pixel array (row-major, top-to-bottom).
 * @dst_x:     Destination X coordinate on screen.
 * @dst_y:     Destination Y coordinate on screen.
 * @src_w:     Width of the source image in pixels.
 * @src_h:     Height of the source image in pixels.
 * @src_stride: Source row stride in bytes (pass src_w * 4 for packed data).
 */
void arm_fb_blit(const uint32_t *src,
                 uint32_t dst_x, uint32_t dst_y,
                 uint32_t src_w, uint32_t src_h,
                 uint32_t src_stride);

/**
 * arm_fb_draw_char() - Render a single 8x8 ASCII character.
 *
 * Uses the embedded 8x8 IBM VGA / public-domain bitmap font.  Characters
 * outside the printable ASCII range (0x20–0x7E) are rendered as a space.
 *
 * @x, @y: Top-left pixel coordinate of the glyph.
 * @c:     Character to render (7-bit ASCII).
 * @fg:    Foreground color (0x00RRGGBB).
 * @bg:    Background color (0x00RRGGBB).  Every glyph pixel not set in the
 *         font bitmap is drawn in @bg.
 */
void arm_fb_draw_char(uint32_t x, uint32_t y, char c,
                      uint32_t fg, uint32_t bg);

/**
 * arm_fb_draw_string() - Render a NUL-terminated ASCII string.
 *
 * Characters are placed left-to-right, 8 pixels apart horizontally.
 * No line wrapping or scrolling is performed.  '\n' is interpreted as a
 * newline (moves to x_start, advances y by 8).
 *
 * @x, @y:  Starting top-left coordinate.
 * @str:    NUL-terminated string to draw.
 * @fg:     Foreground color.
 * @bg:     Background color.
 */
void arm_fb_draw_string(uint32_t x, uint32_t y, const char *str,
                        uint32_t fg, uint32_t bg);

/**
 * arm_fb_rgb() - Compose a 32-bit color value from R, G, B components.
 *
 * @r: Red   channel (0–255).
 * @g: Green channel (0–255).
 * @b: Blue  channel (0–255).
 *
 * @return: 0x00RRGGBB packed color, suitable for all arm_fb_* drawing
 *          functions.
 */
uint32_t arm_fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * arm_fb_get_width() - Return the current framebuffer width in pixels.
 */
uint32_t arm_fb_get_width(void);

/**
 * arm_fb_get_height() - Return the current framebuffer height in pixels.
 */
uint32_t arm_fb_get_height(void);

#endif /* ARM32_FRAMEBUFFER_H */
