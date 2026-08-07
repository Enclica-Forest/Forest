/*
 * LeafGFX - Core Implementation
 *
 * Provides basic framebuffer access and drawing primitives
 * for Forest OS userspace applications.
 */

#include "leafgfx.h"
#include "leafgfx_bmp.h"
#include "leafgfx_font.h"
#include "leafgfx_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Use sysroot syscall interface to avoid enum/macro conflicts with kernel headers
#include <forestos/syscalls.h>
#include "framebuffer.h"

// ============================================================================
// Syscall Interface
// ============================================================================

static long syscall_raw(int num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long result;
#if defined(__x86_64__)
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ __volatile__(
        "syscall"
        : "=a"(result)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
#else
    __asm__ __volatile__(
        "push %%ebp\n"
        "mov %7, %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(result)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), "g"(a6)
        : "memory"
    );
#endif
    return result;
}

#define syscall0(n)                     syscall_raw(n, 0, 0, 0, 0, 0, 0)
#define syscall1(n, a)                  syscall_raw(n, (long)(a), 0, 0, 0, 0, 0)

// ============================================================================
// Global State
// ============================================================================

static gfx_framebuffer_t g_fb = {0};
static bool g_initialized = false;
static int g_oob_pixel_count = 0;

// Clipping rectangle
static gfx_rect_t g_clip = {0};
static bool g_clip_enabled = false;

// Clip stack
#define GFX_CLIP_STACK_MAX 8
static gfx_rect_t g_clip_stack[GFX_CLIP_STACK_MAX];
static int32_t g_clip_stack_top = -1;

// Dirty tracking
static int32_t g_dirty_x = 0;
static int32_t g_dirty_y = 0;
static int32_t g_dirty_x2 = 0;
static int32_t g_dirty_y2 = 0;
static bool g_dirty_active = false;

// Back buffer
static uint32_t* g_backbuffer = NULL;
static size_t g_backbuffer_size = 0;  // bytes actually allocated for g_backbuffer
static bool g_use_backbuffer = false;
static bool g_backbuffer_valid = false;

// Last-seen SYS_GET_FB_GENERATION value; see gfx_refresh_if_mode_changed().
static uint32_t g_fb_mode_generation = 0;

// Sanity bounds mirroring the kernel's user-space layout (see
// MEMORY_USER_START/MEMORY_USER_END in src/include/memory_new.h and
// src/include/memory.h). LeafGFX runs in ring 3 and has no syscall to query
// these, so they're hardcoded to match the kernel ABI. Used only to catch
// gross pointer corruption (e.g. a kernel-space address landing in
// g_backbuffer) before it turns into a wild write.
#define LEAFGFX_USER_SPACE_MIN 0x40000000u
#define LEAFGFX_USER_SPACE_MAX 0xC0000000u

// Cheap per-access sanity check for g_backbuffer: non-NULL, 4-byte aligned,
// and inside the user address range. Deliberately does NOT recheck
// g_backbuffer_size here (that's only meaningful right after
// alloc/mode-change) so this stays cheap enough for the per-pixel hot path.
static inline bool gfx_backbuffer_ptr_sane(void) {
    uintptr_t addr = (uintptr_t)g_backbuffer;
    return addr != 0 &&
           (addr & (sizeof(uint32_t) - 1)) == 0 &&
           addr >= LEAFGFX_USER_SPACE_MIN &&
           addr < LEAFGFX_USER_SPACE_MAX;
}

static inline void gfx_validate_backbuffer(void) {
    if (!g_use_backbuffer || !g_backbuffer) {
        g_backbuffer_valid = false;
        return;
    }

    size_t needed = (size_t)g_fb.width * (size_t)g_fb.height * sizeof(uint32_t);
    bool sane = gfx_backbuffer_ptr_sane() && g_backbuffer_size >= needed;

    if (!sane) {
        static int warned = 0;
        if (warned < 8) {
            printf("[LeafGFX] ERROR: backbuffer failed sanity check (ptr=%p size=%zu needed=%zu) "
                   "- disabling backbuffer\n",
                   (void*)g_backbuffer, g_backbuffer_size, needed);
            warned++;
        }
        g_use_backbuffer = false;
        g_backbuffer_valid = false;
        return;
    }

    g_backbuffer_valid = true;
}

// Choke point for every backbuffer read/write. Re-checks the pointer itself
// (not just the cached g_backbuffer_valid flag) so corruption discovered
// mid-session disables the backbuffer immediately instead of faulting on
// the next dereference. Cheap enough to call per-pixel: a handful of
// integer comparisons, no syscalls.
static inline bool gfx_backbuffer_use_ok(void) {
    if (!g_backbuffer_valid) return false;
    if (gfx_backbuffer_ptr_sane()) return true;

    static int warned = 0;
    if (warned < 8) {
        printf("[LeafGFX] ERROR: backbuffer pointer corrupted mid-session (ptr=%p) "
               "- disabling backbuffer\n", (void*)g_backbuffer);
        warned++;
    }
    g_use_backbuffer = false;
    g_backbuffer_valid = false;
    return false;
}

// Custom cursor
static uint32_t* g_custom_cursor = NULL;
static int32_t g_cursor_w = 0;
static int32_t g_cursor_h = 0;
static int32_t g_cursor_hotx = 0;
static int32_t g_cursor_hoty = 0;

// Offscreen app buffer
static gfx_app_buffer_t g_app_buffer = {0};
static gfx_app_buffer_t* g_current_app_buffer = NULL;

static inline uint8_t gfx_argb_to_index8(uint32_t color) {
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);
    uint8_t rq = (uint8_t)((r * 5u) / 255u);
    uint8_t gq = (uint8_t)((g * 5u) / 255u);
    uint8_t bq = (uint8_t)((b * 5u) / 255u);
    return (uint8_t)(16u + rq * 36u + gq * 6u + bq);
}

static inline uint32_t gfx_expected_bpp_for_format(uint32_t format) {
    switch (format) {
        case GFX_FORMAT_INDEXED_8:
            return 1;
        case GFX_FORMAT_RGB_555:
        case GFX_FORMAT_RGB_565:
            return 2;
        case GFX_FORMAT_RGB_888:
        case GFX_FORMAT_BGR_888:
            return 3;
        case GFX_FORMAT_RGBA_8888:
        case GFX_FORMAT_BGRA_8888:
            return 4;
        default:
            return 0;
    }
}

static inline uint32_t gfx_pick_format_for_bpp(uint32_t bytes_per_pixel, uint32_t hint_format) {
    switch (bytes_per_pixel) {
        case 1:
            return GFX_FORMAT_INDEXED_8;
        case 2:
            return (hint_format == GFX_FORMAT_RGB_555) ? GFX_FORMAT_RGB_555 : GFX_FORMAT_RGB_565;
        case 3:
            return (hint_format == GFX_FORMAT_RGB_888) ? GFX_FORMAT_RGB_888 : GFX_FORMAT_BGR_888;
        case 4:
        default:
            return (hint_format == GFX_FORMAT_RGBA_8888) ? GFX_FORMAT_RGBA_8888 : GFX_FORMAT_BGRA_8888;
    }
}

static inline uint32_t gfx_effective_format(void) {
    uint32_t expected = gfx_expected_bpp_for_format(g_fb.format);
    if (expected != 0 && expected == g_fb.bytes_per_pixel) {
        return g_fb.format;
    }
    return UINT32_MAX;
}

// ============================================================================
// Initialization
// ============================================================================

/*
 * The kernel has no signal/IPC push mechanism to tell a running client its
 * SYS_MMAP_FB mapping went stale after SYS_SET_FB_MODE (see
 * sys_set_fb_mode()'s framebuffer_mmap_unmap_all() call in src/syscall.c).
 * Instead it exposes a cheap poll-able counter, SYS_GET_FB_GENERATION,
 * bumped every time that happens. Call this once per frame, before
 * touching g_fb.addr, to notice the change and re-map/re-derive geometry.
 * A negative return from the syscall (e.g. running against an older kernel
 * without this syscall) is treated as "unsupported" and left alone --
 * no worse than before this existed.
 */
static void gfx_refresh_if_mode_changed(void) {
    if (!g_initialized) {
        return;
    }

    // Re-check the backbuffer pointer once per frame regardless of whether
    // the fb mode changed -- this is the only per-frame choke point that
    // runs before drawing starts, so it's the earliest place to catch
    // mid-session corruption of g_backbuffer before it's used.
    if (g_use_backbuffer) {
        gfx_validate_backbuffer();
    }

    long gen = syscall0(SYS_GET_FB_GENERATION);
    if (gen < 0 || (uint32_t)gen == g_fb_mode_generation) {
        return;
    }

    fb_info_t fb_info;
    long result = syscall1(SYS_GET_FB_INFO, (long)&fb_info);
    if (result < 0 || fb_info.width == 0 || fb_info.height == 0) {
        return;
    }

    long fb_addr = syscall0(SYS_MMAP_FB);
    if (fb_addr <= 0) {
        return;
    }

    g_fb.addr = (void*)fb_addr;
    g_fb.width = fb_info.width;
    g_fb.height = fb_info.height;
    g_fb.pitch = fb_info.pitch;
    g_fb.bpp = fb_info.bpp;
    g_fb.phys_addr = fb_info.phys_addr;
    g_fb.size = fb_info.size;
    g_fb.format = fb_info.format;
    g_fb.bytes_per_pixel = (fb_info.bpp + 7) / 8;
    if (g_fb.bytes_per_pixel == 0 || g_fb.bytes_per_pixel > 4) {
        g_fb.bytes_per_pixel = 4;
    }

    g_clip.x = 0;
    g_clip.y = 0;
    g_clip.width = g_fb.width;
    g_clip.height = g_fb.height;
    g_clip_enabled = false;
    g_clip_stack_top = -1;
    g_dirty_active = false;

    if (g_use_backbuffer) {
        gfx_destroy_backbuffer();
        g_use_backbuffer = true;
        if (gfx_create_backbuffer() != 0) {
            g_use_backbuffer = false;
        }
    }

    g_fb_mode_generation = (uint32_t)gen;
    printf("[LeafGFX] Framebuffer mode changed, remapped to %ux%u %ubpp\n",
           g_fb.width, g_fb.height, g_fb.bpp);
}

