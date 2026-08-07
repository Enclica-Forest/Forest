/*
 * framebuffer_dbuf.c - Double-buffering and dirty-rect presentation for the
 *                      kernel framebuffer path.
 *
 * Implements the clean fb_* API declared in include/framebuffer.h:
 *
 *   - fb_init / fb_shutdown        allocate and release the back buffer
 *   - fb_get_dbuf_info             populate a kernel-side fb_info descriptor
 *   - fb_present / fb_present_full flip back->front (dirty rects or full)
 *   - fb_clear / fb_fill_rect      write to the rendering target
 *   - fb_invalidate_rect / _full   mark dirty regions
 *   - fb_set_palette               install an indexed palette
 *   - fb_wait_vsync                device vsync wait (bounded busy-wait fallback)
 *   - fb_scroll_rect               vertical scroll of a rectangle
 *
 * Dirty-rect merging: fb_invalidate_rect() clips the rect to the screen and
 * either absorbs it into an existing rect that overlaps/abuts it, or appends
 * a new entry. When dirty_count would exceed FB_MAX_DIRTY (or the threshold
 * fraction set by FB_DIRTY_FULL_THRESHOLD), the tracker collapses to a
 * full-screen invalidate so fb_present() can do a single bulk copy.
 *
 * When ENABLE_DOUBLE_BUFFERING is 0, rendering targets the front buffer
 * directly and fb_present() is a no-op. When HAS_FRAMEBUFFER is 0 the whole
 * file compiles down to stubs so no-fb builds stay small.
 */

#include "include/framebuffer.h"
#include "include/gfx_config.h"
#include "include/debuglog.h"
#include "include/timer.h"
#include "include/string.h"
#include "include/graphics/graphics_manager.h"
#include "include/graphics/graphics_types.h"

#if HAS_FRAMEBUFFER

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

/* ---------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */

static struct fb_info g_dbuf;
static bool           g_dbuf_inited;
static void*          g_back_alloc;     /* kmalloc'd back buffer (owned) */
static size_t         g_back_alloc_size;
static uint32_t       g_palette_storage[256];

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static size_t fb_frame_bytes(void)
{
    return (size_t)g_dbuf.pitch * (size_t)g_dbuf.height;
}

static void* fb_render_target(void)
{
#if HAS_DOUBLE_BUFFERING
    if (g_dbuf.double_buffered && g_dbuf.back)
        return g_dbuf.back;
#endif
    return g_dbuf.front;
}

static void fb_clip_rect(fb_rect_t* r)
{
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > (int)g_dbuf.width)  r->w = (int)g_dbuf.width  - r->x;
    if (r->y + r->h > (int)g_dbuf.height) r->h = (int)g_dbuf.height - r->y;
    if (r->w < 0) r->w = 0;
    if (r->h < 0) r->h = 0;
}

static bool fb_rects_overlap_or_adjacent(const fb_rect_t* a, const fb_rect_t* b)
{
    return !(a->x + a->w < b->x         ||
             b->x + b->w < a->x         ||
             a->y + a->h < b->y         ||
             b->y + b->h < a->y);
}

static void fb_union_rect(fb_rect_t* dst, const fb_rect_t* src)
{
    int x0 = dst->x < src->x ? dst->x : src->x;
    int y0 = dst->y < src->y ? dst->y : src->y;
    int x1 = dst->x + dst->w > src->x + src->w ? dst->x + dst->w : src->x + src->w;
    int y1 = dst->y + dst->h > src->y + src->h ? dst->y + dst->h : src->y + src->h;
    dst->x = x0; dst->y = y0;
    dst->w = x1 - x0; dst->h = y1 - y0;
}

static void fb_collapse_to_full(void)
{
    g_dbuf.dirty[0].x = 0;
    g_dbuf.dirty[0].y = 0;
    g_dbuf.dirty[0].w = (int)g_dbuf.width;
    g_dbuf.dirty[0].h = (int)g_dbuf.height;
    g_dbuf.dirty_count = 1;
}

