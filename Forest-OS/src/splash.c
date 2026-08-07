/*
 * splash.c - Fern boot splash screen (XP style, animated marquee)
 *
 * Runs a dedicated animation thread at 30fps that continuously sweeps
 * a marquee highlight across the progress bar. The main boot thread
 * calls splash_set_progress() and splash_update_status() to update
 * state; the animation thread handles all rendering.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "include/splash.h"
#include "include/render_layers.h"
#include "include/graphics/font8x8.h"
#include "include/graphics/graphics_types.h"
#include "include/gfx_config.h"
#include "include/debuglog.h"
#include "include/task.h"
#include "include/timer.h"

#if HAS_SPLASH

/* External framebuffer globals (defined in kernel.c) */
extern void*    g_multiboot_framebuffer;
extern uint32_t g_multiboot_fb_width;
extern uint32_t g_multiboot_fb_height;
extern uint32_t g_multiboot_fb_pitch;
extern uint32_t g_multiboot_fb_bpp;

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

/* ---------------------------------------------------------------------------
 * Module state (shared between boot thread and animation thread)
 * ------------------------------------------------------------------------- */
static volatile splash_state_t g_splash_state   = SPLASH_STATE_IDLE;
static volatile bool g_splash_running           = false;
static volatile bool g_anim_thread_active       = false;
static volatile bool g_anim_task_created        = false;
static volatile uint32_t g_anim_task_id         = 0;

/* Written by boot thread, read by animation thread */
static volatile uint8_t g_progress_percent  = 0;
static volatile uint8_t g_prev_progress     = 0xFF; /* force initial render */

/* Status text: double-buffered for thread safety */
static char g_status_text[64]     = {0};
static char g_prev_status[64]     = {0};
static volatile bool g_status_dirty = false;

/* Marquee animation state (owned by animation thread) */
static uint32_t g_marquee_offset = 0;
static uint32_t g_anim_frame     = 0;

/* Early-boot buffer (before render layers available) */
static uint8_t*  g_splash_early_buf  = NULL;
static bool      g_splash_using_layer = false;

/* Fade out request flag */
static volatile bool g_fadeout_requested = false;
static volatile bool g_fadeout_done      = false;

/* Animation FPS */
#define SPLASH_FPS          30
#define SPLASH_FRAME_MS     (1000 / SPLASH_FPS)  /* ~33ms */

/* ---------------------------------------------------------------------------
 * XP-style colour palette
 * ------------------------------------------------------------------------- */
#define COL_BG_TOP_R     0
#define COL_BG_TOP_G     58
#define COL_BG_TOP_B     174
#define COL_BG_BOT_R     0
#define COL_BG_BOT_G     0
#define COL_BG_BOT_B     80

#define COL_LOGO         0xFFFFFFu
#define COL_VERSION      0xA0C0FFu
#define COL_BAR_BG       0x002470u
#define COL_BAR_FG       0x3399FFu
#define COL_BAR_GLOW     0x66BBFFu
#define COL_BAR_BORDER   0x001850u
#define COL_STATUS       0xCCDDFFu

/* Progress bar geometry */
#define BAR_WIDTH_PCT   30u
#define BAR_HEIGHT      10u
#define BAR_Y_PCT       78u
#define MARQUEE_WIDTH   60u
#define MARQUEE_SPEED   3u

/* ---------------------------------------------------------------------------
 * Layer helpers
 * ------------------------------------------------------------------------- */
static inline rl_layer_t* splash_layer(void)
{
    return rl_is_initialized() ? rl_get_layer(RL_LAYER_SPLASH) : NULL;
}

static inline uint8_t* splash_target_buf(void)
{
    rl_layer_t* layer = splash_layer();
    if (layer && layer->buffer) return layer->buffer;
    if (g_splash_early_buf)     return g_splash_early_buf;
    return (uint8_t*)g_multiboot_framebuffer;
}

static inline uint32_t splash_target_pitch(void)
{
    rl_layer_t* layer = splash_layer();
    if (layer && layer->buffer) return layer->pitch;
    return g_multiboot_fb_pitch;
}

/* ---------------------------------------------------------------------------
 * Low-level pixel/fill operations
 * ------------------------------------------------------------------------- */