int gfx_init(void) {
    if (g_initialized) {
        return 0;
    }

    fb_info_t fb_info;
    long result = syscall1(SYS_GET_FB_INFO, (long)&fb_info);
    if (result < 0) {
        printf("[LeafGFX] ERROR: Failed to get framebuffer info: %ld\n", result);
        return (int)result;
    }

    if (fb_info.width == 0 || fb_info.height == 0) {
        printf("[LeafGFX] ERROR: Invalid framebuffer dimensions: %ux%u\n",
               fb_info.width, fb_info.height);
        return -1;
    }

    if (fb_info.bpp < 8 || fb_info.bpp > 32) {
        printf("[LeafGFX] ERROR: Unsupported bpp: %u\n", fb_info.bpp);
        return -1;
    }

    result = syscall0(SYS_MMAP_FB);
    if (result < 0) {
        printf("[LeafGFX] ERROR: Failed to map framebuffer: %ld\n", result);
        return (int)result;
    }
    if (result == 0) {
        printf("[LeafGFX] ERROR: SYS_MMAP_FB returned NULL address\n");
        return -1;
    }

    g_fb.addr = (void*)result;
    g_fb.width = fb_info.width;
    g_fb.height = fb_info.height;
    g_fb.pitch = fb_info.pitch;
    g_fb.bpp = fb_info.bpp;
    g_fb.phys_addr = fb_info.phys_addr;
    g_fb.size = fb_info.size;
    g_fb.format = fb_info.format;

    if (fb_info.bpp == 0) {
        g_fb.bytes_per_pixel = 4;
    } else {
        g_fb.bytes_per_pixel = (fb_info.bpp + 7) / 8;
    }

    if (g_fb.bytes_per_pixel == 0 || g_fb.bytes_per_pixel > 4) {
        printf("[LeafGFX] WARNING: Invalid bpp %u, defaulting to 4 bytes/pixel\n", g_fb.bpp);
        g_fb.bytes_per_pixel = 4;
    }

    if (g_fb.width != 0 &&
        g_fb.pitch >= g_fb.width &&
        (g_fb.pitch % g_fb.width) == 0) {
        uint32_t stride_bpp = g_fb.pitch / g_fb.width;
        if (stride_bpp >= 1 && stride_bpp <= 4 && stride_bpp != g_fb.bytes_per_pixel) {
            printf("[LeafGFX] WARNING: bpp mismatch (reported %u bpp -> %u Bpp, pitch/width -> %u Bpp). "
                   "Using stride-derived Bpp.\n", g_fb.bpp, g_fb.bytes_per_pixel, stride_bpp);
            g_fb.bytes_per_pixel = stride_bpp;
            g_fb.bpp = stride_bpp * 8;
        }
    }

    uint32_t expected = gfx_expected_bpp_for_format(g_fb.format);
    if (g_fb.format > GFX_FORMAT_BGRA_8888 || (expected != 0 && expected != g_fb.bytes_per_pixel)) {
        uint32_t old_format = g_fb.format;
        g_fb.format = gfx_pick_format_for_bpp(g_fb.bytes_per_pixel, g_fb.format);
        printf("[LeafGFX] WARNING: Normalizing format %u -> %u for %u Bpp\n",
               old_format, g_fb.format, g_fb.bytes_per_pixel);
    }

    if (g_fb.size == 0) {
        g_fb.size = (size_t)g_fb.height * g_fb.pitch;
    }

    const char* format_name;
    switch (g_fb.format) {
        case GFX_FORMAT_INDEXED_8: format_name = "INDEXED8"; break;
        case GFX_FORMAT_RGB_555:   format_name = "RGB555"; break;
        case GFX_FORMAT_RGBA_8888: format_name = "RGBA8888"; break;
        case GFX_FORMAT_BGRA_8888: format_name = "BGRA8888"; break;
        case GFX_FORMAT_RGB_565:   format_name = "RGB565"; break;
        case GFX_FORMAT_RGB_888:   format_name = "RGB888"; break;
        case GFX_FORMAT_BGR_888:   format_name = "BGR888"; break;
        default:                   format_name = "UNKNOWN"; break;
    }
    printf("[LeafGFX] Framebuffer: %ux%u, %ubpp, pitch=%u, format=%s (%u), size=%u bytes\n",
           g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch, format_name, g_fb.format, g_fb.size);

    /*
     * Do not enable kernel-side framebuffer watcher from LeafGFX.
     * GUI apps explicitly present frames via SYS_FB_FLUSH after gfx_flip(),
     * and periodic kernel watcher activity can race userspace compositing.
     */

    g_clip.x = 0;
    g_clip.y = 0;
    g_clip.width = g_fb.width;
    g_clip.height = g_fb.height;
    g_clip_enabled = false;
    g_clip_stack_top = -1;
    g_dirty_active = false;

    g_initialized = true;

    long init_gen = syscall0(SYS_GET_FB_GENERATION);
    g_fb_mode_generation = (init_gen >= 0) ? (uint32_t)init_gen : 0;

    g_use_backbuffer = true;
    if (gfx_create_backbuffer() != 0) {
        g_use_backbuffer = false;
        printf("[LeafGFX] WARNING: Backbuffer allocation failed, drawing directly to framebuffer\n");
    }

    return 0;
}

void gfx_cleanup(void) {
    if (!g_initialized) {
        return;
    }

    if (g_backbuffer) {
        free(g_backbuffer);
        g_backbuffer = NULL;
    }
    g_backbuffer_size = 0;
    g_backbuffer_valid = false;
    g_use_backbuffer = false;

    if (g_custom_cursor) {
        free(g_custom_cursor);
        g_custom_cursor = NULL;
    }

    syscall0(SYS_MUNMAP_FB);
    g_fb.addr = NULL;
    g_initialized = false;
}

void gfx_app_shutdown(void) {
    /*
     * Order matters:
     * 1) stop input I/O handles
     * 2) free tracked assets (fonts/images)
     * 3) unmap framebuffer and free core graphics buffers
     */
    gfx_input_shutdown();
    gfx_font_release_all_tracked();
    gfx_image_release_all_tracked();
    gfx_cleanup();
}

const gfx_framebuffer_t* gfx_get_framebuffer(void) {
    return g_initialized ? &g_fb : NULL;
}

uint32_t gfx_screen_width(void) {
    return g_fb.width;
}

uint32_t gfx_screen_height(void) {
    return g_fb.height;
}

// ============================================================================
// Pixel Format Conversion Implementation
// ============================================================================

uint32_t gfx_color_to_fb(uint32_t argb_color) {
    if (!g_initialized) return argb_color;

    uint8_t a = (argb_color >> 24) & 0xFF;
    uint8_t r = (argb_color >> 16) & 0xFF;
    uint8_t g = (argb_color >> 8) & 0xFF;
    uint8_t b = argb_color & 0xFF;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_INDEXED_8:
            return gfx_argb_to_index8(argb_color);

        case GFX_FORMAT_RGB_555:
            return gfx_argb_to_rgb555(argb_color);

        case GFX_FORMAT_BGRA_8888:
            return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;

        case GFX_FORMAT_RGB_565:
            return gfx_argb_to_rgb565(argb_color);

        case GFX_FORMAT_RGB_888:
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

        case GFX_FORMAT_BGR_888:
            return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;

        case GFX_FORMAT_RGBA_8888:
            return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;

        default:
            return argb_color;
    }
}