/* ---------------------------------------------------------------------------
 * Init / shutdown
 * ------------------------------------------------------------------------- */

int fb_init(void)
{
    if (g_dbuf_inited)
        return FB_SUCCESS;

    memset(&g_dbuf, 0, sizeof(g_dbuf));

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        debuglog(DEBUG_WARN, "[FB_DBUF] no framebuffer from graphics manager\n");
        return FB_ERROR_NOT_FOUND;
    }

    g_dbuf.front  = (void*)fb->virtual_addr;
    g_dbuf.width  = fb->width;
    g_dbuf.height = fb->height;
    g_dbuf.pitch  = fb->pitch ? fb->pitch : fb->width * ((fb->bpp + 7) / 8);
    g_dbuf.bpp    = fb->bpp ? fb->bpp : FB_DEFAULT_BPP;
    g_dbuf.palette_mode = (fb->format == PIXEL_FORMAT_INDEXED_8);
    g_dbuf.palette = g_dbuf.palette_mode ? g_palette_storage : NULL;

#if HAS_DOUBLE_BUFFERING
    if (fb->double_buffered && fb->back_buffer) {
        g_dbuf.back = (void*)fb->back_buffer;
        g_dbuf.double_buffered = 1;
    } else {
        size_t need = fb_frame_bytes();
        if (need && (g_back_alloc = kmalloc(need)) != NULL) {
            g_back_alloc_size = need;
            memcpy(g_back_alloc, g_dbuf.front, need);
            g_dbuf.back = g_back_alloc;
            g_dbuf.double_buffered = 1;
        } else {
            debuglog(DEBUG_WARN, "[FB_DBUF] back buffer alloc failed; single-buffered\n");
            g_dbuf.double_buffered = 0;
        }
    }
#else
    g_dbuf.double_buffered = 0;
    g_dbuf.back = NULL;
#endif

    fb_invalidate_full();
    g_dbuf_inited = true;

    debuglog(DEBUG_INFO, "[FB_DBUF] init %ux%u %ubpp pitch=%u dbuf=%d\n",
             g_dbuf.width, g_dbuf.height, g_dbuf.bpp, g_dbuf.pitch,
             g_dbuf.double_buffered);
    return FB_SUCCESS;
}

void fb_shutdown(void)
{
    if (g_back_alloc) {
        kfree(g_back_alloc);
        g_back_alloc = NULL;
        g_back_alloc_size = 0;
    }
    g_dbuf.back = NULL;
    g_dbuf.front = NULL;
    g_dbuf.double_buffered = 0;
    g_dbuf.dirty_count = 0;
    g_dbuf.palette = NULL;
    g_dbuf.palette_mode = 0;
    g_dbuf_inited = false;
}

