#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "gfx_config.h"

// Framebuffer information structure for userspace applications
// This provides all necessary metadata for applications to properly
// use the shared memory framebuffer
typedef struct {
    void* addr;            // Virtual address of mapped framebuffer
    uintptr_t phys_addr;   // Physical address of framebuffer
    uint32_t width;        // Width in pixels
    uint32_t height;       // Height in pixels
    uint32_t pitch;        // Bytes per scanline
    uint32_t bpp;          // Bits per pixel
    uint32_t size;         // Total size in bytes
    uint32_t format;       // Pixel format (see graphics_types.h)
    uint32_t flags;        // Framebuffer flags
} fb_info_t;

// Framebuffer flags
#define FB_FLAG_USER_ACCESSIBLE   0x00000001  // Userspace can access
#define FB_FLAG_CACHE_DISABLED     0x00000002  // Cache disabled (PCD bit set)
#define FB_FLAG_WRITE_COMBINING    0x00000004  // Write combining enabled (PAT)
#define FB_FLAG_DOUBLE_BUFFERED    0x00000008  // Double buffering available
#define FB_FLAG_HARDWARE_CURSOR    0x00000010  // Hardware cursor available

// Pixel formats (matching graphics_types.h)
#define FB_FORMAT_TEXT_MODE        0
#define FB_FORMAT_INDEXED_8        1
#define FB_FORMAT_RGB_555          2
#define FB_FORMAT_RGB_565          3
#define FB_FORMAT_RGB_888          4
#define FB_FORMAT_RGBA_8888        5
#define FB_FORMAT_BGR_888          6
#define FB_FORMAT_BGRA_8888        7

// Syscall numbers for framebuffer operations (defined in syscall.h)

// Error codes
#define FB_SUCCESS                0
#define FB_ERROR_INVALID_PARAM    -1
#define FB_ERROR_NOT_FOUND        -2
#define FB_ERROR_NO_MEMORY        -3
#define FB_ERROR_PERMISSION       -4
#define FB_ERROR_ALREADY_MAPPED   -5
#define FB_ERROR_NOT_MAPPED       -6
#define FB_ERROR_NOT_SUPPORTED    -7

/* =========================================================================
 * Double-buffering / present API (kernel-side)
 *
 * The legacy userspace ABI (fb_info_t + fb_get_info/fb_mmap/fb_munmap) is
 * preserved unchanged above. The structures and functions below form the
 * clean kernel framebuffer API used by the compositor, splash, panic UI,
 * and the mmap path. When HAS_FRAMEBUFFER is 0 the functions are still
 * declared (so callers compile) but resolve to no-op stubs.
 * ====================================================================== */

/* Compile-time tunables. These honour -D overrides from the build system
 * via build_options.h, and provide safe defaults otherwise. FB_MAX_DIRTY
 * tracks FB_MAX_DIRTY_RECTS from build_options.h (always included via
 * gfx_config.h) unless overridden directly. */
#ifndef FB_MAX_DIRTY
#  define FB_MAX_DIRTY FB_MAX_DIRTY_RECTS
#endif

#ifndef FB_DEFAULT_BPP
#  define FB_DEFAULT_BPP 32
#endif

/* Hardware panning flip (page-flip via y-offset / panning registers).
 * Defined as 0/1 so callers can use #if FB_USE_PANNING. */
#ifndef FB_USE_PANNING
#  define FB_USE_PANNING HAS_FB_PANNING
#endif

/* Dirty rectangle (signed so off-screen / negative clipping works). */
typedef struct fb_rect {
    int x, y, w, h;
} fb_rect_t;

/* Kernel-side framebuffer descriptor. Holds front/back pointers, dirty
 * tracking state, and an optional indexed palette. `front` is the live
 * hardware scanout buffer; `back` is the rendering target when
 * double_buffered is non-zero (otherwise rendering goes to `front`). */
typedef struct fb_info {
    void*    mmio;            /* memory-mapped control registers (NULL if none) */
    void*    back;            /* back buffer (NULL when !double_buffered) */
    void*    front;           /* live scanout buffer */
    uint32_t width;           /* pixels per scanline */
    uint32_t height;          /* scanlines */
    uint32_t pitch;           /* bytes per scanline */
    uint32_t bpp;             /* bits per pixel */
    int      double_buffered; /* 1 = rendering to back, 0 = rendering to front */
    int      dirty_count;     /* number of valid entries in dirty[] */
    fb_rect_t dirty[FB_MAX_DIRTY]; /* dirty regions awaiting flip */
    uint32_t* palette;        /* indexed-mode palette (NULL for direct colour) */
    int      palette_mode;    /* non-zero when the FB is in indexed mode */
} fb_info_t_dbuf;