uint32_t gfx_fb_to_color(void* fb_addr, uint32_t x, uint32_t y) {
    if (!g_initialized || !fb_addr) return 0;
    if (x >= g_fb.width || y >= g_fb.height) return 0;

    uint8_t* row = (uint8_t*)fb_addr + y * g_fb.pitch;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_INDEXED_8: {
            uint8_t idx = row[x];
            uint8_t v = (uint8_t)((idx * 255u) / 255u);
            return 0xFF000000 | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        }
        case GFX_FORMAT_RGB_555: {
            uint16_t rgb555 = ((uint16_t*)row)[x];
            return gfx_rgb555_to_argb(rgb555);
        }
        case GFX_FORMAT_BGRA_8888: {
            uint32_t bgra = ((uint32_t*)row)[x];
            return ((bgra & 0xFF00FF00) |
                    ((bgra & 0x00FF0000) >> 16) |
                    ((bgra & 0x000000FF) << 16));
        }
        case GFX_FORMAT_RGB_565: {
            uint16_t rgb565 = ((uint16_t*)row)[x];
            return gfx_rgb565_to_argb(rgb565);
        }
        case GFX_FORMAT_RGB_888: {
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
        case GFX_FORMAT_BGR_888: {
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
        case GFX_FORMAT_RGBA_8888: {
            uint32_t rgba = ((uint32_t*)row)[x];
            return ((rgba >> 8) | ((rgba & 0xFF) << 24)) | 0xFF000000;
        }
        default:
            switch (g_fb.bytes_per_pixel) {
                case 1: {
                    uint8_t v = row[x];
                    return 0xFF000000 | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
                }
                case 2: {
                    uint16_t p16 = ((uint16_t*)row)[x];
                    return gfx_rgb565_to_argb(p16);
                }
                case 3: {
                    uint8_t b = row[x * 3 + 0];
                    uint8_t g = row[x * 3 + 1];
                    uint8_t r = row[x * 3 + 2];
                    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                }
                case 4:
                default:
                    return ((uint32_t*)row)[x];
            }
    }
}

void gfx_fb_put_pixel(void* fb_addr, uint32_t x, uint32_t y, uint32_t color) {
    if (!g_initialized || !fb_addr) return;
    if (x >= g_fb.width || y >= g_fb.height) return;

    uint8_t* row = (uint8_t*)fb_addr + y * g_fb.pitch;

    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_INDEXED_8:
            row[x] = gfx_argb_to_index8(color);
            break;
        case GFX_FORMAT_RGB_555:
            ((uint16_t*)row)[x] = gfx_argb_to_rgb555(color);
            break;
        case GFX_FORMAT_BGRA_8888:
            ((uint32_t*)row)[x] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
            break;
        case GFX_FORMAT_RGB_565:
            ((uint16_t*)row)[x] = gfx_argb_to_rgb565(color);
            break;
        case GFX_FORMAT_BGR_888:
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
            break;
        case GFX_FORMAT_RGB_888:
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
            break;
        case GFX_FORMAT_RGBA_8888:
            ((uint32_t*)row)[x] = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
            break;
        default:
            switch (g_fb.bytes_per_pixel) {
                case 1:
                    row[x] = gfx_argb_to_index8(color);
                    break;
                case 2:
                    ((uint16_t*)row)[x] = gfx_argb_to_rgb565(color);
                    break;
                case 3:
                    row[x * 3 + 0] = b;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                    break;
                case 4:
                default:
                    ((uint32_t*)row)[x] = color;
                    break;
            }
            break;
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

static inline uint32_t* get_target_buffer(void) {
    return gfx_backbuffer_use_ok() ? g_backbuffer : (uint32_t*)g_fb.addr;
}

static inline bool clip_point(int32_t x, int32_t y) {
    if (!g_initialized) return false;

    if (x < 0 || x >= (int32_t)g_fb.width ||
        y < 0 || y >= (int32_t)g_fb.height) {
        return false;
    }

    if (g_clip_enabled) {
        if (x < g_clip.x || x >= g_clip.x + (int32_t)g_clip.width ||
            y < g_clip.y || y >= g_clip.y + (int32_t)g_clip.height) {
            return false;
        }
    }

    return true;
}

static inline bool clip_rect_to_bounds(int32_t* x, int32_t* y, int32_t* w, int32_t* h) {
    if (!g_initialized || !x || !y || !w || !h) return false;
    if (*w <= 0 || *h <= 0) return false;

    int32_t x0 = *x;
    int32_t y0 = *y;
    int32_t x1 = x0 + *w;
    int32_t y1 = y0 + *h;

    int32_t clip_x0 = 0;
    int32_t clip_y0 = 0;
    int32_t clip_x1 = (int32_t)g_fb.width;
    int32_t clip_y1 = (int32_t)g_fb.height;

    if (g_clip_enabled) {
        clip_x0 = g_clip.x;
        clip_y0 = g_clip.y;
        clip_x1 = g_clip.x + (int32_t)g_clip.width;
        clip_y1 = g_clip.y + (int32_t)g_clip.height;
    }

    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;

    if (x1 <= x0 || y1 <= y0) return false;

    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

static inline void gfx_pixel_blend_unclipped(int32_t x, int32_t y, uint32_t color) {
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) {
        if (g_oob_pixel_count < 8) {
            printf("[LEAFGFX] OOB pixel_blend_unclipped: x=%d y=%d screen=%ux%u\n",
                   x, y, g_fb.width, g_fb.height);
            g_oob_pixel_count++;
        }
        return;
    }
    uint8_t sa = (uint8_t)((color >> 24) & 0xFF);
    if (sa == 0) return;
    if (sa == 255) {
        gfx_pixel(x, y, color);
        return;
    }

    uint32_t dst;
    if (gfx_backbuffer_use_ok()) {
        dst = g_backbuffer[y * g_fb.width + x];
        g_backbuffer[y * g_fb.width + x] = gfx_blend(dst, color);
    } else {
        dst = gfx_fb_to_color(g_fb.addr, x, y);
        gfx_fb_put_pixel(g_fb.addr, x, y, gfx_blend(dst, color));
    }
}

// ============================================================================
// Basic Drawing Primitives
// ============================================================================

void gfx_clear(uint32_t color) {
    if (!g_initialized) return;

    if (gfx_backbuffer_use_ok()) {
        uint32_t* buf32 = g_backbuffer;
        uint32_t pixels = g_fb.width * g_fb.height;
        for (uint32_t i = 0; i < pixels; i++) {
            buf32[i] = color;
        }
        return;
    }

    uint8_t* buf = (uint8_t*)g_fb.addr;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_INDEXED_8:
            {
                uint8_t idx = gfx_argb_to_index8(color);
                for (uint32_t y = 0; y < g_fb.height; y++) {
                    uint8_t* row = buf + y * g_fb.pitch;
                    for (uint32_t x = 0; x < g_fb.width; x++) {
                        row[x] = idx;
                    }
                }
            }
            break;

        case GFX_FORMAT_RGB_555:
            {
                uint16_t c555 = gfx_argb_to_rgb555(color);
                uint16_t* buf16 = (uint16_t*)buf;
                uint32_t pixels = g_fb.height * (g_fb.pitch / 2);
                for (uint32_t i = 0; i < pixels; i++) {
                    buf16[i] = c555;
                }
            }
            break;

        case GFX_FORMAT_RGB_565:
            {
                uint16_t c565 = gfx_argb_to_rgb565(color);
                uint16_t* buf16 = (uint16_t*)buf;
                uint32_t pixels = g_fb.height * (g_fb.pitch / 2);
                for (uint32_t i = 0; i < pixels; i++) {
                    buf16[i] = c565;
                }
            }
            break;

        case GFX_FORMAT_BGR_888:
            {
                for (uint32_t y = 0; y < g_fb.height; y++) {
                    uint8_t* row = buf + y * g_fb.pitch;
                    for (uint32_t x = 0; x < g_fb.width; x++) {
                        row[x * 3 + 0] = b;
                        row[x * 3 + 1] = g;
                        row[x * 3 + 2] = r;
                    }
                }
            }
            break;

        case GFX_FORMAT_RGB_888:
            {
                for (uint32_t y = 0; y < g_fb.height; y++) {
                    uint8_t* row = buf + y * g_fb.pitch;
                    for (uint32_t x = 0; x < g_fb.width; x++) {
                        row[x * 3 + 0] = r;
                        row[x * 3 + 1] = g;
                        row[x * 3 + 2] = b;
                    }
                }
            }
            break;

        case GFX_FORMAT_RGBA_8888:
            {
                uint32_t rgba_color = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
                uint32_t* buf32 = (uint32_t*)buf;
                uint32_t pixels = g_fb.height * (g_fb.pitch / 4);
                for (uint32_t i = 0; i < pixels; i++) {
                    buf32[i] = rgba_color;
                }
            }
            break;

        case GFX_FORMAT_BGRA_8888:
            {
                uint32_t bgra_color = ((uint32_t)0xFF << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
                uint32_t* buf32 = (uint32_t*)buf;
                uint32_t pixels = g_fb.height * (g_fb.pitch / 4);
                for (uint32_t i = 0; i < pixels; i++) {
                    buf32[i] = bgra_color;
                }
            }
            break;

        default:
            {
                for (uint32_t y = 0; y < g_fb.height; y++) {
                    for (uint32_t x = 0; x < g_fb.width; x++) {
                        gfx_fb_put_pixel(g_fb.addr, x, y, color);
                    }
                }
            }
            break;
    }
}

void gfx_pixel(int32_t x, int32_t y, uint32_t color) {
    if (!clip_point(x, y)) return;

    if (gfx_backbuffer_use_ok()) {
        g_backbuffer[y * g_fb.width + x] = color;
    } else {
        gfx_fb_put_pixel(g_fb.addr, x, y, color);
    }
}

void gfx_pixel_blend(int32_t x, int32_t y, uint32_t color) {
    if (!clip_point(x, y)) return;

    uint8_t sa = (color >> 24) & 0xFF;
    if (sa == 0) return;
    if (sa == 255) {
        gfx_pixel(x, y, color);
        return;
    }

    uint32_t dst;
    if (gfx_backbuffer_use_ok()) {
        dst = g_backbuffer[y * g_fb.width + x];
        g_backbuffer[y * g_fb.width + x] = gfx_blend(dst, color);
    } else {
        dst = gfx_fb_to_color(g_fb.addr, x, y);
        gfx_fb_put_pixel(g_fb.addr, x, y, gfx_blend(dst, color));
    }
}

uint32_t gfx_read_pixel(int32_t x, int32_t y) {
    if (!g_initialized ||
        x < 0 || x >= (int32_t)g_fb.width ||
        y < 0 || y >= (int32_t)g_fb.height) {
        return 0;
    }

    if (gfx_backbuffer_use_ok()) {
        return g_backbuffer[y * g_fb.width + x];
    } else {
        return gfx_fb_to_color(g_fb.addr, x, y);
    }
}

void gfx_hline(int32_t x, int32_t y, int32_t width, uint32_t color) {
    int32_t w = width;
    int32_t h = 1;
    if (!clip_rect_to_bounds(&x, &y, &w, &h)) return;

    uint8_t sa = (uint8_t)((color >> 24) & 0xFF);
    if (sa == 0) return;

    if (sa == 255) {
        if (gfx_backbuffer_use_ok()) {
            uint32_t max_idx = g_fb.width * g_fb.height;
            uint32_t base = (uint32_t)y * g_fb.width + (uint32_t)x;
            for (int32_t i = 0; i < w; i++) {
                uint32_t idx = base + (uint32_t)i;
                if (idx < max_idx) {
                    g_backbuffer[idx] = color;
                }
            }
        } else {
            for (int32_t i = 0; i < w; i++) {
                gfx_fb_put_pixel(g_fb.addr, x + i, y, color);
            }
        }
        return;
    }

    for (int32_t i = 0; i < w; i++) {
        gfx_pixel_blend_unclipped(x + i, y, color);
    }
}

void gfx_vline(int32_t x, int32_t y, int32_t height, uint32_t color) {
    int32_t w = 1;
    int32_t h = height;
    if (!clip_rect_to_bounds(&x, &y, &w, &h)) return;

    uint8_t sa = (uint8_t)((color >> 24) & 0xFF);
    if (sa == 0) return;

    if (sa == 255) {
        if (gfx_backbuffer_use_ok()) {
            uint32_t w32 = g_fb.width;
            uint32_t h32 = g_fb.height;
            uint32_t max_idx = w32 * h32;
            uint32_t ux = (uint32_t)x;
            uint32_t uy = (uint32_t)y;
            for (int32_t i = 0; i < h; i++) {
                uint32_t idx = (uy + (uint32_t)i) * w32 + ux;
                if (idx < max_idx) {
                    g_backbuffer[idx] = color;
                }
            }
        } else {
            for (int32_t i = 0; i < h; i++) {
                gfx_fb_put_pixel(g_fb.addr, x, y + i, color);
            }
        }
        return;
    }

    for (int32_t i = 0; i < h; i++) {
        gfx_pixel_blend_unclipped(x, y + i, color);
    }
}

void gfx_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    // Bresenham's line algorithm
    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    int32_t sx = (dx > 0) ? 1 : -1;
    int32_t sy = (dy > 0) ? 1 : -1;

    dx = (dx < 0) ? -dx : dx;
    dy = (dy < 0) ? -dy : dy;

    int32_t err = dx - dy;

    while (1) {
        gfx_pixel_blend(x1, y1, color);

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
}

void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!clip_rect_to_bounds(&x, &y, &w, &h)) return;

    uint8_t sa = (uint8_t)((color >> 24) & 0xFF);
    if (sa == 0) return;

    if (sa == 255) {
        if (gfx_backbuffer_use_ok()) {
            uint32_t max_idx = g_fb.width * g_fb.height;
            for (int32_t py = y; py < y + h; py++) {
                uint32_t base = (uint32_t)py * g_fb.width + (uint32_t)x;
                for (int32_t px = 0; px < w; px++) {
                    uint32_t idx = base + (uint32_t)px;
                    if (idx < max_idx) {
                        g_backbuffer[idx] = color;
                    }
                }
            }
        } else {
            for (int32_t py = y; py < y + h; py++) {
                for (int32_t px = x; px < x + w; px++) {
                    gfx_fb_put_pixel(g_fb.addr, px, py, color);
                }
            }
        }
        return;
    }

    for (int32_t py = y; py < y + h; py++) {
        for (int32_t px = x; px < x + w; px++) {
            gfx_pixel_blend_unclipped(px, py, color);
        }
    }
}

void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

// ============================================================================
// Circles
// ============================================================================

void gfx_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    uint8_t sa = gfx_alpha_from_color(color);
    if (sa == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;
    uint32_t src = ((uint32_t)sa << 24) | rgb;

    if (radius <= 0) {
        gfx_pixel_blend(cx, cy, src);
        return;
    }

    if (radius <= 12 || sa < 255) {
        gfx_fill_circle_aa(cx, cy, radius, src);
        return;
    }

    int32_t r2 = radius * radius;
    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= r2) {
                gfx_pixel_blend(cx + dx, cy + dy, src);
            }
        }
    }
}