static inline void splash_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (x >= g_multiboot_fb_width || y >= g_multiboot_fb_height) return;
    uint8_t* buf = splash_target_buf();
    if (!buf) return;
    uint32_t bytes_pp = (g_multiboot_fb_bpp + 7) / 8;
    uint32_t pitch    = splash_target_pitch();
    uint8_t* p = buf + y * pitch + x * bytes_pp;
    p[0] = (rgb)       & 0xFF;
    p[1] = (rgb >>  8) & 0xFF;
    p[2] = (rgb >> 16) & 0xFF;
    if (bytes_pp == 4) p[3] = 0xFF;
}

static void splash_fill_rect(uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h,
                              uint32_t rgb)
{
    if (x >= g_multiboot_fb_width || y >= g_multiboot_fb_height) return;
    uint32_t x1 = (x + w < g_multiboot_fb_width)  ? x + w : g_multiboot_fb_width;
    uint32_t y1 = (y + h < g_multiboot_fb_height) ? y + h : g_multiboot_fb_height;
    for (uint32_t py = y; py < y1; py++)
        for (uint32_t px = x; px < x1; px++)
            splash_put_pixel(px, py, rgb);
}

static uint32_t colour_lerp(uint32_t c1, uint32_t c2, uint32_t t)
{
    uint32_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint32_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint32_t r = (r1 * (255 - t) + r2 * t) / 255;
    uint32_t g = (g1 * (255 - t) + g2 * t) / 255;
    uint32_t b = (b1 * (255 - t) + b2 * t) / 255;
    return (r << 16) | (g << 8) | b;
}

/* ---------------------------------------------------------------------------
 * Text rendering
 * ------------------------------------------------------------------------- */
#define SPLASH_TRANSPARENT 0xFFFFFFFFu

static void splash_draw_char(uint32_t px, uint32_t py, char c, uint32_t scale,
                              uint32_t fg_rgb, uint32_t bg_rgb)
{
    uint8_t idx = (uint8_t)c;
    if (idx >= 128) idx = '?';
    const char* glyph = font8x8_basic[idx];
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = (uint8_t)glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            bool set = (bits >> col) & 1;
            uint32_t color = set ? fg_rgb : bg_rgb;
            if (color == SPLASH_TRANSPARENT) continue;
            for (uint32_t sy = 0; sy < scale; sy++)
                for (uint32_t sx = 0; sx < scale; sx++)
                    splash_put_pixel(px + col * scale + sx, py + row * scale + sy, color);
        }
    }
}

static void splash_draw_string(uint32_t px, uint32_t py, const char* str,
                                uint32_t scale, uint32_t fg_rgb, uint32_t bg_rgb)
{
    if (!str) return;
    while (*str) {
        splash_draw_char(px, py, *str, scale, fg_rgb, bg_rgb);
        px += 8 * scale;
        str++;
    }
}

static void splash_draw_string_centered(uint32_t py, const char* str,
                                         uint32_t scale, uint32_t fg_rgb, uint32_t bg_rgb)
{
    if (!str) return;
    uint32_t len = 0;
    const char* p = str;
    while (*p++) len++;
    uint32_t text_w = len * 8 * scale;
    uint32_t px = (g_multiboot_fb_width > text_w)
                  ? (g_multiboot_fb_width - text_w) / 2 : 0;
    splash_draw_string(px, py, str, scale, fg_rgb, bg_rgb);
}

static uint32_t splash_string_pixel_len(const char* str, uint32_t scale)
{
    uint32_t len = 0;
    while (str && *str++) len++;
    return len * 8 * scale;
}

/* ---------------------------------------------------------------------------
 * Background: blue vertical gradient
 * ------------------------------------------------------------------------- */
static void splash_render_background(void)
{
    uint32_t h = g_multiboot_fb_height;
    uint32_t w = g_multiboot_fb_width;
    uint32_t top_rgb = ((uint32_t)COL_BG_TOP_R << 16) | ((uint32_t)COL_BG_TOP_G << 8) | COL_BG_TOP_B;
    uint32_t bot_rgb = ((uint32_t)COL_BG_BOT_R << 16) | ((uint32_t)COL_BG_BOT_G << 8) | COL_BG_BOT_B;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t t = (h > 1) ? (y * 255 / (h - 1)) : 0;
        uint32_t rgb = colour_lerp(top_rgb, bot_rgb, t);
        for (uint32_t x = 0; x < w; x++)
            splash_put_pixel(x, y, rgb);
    }
}