int fb_get_dbuf_info(struct fb_info* info)
{
    if (!info)
        return FB_ERROR_INVALID_PARAM;
    if (!g_dbuf_inited) {
        int r = fb_init();
        if (r != FB_SUCCESS)
            return r;
    }
    memcpy(info, &g_dbuf, sizeof(*info));
    return FB_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Dirty tracking
 * ------------------------------------------------------------------------- */

void fb_invalidate_full(void)
{
    if (!g_dbuf_inited) return;
    fb_collapse_to_full();
}

void fb_invalidate_rect(const fb_rect_t* r)
{
    if (!g_dbuf_inited || !r) return;

#if !HAS_FB_DIRTY_RECTS
    fb_collapse_to_full();
    return;
#endif

    fb_rect_t clip = *r;
    fb_clip_rect(&clip);
    if (clip.w <= 0 || clip.h <= 0)
        return;

    if (g_dbuf.dirty_count == 0) {
        g_dbuf.dirty[0] = clip;
        g_dbuf.dirty_count = 1;
        return;
    }

    /* Absorb into an existing overlapping/adjacent rect if possible. */
    for (int i = 0; i < g_dbuf.dirty_count; i++) {
        if (fb_rects_overlap_or_adjacent(&g_dbuf.dirty[i], &clip)) {
            fb_union_rect(&g_dbuf.dirty[i], &clip);
            /* Unioning may have bridged two existing rects; re-merge pass. */
            for (int j = 0; j < g_dbuf.dirty_count; j++) {
                if (i == j) continue;
                if (fb_rects_overlap_or_adjacent(&g_dbuf.dirty[i], &g_dbuf.dirty[j])) {
                    fb_union_rect(&g_dbuf.dirty[i], &g_dbuf.dirty[j]);
                    g_dbuf.dirty[j] = g_dbuf.dirty[g_dbuf.dirty_count - 1];
                    g_dbuf.dirty_count--;
                    j--;
                }
            }
            return;
        }
    }

    if (FB_DIRTY_FULL_THRESHOLD(g_dbuf.dirty_count + 1)) {
        fb_collapse_to_full();
        return;
    }

    if (g_dbuf.dirty_count < FB_MAX_DIRTY) {
        g_dbuf.dirty[g_dbuf.dirty_count++] = clip;
    } else {
        fb_collapse_to_full();
    }
}

/* ---------------------------------------------------------------------------
 * Presentation (flip)
 * ------------------------------------------------------------------------- */

static void fb_copy_rect_back_to_front(const fb_rect_t* r)
{
    if (!g_dbuf.back || !g_dbuf.front) return;
    if (r->w <= 0 || r->h <= 0) return;

    uint32_t bpp = (g_dbuf.bpp + 7) / 8;
    const uint8_t* src = (const uint8_t*)g_dbuf.back +
                         (size_t)r->y * g_dbuf.pitch + (size_t)r->x * bpp;
    uint8_t* dst = (uint8_t*)g_dbuf.front +
                   (size_t)r->y * g_dbuf.pitch + (size_t)r->x * bpp;
    size_t row_bytes = (size_t)r->w * bpp;

    for (int y = 0; y < r->h; y++) {
        memcpy(dst, src, row_bytes);
        src += g_dbuf.pitch;
        dst += g_dbuf.pitch;
    }
}

#if FB_USE_PANNING
static int fb_panning_flip(void)
{
    /* TODO: driver hook. When a panning ioctl is available, set the scanout
     * y-offset to the back buffer and swap front/back pointers instead of
     * copying. Fall through to memcpy until a driver exposes this. */
    return -1;
}
#endif

int fb_present(int mode)
{
    if (!g_dbuf_inited) return FB_ERROR_NOT_FOUND;

#if HAS_DOUBLE_BUFFERING
    if (!g_dbuf.double_buffered || !g_dbuf.back) {
        /* Single-buffered: rendering already hit the front buffer. */
        g_dbuf.dirty_count = 0;
        return FB_SUCCESS;
    }
#else
    g_dbuf.dirty_count = 0;
    return FB_SUCCESS;
#endif

#if HAS_FB_VSYNC_WAIT
    (void)fb_wait_vsync();
#endif

    if (mode == FB_PRESENT_FULL || g_dbuf.dirty_count == 0) {
        size_t bytes = fb_frame_bytes();
        if (bytes && g_dbuf.front && g_dbuf.back)
            memcpy(g_dbuf.front, g_dbuf.back, bytes);
        g_dbuf.dirty_count = 0;
        return FB_SUCCESS;
    }

#if FB_USE_PANNING
    if (fb_panning_flip() == 0) {
        g_dbuf.dirty_count = 0;
        return FB_SUCCESS;
    }
#endif

    for (int i = 0; i < g_dbuf.dirty_count; i++)
        fb_copy_rect_back_to_front(&g_dbuf.dirty[i]);

    g_dbuf.dirty_count = 0;
    return FB_SUCCESS;
}

int fb_present_full(void)
{
    return fb_present(FB_PRESENT_FULL);
}

/* ---------------------------------------------------------------------------
 * Drawing primitives (write to the render target)
 * ------------------------------------------------------------------------- */

static uint32_t fb_argb_to_native(uint32_t argb)
{
    /* Convert 0xAARRGGBB to the framebuffer's native pixel layout. For now
     * we assume 32bpp BGRA/RGBA which is a 1:1 byte move; other layouts use
     * a simple RGB extraction. */
    if (g_dbuf.bpp == 32)
        return argb;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8)  & 0xFF;
    uint8_t b = (argb >> 0)  & 0xFF;
    if (g_dbuf.bpp == 24)
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    if (g_dbuf.bpp == 16)
        return (((uint16_t)r >> 3) << 11) | (((uint16_t)g >> 2) << 5) | ((uint16_t)b >> 3);
    return argb;
}