void gfx_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    uint8_t sa = gfx_alpha_from_color(color);
    if (sa == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;
    uint32_t src = ((uint32_t)sa << 24) | rgb;

    if (radius <= 12) {
        gfx_draw_circle_aa(cx, cy, radius, src);
        return;
    }

    // Midpoint circle algorithm
    int32_t x = radius;
    int32_t y = 0;
    int32_t err = 0;

    while (x >= y) {
        gfx_pixel_blend(cx + x, cy + y, src);
        gfx_pixel_blend(cx + y, cy + x, src);
        gfx_pixel_blend(cx - y, cy + x, src);
        gfx_pixel_blend(cx - x, cy + y, src);
        gfx_pixel_blend(cx - x, cy - y, src);
        gfx_pixel_blend(cx - y, cy - x, src);
        gfx_pixel_blend(cx + y, cy - x, src);
        gfx_pixel_blend(cx + x, cy - y, src);

        y++;
        if (err <= 0) {
            err += 2 * y + 1;
        }
        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}

void gfx_fill_circle_aa(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    int32_t r_sq = radius * radius;
    int32_t r_inner_sq = (radius - 1) * (radius - 1);

    for (int32_t dy = -radius - 1; dy <= radius + 1; dy++) {
        for (int32_t dx = -radius - 1; dx <= radius + 1; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            if (dist_sq > r_sq + 2 * radius) continue;

            uint8_t alpha;
            if (dist_sq <= r_inner_sq) {
                alpha = base_a;
            } else if (dist_sq >= r_sq) {
                alpha = 0;
            } else {
                int32_t span = r_sq - r_inner_sq;
                if (span > 0) {
                    alpha = (uint8_t)(base_a * (r_sq - dist_sq) / span);
                } else {
                    alpha = 0;
                }
            }

            if (alpha > 0) {
                gfx_pixel_blend(cx + dx, cy + dy, (alpha << 24) | rgb);
            }
        }
    }
}

void gfx_draw_ring(int32_t cx, int32_t cy, int32_t radius, int32_t thickness, uint32_t color) {
    uint8_t sa = gfx_alpha_from_color(color);
    if (sa == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;
    uint32_t src = ((uint32_t)sa << 24) | rgb;

    if (thickness <= 2 && radius <= 20) {
        gfx_draw_ring_aa(cx, cy, radius, thickness, src);
        return;
    }

    int32_t outer_r = radius + thickness / 2;
    int32_t inner_r = radius - thickness / 2;
    int32_t outer_sq = outer_r * outer_r;
    int32_t inner_sq = inner_r * inner_r;

    for (int32_t dy = -outer_r; dy <= outer_r; dy++) {
        for (int32_t dx = -outer_r; dx <= outer_r; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            if (dist_sq <= outer_sq && dist_sq >= inner_sq) {
                gfx_pixel_blend(cx + dx, cy + dy, src);
            }
        }
    }
}

// ============================================================================
// Rounded Rectangles
// ============================================================================

void gfx_fill_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    uint8_t sa = gfx_alpha_from_color(color);
    if (sa == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;
    uint32_t src = ((uint32_t)sa << 24) | rgb;

    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    if (radius <= 12 && w <= 240 && h <= 240) {
        gfx_fill_rounded_rect_aa(x, y, w, h, radius, src);
        return;
    }

    // Center rectangles
    gfx_fill_rect(x + radius, y, w - 2 * radius, h, src);
    gfx_fill_rect(x, y + radius, radius, h - 2 * radius, src);
    gfx_fill_rect(x + w - radius, y + radius, radius, h - 2 * radius, src);

    // Corners (using anti-aliased circles)
    gfx_fill_circle_aa(x + radius, y + radius, radius, src);
    gfx_fill_circle_aa(x + w - radius - 1, y + radius, radius, src);
    gfx_fill_circle_aa(x + radius, y + h - radius - 1, radius, src);
    gfx_fill_circle_aa(x + w - radius - 1, y + h - radius - 1, radius, src);
}

void gfx_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    uint8_t sa = gfx_alpha_from_color(color);
    if (sa == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;
    uint32_t src = ((uint32_t)sa << 24) | rgb;

    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    if (radius <= 12) {
        gfx_draw_rounded_rect_aa(x, y, w, h, radius, 1, src);
        return;
    }

    // Top and bottom edges
    gfx_hline(x + radius, y, w - 2 * radius, src);
    gfx_hline(x + radius, y + h - 1, w - 2 * radius, src);

    // Left and right edges
    gfx_vline(x, y + radius, h - 2 * radius, src);
    gfx_vline(x + w - 1, y + radius, h - 2 * radius, src);

    // Corner arcs (quarter circles)
    int32_t cx, cy;

    // Top-left
    cx = x + radius;
    cy = y + radius;
    for (int32_t xi = radius; xi >= 0; xi--) {
        for (int32_t yi = 0; yi <= radius; yi++) {
            int32_t d = xi * xi + yi * yi - radius * radius;
            if (d >= -radius && d <= radius) {
                gfx_pixel_blend(cx - xi, cy - yi, src);
            }
        }
    }

    // Top-right
    cx = x + w - radius - 1;
    cy = y + radius;
    for (int32_t xi = 0; xi <= radius; xi++) {
        for (int32_t yi = 0; yi <= radius; yi++) {
            int32_t d = xi * xi + yi * yi - radius * radius;
            if (d >= -radius && d <= radius) {
                gfx_pixel_blend(cx + xi, cy - yi, src);
            }
        }
    }

    // Bottom-left
    cx = x + radius;
    cy = y + h - radius - 1;
    for (int32_t xi = radius; xi >= 0; xi--) {
        for (int32_t yi = 0; yi <= radius; yi++) {
            int32_t d = xi * xi + yi * yi - radius * radius;
            if (d >= -radius && d <= radius) {
                gfx_pixel_blend(cx - xi, cy + yi, src);
            }
        }
    }

    // Bottom-right
    cx = x + w - radius - 1;
    cy = y + h - radius - 1;
    for (int32_t xi = 0; xi <= radius; xi++) {
        for (int32_t yi = 0; yi <= radius; yi++) {
            int32_t d = xi * xi + yi * yi - radius * radius;
            if (d >= -radius && d <= radius) {
                gfx_pixel_blend(cx + xi, cy + yi, src);
            }
        }
    }
}

// ============================================================================
// Anti-Aliased Drawing (Modern UI)
// ============================================================================

// Integer square root approximation
static int32_t isqrt(int32_t n) {
    if (n < 0) return 0;
    if (n < 2) return n;
    int32_t x = n;
    int32_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// Fixed-point square root (8.8 format for AA calculations)
static int32_t sqrt_fp8(int32_t n) {
    if (n <= 0) return 0;
    return isqrt(n * 256);  // Returns 8.8 fixed point
}

// Wu's anti-aliased line algorithm
void gfx_line_aa(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;

    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    // Handle special cases
    if (dx == 0 && dy == 0) {
        gfx_pixel_blend(x1, y1, color);
        return;
    }

    bool steep = (dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx);

    if (steep) {
        int32_t t;
        t = x1; x1 = y1; y1 = t;
        t = x2; x2 = y2; y2 = t;
    }

    if (x1 > x2) {
        int32_t t;
        t = x1; x1 = x2; x2 = t;
        t = y1; y1 = y2; y2 = t;
    }

    dx = x2 - x1;
    dy = y2 - y1;

    int32_t gradient = dx == 0 ? 65536 : (dy * 65536) / dx;
    int32_t y_fp = y1 * 65536 + 32768;  // 16.16 fixed point

    for (int32_t x = x1; x <= x2; x++) {
        int32_t y = y_fp >> 16;
        int32_t frac = (y_fp >> 8) & 0xFF;  // 8-bit fractional part

        uint8_t a1 = (uint8_t)((base_a * (255 - frac)) >> 8);
        uint8_t a2 = (uint8_t)((base_a * frac) >> 8);

        if (steep) {
            if (a1 > 0) gfx_pixel_blend(y, x, (a1 << 24) | rgb);
            if (a2 > 0) gfx_pixel_blend(y + 1, x, (a2 << 24) | rgb);
        } else {
            if (a1 > 0) gfx_pixel_blend(x, y, (a1 << 24) | rgb);
            if (a2 > 0) gfx_pixel_blend(x, y + 1, (a2 << 24) | rgb);
        }

        y_fp += gradient;
    }
}

// AA line with thickness
void gfx_line_aa_thick(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       int32_t thickness, uint32_t color) {
    if (thickness <= 1) {
        gfx_line_aa(x1, y1, x2, y2, color);
        return;
    }

    int32_t dx = x2 - x1;
    int32_t dy = y2 - y1;
    int32_t len = isqrt(dx * dx + dy * dy);
    if (len == 0) len = 1;

    // Perpendicular offset
    int32_t px = -(dy * thickness / 2) / len;
    int32_t py = (dx * thickness / 2) / len;

    // Draw multiple parallel lines
    for (int32_t i = -thickness / 2; i <= thickness / 2; i++) {
        int32_t ox = (i * (-dy)) / len;
        int32_t oy = (i * dx) / len;
        gfx_line_aa(x1 + ox, y1 + oy, x2 + ox, y2 + oy, color);
    }
}

// SDF-based circle with smooth anti-aliasing
void gfx_fill_circle_sdf(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    int32_t r_outer = radius + 2;  // Padding for AA

    for (int32_t dy = -r_outer; dy <= r_outer; dy++) {
        for (int32_t dx = -r_outer; dx <= r_outer; dx++) {
            // Calculate distance from center (8.8 fixed point)
            int32_t dist_sq = dx * dx + dy * dy;
            int32_t dist_fp8 = sqrt_fp8(dist_sq);  // 8.8 fixed point distance
            int32_t radius_fp8 = radius * 256;      // 8.8 fixed point radius

            // Signed distance from circle edge
            int32_t sdf = radius_fp8 - dist_fp8;

            uint8_t alpha;
            if (sdf >= 256) {
                // Fully inside
                alpha = base_a;
            } else if (sdf <= -256) {
                // Fully outside
                continue;
            } else {
                // On the edge - smooth transition
                int32_t t = (sdf + 256) / 2;  // Map [-256, 256] to [0, 256]
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                alpha = (uint8_t)((base_a * t) >> 8);
            }

            if (alpha > 0) {
                gfx_pixel_blend(cx + dx, cy + dy, (alpha << 24) | rgb);
            }
        }
    }
}

// AA circle outline
void gfx_draw_circle_aa(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    gfx_draw_ring_aa(cx, cy, radius, 1, color);
}

// AA ring with smooth edges
void gfx_draw_ring_aa(int32_t cx, int32_t cy, int32_t radius, int32_t thickness, uint32_t color) {
    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    int32_t outer_r = radius + thickness / 2;
    int32_t inner_r = radius - thickness / 2;
    if (inner_r < 0) inner_r = 0;

    int32_t r_outer = outer_r + 2;

    for (int32_t dy = -r_outer; dy <= r_outer; dy++) {
        for (int32_t dx = -r_outer; dx <= r_outer; dx++) {
            int32_t dist_sq = dx * dx + dy * dy;
            int32_t dist_fp8 = sqrt_fp8(dist_sq);

            int32_t outer_fp8 = outer_r * 256;
            int32_t inner_fp8 = inner_r * 256;

            // Outside outer edge
            if (dist_fp8 > outer_fp8 + 256) continue;
            // Inside inner edge
            if (dist_fp8 < inner_fp8 - 256) continue;

            uint8_t alpha = 0;

            // Outer edge
            int32_t outer_sdf = outer_fp8 - dist_fp8;
            // Inner edge
            int32_t inner_sdf = dist_fp8 - inner_fp8;

            int32_t sdf = outer_sdf < inner_sdf ? outer_sdf : inner_sdf;

            if (sdf >= 256) {
                alpha = base_a;
            } else if (sdf > -256) {
                int32_t t = (sdf + 256) / 2;
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                alpha = (uint8_t)((base_a * t) >> 8);
            }

            if (alpha > 0) {
                gfx_pixel_blend(cx + dx, cy + dy, (alpha << 24) | rgb);
            }
        }
    }
}

// SDF for rounded rectangle
static int32_t sdf_rounded_rect(int32_t px, int32_t py, int32_t w, int32_t h, int32_t r) {
    // Transform to first quadrant
    int32_t qx = px < 0 ? -px : px;
    int32_t qy = py < 0 ? -py : py;

    // Half dimensions minus radius
    int32_t hx = w / 2 - r;
    int32_t hy = h / 2 - r;
    if (hx < 0) hx = 0;
    if (hy < 0) hy = 0;

    // Distance to corner box
    int32_t dx = qx - hx;
    int32_t dy = qy - hy;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;

    // Distance to rounded corner
    int32_t dist = sqrt_fp8(dx * dx + dy * dy) - r * 256;

    // Account for inside the box
    int32_t inside_x = qx - (w / 2);
    int32_t inside_y = qy - (h / 2);
    int32_t max_inside = inside_x > inside_y ? inside_x : inside_y;

    if (qx <= hx && qy <= hy) {
        // Inside the center, return negative distance
        return -(max_inside < 0 ? -max_inside * 256 : 0);
    }

    return dist;
}

// AA rounded rectangle
void gfx_fill_rounded_rect_aa(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t radius, uint32_t color) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    int32_t cx = x + w / 2;
    int32_t cy = y + h / 2;

    for (int32_t py = y - 1; py <= y + h; py++) {
        for (int32_t px = x - 1; px <= x + w; px++) {
            // Relative to center
            int32_t rx = px - cx;
            int32_t ry = py - cy;

            int32_t sdf = sdf_rounded_rect(rx, ry, w, h, radius);

            uint8_t alpha;
            if (sdf <= -256) {
                alpha = base_a;
            } else if (sdf >= 256) {
                continue;
            } else {
                int32_t t = (256 - sdf) / 2;
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                alpha = (uint8_t)((base_a * t) >> 8);
            }

            if (alpha > 0) {
                gfx_pixel_blend(px, py, (alpha << 24) | rgb);
            }
        }
    }
}

// AA rounded rectangle outline
void gfx_draw_rounded_rect_aa(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t radius, int32_t thickness, uint32_t color) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    int32_t cx = x + w / 2;
    int32_t cy = y + h / 2;

    for (int32_t py = y - thickness; py <= y + h + thickness; py++) {
        for (int32_t px = x - thickness; px <= x + w + thickness; px++) {
            int32_t rx = px - cx;
            int32_t ry = py - cy;

            int32_t sdf = sdf_rounded_rect(rx, ry, w, h, radius);
            int32_t dist_from_edge = sdf < 0 ? -sdf : sdf;

            // Map to ring thickness
            int32_t half_thick = thickness * 256 / 2;
            int32_t ring_sdf = half_thick - dist_from_edge;

            uint8_t alpha;
            if (ring_sdf >= 256) {
                alpha = base_a;
            } else if (ring_sdf <= -256) {
                continue;
            } else {
                int32_t t = (ring_sdf + 256) / 2;
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                alpha = (uint8_t)((base_a * t) >> 8);
            }

            if (alpha > 0) {
                gfx_pixel_blend(px, py, (alpha << 24) | rgb);
            }
        }
    }
}

// Capsule (pill) shape
void gfx_fill_capsule(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t radius = (w < h ? w : h) / 2;
    gfx_fill_rounded_rect_aa(x, y, w, h, radius, color);
}

void gfx_draw_capsule(int32_t x, int32_t y, int32_t w, int32_t h, int32_t thickness, uint32_t color) {
    int32_t radius = (w < h ? w : h) / 2;
    gfx_draw_rounded_rect_aa(x, y, w, h, radius, thickness, color);
}

// Squircle (superellipse) - n=4 gives iOS-style corners
void gfx_fill_squircle(int32_t x, int32_t y, int32_t size, int32_t smoothness_fp, uint32_t color) {
    uint8_t base_a = gfx_alpha_from_color(color);
    if (base_a == 0) return;
    uint32_t rgb = color & 0x00FFFFFF;

    // smoothness_fp is 16.16 fixed point, 0 = square, 65536 = circle
    // Map to exponent: 2 (square) to 4 (more rounded) to inf (circle)
    // For simplicity, use a blend between rounded rect and circle based on smoothness

    int32_t half = size / 2;
    int32_t cx = x + half;
    int32_t cy = y + half;

    // Radius for the rounded corners (scales with smoothness)
    int32_t max_radius = half;
    int32_t radius = (max_radius * smoothness_fp) >> 16;
    if (radius > max_radius) radius = max_radius;

    for (int32_t py = y - 1; py <= y + size; py++) {
        for (int32_t px = x - 1; px <= x + size; px++) {
            int32_t rx = px - cx;
            int32_t ry = py - cy;

            // Use SDF with calculated radius
            int32_t sdf = sdf_rounded_rect(rx, ry, size, size, radius);

            uint8_t alpha;
            if (sdf <= -256) {
                alpha = base_a;
            } else if (sdf >= 256) {
                continue;
            } else {
                int32_t t = (256 - sdf) / 2;
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                alpha = (uint8_t)((base_a * t) >> 8);
            }

            if (alpha > 0) {
                gfx_pixel_blend(px, py, (alpha << 24) | rgb);
            }
        }
    }
}

// Stadium shape (horizontal capsule)
void gfx_fill_stadium(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    gfx_fill_capsule(x, y, w, h, color);
}

// ============================================================================
// Gradients
// ============================================================================

void gfx_gradient_vertical(int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t top_color, uint32_t bottom_color) {
    for (int32_t py = 0; py < h; py++) {
        uint8_t t = (h > 1) ? (uint8_t)((py * 255) / (h - 1)) : 0;
        uint32_t color = gfx_lerp_color(top_color, bottom_color, t);
        gfx_hline(x, y + py, w, color);
    }
}

void gfx_gradient_horizontal(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t left_color, uint32_t right_color) {
    for (int32_t px = 0; px < w; px++) {
        uint8_t t = (w > 1) ? (uint8_t)((px * 255) / (w - 1)) : 0;
        uint32_t color = gfx_lerp_color(left_color, right_color, t);
        gfx_vline(x + px, y, h, color);
    }
}

void gfx_draw_shadow_soft(const gfx_rect_t* rect, int32_t corner_radius,
                          const gfx_shadow_params_t* shadow) {
    if (!rect || !shadow) {
        return;
    }
    if (shadow->color == 0) {
        return;
    }

    int32_t blur = shadow->blur_radius;
    int32_t spread = shadow->spread;

    uint8_t base_a = (shadow->color >> 24) & 0xFF;
    uint32_t rgb = shadow->color & 0x00FFFFFF;

    int32_t layers = blur > 0 ? blur : 1;
    if (layers > 16) {
        layers = 16;
    }

    for (int32_t layer = layers - 1; layer >= 0; layer--) {
        int32_t layer_expand = layer;
        int32_t layer_radius = corner_radius + layer_expand;

        int32_t dist = layer;
        uint8_t layer_alpha = (uint8_t)((base_a * (layers - dist)) / (layers * 3));

        gfx_rect_t layer_rect = {
            .x = rect->x + shadow->offset_x - layer_expand - spread,
            .y = rect->y + shadow->offset_y - layer_expand - spread,
            .width = rect->width + 2 * (layer_expand + spread),
            .height = rect->height + 2 * (layer_expand + spread)
        };

        gfx_fill_rounded_rect_aa(layer_rect.x, layer_rect.y,
                                 layer_rect.width, layer_rect.height,
                                 layer_radius, (layer_alpha << 24) | rgb);
    }
}

void gfx_glass_panel_fast(int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t corner_radius, uint32_t tint_color) {
    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (!fb) {
        return;
    }

    uint32_t sum_r = 0;
    uint32_t sum_g = 0;
    uint32_t sum_b = 0;
    int32_t samples = 0;

    for (int32_t sy = y; sy < y + h; sy += 8) {
        for (int32_t sx = x; sx < x + w; sx += 8) {
            if (sx >= 0 && sx < (int32_t)fb->width && sy >= 0 && sy < (int32_t)fb->height) {
                uint32_t c = gfx_read_pixel(sx, sy);
                sum_r += (c >> 16) & 0xFF;
                sum_g += (c >> 8) & 0xFF;
                sum_b += c & 0xFF;
                samples++;
            }
        }
    }

    if (samples > 0) {
        uint8_t avg_r = (uint8_t)(sum_r / (uint32_t)samples);
        uint8_t avg_g = (uint8_t)(sum_g / (uint32_t)samples);
        uint8_t avg_b = (uint8_t)(sum_b / (uint32_t)samples);

        uint8_t tint_a = (uint8_t)((tint_color >> 24) & 0xFF);
        uint8_t tint_r = (uint8_t)((tint_color >> 16) & 0xFF);
        uint8_t tint_g = (uint8_t)((tint_color >> 8) & 0xFF);
        uint8_t tint_b = (uint8_t)(tint_color & 0xFF);

        uint8_t r = (uint8_t)(avg_r + ((tint_r - avg_r) * tint_a) / 255);
        uint8_t g = (uint8_t)(avg_g + ((tint_g - avg_g) * tint_a) / 255);
        uint8_t b = (uint8_t)(avg_b + ((tint_b - avg_b) * tint_a) / 255);

        uint32_t blend_color = 0xE0000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        gfx_fill_rounded_rect_aa(x, y, w, h, corner_radius, blend_color);
    } else {
        gfx_fill_rounded_rect_aa(x, y, w, h, corner_radius, tint_color);
    }
}

void gfx_gradient_vertical_3(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t top, uint32_t mid, uint32_t bottom) {
    int32_t half = h / 2;
    for (int32_t py = 0; py < h; py++) {
        uint32_t color;
        if (py < half) {
            uint8_t t = (half > 0) ? (uint8_t)((py * 255) / half) : 0;
            color = gfx_lerp_color(top, mid, t);
        } else {
            int32_t denom = (h - half - 1);
            uint8_t t = (denom > 0) ? (uint8_t)(((py - half) * 255) / denom) : 0;
            color = gfx_lerp_color(mid, bottom, t);
        }
        gfx_hline(x, y + py, w, color);
    }
}

void gfx_gradient_linear(int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t start_x, int32_t start_y,
                          int32_t end_x, int32_t end_y,
                          uint32_t color_start, uint32_t color_end) {
    int32_t dx = end_x - start_x;
    int32_t dy = end_y - start_y;
    int64_t len_sq = (int64_t)dx * dx + (int64_t)dy * dy;
    if (len_sq == 0) {
        gfx_fill_rect(x, y, w, h, color_start);
        return;
    }

    for (int32_t py = y; py < y + h; py++) {
        for (int32_t px = x; px < x + w; px++) {
            int32_t px0 = px - start_x;
            int32_t py0 = py - start_y;
            int64_t dot = (int64_t)px0 * dx + (int64_t)py0 * dy;
            int32_t t = (int32_t)((dot * 255) / len_sq);
            if (t < 0) t = 0;
            if (t > 255) t = 255;
            uint32_t color = gfx_lerp_color(color_start, color_end, (uint8_t)t);
            gfx_pixel_blend(px, py, color);
        }
    }
}

void gfx_gradient_radial_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t center_x, int32_t center_y, int32_t radius,
                               uint32_t inner_color, uint32_t outer_color) {
    if (radius <= 0) {
        gfx_fill_rect(x, y, w, h, inner_color);
        return;
    }
    int32_t r_sq = radius * radius;

    for (int32_t py = y; py < y + h; py++) {
        for (int32_t px = x; px < x + w; px++) {
            int32_t dx = px - center_x;
            int32_t dy = py - center_y;
            int32_t dist_sq = dx * dx + dy * dy;
            int32_t t;
            if (dist_sq >= r_sq) {
                t = 255;
            } else {
                int32_t dist = isqrt(dist_sq);
                t = (dist * 255) / radius;
            }
            uint32_t color = gfx_lerp_color(inner_color, outer_color, (uint8_t)t);
            gfx_pixel_blend(px, py, color);
        }
    }
}

