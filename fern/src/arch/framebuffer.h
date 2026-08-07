/*
 * Cross-architecture framebuffer interface
 *
 * Provides a unified API for framebuffer detection and basic pixel
 * operations across all supported architectures and boot protocols:
 *   - x86 BIOS: BGA/VBE (via multiboot framebuffer info)
 *   - x86 UEFI: GOP (via uefi_gop_get_info)
 *   - ARM32/AArch64: DTB /chosen/framebuffer or /soc/framebuffer
 *   - RISC-V: DTB /chosen/framebuffer
 *   - All architectures: UEFI GOP if booted via UEFI
 *
 * Detection order: UEFI GOP > DTB > multiboot (BGA/VBE)
 */

#ifndef ARCH_FRAMEBUFFER_H
#define ARCH_FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>

/* Framebuffer pixel format */
#define FB_PIX_FMT_UNKNOWN     0
#define FB_PIX_FMT_RGB888      1   /* 24-bit, 8 bits per channel, RGB order */
#define FB_PIX_FMT_BGR888      2   /* 24-bit, BGR order */
#define FB_PIX_FMT_RGBA8888    3   /* 32-bit, RGBA order */
#define FB_PIX_FMT_BGRA8888    4   /* 32-bit, BGRA order (typical UEFI) */
#define FB_PIX_FMT_RGB565      5   /* 16-bit, 5-6-5 */

/* Framebuffer source */
#define FB_SOURCE_NONE          0
#define FB_SOURCE_UEFI_GOP      1
#define FB_SOURCE_DTB           2
#define FB_SOURCE_MULTIBOOT     3   /* BGA/VBE via multiboot */

/* Framebuffer info structure */
typedef struct {
    void*       addr;       /* Kernel virtual address (NULL if unmapped) */
    uintptr_t   phys;       /* Physical address */
    uint32_t    width;      /* Width in pixels */
    uint32_t    height;     /* Height in pixels */
    uint32_t    pitch;      /* Bytes per scanline */
    uint32_t    bpp;        /* Bits per pixel (16, 24, or 32) */
    uint32_t    format;     /* Pixel format (FB_PIX_FMT_*) */
    uint32_t    size;       /* Total size in bytes */
    uint32_t    source;     /* Detection source (FB_SOURCE_*) */
} arch_framebuffer_info_t;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * framebuffer_init - Detect and initialize the framebuffer.
 *
 * Probes sources in priority order (UEFI > DTB > multiboot) and
 * populates the global framebuffer info. Must be called after the
 * boot protocol has been identified and FDT parsed (if applicable).
 *
 * Returns 0 on success (framebuffer found), -1 on failure.
 */
int framebuffer_init(void);

/*
 * framebuffer_get_info - Retrieve display parameters.
 *
 * All output parameters are optional (may be NULL).
 */
void framebuffer_get_info(uint32_t* width, uint32_t* height,
                          uint32_t* pitch, uint32_t* bpp);

/*
 * framebuffer_get_buffer - Return the framebuffer base address.
 *
 * Returns the kernel virtual address of the framebuffer, or NULL
 * if no framebuffer is available or it hasn't been mapped yet.
 */
void* framebuffer_get_buffer(void);

/*
 * framebuffer_get_phys - Return the physical address of the framebuffer.
 *
 * Returns the physical address, or 0 if unavailable.
 */
uintptr_t framebuffer_get_phys(void);

/*
 * framebuffer_put_pixel - Set a single pixel.
 *
 * Color is in 0x00RRGGBB format. Format conversion is handled
 * internally based on the detected pixel format.
 */
void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color);

/*
 * framebuffer_fill_rect - Fill a rectangle with a solid color.
 *
 * Color is in 0x00RRGGBB format. Performs clipping against screen
 * boundaries.
 */
void framebuffer_fill_rect(uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t color);

/*
 * framebuffer_clear - Clear the entire screen.
 *
 * Color is in 0x00RRGGBB format.
 */
void framebuffer_clear(uint32_t color);

/*
 * framebuffer_is_available - Check if a framebuffer was detected.
 */
bool framebuffer_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* ARCH_FRAMEBUFFER_H */
