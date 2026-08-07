/*
 * Cross-architecture framebuffer implementation
 *
 * Detects and initializes the framebuffer from multiple boot sources:
 *   1. UEFI GOP (Graphics Output Protocol) - highest priority
 *   2. DTB /chosen/framebuffer or /soc/framebuffer (ARM, RISC-V)
 *   3. Multiboot framebuffer info (x86 BIOS BGA/VBE)
 *
 * All pixel operations use stored info and handle format conversion
 * from 0x00RRGGBB to the native pixel layout.
 */

#include "framebuffer.h"
#include "arch.h"

#if ARCH_IS_X86
#include "../include/multiboot.h"
#endif

#include "../fdt.h"

/* UEFI GOP info (from uefi/uefi_gop.c) */
typedef struct {
    void*    BaseAddress;
    uint64_t BufferSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t PixelsPerScanLine;
    uint32_t BitsPerPixel;
    uint32_t PixelFormat;
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} uefi_gop_info_t;

extern const uefi_gop_info_t* uefi_gop_get_info(void) __attribute__((weak));

/* Debug logging (minimal, early-boot safe) */
#if !defined(__riscv)
extern void debuglog(int level, const char* fmt, ...);
#define FB_LOG(level, ...) debuglog(level, __VA_ARGS__)
#define FB_DEBUG  3
#define FB_INFO   2
#define FB_WARN   1
#define FB_ERROR  0
#else
/* RISC-V uses uart_puts directly */
extern void riscv64_uart_puts(const char* s);
#define FB_LOG(level, ...) ((void)0)
#define FB_DEBUG  3
#define FB_INFO   2
#define FB_WARN   1
#define FB_ERROR  0
#endif

/* Global framebuffer state */
static arch_framebuffer_info_t g_fb_info = {0};
static bool g_fb_initialized = false;

/* Forward declarations */
static int fb_try_uefi_gop(void);
static int fb_try_dtb(void);
#if ARCH_IS_X86
static int fb_try_multiboot(void);
#endif

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t fb_calc_pitch(uint32_t width, uint32_t bpp)
{
    return width * ((bpp + 7) / 8);
}