// ============================================================================
// Clipping
// ============================================================================

void gfx_set_clip(int32_t x, int32_t y, int32_t w, int32_t h) {
    g_clip.x = x;
    g_clip.y = y;
    g_clip.width = w;
    g_clip.height = h;
    g_clip_enabled = true;
}

void gfx_clear_clip(void) {
    g_clip_enabled = false;
}

gfx_rect_t gfx_get_clip(void) {
    return g_clip_enabled ? g_clip : (gfx_rect_t){0, 0, g_fb.width, g_fb.height};
}

void gfx_push_clip(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (g_clip_stack_top >= GFX_CLIP_STACK_MAX - 1) return;
    g_clip_stack_top++;
    g_clip_stack[g_clip_stack_top] = g_clip;

    if (g_clip_enabled) {
        int32_t nx = x > g_clip.x ? x : g_clip.x;
        int32_t ny = y > g_clip.y ? y : g_clip.y;
        int32_t nx2 = (x + w) < (g_clip.x + (int32_t)g_clip.width)
                     ? (x + w) : (g_clip.x + (int32_t)g_clip.width);
        int32_t ny2 = (y + h) < (g_clip.y + (int32_t)g_clip.height)
                     ? (y + h) : (g_clip.y + (int32_t)g_clip.height);
        if (nx2 > nx && ny2 > ny) {
            g_clip.x = nx;
            g_clip.y = ny;
            g_clip.width = nx2 - nx;
            g_clip.height = ny2 - ny;
        } else {
            g_clip.width = 0;
            g_clip.height = 0;
        }
    } else {
        g_clip.x = x;
        g_clip.y = y;
        g_clip.width = w;
        g_clip.height = h;
    }
    g_clip_enabled = true;
}