/* ---------------------------------------------------------------------------
 * Progress bar with marquee animation
 * ------------------------------------------------------------------------- */
static void splash_render_progress_bar(uint8_t percent)
{
    uint32_t bar_w = g_multiboot_fb_width * BAR_WIDTH_PCT / 100u;
    uint32_t bar_h = BAR_HEIGHT;
    uint32_t bar_x = (g_multiboot_fb_width - bar_w) / 2;
    uint32_t bar_y = g_multiboot_fb_height * BAR_Y_PCT / 100u;

    /* Border */
    splash_fill_rect(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, COL_BAR_BORDER);
    /* Trough */
    splash_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_BAR_BG);

    /* Filled portion */
    uint32_t fill_w = 0;
    if (percent > 0) {
        fill_w = bar_w * (uint32_t)percent / 100u;
        if (fill_w > bar_w) fill_w = bar_w;
        if (fill_w > 0) {
            splash_fill_rect(bar_x, bar_y, fill_w, bar_h, COL_BAR_FG);
            splash_fill_rect(bar_x, bar_y, fill_w, 2, COL_BAR_GLOW);
        }
    }

    /* Marquee sweep */
    uint32_t sweep_w = (percent > 0 && fill_w > 0) ? fill_w : bar_w;
    if (sweep_w > MARQUEE_WIDTH) {
        g_marquee_offset = (g_marquee_offset + MARQUEE_SPEED) % sweep_w;

        uint32_t half = MARQUEE_WIDTH / 2;
        uint32_t core = MARQUEE_WIDTH / 3;
        uint32_t fade = (MARQUEE_WIDTH - core) / 2;
        if (fade == 0) fade = 1;

        for (uint32_t i = 0; i < sweep_w; i++) {
            uint32_t center = g_marquee_offset;
            uint32_t dist = (i >= center) ? i - center : sweep_w - center + i;
            if (dist > sweep_w / 2) dist = sweep_w - dist;

            uint32_t rgb;
            if (dist <= core / 2) {
                rgb = COL_BAR_GLOW;
            } else if (dist <= half) {
                uint32_t t = (dist - core / 2) * 255 / fade;
                if (t > 255) t = 255;
                rgb = colour_lerp(COL_BAR_GLOW, COL_BAR_BG, t);
            } else {
                continue;
            }
            splash_fill_rect(bar_x + i, bar_y, 1, bar_h, rgb);
        }

        /* Re-draw glass highlight over filled region */
        if (fill_w > 0)
            splash_fill_rect(bar_x, bar_y, fill_w, 2, COL_BAR_GLOW);
    }
}

/* ---------------------------------------------------------------------------
 * Status label with gradient-matching erase
 * ------------------------------------------------------------------------- */
static void splash_render_status_label(const char* msg)
{
    uint32_t bar_y   = g_multiboot_fb_height * BAR_Y_PCT / 100u;
    uint32_t label_y = bar_y + BAR_HEIGHT + 8u;
    uint32_t scale   = 1;

    /* Erase previous label */
    if (g_prev_status[0]) {
        uint32_t old_w = splash_string_pixel_len(g_prev_status, scale);
        uint32_t old_x = (g_multiboot_fb_width > old_w)
                         ? (g_multiboot_fb_width - old_w) / 2 : 0;
        uint32_t h = g_multiboot_fb_height;
        uint32_t t = (h > 1) ? (label_y * 255 / (h - 1)) : 0;
        uint32_t top_rgb = ((uint32_t)COL_BG_TOP_R << 16) | ((uint32_t)COL_BG_TOP_G << 8) | COL_BG_TOP_B;
        uint32_t bot_rgb = ((uint32_t)COL_BG_BOT_R << 16) | ((uint32_t)COL_BG_BOT_G << 8) | COL_BG_BOT_B;
        uint32_t bg = colour_lerp(top_rgb, bot_rgb, t);
        splash_fill_rect(old_x, label_y, old_w, 8u * scale + 2, bg);
    }

    /* Draw new label */
    if (msg && msg[0]) {
        splash_draw_string_centered(label_y, msg, scale, COL_STATUS, SPLASH_TRANSPARENT);
    }
}