int fb_clear(uint32_t argb)
{
    if (!g_dbuf_inited) return FB_ERROR_NOT_FOUND;
    void* dst = fb_render_target();
    if (!dst) return FB_ERROR_NOT_FOUND;

    uint32_t native = fb_argb_to_native(argb);
    uint32_t bpp = (g_dbuf.bpp + 7) / 8;
    size_t bytes = fb_frame_bytes();

    if (bpp == 4) {
        uint32_t* p = (uint32_t*)dst;
        size_t count = bytes / 4;
        for (size_t i = 0; i < count; i++) p[i] = native;
    } else if (bpp == 2) {
        uint16_t* p = (uint16_t*)dst;
        size_t count = bytes / 2;
        for (size_t i = 0; i < count; i++) p[i] = (uint16_t)native;
    } else {
        memset(dst, (uint8_t)(native & 0xFF), bytes);
    }

    fb_invalidate_full();
    return FB_SUCCESS;
}

int fb_fill_rect(const fb_rect_t* r, uint32_t argb)
{
    if (!g_dbuf_inited || !r) return FB_ERROR_INVALID_PARAM;
    fb_rect_t clip = *r;
    fb_clip_rect(&clip);
    if (clip.w <= 0 || clip.h <= 0) return FB_SUCCESS;

    void* dst = fb_render_target();
    if (!dst) return FB_ERROR_NOT_FOUND;

    uint32_t native = fb_argb_to_native(argb);
    uint32_t bpp = (g_dbuf.bpp + 7) / 8;
    size_t row_bytes = (size_t)clip.w * bpp;

    uint8_t* row = (uint8_t*)dst + (size_t)clip.y * g_dbuf.pitch + (size_t)clip.x * bpp;
    for (int y = 0; y < clip.h; y++) {
        if (bpp == 4) {
            uint32_t* p = (uint32_t*)row;
            for (int x = 0; x < clip.w; x++) p[x] = native;
        } else if (bpp == 2) {
            uint16_t* p = (uint16_t*)row;
            for (int x = 0; x < clip.w; x++) p[x] = (uint16_t)native;
        } else {
            memset(row, (uint8_t)(native & 0xFF), row_bytes);
        }
        row += g_dbuf.pitch;
    }

    fb_invalidate_rect(&clip);
    return FB_SUCCESS;
}