void gfx_pop_clip(void) {
    if (g_clip_stack_top < 0) return;
    g_clip = g_clip_stack[g_clip_stack_top];
    g_clip_stack_top--;
    if (g_clip_stack_top < 0) {
        g_clip_enabled = false;
    }
}

// ============================================================================
// Dirty Region Tracking
// ============================================================================

void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    int32_t x2 = x + w;
    int32_t y2 = y + h;
    if (!g_dirty_active) {
        g_dirty_x = x;
        g_dirty_y = y;
        g_dirty_x2 = x2;
        g_dirty_y2 = y2;
        g_dirty_active = true;
    } else {
        if (x < g_dirty_x) g_dirty_x = x;
        if (y < g_dirty_y) g_dirty_y = y;
        if (x2 > g_dirty_x2) g_dirty_x2 = x2;
        if (y2 > g_dirty_y2) g_dirty_y2 = y2;
    }
}

void gfx_clear_dirty(void) {
    g_dirty_active = false;
}

bool gfx_has_dirty(void) {
    return g_dirty_active;
}

void gfx_get_dirty_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h) {
    if (!g_dirty_active) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (x) *x = g_dirty_x;
    if (y) *y = g_dirty_y;
    if (w) *w = g_dirty_x2 - g_dirty_x;
    if (h) *h = g_dirty_y2 - g_dirty_y;
}

// ============================================================================
// Double Buffering
// ============================================================================

int gfx_create_backbuffer(void) {
    if (!g_initialized) return -1;
    if (g_backbuffer) return 0;

    if (g_fb.bytes_per_pixel == 0 || g_fb.bytes_per_pixel > 4) {
        printf("[LeafGFX] INFO: Backbuffer disabled for unsupported %ubpp framebuffer\n",
               g_fb.bpp);
        return -1;
    }

    size_t size = (size_t)g_fb.width * (size_t)g_fb.height * sizeof(uint32_t);
    g_backbuffer = (uint32_t*)malloc(size);
    if (!g_backbuffer) return -1;

    g_backbuffer_size = size;
    memset(g_backbuffer, 0, size);
    gfx_validate_backbuffer();
    printf("[LeafGFX] Backbuffer enabled (%ux%u ARGB -> fb %ubpp)\n",
           g_fb.width, g_fb.height, g_fb.bpp);
    return 0;
}

void gfx_destroy_backbuffer(void) {
    if (g_backbuffer) {
        free(g_backbuffer);
        g_backbuffer = NULL;
    }
    g_backbuffer_size = 0;
    g_use_backbuffer = false;
    g_backbuffer_valid = false;
}

void gfx_flip(void) {
    if (!g_initialized) {
        printf("[LeafGFX] WARNING: gfx_flip called before init\n");
        return;
    }

    gfx_refresh_if_mode_changed();

    if (!gfx_backbuffer_use_ok()) {
        __asm__ volatile("mfence" ::: "memory");
        for (int i = 0; i < 3; i++) {
            long r = syscall0(SYS_FB_FLUSH);
            if (r >= 0) return;
            printf("[LeafGFX] WARNING: SYS_FB_FLUSH attempt %d failed: %ld\n", i + 1, r);
        }
        return;
    }

    uint8_t* fb_row = (uint8_t*)g_fb.addr;
    uint32_t* bb_row = g_backbuffer;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_BGRA_8888:
            for (uint32_t y = 0; y < g_fb.height; y++) {
                uint32_t* fb_ptr = (uint32_t*)(fb_row + y * g_fb.pitch);
                uint32_t* bb_ptr = bb_row + y * g_fb.width;
                for (uint32_t x = 0; x < g_fb.width; x++) {
                    uint32_t color = bb_ptr[x];
                    fb_ptr[x] = ((color & 0xFF00FF00) |
                                ((color & 0x00FF0000) >> 16) |
                                ((color & 0x000000FF) << 16));
                }
            }
            break;

        case GFX_FORMAT_RGB_565:
            for (uint32_t y = 0; y < g_fb.height; y++) {
                uint16_t* fb_ptr = (uint16_t*)(fb_row + y * g_fb.pitch);
                uint32_t* bb_ptr = bb_row + y * g_fb.width;
                for (uint32_t x = 0; x < g_fb.width; x++) {
                    fb_ptr[x] = gfx_argb_to_rgb565(bb_ptr[x]);
                }
            }
            break;

        case GFX_FORMAT_RGB_888:
            for (uint32_t y = 0; y < g_fb.height; y++) {
                uint8_t* fb_ptr = fb_row + y * g_fb.pitch;
                uint32_t* bb_ptr = bb_row + y * g_fb.width;
                for (uint32_t x = 0; x < g_fb.width; x++) {
                    uint32_t color = bb_ptr[x];
                    fb_ptr[x * 3 + 0] = (color >> 16) & 0xFF;
                    fb_ptr[x * 3 + 1] = (color >> 8) & 0xFF;
                    fb_ptr[x * 3 + 2] = color & 0xFF;
                }
            }
            break;

        case GFX_FORMAT_BGR_888:
            for (uint32_t y = 0; y < g_fb.height; y++) {
                uint8_t* fb_ptr = fb_row + y * g_fb.pitch;
                uint32_t* bb_ptr = bb_row + y * g_fb.width;
                for (uint32_t x = 0; x < g_fb.width; x++) {
                    uint32_t color = bb_ptr[x];
                    fb_ptr[x * 3 + 0] = color & 0xFF;
                    fb_ptr[x * 3 + 1] = (color >> 8) & 0xFF;
                    fb_ptr[x * 3 + 2] = (color >> 16) & 0xFF;
                }
            }
            break;

        case GFX_FORMAT_RGBA_8888:
            for (uint32_t y = 0; y < g_fb.height; y++) {
                uint32_t* fb_ptr = (uint32_t*)(fb_row + y * g_fb.pitch);
                uint32_t* bb_ptr = bb_row + y * g_fb.width;
                for (uint32_t x = 0; x < g_fb.width; x++) {
                    uint32_t color = bb_ptr[x];
                    fb_ptr[x] = (color << 8) | ((color >> 24) & 0xFF);
                }
            }
            break;

        default:
            if (g_fb.bytes_per_pixel == 4) {
                if (g_fb.pitch == g_fb.width * sizeof(uint32_t)) {
                    memcpy(g_fb.addr, g_backbuffer, g_fb.height * g_fb.pitch);
                } else {
                    for (uint32_t y = 0; y < g_fb.height; y++) {
                        memcpy(fb_row + y * g_fb.pitch,
                               bb_row + y * g_fb.width,
                               g_fb.width * sizeof(uint32_t));
                    }
                }
            } else {
                for (uint32_t y = 0; y < g_fb.height; y++) {
                    for (uint32_t x = 0; x < g_fb.width; x++) {
                        gfx_fb_put_pixel(g_fb.addr, x, y, g_backbuffer[y * g_fb.width + x]);
                    }
                }
            }
            break;
    }

    __asm__ volatile("mfence" ::: "memory");
    for (int i = 0; i < 3; i++) {
        long r = syscall0(SYS_FB_FLUSH);
        if (r >= 0) return;
        printf("[LeafGFX] WARNING: SYS_FB_FLUSH attempt %d failed: %ld\n", i + 1, r);
    }
    printf("[LeafGFX] ERROR: SYS_FB_FLUSH failed after 3 attempts\n");
}

void gfx_flip_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!g_initialized) return;

    gfx_refresh_if_mode_changed();

    if (!gfx_backbuffer_use_ok()) {
        __asm__ volatile("mfence" ::: "memory");
        for (int i = 0; i < 3; i++) {
            long r = syscall0(SYS_FB_FLUSH);
            if (r >= 0) return;
            printf("[LeafGFX] WARNING: SYS_FB_FLUSH (rect) attempt %d failed: %ld\n", i + 1, r);
        }
        return;
    }

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)g_fb.width) w = (int32_t)g_fb.width - x;
    if (y + h > (int32_t)g_fb.height) h = (int32_t)g_fb.height - y;
    if (w <= 0 || h <= 0) return;

    uint8_t* fb_row = (uint8_t*)g_fb.addr;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_BGRA_8888:
            for (int32_t py = y; py < y + h; py++) {
                uint32_t* fb_ptr = (uint32_t*)(fb_row + py * g_fb.pitch);
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = x; px < x + w; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px] = ((color & 0xFF00FF00) |
                                  ((color & 0x00FF0000) >> 16) |
                                  ((color & 0x000000FF) << 16));
                }
            }
            break;

        case GFX_FORMAT_RGB_565:
            for (int32_t py = y; py < y + h; py++) {
                uint16_t* fb_ptr = (uint16_t*)(fb_row + py * g_fb.pitch);
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = x; px < x + w; px++) {
                    fb_ptr[px] = gfx_argb_to_rgb565(bb_ptr[px]);
                }
            }
            break;

        case GFX_FORMAT_RGB_888:
            for (int32_t py = y; py < y + h; py++) {
                uint8_t* fb_ptr = fb_row + py * g_fb.pitch;
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = x; px < x + w; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px * 3 + 0] = (color >> 16) & 0xFF;
                    fb_ptr[px * 3 + 1] = (color >> 8) & 0xFF;
                    fb_ptr[px * 3 + 2] = color & 0xFF;
                }
            }
            break;

        case GFX_FORMAT_BGR_888:
            for (int32_t py = y; py < y + h; py++) {
                uint8_t* fb_ptr = fb_row + py * g_fb.pitch;
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = x; px < x + w; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px * 3 + 0] = color & 0xFF;
                    fb_ptr[px * 3 + 1] = (color >> 8) & 0xFF;
                    fb_ptr[px * 3 + 2] = (color >> 16) & 0xFF;
                }
            }
            break;

        case GFX_FORMAT_RGBA_8888:
            for (int32_t py = y; py < y + h; py++) {
                uint32_t* fb_ptr = (uint32_t*)(fb_row + py * g_fb.pitch);
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = x; px < x + w; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px] = (color << 8) | ((color >> 24) & 0xFF);
                }
            }
            break;

        default:
            for (int32_t py = y; py < y + h; py++) {
                for (int32_t px = x; px < x + w; px++) {
                    gfx_fb_put_pixel(g_fb.addr, px, py, g_backbuffer[py * g_fb.width + px]);
                }
            }
            break;
    }

    __asm__ volatile("mfence" ::: "memory");
    for (int i = 0; i < 3; i++) {
        long r = syscall0(SYS_FB_FLUSH);
        if (r >= 0) return;
        printf("[LeafGFX] WARNING: SYS_FB_FLUSH (rect) attempt %d failed: %ld\n", i + 1, r);
    }
    printf("[LeafGFX] ERROR: SYS_FB_FLUSH (rect) failed after 3 attempts\n");
}