/* ---------------------------------------------------------------------------
 * Branding
 * ------------------------------------------------------------------------- */
static void splash_render_branding(void)
{
    uint32_t logo_y = g_multiboot_fb_height * 40u / 100u - 12u;
    splash_draw_string_centered(logo_y, "Fern", 3, COL_LOGO, SPLASH_TRANSPARENT);
    uint32_t ver_y = logo_y + 3 * 8 + 10;
    splash_draw_string_centered(ver_y, "v1.0", 2, COL_VERSION, SPLASH_TRANSPARENT);
}

/* ---------------------------------------------------------------------------
 * Composite layer buffer to visible framebuffer
 * ------------------------------------------------------------------------- */
static void splash_composite_to_fb(void)
{
    if (!g_multiboot_framebuffer) return;
    rl_layer_t* layer = splash_layer();
    uint8_t* src = NULL;
    uint32_t src_pitch = g_multiboot_fb_pitch;

    if (layer && layer->buffer) { src = layer->buffer; src_pitch = layer->pitch; }
    else if (g_splash_early_buf) { src = g_splash_early_buf; src_pitch = g_multiboot_fb_pitch; }
    if (!src) return;

    uint32_t bytes_pp  = (g_multiboot_fb_bpp + 7) / 8;
    uint32_t row_bytes = g_multiboot_fb_width * bytes_pp;
    for (uint32_t y = 0; y < g_multiboot_fb_height; y++) {
        uint8_t* dst = (uint8_t*)g_multiboot_framebuffer + y * g_multiboot_fb_pitch;
        memcpy(dst, src + y * src_pitch, row_bytes);
    }
}

/* ---------------------------------------------------------------------------
 * Full splash render (called by animation thread each frame)
 * ------------------------------------------------------------------------- */
static void splash_render_frame(void)
{
    /* Re-render progress bar (includes marquee advance) */
    splash_render_progress_bar(g_progress_percent);

    /* Update status text if dirty */
    if (g_status_dirty) {
        splash_render_status_label(g_status_text);
        memcpy(g_prev_status, g_status_text, sizeof(g_prev_status));
        g_status_dirty = false;
    }

    /* Composite to screen */
    splash_composite_to_fb();
}

/* Fade out: darken layer buffer step by step, then clear
 * Runs on the animation thread context when fadeout is requested */
static void splash_do_fadeout(void);  /* forward declaration */

/* ---------------------------------------------------------------------------
 * Splash animation thread
 * Loops at ~30fps, rendering the marquee and any pending updates.
 * Runs independently of the boot thread.
 * ------------------------------------------------------------------------- */
static void splash_animation_thread(void)
{
    g_anim_thread_active = true;

    while (g_splash_running && !g_fadeout_requested) {
        g_anim_frame++;

        /* Render progress bar with marquee */
        splash_render_progress_bar(g_progress_percent);

        /* Update status text if dirty */
        if (g_status_dirty) {
            splash_render_status_label(g_status_text);
            memcpy(g_prev_status, g_status_text, sizeof(g_prev_status));
            g_status_dirty = false;
        }

        /* Composite to screen */
        splash_composite_to_fb();

        timer_sleep_ms(SPLASH_FRAME_MS);
    }

    /* If fadeout was requested, do it */
    if (g_fadeout_requested) {
        splash_do_fadeout();
    }

    g_anim_thread_active = false;
}

/* ---------------------------------------------------------------------------
 * Fade out: darken layer buffer step by step, then clear
 * Runs on the animation thread context when fadeout is requested
 * ------------------------------------------------------------------------- */