static uint32_t fb_detect_format(uint32_t bpp, uint32_t uefi_pixel_format)
{
    switch (bpp) {
    case 32:
        /* UEFI GOP: PixelBlueGreenRedReserved8BitPerColor = 1 is BGRA */
        if (uefi_pixel_format == 1)
            return FB_PIX_FMT_BGRA8888;
        /* Default to BGRA for 32bpp (most common on UEFI) */
        return FB_PIX_FMT_BGRA8888;
    case 24:
        return FB_PIX_FMT_BGR888;
    case 16:
        return FB_PIX_FMT_RGB565;
    default:
        return FB_PIX_FMT_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* Source: UEFI GOP                                                    */
/* ------------------------------------------------------------------ */

static int fb_try_uefi_gop(void)
{
    const uefi_gop_info_t* gop = uefi_gop_get_info();
    if (!gop || !gop->BaseAddress || gop->Width == 0 || gop->Height == 0)
        return -1;

    g_fb_info.phys    = (uintptr_t)gop->BaseAddress;
    g_fb_info.width   = gop->Width;
    g_fb_info.height  = gop->Height;
    g_fb_info.bpp     = gop->BitsPerPixel;
    g_fb_info.pitch   = gop->PixelsPerScanLine * ((gop->BitsPerPixel + 7) / 8);
    g_fb_info.format  = fb_detect_format(gop->BitsPerPixel, gop->PixelFormat);
    g_fb_info.size    = g_fb_info.pitch * g_fb_info.height;
    g_fb_info.source  = FB_SOURCE_UEFI_GOP;
    g_fb_info.addr    = NULL; /* Mapping deferred to VMM init */

    return 0;
}

/* ------------------------------------------------------------------ */
/* Source: DTB (ARM / RISC-V)                                         */
/* ------------------------------------------------------------------ */

/*
 * Try to extract framebuffer info from the device tree.
 *
 * Checks these DTB paths (in order):
 *   /chosen/framebuffer
 *   /chosen/framebuffer@<addr>
 *   /soc/framebuffer
 *   /soc/framebuffer@<addr>
 *
 * Properties read: reg (base + size), width, height, stride, bits-per-pixel
 */
static int fb_try_dtb(void)
{
    /* Try /chosen/framebuffer first */
    const char* paths[] = {
        "/chosen/framebuffer",
        "/chosen/framebuffer@0",
        "/soc/framebuffer",
        "/soc/framebuffer@0",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        uint32_t w = 0, h = 0, stride = 0, bpp = 0;

        /* width and height are required */
        w = fdt_get_u32(paths[i], "width", 0);
        h = fdt_get_u32(paths[i], "height", 0);
        if (w == 0 || h == 0)
            continue;

        /* stride (bytes per line) — some DTBs call it "stride" */
        stride = fdt_get_u32(paths[i], "stride", 0);

        /* bits-per-pixel */
        bpp = fdt_get_u32(paths[i], "bits-per-pixel", 0);

        /* Try reg property for base address (64-bit: addr_hi . addr_lo) */
        uint32_t reg_len = 0;
        const uint32_t* reg = fdt_get_property(paths[i], "reg", &reg_len);
        uintptr_t base = 0;
        if (reg && reg_len >= 8) {
#if ARCH_BITS == 64
            base = ((uint64_t)fdt32_to_cpu(reg[0]) << 32) |
                    (uint64_t)fdt32_to_cpu(reg[1]);
#else
            base = (uint64_t)fdt32_to_cpu(reg[0]);
#endif
        }

        if (base == 0)
            continue;

        /* Fill in defaults */
        if (bpp == 0) bpp = 32;
        if (stride == 0) stride = fb_calc_pitch(w, bpp);

        g_fb_info.phys   = base;
        g_fb_info.width  = w;
        g_fb_info.height = h;
        g_fb_info.pitch  = stride;
        g_fb_info.bpp    = bpp;
        g_fb_info.format = fb_detect_format(bpp, 0);
        g_fb_info.size   = stride * h;
        g_fb_info.source = FB_SOURCE_DTB;
        g_fb_info.addr   = NULL;

        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Source: Multiboot (x86 BIOS BGA/VBE)                               */
/* ------------------------------------------------------------------ */

#if ARCH_IS_X86

/* Defined in kernel.c — populated early from multiboot info */
extern bool kernel_get_multiboot_framebuffer(uintptr_t* addr,
                                              uint32_t* width,
                                              uint32_t* height,
                                              uint32_t* bpp,
                                              uint32_t* pitch);

static int fb_try_multiboot(void)
{
    uintptr_t addr = 0;
    uint32_t w = 0, h = 0, bpp = 0, pitch = 0;

    if (!kernel_get_multiboot_framebuffer(&addr, &w, &h, &bpp, &pitch))
        return -1;

    if (addr == 0 || w == 0 || h == 0)
        return -1;

    if (pitch == 0)
        pitch = fb_calc_pitch(w, bpp);

    g_fb_info.phys   = addr;
    g_fb_info.width  = w;
    g_fb_info.height = h;
    g_fb_info.pitch  = pitch;
    g_fb_info.bpp    = bpp;
    g_fb_info.format = fb_detect_format(bpp, 0);
    g_fb_info.size   = pitch * h;
    g_fb_info.source = FB_SOURCE_MULTIBOOT;
    g_fb_info.addr   = NULL;

    return 0;
}

#endif /* ARCH_IS_X86 */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int framebuffer_init(void)
{
    if (g_fb_initialized)
        return 0;

    /* Clear info */
    g_fb_info = (arch_framebuffer_info_t){0};

    /* Try sources in priority order */

    /* 1. UEFI GOP (highest priority, any arch) */
    if (fb_try_uefi_gop() == 0) {
        FB_LOG(FB_INFO, "[FB] Detected UEFI GOP: %ux%u %ubpp phys=0x%lx\n",
               g_fb_info.width, g_fb_info.height, g_fb_info.bpp,
               (unsigned long)g_fb_info.phys);
        g_fb_initialized = true;
        return 0;
    }

    /* 2. DTB (ARM, RISC-V, or any arch with DTB) */
    if (fb_try_dtb() == 0) {
        FB_LOG(FB_INFO, "[FB] Detected DTB framebuffer: %ux%u %ubpp phys=0x%lx\n",
               g_fb_info.width, g_fb_info.height, g_fb_info.bpp,
               (unsigned long)g_fb_info.phys);
        g_fb_initialized = true;
        return 0;
    }

    /* 3. Multiboot / BGA/VBE (x86 BIOS) */
#if ARCH_IS_X86
    if (fb_try_multiboot() == 0) {
        FB_LOG(FB_INFO, "[FB] Detected multiboot framebuffer: %ux%u %ubpp phys=0x%lx\n",
               g_fb_info.width, g_fb_info.height, g_fb_info.bpp,
               (unsigned long)g_fb_info.phys);
        g_fb_initialized = true;
        return 0;
    }
#endif

    FB_LOG(FB_WARN, "[FB] No framebuffer detected from any source\n");
    return -1;
}

void framebuffer_get_info(uint32_t* width, uint32_t* height,
                          uint32_t* pitch, uint32_t* bpp)
{
    if (width)  *width  = g_fb_info.width;
    if (height) *height = g_fb_info.height;
    if (pitch)  *pitch  = g_fb_info.pitch;
    if (bpp)    *bpp    = g_fb_info.bpp;
}

void* framebuffer_get_buffer(void)
{
    return g_fb_info.addr;
}

uintptr_t framebuffer_get_phys(void)
{
    return g_fb_info.phys;
}

bool framebuffer_is_available(void)
{
    return g_fb_initialized && g_fb_info.phys != 0;
}

/* ------------------------------------------------------------------ */
/* Pixel operations                                                    */
/* ------------------------------------------------------------------ */

/*
 * Convert 0x00RRGGBB to the native pixel format and write it to the
 * framebuffer at (x, y). Handles bounds checking.
 */
void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!g_fb_initialized || !g_fb_info.addr)
        return;
    if (x >= g_fb_info.width || y >= g_fb_info.height)
        return;

    uint8_t* dst = (uint8_t*)g_fb_info.addr +
                   (size_t)y * g_fb_info.pitch;

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b = (color >>  0) & 0xFF;

    switch (g_fb_info.format) {
    case FB_PIX_FMT_BGRA8888: {
        uint32_t* p = (uint32_t*)(dst + x * 4);
        *p = (0xFFu << 24) | ((uint32_t)r << 16) |
             ((uint32_t)g << 8) | (uint32_t)b;
        break;
    }
    case FB_PIX_FMT_RGBA8888: {
        uint32_t* p = (uint32_t*)(dst + x * 4);
        *p = ((uint32_t)r << 24) | ((uint32_t)g << 16) |
             ((uint32_t)b << 8) | 0xFFu;
        break;
    }
    case FB_PIX_FMT_BGR888: {
        uint8_t* p = dst + x * 3;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        break;
    }
    case FB_PIX_FMT_RGB888: {
        uint8_t* p = dst + x * 3;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        break;
    }
    case FB_PIX_FMT_RGB565: {
        uint16_t* p = (uint16_t*)(dst + x * 2);
        *p = (uint16_t)(((uint16_t)r >> 3) << 11) |
             (uint16_t)(((uint16_t)g >> 2) << 5) |
             (uint16_t)((uint16_t)b >> 3);
        break;
    }
    default:
        break;
    }
}

/*
 * Fill a rectangle with a solid color.
 * Clips to screen boundaries.
 */
void framebuffer_fill_rect(uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t color)
{
    if (!g_fb_initialized || !g_fb_info.addr)
        return;
    if (w == 0 || h == 0)
        return;

    /* Clip to screen */
    if (x >= g_fb_info.width || y >= g_fb_info.height)
        return;
    if (x + w > g_fb_info.width)
        w = g_fb_info.width - x;
    if (y + h > g_fb_info.height)
        h = g_fb_info.height - y;

    /* Prepare the native pixel value */
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b = (color >>  0) & 0xFF;

    uint8_t* row = (uint8_t*)g_fb_info.addr +
                   (size_t)y * g_fb_info.pitch + x * ((g_fb_info.bpp + 7) / 8);

    for (uint32_t j = 0; j < h; j++) {
        uint8_t* p = row;

        switch (g_fb_info.format) {
        case FB_PIX_FMT_BGRA8888:
        case FB_PIX_FMT_RGBA8888: {
            uint32_t native;
            if (g_fb_info.format == FB_PIX_FMT_BGRA8888)
                native = 0xFF000000u | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | (uint32_t)b;
            else
                native = ((uint32_t)r << 24) | ((uint32_t)g << 16) |
                         ((uint32_t)b << 8) | 0xFFu;
            uint32_t* wp = (uint32_t*)p;
            for (uint32_t i = 0; i < w; i++)
                wp[i] = native;
            break;
        }
        case FB_PIX_FMT_BGR888:
        case FB_PIX_FMT_RGB888: {
            for (uint32_t i = 0; i < w; i++) {
                if (g_fb_info.format == FB_PIX_FMT_BGR888) {
                    p[i*3 + 0] = b;
                    p[i*3 + 1] = g;
                    p[i*3 + 2] = r;
                } else {
                    p[i*3 + 0] = r;
                    p[i*3 + 1] = g;
                    p[i*3 + 2] = b;
                }
            }
            break;
        }
        case FB_PIX_FMT_RGB565: {
            uint16_t native = (uint16_t)(((uint16_t)r >> 3) << 11) |
                              (uint16_t)(((uint16_t)g >> 2) << 5) |
                              (uint16_t)((uint16_t)b >> 3);
            uint16_t* wp = (uint16_t*)p;
            for (uint32_t i = 0; i < w; i++)
                wp[i] = native;
            break;
        }
        default:
            /* Unknown format: fill with black */
            for (uint32_t i = 0; i < w; i++) {
                p[i*3 + 0] = 0;
                p[i*3 + 1] = 0;
                p[i*3 + 2] = 0;
            }
            break;
        }

        row += g_fb_info.pitch;
    }
}

/*
 * Clear the entire screen with a solid color.
 */
void framebuffer_clear(uint32_t color)
{
    framebuffer_fill_rect(0, 0, g_fb_info.width, g_fb_info.height, color);
}