void gfx_set_target_backbuffer(bool use_backbuffer) {
    g_use_backbuffer = use_backbuffer && g_backbuffer;
    gfx_validate_backbuffer();
}

void gfx_flip_dirty(void) {
    if (!g_initialized) return;
    if (!gfx_backbuffer_use_ok()) {
        gfx_flip();
        return;
    }

    if (!g_dirty_active) {
        gfx_flip();
        return;
    }

    int32_t dx = g_dirty_x;
    int32_t dy = g_dirty_y;
    int32_t dw = g_dirty_x2 - g_dirty_x;
    int32_t dh = g_dirty_y2 - g_dirty_y;

    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > (int32_t)g_fb.width) dw = (int32_t)g_fb.width - dx;
    if (dy + dh > (int32_t)g_fb.height) dh = (int32_t)g_fb.height - dy;
    if (dw <= 0 || dh <= 0) {
        g_dirty_active = false;
        return;
    }

    uint8_t* fb_row = (uint8_t*)g_fb.addr;

    switch (gfx_effective_format()) {
        case GFX_FORMAT_BGRA_8888:
            for (int32_t py = dy; py < dy + dh; py++) {
                uint32_t* fb_ptr = (uint32_t*)(fb_row + py * g_fb.pitch);
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = dx; px < dx + dw; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px] = ((color & 0xFF00FF00) |
                                  ((color & 0x00FF0000) >> 16) |
                                  ((color & 0x000000FF) << 16));
                }
            }
            break;

        case GFX_FORMAT_RGB_565:
            for (int32_t py = dy; py < dy + dh; py++) {
                uint16_t* fb_ptr = (uint16_t*)(fb_row + py * g_fb.pitch);
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = dx; px < dx + dw; px++) {
                    fb_ptr[px] = gfx_argb_to_rgb565(bb_ptr[px]);
                }
            }
            break;

        case GFX_FORMAT_RGB_888:
            for (int32_t py = dy; py < dy + dh; py++) {
                uint8_t* fb_ptr = fb_row + py * g_fb.pitch;
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = dx; px < dx + dw; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px * 3 + 0] = (color >> 16) & 0xFF;
                    fb_ptr[px * 3 + 1] = (color >> 8) & 0xFF;
                    fb_ptr[px * 3 + 2] = color & 0xFF;
                }
            }
            break;

        case GFX_FORMAT_BGR_888:
            for (int32_t py = dy; py < dy + dh; py++) {
                uint8_t* fb_ptr = fb_row + py * g_fb.pitch;
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = dx; px < dx + dw; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px * 3 + 0] = color & 0xFF;
                    fb_ptr[px * 3 + 1] = (color >> 8) & 0xFF;
                    fb_ptr[px * 3 + 2] = (color >> 16) & 0xFF;
                }
            }
            break;

        case GFX_FORMAT_RGBA_8888:
            for (int32_t py = dy; py < dy + dh; py++) {
                uint32_t* fb_ptr = (uint32_t*)(fb_row + py * g_fb.pitch);
                uint32_t* bb_ptr = g_backbuffer + py * g_fb.width;
                for (int32_t px = dx; px < dx + dw; px++) {
                    uint32_t color = bb_ptr[px];
                    fb_ptr[px] = (color << 8) | ((color >> 24) & 0xFF);
                }
            }
            break;

        default:
            for (int32_t py = dy; py < dy + dh; py++) {
                for (int32_t px = dx; px < dx + dw; px++) {
                    gfx_fb_put_pixel(g_fb.addr, px, py, g_backbuffer[py * g_fb.width + px]);
                }
            }
            break;
    }

    g_dirty_active = false;
    __asm__ volatile("mfence" ::: "memory");
    for (int i = 0; i < 3; i++) {
        long r = syscall0(SYS_FB_FLUSH);
        if (r >= 0) return;
        printf("[LeafGFX] WARNING: SYS_FB_FLUSH (dirty) attempt %d failed: %ld\n", i + 1, r);
    }
    printf("[LeafGFX] ERROR: SYS_FB_FLUSH (dirty) failed after 3 attempts\n");
}

// ============================================================================
// Mouse Cursor
// ============================================================================

// Standard arrow cursor (12x19 pixels)
static const uint8_t cursor_arrow[] = {
    0b10000000, 0b00000000,
    0b11000000, 0b00000000,
    0b11100000, 0b00000000,
    0b11110000, 0b00000000,
    0b11111000, 0b00000000,
    0b11111100, 0b00000000,
    0b11111110, 0b00000000,
    0b11111111, 0b00000000,
    0b11111111, 0b10000000,
    0b11111111, 0b11000000,
    0b11111111, 0b11100000,
    0b11111100, 0b00000000,
    0b11101100, 0b00000000,
    0b11000110, 0b00000000,
    0b10000110, 0b00000000,
    0b00000011, 0b00000000,
    0b00000011, 0b00000000,
    0b00000001, 0b10000000,
    0b00000001, 0b10000000,
};

static const uint8_t cursor_arrow_outline[] = {
    0b11000000, 0b00000000,
    0b10100000, 0b00000000,
    0b10010000, 0b00000000,
    0b10001000, 0b00000000,
    0b10000100, 0b00000000,
    0b10000010, 0b00000000,
    0b10000001, 0b00000000,
    0b10000000, 0b10000000,
    0b10000000, 0b01000000,
    0b10000000, 0b00100000,
    0b10000000, 0b00010000,
    0b10000010, 0b00010000,
    0b10010010, 0b00000000,
    0b10101001, 0b00000000,
    0b01001001, 0b00000000,
    0b00000100, 0b10000000,
    0b00000100, 0b10000000,
    0b00000010, 0b01000000,
    0b00000010, 0b01000000,
    0b00000001, 0b11000000,
};

void gfx_draw_cursor(int32_t x, int32_t y, gfx_cursor_type_t type) {
    if (!g_initialized) return;

    if (type == GFX_CURSOR_CUSTOM && g_custom_cursor) {
        // Draw custom cursor
        for (int32_t cy = 0; cy < g_cursor_h; cy++) {
            for (int32_t cx = 0; cx < g_cursor_w; cx++) {
                uint32_t color = g_custom_cursor[cy * g_cursor_w + cx];
                if ((color >> 24) > 0) {
                    gfx_pixel_blend(x + cx - g_cursor_hotx,
                                   y + cy - g_cursor_hoty, color);
                }
            }
        }
        return;
    }

    // Draw standard arrow cursor
    for (int32_t cy = 0; cy < 19; cy++) {
        for (int32_t cx = 0; cx < 12; cx++) {
            int32_t byte_idx = cx / 8;
            int32_t bit_idx = 7 - (cx % 8);

            // Draw outline (black)
            if ((cursor_arrow_outline[cy * 2 + byte_idx] >> bit_idx) & 1) {
                gfx_pixel(x + cx, y + cy, GFX_COLOR_BLACK);
            }
            // Draw fill (white)
            else if ((cursor_arrow[cy * 2 + byte_idx] >> bit_idx) & 1) {
                gfx_pixel(x + cx, y + cy, GFX_COLOR_WHITE);
            }
        }
    }
}

void gfx_set_custom_cursor(const uint32_t* pixels, int32_t w, int32_t h,
                           int32_t hotspot_x, int32_t hotspot_y) {
    if (g_custom_cursor) {
        free(g_custom_cursor);
    }

    g_cursor_w = w;
    g_cursor_h = h;
    g_cursor_hotx = hotspot_x;
    g_cursor_hoty = hotspot_y;

    size_t size = w * h * sizeof(uint32_t);
    g_custom_cursor = (uint32_t*)malloc(size);
    if (g_custom_cursor) {
        memcpy(g_custom_cursor, pixels, size);
    }
}

// ============================================================================
// Animation Helpers (Fixed-Point Implementation)
// ============================================================================

// All easing functions use 16.16 fixed-point format
// Input t: 0 to GFX_FP_ONE (65536)
// Output: 0 to GFX_FP_ONE (65536)

int32_t gfx_ease_in_quad(int32_t t) {
    // t * t
    return GFX_FP_MUL(t, t);
}

int32_t gfx_ease_out_quad(int32_t t) {
    // t * (2 - t) = 2t - t^2
    return GFX_FP_MUL(t, 2 * GFX_FP_ONE - t);
}

int32_t gfx_ease_in_out_quad(int32_t t) {
    // if t < 0.5: 2 * t * t
    // else: -1 + (4 - 2t) * t
    if (t < GFX_FP_HALF) {
        return 2 * GFX_FP_MUL(t, t);
    } else {
        int32_t f = 4 * GFX_FP_ONE - 2 * t;
        return -GFX_FP_ONE + GFX_FP_MUL(f, t);
    }
}

int32_t gfx_ease_in_cubic(int32_t t) {
    // t * t * t
    return GFX_FP_MUL(GFX_FP_MUL(t, t), t);
}

int32_t gfx_ease_out_cubic(int32_t t) {
    // (t - 1)^3 + 1
    int32_t f = t - GFX_FP_ONE;
    return GFX_FP_MUL(GFX_FP_MUL(f, f), f) + GFX_FP_ONE;
}

int32_t gfx_ease_in_out_cubic(int32_t t) {
    // if t < 0.5: 4 * t^3
    // else: (t - 1) * (2t - 2)^2 + 1
    if (t < GFX_FP_HALF) {
        return 4 * GFX_FP_MUL(GFX_FP_MUL(t, t), t);
    } else {
        int32_t f = t - GFX_FP_ONE;
        int32_t g = 2 * t - 2 * GFX_FP_ONE;
        return GFX_FP_MUL(f, GFX_FP_MUL(g, g)) + GFX_FP_ONE;
    }
}

int32_t gfx_ease_out_bounce(int32_t t) {
    // Pre-computed constants (no floating point):
    // n1 = 7.5625 * 65536 = 495616
    // d1 = 65536 / 2.75 = 23831
    // 1.5/2.75 * 65536 = 35746
    // 2/2.75 * 65536 = 47662
    // 2.25/2.75 * 65536 = 53620
    // 2.5/2.75 * 65536 = 59578
    // 2.625/2.75 * 65536 = 62557
    // 0.75 * 65536 = 49152
    // 0.9375 * 65536 = 61440
    // 0.984375 * 65536 = 64512
    const int32_t n1 = 495616;
    const int32_t d1 = 23831;
    const int32_t d2 = 47662;
    const int32_t d3 = 59578;
    const int32_t off1 = 35746;
    const int32_t off2 = 53620;
    const int32_t off3 = 62557;
    const int32_t add1 = 49152;
    const int32_t add2 = 61440;
    const int32_t add3 = 64512;

    if (t < d1) {
        return GFX_FP_MUL(n1, GFX_FP_MUL(t, t)) / GFX_FP_ONE;
    } else if (t < d2) {
        int32_t t2 = t - off1;
        return GFX_FP_MUL(n1, GFX_FP_MUL(t2, t2)) / GFX_FP_ONE + add1;
    } else if (t < d3) {
        int32_t t2 = t - off2;
        return GFX_FP_MUL(n1, GFX_FP_MUL(t2, t2)) / GFX_FP_ONE + add2;
    } else {
        int32_t t2 = t - off3;
        return GFX_FP_MUL(n1, GFX_FP_MUL(t2, t2)) / GFX_FP_ONE + add3;
    }
}