static void splash_do_fadeout(void)
{
    rl_layer_t* layer = splash_layer();
    uint8_t* buf = NULL;
    uint32_t pitch = g_multiboot_fb_pitch;

    if (layer && layer->buffer) { buf = layer->buffer; pitch = layer->pitch; }
    else if (g_splash_early_buf) { buf = g_splash_early_buf; }
    else { buf = (uint8_t*)g_multiboot_framebuffer; pitch = g_multiboot_fb_pitch; }
    if (!buf) { g_fadeout_done = true; return; }

    uint32_t w = g_multiboot_fb_width;
    uint32_t h = g_multiboot_fb_height;
    uint32_t bpp = g_multiboot_fb_bpp;
    uint32_t bytes_pp = (bpp + 7) / 8;

    #define FADE_STEPS 6
    #define FADE_DELAY 8000000u

    for (int step = 0; step < FADE_STEPS; step++) {
        for (uint32_t y = 0; y < h; y++) {
            uint8_t* row = buf + y * pitch;
            for (uint32_t x = 0; x < w; x++) {
                uint8_t* px = row + x * bytes_pp;
                px[0] = (px[0] * 7) >> 3;
                px[1] = (px[1] * 7) >> 3;
                px[2] = (px[2] * 7) >> 3;
            }
        }
        splash_composite_to_fb();
        for (volatile uint32_t i = 0; i < FADE_DELAY; i++);
    }

    /* Clear to black */
    if (buf != (uint8_t*)g_multiboot_framebuffer)
        memset(buf, 0, (size_t)pitch * h);
    splash_composite_to_fb();

    g_fadeout_done = true;
}

/* ---------------------------------------------------------------------------
 * Early buffer allocation
 * ------------------------------------------------------------------------- */