int fb_scroll_rect(const fb_rect_t* r, int dy)
{
    if (!g_dbuf_inited || !r) return FB_ERROR_INVALID_PARAM;
    fb_rect_t clip = *r;
    fb_clip_rect(&clip);
    if (clip.w <= 0 || clip.h <= 0 || dy == 0) return FB_SUCCESS;

    void* dst = fb_render_target();
    if (!dst) return FB_ERROR_NOT_FOUND;

    uint32_t bpp = (g_dbuf.bpp + 7) / 8;
    size_t row_bytes = (size_t)clip.w * bpp;
    uint8_t* base = (uint8_t*)dst + (size_t)clip.y * g_dbuf.pitch + (size_t)clip.x * bpp;

    if (dy > 0) {
        if (dy >= clip.h) {
            for (int y = 0; y < clip.h; y++)
                memset(base + (size_t)y * g_dbuf.pitch, 0, row_bytes);
        } else {
            for (int y = clip.h - 1; y >= dy; y--) {
                uint8_t* d = base + (size_t)y * g_dbuf.pitch;
                uint8_t* s = base + (size_t)(y - dy) * g_dbuf.pitch;
                memmove(d, s, row_bytes);
            }
            for (int y = 0; y < dy; y++)
                memset(base + (size_t)y * g_dbuf.pitch, 0, row_bytes);
        }
    } else {
        int up = -dy;
        if (up >= clip.h) {
            for (int y = 0; y < clip.h; y++)
                memset(base + (size_t)y * g_dbuf.pitch, 0, row_bytes);
        } else {
            for (int y = 0; y < clip.h - up; y++) {
                uint8_t* d = base + (size_t)y * g_dbuf.pitch;
                uint8_t* s = base + (size_t)(y + up) * g_dbuf.pitch;
                memmove(d, s, row_bytes);
            }
            for (int y = clip.h - up; y < clip.h; y++)
                memset(base + (size_t)y * g_dbuf.pitch, 0, row_bytes);
        }
    }

    fb_invalidate_rect(&clip);
    return FB_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

int fb_set_palette(const uint32_t* palette, int entries)
{
    if (!g_dbuf_inited) return FB_ERROR_NOT_FOUND;
    if (!palette || entries <= 0) return FB_ERROR_INVALID_PARAM;
    if (entries > 256) entries = 256;
    if (!g_dbuf.palette) return FB_ERROR_NOT_SUPPORTED;
    memcpy(g_dbuf.palette, palette, (size_t)entries * sizeof(uint32_t));
    return FB_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Vsync
 * ------------------------------------------------------------------------- */

int fb_wait_vsync(void)
{
    if (!g_dbuf_inited) return FB_ERROR_NOT_FOUND;

    /* Prefer the graphics-manager vsync hook (which may issue a driver
     * ioctl). If unsupported, fall back to a bounded busy-wait. */
    graphics_result_t r = graphics_wait_for_vsync();
    if (r == GRAPHICS_SUCCESS)
        return FB_SUCCESS;

#if FB_VSYNC_BUSY_WAIT_US > 0
    /* Spin until at least one timer tick elapses (bounded, no HZ dep). */
    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() == start) {
        __asm__ volatile("" ::: "memory");
    }
    return FB_SUCCESS;
#else
    return FB_SUCCESS;
#endif
}

#else /* !HAS_FRAMEBUFFER */

/* ----- No-framebuffer stubs: keep callers linking, do nothing. ----- */

int  fb_init(void)                                  { return FB_ERROR_NOT_FOUND; }
void fb_shutdown(void)                              { }
int  fb_get_dbuf_info(struct fb_info* info)         { (void)info; return FB_ERROR_NOT_FOUND; }
int  fb_present(int mode)                           { (void)mode; return FB_ERROR_NOT_FOUND; }
int  fb_present_full(void)                          { return FB_ERROR_NOT_FOUND; }
int  fb_clear(uint32_t argb)                        { (void)argb; return FB_ERROR_NOT_FOUND; }
int  fb_fill_rect(const fb_rect_t* r, uint32_t argb){ (void)r; (void)argb; return FB_ERROR_NOT_FOUND; }
void fb_invalidate_rect(const fb_rect_t* r)         { (void)r; }
void fb_invalidate_full(void)                       { }
int  fb_set_palette(const uint32_t* palette, int e) { (void)palette; (void)e; return FB_ERROR_NOT_FOUND; }
int  fb_wait_vsync(void)                            { return FB_ERROR_NOT_FOUND; }
int  fb_scroll_rect(const fb_rect_t* r, int dy)     { (void)r; (void)dy; return FB_ERROR_NOT_FOUND; }

#endif /* HAS_FRAMEBUFFER */