int32_t gfx_ease_in_sine(int32_t t) {
    int64_t x = ((int64_t)t * 102943) >> GFX_FP_BITS;
    int64_t x2 = (x * x) >> GFX_FP_BITS;
    int64_t x4 = (x2 * x2) >> GFX_FP_BITS;
    int64_t x6 = (x4 * x2) >> GFX_FP_BITS;
    int64_t cos_val = GFX_FP_ONE - x2 / 2 + x4 / 24 - x6 / 720;
    int32_t result = (int32_t)(GFX_FP_ONE - cos_val);
    if (result < 0) result = 0;
    if (result > GFX_FP_ONE) result = GFX_FP_ONE;
    return result;
}

int32_t gfx_ease_out_sine(int32_t t) {
    int64_t x = ((int64_t)t * 102943) >> GFX_FP_BITS;
    int64_t x2 = (x * x) >> GFX_FP_BITS;
    int64_t x3 = (x2 * x) >> GFX_FP_BITS;
    int64_t x5 = (x3 * x2) >> GFX_FP_BITS;
    int64_t sin_val = x - x3 / 6 + x5 / 120;
    int32_t result = (int32_t)sin_val;
    if (result < 0) result = 0;
    if (result > GFX_FP_ONE) result = GFX_FP_ONE;
    return result;
}

int32_t gfx_ease_in_out_sine(int32_t t) {
    int64_t x = ((int64_t)t * 205887) >> GFX_FP_BITS;
    int64_t x2 = (x * x) >> GFX_FP_BITS;
    int64_t x4 = (x2 * x2) >> GFX_FP_BITS;
    int64_t x6 = (x4 * x2) >> GFX_FP_BITS;
    int64_t cos_val = GFX_FP_ONE - x2 / 2 + x4 / 24 - x6 / 720;
    if (cos_val < -GFX_FP_ONE) cos_val = -GFX_FP_ONE;
    if (cos_val > GFX_FP_ONE) cos_val = GFX_FP_ONE;
    int64_t result = (GFX_FP_ONE - cos_val) / 2;
    if (result < 0) result = 0;
    if (result > GFX_FP_ONE) result = GFX_FP_ONE;
    return (int32_t)result;
}

int32_t gfx_spring_interpolate(int32_t t, int32_t frequency_fp, int32_t damping_fp) {
    if (t <= 0) return 0;
    if (t >= GFX_FP_ONE) return GFX_FP_ONE;

    int64_t phase = ((int64_t)t * frequency_fp * 2) >> GFX_FP_BITS;
    int64_t cx = ((int64_t)phase * 102943) >> GFX_FP_BITS;
    int64_t cx2 = (cx * cx) >> GFX_FP_BITS;
    int64_t cx4 = (cx2 * cx2) >> GFX_FP_BITS;
    int64_t cx6 = (cx4 * cx2) >> GFX_FP_BITS;
    int64_t cos_val = GFX_FP_ONE - cx2 / 2 + cx4 / 24 - cx6 / 720;
    if (cos_val < -GFX_FP_ONE) cos_val = -GFX_FP_ONE;
    if (cos_val > GFX_FP_ONE) cos_val = GFX_FP_ONE;

    int64_t decay = ((int64_t)damping_fp * t) / GFX_FP_ONE;
    if (decay > GFX_FP_ONE * 4) decay = GFX_FP_ONE * 4;
    int64_t exp_val = GFX_FP_ONE - decay + (decay * decay) / (2 * GFX_FP_ONE)
                    - (decay * decay * decay) / (6 * GFX_FP_ONE * GFX_FP_ONE);
    if (exp_val < 0) exp_val = 0;

    int64_t oscillation = (exp_val * cos_val) / GFX_FP_ONE;
    int64_t result = GFX_FP_ONE - oscillation;
    if (result < 0) result = 0;
    if (result > GFX_FP_ONE) result = GFX_FP_ONE;
    return (int32_t)result;
}

// Forest OS gettimeofday ABI uses 32-bit fields in userspace headers.
typedef struct {
    uint32_t tv_sec;
    uint32_t tv_usec;
} gfx_timeval_t;

static inline uint64_t gfx_read_tsc(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

uint32_t gfx_get_ticks(void) {
    static uint32_t last_ticks = 0;
    static uint32_t last_hw_ticks = 0;
    static uint64_t tsc_base = 0;
    static uint32_t synth_base_ms = 0;
    static uint64_t calib_tsc = 0;
    static uint32_t calib_hw_ms = 0;
    static uint64_t cycles_per_ms = 1200000ULL; // conservative start (~1.2 GHz)
    const uint64_t min_cycles_per_ms = 100000ULL;
    const uint64_t max_cycles_per_ms = 10000000ULL;
    const uint32_t synth_speed_num = 5; // 1.25x synthetic-time speed
    const uint32_t synth_speed_den = 4;

    // Use gettimeofday which returns actual timer-based time
    gfx_timeval_t tv;
    long result = syscall1(SYS_GETTIMEOFDAY, (long)&tv);
    uint64_t tsc_now = gfx_read_tsc();
    if (result == 0) {
        // Convert to milliseconds (tv_sec * 1000 + tv_usec / 1000)
        // Use only the lower bits of tv_sec to avoid overflow
        uint32_t now = (uint32_t)((tv.tv_sec & 0xFFFFF) * 1000 + tv.tv_usec / 1000);

        if (now > last_hw_ticks) {
            if (calib_tsc != 0 && calib_hw_ms != 0) {
                uint32_t delta_ms = now - calib_hw_ms;
                if (delta_ms >= 16) {
                    uint64_t delta_tsc = tsc_now - calib_tsc;
                    uint64_t sample = delta_tsc / delta_ms;
                    if (sample < min_cycles_per_ms) sample = min_cycles_per_ms;
                    if (sample > max_cycles_per_ms) sample = max_cycles_per_ms;
                    // EMA smoothing: 7/8 old + 1/8 new
                    cycles_per_ms = ((cycles_per_ms * 7ULL) + sample) / 8ULL;
                }
            }

            calib_tsc = tsc_now;
            calib_hw_ms = now;
            last_hw_ticks = now;
            tsc_base = tsc_now;
            synth_base_ms = now;
            if (now < last_ticks) {
                now = last_ticks;
            }
            last_ticks = now;
            return now;
        }

        // Hardware time appears stuck; switch to TSC-derived synthetic time.
        if (tsc_base == 0) {
            tsc_base = tsc_now;
            synth_base_ms = last_ticks;
        }
        uint32_t elapsed_ms = (uint32_t)((tsc_now - tsc_base) / cycles_per_ms);
        elapsed_ms = (uint32_t)(((uint64_t)elapsed_ms * synth_speed_num) / synth_speed_den);
        uint32_t synth = synth_base_ms + elapsed_ms;
        if (synth < last_ticks) {
            synth = last_ticks;
        }
        last_ticks = synth;
        return synth;
    }

    // Syscall failed: pure TSC-based monotonic fallback.
    if (tsc_base == 0) {
        tsc_base = tsc_now;
        synth_base_ms = last_ticks;
    }
    uint32_t elapsed_ms = (uint32_t)((tsc_now - tsc_base) / cycles_per_ms);
    elapsed_ms = (uint32_t)(((uint64_t)elapsed_ms * synth_speed_num) / synth_speed_den);
    uint32_t synth = synth_base_ms + elapsed_ms;
    if (synth < last_ticks) {
        synth = last_ticks;
    }
    last_ticks = synth;
    return synth;
}

// Delay with scheduler-friendly yielding based on wall-clock ticks
void gfx_sleep(uint32_t ms) {
    if (ms == 0) {
        syscall0(SYS_SCHED_YIELD);
        return;
    }

    uint32_t start = gfx_get_ticks();
    while ((uint32_t)(gfx_get_ticks() - start) < ms) {
        syscall0(SYS_SCHED_YIELD);
    }
}

// ============================================================================
// Offscreen App Buffers
// ============================================================================

gfx_app_buffer_t* gfx_app_create_buffer(uint32_t width, uint32_t height) {
    if (!g_initialized || width == 0 || height == 0) {
        return NULL;
    }

    g_app_buffer.width = width;
    g_app_buffer.height = height;
    g_app_buffer.pitch = width * sizeof(uint32_t);
    g_app_buffer.format = GFX_FORMAT_BGRA_8888;
    g_app_buffer.size = height * g_app_buffer.pitch;

    g_app_buffer.data = malloc(g_app_buffer.size);
    if (!g_app_buffer.data) {
        return NULL;
    }

    memset(g_app_buffer.data, 0, g_app_buffer.size);
    printf("[LeafGFX] App buffer created: %ux%u\n", width, height);
    return &g_app_buffer;
}

void gfx_app_destroy_buffer(gfx_app_buffer_t* buffer) {
    if (buffer && buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
    }
    if (g_current_app_buffer == buffer) {
        g_current_app_buffer = NULL;
    }
}

void gfx_app_set_buffer(gfx_app_buffer_t* buffer) {
    g_current_app_buffer = buffer;
}

gfx_app_buffer_t* gfx_app_get_buffer(void) {
    return g_current_app_buffer;
}

void gfx_app_flip_to_screen(void) {
    if (!g_initialized || !g_current_app_buffer || !g_current_app_buffer->data) {
        return;
    }

    gfx_app_buffer_t* buf = g_current_app_buffer;
    uint32_t* src = (uint32_t*)buf->data;
    uint32_t dst_w = g_fb.width;
    uint32_t dst_h = g_fb.height;

    for (uint32_t y = 0; y < dst_h && y < buf->height; y++) {
        for (uint32_t x = 0; x < dst_w && x < buf->width; x++) {
            uint32_t color = src[y * buf->width + x];
            uint32_t pixel = ((color & 0xFF00FF00) |
                            ((color & 0x00FF0000) >> 16) |
                            ((color & 0x000000FF) << 16));
            uint32_t* fb_ptr = (uint32_t*)((uint8_t*)g_fb.addr + y * g_fb.pitch + x * g_fb.bytes_per_pixel);
            *fb_ptr = pixel;
        }
    }

    __asm__ volatile("mfence" ::: "memory");
    syscall0(SYS_FB_FLUSH);
}

void gfx_shutdown(void) {
    if (!g_initialized) return;

    gfx_input_shutdown();
    gfx_font_release_all_tracked();
    gfx_image_release_all_tracked();

    if (g_backbuffer) {
        free(g_backbuffer);
        g_backbuffer = NULL;
    }
    g_backbuffer_size = 0;
    g_backbuffer_valid = false;
    g_use_backbuffer = false;

    if (g_custom_cursor) {
        free(g_custom_cursor);
        g_custom_cursor = NULL;
    }

    syscall0(SYS_MUNMAP_FB);
    g_fb.addr = NULL;
    g_fb.width = 0;
    g_fb.height = 0;
    g_initialized = false;
}