static bool splash_alloc_early_buf(void)
{
    if (g_splash_early_buf) return true;
    size_t buf_size = (size_t)g_multiboot_fb_pitch * g_multiboot_fb_height;
    g_splash_early_buf = (uint8_t*)kmalloc(buf_size);
    if (!g_splash_early_buf) return false;
    memset(g_splash_early_buf, 0, buf_size);
    return true;
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

bool splash_init(const splash_config_t* config)
{
    (void)config;
    g_splash_state      = SPLASH_STATE_INITIALIZING;
    g_progress_percent  = 0;
    g_prev_progress     = 0xFF;
    g_status_text[0]    = '\0';
    g_prev_status[0]    = '\0';
    g_status_dirty      = false;
    g_marquee_offset    = 0;
    g_anim_frame        = 0;
    g_fadeout_requested = false;
    g_fadeout_done      = false;

    if (!g_multiboot_framebuffer || g_multiboot_fb_width == 0 || g_multiboot_fb_height == 0) {
        g_splash_state = SPLASH_STATE_IDLE;
        return false;
    }

    /* Allocate render target */
    if (rl_is_initialized()) {
        rl_set_visible(RL_LAYER_SPLASH, true);
        rl_set_opacity(RL_LAYER_SPLASH, 255);
        g_splash_using_layer = true;
    } else {
        splash_alloc_early_buf();
        g_splash_using_layer = false;
    }

    /* Render initial static content */
    splash_render_background();
    splash_render_branding();
    splash_render_progress_bar(0);
    strncpy(g_status_text, "Loading...", sizeof(g_status_text) - 1);
    g_status_dirty = true;
    /* Do one initial render to show something immediately */
    splash_render_frame();

    g_splash_state = SPLASH_STATE_RUNNING;
    return true;
}

bool splash_start(void)
{
    if (!g_multiboot_framebuffer || g_multiboot_fb_width == 0 || g_multiboot_fb_height == 0)
        return false;

    g_splash_running = true;
    g_splash_state   = SPLASH_STATE_RUNNING;

    /* splash_start() runs very early in boot, well before tasks_init(). A
     * kernel task created via task_create_kernel() before tasks_init() runs
     * is never linked into the ready queue tasks_init() builds - it's
     * silently unreachable by the scheduler forever, and turned out to also
     * hit an unrelated, never-before-exercised bug in its first-ever context
     * switch. Defer actual task creation to splash_start_animation_task(),
     * which kernel_main() calls right after tasks_init(); until then the
     * splash layer just shows its static initial render (no live marquee),
     * exactly like the pre-existing "task creation failed" fallback below. */
    if (!current_task) {
        debuglog(DEBUG_INFO, "[SPLASH] Task system not ready yet; animation thread deferred\n");
        return true;
    }

    splash_start_animation_task();
    return true;
}

void splash_start_animation_task(void)
{
    if (g_anim_task_created || !g_splash_running) {
        return;
    }

    // Disabled: task_create_kernel()'s first-ever context switch has a
    // pre-existing bug (see the comment in splash_start() above) that
    // reliably crashes this task with an invalid opcode at a garbage
    // near-null EIP immediately on its first schedule. The crash-recovery
    // path (kill the task, park it, let the scheduler move on) does not
    // reliably resolve in practice, leaving the boot splash frozen on
    // screen with no visible login prompt and no working input -- a full
    // boot block, not merely a missing animation. Skipping animation-task
    // creation entirely leaves the static first-frame splash image up
    // (drawn once by splash_init()) until splash_stop() clears it
    // unconditionally and reliably (g_anim_task_created stays false, so
    // splash_stop() takes its "task never created" path and skips straight
    // to hiding the layer, no wait involved).
    debuglog(DEBUG_INFO, "[SPLASH] Animation thread disabled (first-schedule crash workaround); static splash only\n");
}

void splash_stop(void)
{
    if (!g_multiboot_framebuffer) {
        g_splash_running = false;
        g_splash_state   = SPLASH_STATE_DONE;
        return;
    }

    /* Show 100% + "Done!" briefly */
    g_progress_percent = 100;
    strncpy(g_status_text, "Done!", sizeof(g_status_text) - 1);
    g_status_dirty = true;
    /* Give the animation thread time to render the final frame */
    timer_sleep_ms(200);

    /* If the animation task was never created (e.g. splash_start() ran
     * before the task scheduler was ready and splash_start_animation_task()
     * was never called), there is nothing that will ever set g_fadeout_done
     * or clear g_anim_thread_active - waiting on them would spin forever.
     *
     * Even when the task was created, bound these waits: boot must never
     * hang forever on a decorative animation thread that stalls (or gets
     * corrupted by a scheduler bug and never reaches its exit path). 5
     * seconds is generous for a fadeout that normally takes well under 1s. */
    if (g_anim_task_created) {
        #define SPLASH_STOP_MAX_WAIT_ITERS 500 /* 500 * 10ms = 5s */

        /* Request fadeout and wait for it */
        g_fadeout_requested = true;
        for (uint32_t i = 0; !g_fadeout_done && i < SPLASH_STOP_MAX_WAIT_ITERS; i++) {
            timer_sleep_ms(10);
        }
        if (!g_fadeout_done) {
            debuglog(DEBUG_WARN, "[SPLASH] Timed out waiting for fadeout; continuing boot anyway\n");
        }

        /* Stop the animation thread */
        g_splash_running = false;
        /* Wait for thread to exit */
        for (uint32_t i = 0; g_anim_thread_active && i < SPLASH_STOP_MAX_WAIT_ITERS; i++) {
            timer_sleep_ms(10);
        }
        if (g_anim_thread_active) {
            debuglog(DEBUG_WARN, "[SPLASH] Timed out waiting for animation thread to exit; continuing boot anyway\n");
        }

        /* Either wait above timing out means the animation task is stuck (or
         * was left running garbage code by a scheduler bug) rather than
         * cleanly exited. Force it down via SIGKILL so it can't keep
         * consuming timeslices or corrupting state in the background for
         * the rest of boot; harmless / a no-op if it already exited normally. */
        if (!g_fadeout_done || g_anim_thread_active) {
            task_kill(g_anim_task_id);
        }

        #undef SPLASH_STOP_MAX_WAIT_ITERS
    } else {
        g_splash_running = false;
    }

    /* Hide splash layer */
    if (rl_is_initialized()) {
        rl_layer_t* layer = rl_get_layer(RL_LAYER_SPLASH);
        if (layer && layer->buffer)
            memset(layer->buffer, 0, (size_t)layer->pitch * layer->height);
        rl_set_visible(RL_LAYER_SPLASH, false);
        rl_mark_dirty(RL_LAYER_SPLASH);
    }

    /* Clear framebuffer */
    if (g_multiboot_framebuffer)
        memset(g_multiboot_framebuffer, 0, (size_t)g_multiboot_fb_pitch * g_multiboot_fb_height);

    g_splash_state = SPLASH_STATE_DONE;
}

void splash_draw_background(void)
{
    if (g_multiboot_framebuffer) {
        g_prev_status[0] = '\0';
        splash_render_background();
        splash_render_branding();
        splash_render_progress_bar(0);
        strncpy(g_status_text, "Loading...", sizeof(g_status_text) - 1);
        g_status_dirty = true;
    }
}

void splash_draw_frame(uint32_t frame_number)
{
    (void)frame_number;
}

bool splash_is_running(void) { return g_splash_running; }
splash_state_t splash_get_state(void) { return g_splash_state; }

void splash_set_progress(uint8_t progress_percent)
{
    if (progress_percent > 100) progress_percent = 100;
    g_progress_percent = progress_percent;
}

void splash_update_status(const char* status_message, bool success)
{
    (void)success;
    if (status_message && status_message[0]) {
        /* Copy to the shared buffer (boot thread writes, anim thread reads) */
        strncpy(g_status_text, status_message, sizeof(g_status_text) - 1);
        g_status_text[sizeof(g_status_text) - 1] = '\0';
        g_status_dirty = true;
    }
}

void splash_cleanup(void)
{
    g_splash_running = false;
    g_splash_state   = SPLASH_STATE_DONE;
    g_status_text[0] = '\0';
    g_prev_status[0] = '\0';

    if (g_splash_early_buf) {
        kfree(g_splash_early_buf);
        g_splash_early_buf = NULL;
    }
}

void splash_animation_task(void) {}

void splash_record_start_ticks(void)
{
    /* Not used with threaded animation */
}

bool splash_should_timeout(void)
{
    return false;
}

void splash_rerender(void)
{
    if (g_splash_state != SPLASH_STATE_RUNNING) return;
    g_prev_status[0] = '\0';
    g_status_dirty = true;
}

void splash_migrate_to_layer(void)
{
    if (g_splash_using_layer || !rl_is_initialized()) return;

    rl_layer_t* layer = rl_get_layer(RL_LAYER_SPLASH);
    if (!layer || !layer->buffer) return;

    if (g_splash_early_buf) {
        uint32_t bytes_pp  = (g_multiboot_fb_bpp + 7) / 8;
        uint32_t row_bytes = g_multiboot_fb_width * bytes_pp;
        for (uint32_t y = 0; y < g_multiboot_fb_height; y++)
            memcpy(layer->buffer + y * layer->pitch,
                   g_splash_early_buf + y * g_multiboot_fb_pitch, row_bytes);
    }

    rl_set_visible(RL_LAYER_SPLASH, true);
    rl_set_opacity(RL_LAYER_SPLASH, 255);
    rl_mark_dirty(RL_LAYER_SPLASH);
    g_splash_using_layer = true;
}

/* ---------------------------------------------------------------------------
 * Animation thread integration with the render loop
 *
 * The splash_animation_thread() loop checks g_fadeout_requested each frame.
 * When set, it runs the fadeout and sets g_fadeout_done, then exits.
 * ------------------------------------------------------------------------- */

#else /* !HAS_SPLASH */

/* No-framebuffer / no-splash stubs. Boot proceeds without a splash screen;
 * status messages are ignored. Initialisers return success so callers do
 * not treat a disabled splash as a boot failure. */

bool splash_init(const splash_config_t* config) { (void)config; return true; }
bool splash_start(void)                          { return false; }
void splash_start_animation_task(void)           { }
void splash_stop(void)                           { }
void splash_draw_background(void)                { }
void splash_draw_frame(uint32_t frame_number)    { (void)frame_number; }
bool splash_is_running(void)                     { return false; }
splash_state_t splash_get_state(void)            { return SPLASH_STATE_IDLE; }
void splash_set_progress(uint8_t progress_percent){ (void)progress_percent; }
void splash_update_status(const char* status_message, bool success) {
    (void)status_message; (void)success;
}
void splash_cleanup(void)                        { }
void splash_animation_task(void)                 { }
void splash_record_start_ticks(void)             { }
bool splash_should_timeout(void)                 { return true; }
void splash_rerender(void)                       { }
void splash_migrate_to_layer(void)               { }

#endif /* HAS_SPLASH */