/* Present modes for fb_present(). */
#define FB_PRESENT_DIRTY   0   /* copy only merged dirty rects (default) */
#define FB_PRESENT_FULL    1   /* copy the entire back buffer */

#ifdef __cplusplus
extern "C" {
#endif

// Userspace API functions
int fb_get_info(fb_info_t* info);
void* fb_mmap(void);
int fb_munmap(void* addr);

// Kernel-internal functions
// Call this from timer interrupt to enable automatic framebuffer refresh
void framebuffer_update_periodic(void);

// Initialize framebuffer mmap subsystem
int framebuffer_mmap_init(void);

// Validate all framebuffer mappings (call on context switch or periodically)
void framebuffer_validate_all_mappings(void);

// Force refresh of framebuffer info (call after mode change)
void framebuffer_mmap_refresh(void);

// Unmap framebuffer from all tasks - call when switching display modes
// to ensure clean state and prevent multiple processes from writing
void framebuffer_mmap_unmap_all(void);

// Poll-able generation counter, bumped by framebuffer_mmap_unmap_all().
// Userspace clients compare this against their last-seen value to detect
// a mode change and re-map. See SYS_GET_FB_GENERATION in syscall.h.
uint32_t framebuffer_mmap_get_mode_generation(void);

// Clean up framebuffer mappings for a task when it exits
void framebuffer_mmap_task_exit(task_t* task);

// Set the task that enabled double buffering (called when double buffering is enabled)
void framebuffer_mmap_set_double_buffer_owner(uint32_t pid);

// Returns true if at least one userspace process has the framebuffer mapped.
// The WM compositor uses this to avoid overwriting userspace framebuffer writes.
bool framebuffer_has_userspace_mapping(void);

// Returns the tick count when userspace last called SYS_FB_FLUSH.
uint32_t framebuffer_get_userspace_last_flush(void);

// Returns milliseconds since userspace last flushed (0 if no mapping).
uint32_t framebuffer_get_userspace_mapping_duration_ms(void);

bool framebuffer_mmap_handle_page_fault(uint32_t fault_addr);

// Framebuffer handoff: preserve the last frame during DM-to-DE transitions.
// When enabled, task exit won't restore double-buffering (which could flash
// the screen), and the physical framebuffer content persists until the new
// graphics app renders its first frame.
void framebuffer_set_preserve_last_frame(bool preserve);

/* -------------------------------------------------------------------------
 * Clean framebuffer API (double-buffer aware)
 *
 * fb_init        - initialise the framebuffer descriptor (front/back/palette)
 * fb_shutdown    - release back buffer and palette
 * fb_get_dbuf_info - fill in a kernel-side fb_info descriptor
 * fb_present     - flip back->front for dirty rects (mode=FB_PRESENT_DIRTY)
 *                  or the whole buffer (mode=FB_PRESENT_FULL)
 * fb_present_full- convenience: copy the entire back buffer to front
 * fb_clear       - clear the rendering target (back when double-buffered)
 * fb_fill_rect   - solid fill a rectangle on the rendering target
 * fb_invalidate_rect / fb_invalidate_full - mark dirty regions
 * fb_set_palette - install an indexed-mode palette
 * fb_wait_vsync  - wait for device vsync (bounded busy-wait fallback)
 * fb_scroll_rect - vertically scroll a rectangle by dy lines
 *
 * When HAS_FRAMEBUFFER is 0 every function above is a no-op stub that
 * returns FB_ERROR_NOT_FOUND / does nothing, so non-fb builds link cleanly.
 * -----------------------------------------------------------------------*/
int  fb_init(void);
void fb_shutdown(void);
int  fb_get_dbuf_info(struct fb_info* info);
int  fb_present(int mode);
int  fb_present_full(void);
int  fb_clear(uint32_t argb);
int  fb_fill_rect(const fb_rect_t* r, uint32_t argb);
void fb_invalidate_rect(const fb_rect_t* r);
void fb_invalidate_full(void);
int  fb_set_palette(const uint32_t* palette, int entries);
int  fb_wait_vsync(void);
int  fb_scroll_rect(const fb_rect_t* r, int dy);

#ifdef __cplusplus
}
#endif

#endif // FRAMEBUFFER_H