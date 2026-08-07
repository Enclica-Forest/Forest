// TTY status-bar / decoration rendering backend, split out of tty.c.
//
// This translation unit owns the direct-framebuffer drawing helpers for
// the status bar, its embedded boot logo, CPU-core indicators, keyboard
// modifier indicators, scroll position indicator, and the brief VT
// transition/fade effect. It shares tty.c's public API surface (declared
// in include/tty.h) plus a small kernel-internal cross-file interface
// (tty_internal.h) used only between tty.c and this file.
//
// Core output/panic paths (cell rendering, cursor handling, 256-color
// palette, and the crash screen) are NOT part of this split and remain
// in tty.c.

#include "include/tty.h"
#include "tty_internal.h"

#include "include/graphics/graphics_manager.h"
#include "include/graphics/font_renderer.h"
#include "include/graphics/tty_font_renderer.h"
#include "include/graphics/graphics_types.h"
#include "include/debuglog.h"
#include "include/libc/stdio.h"
#include "include/smp.h"
#include "include/bmp.h"
#include "include/cmos_rtc.h"
#include "include/keyboard_interrupt_handler.h"
#include "include/framebuffer.h"
#include "include/string.h"
#include "include/memory.h"

// TTY status bar
#define TTY_STATUS_BAR_GRADIENT_TOP_R 25
#define TTY_STATUS_BAR_GRADIENT_TOP_G 25
#define TTY_STATUS_BAR_GRADIENT_TOP_B 30
#define TTY_STATUS_BAR_GRADIENT_BOT_R 35
#define TTY_STATUS_BAR_GRADIENT_BOT_G 35
#define TTY_STATUS_BAR_GRADIENT_BOT_B 42
#define TTY_ACCENT_R 100
#define TTY_ACCENT_G 180
#define TTY_ACCENT_B 255
static bool status_bar_drawn = false;
static bool status_bar_visible = true;

// Keyboard modifier tracking for status bar display
static bool g_kbd_ctrl = false;
static bool g_kbd_alt = false;
static bool g_kbd_shift = false;

// Status bar logo cached data
static bmp_image_t* g_statusbar_logo = NULL;
static bool g_statusbar_logo_loaded = false;
static bool g_statusbar_logo_attempted = false;

// Login status tracking (g_status_text / g_user_logged_in are only ever
// touched by functions in this file; g_login_status / g_current_user are
// shared with tty.c via tty_internal.h since tty_set_login_status_text()
// and tty_set_current_user_text() stay in tty.c).
static bool g_user_logged_in = false;
static char g_status_text[96] = "";

// Local mirror of tty.c's panic-lockdown gate, expressed via the public
// tty_is_panic_lockdown() accessor so this file never needs direct access
// to tty.c's file-local tty_state.
static inline bool tty_runtime_mutation_allowed(void) {
    return !tty_is_panic_lockdown();
}

// =============================================================================
// STATUS BAR LOGO - Load, scale, and render the bootup logo (BMP)
// =============================================================================

// Load logo from initrd (BMP file)
static bool tty_load_statusbar_logo(void) {
    if (g_statusbar_logo_loaded) {
        return true; // Already loaded
    }
    if (g_statusbar_logo_attempted) {
        return false; // Already attempted and failed
    }
    g_statusbar_logo_attempted = true;

    static const char* logo_paths[] = {
        "/usr/share/images/bootup/logo.bmp",
        "/bootup/logo.bmp",
        "/usr/share/images/bootup/logo.png",
        "/bootup/logo.png",
    };

    bmp_result_t last_error = BMP_ERROR_FILE_NOT_FOUND;
    g_statusbar_logo = NULL;
    for (uint32 i = 0; i < (sizeof(logo_paths) / sizeof(logo_paths[0])); i++) {
        bmp_result_t result = bmp_load_from_file(logo_paths[i], &g_statusbar_logo);
        if (result == BMP_SUCCESS) {
            break;
        }
        last_error = result;
        g_statusbar_logo = NULL;
    }

    if (!g_statusbar_logo || !g_statusbar_logo->pixel_data) {
        debuglog(DEBUG_WARN, "TTY: Could not load boot logo from initrd (error=%d)\n", last_error);
        return false;
    }

    g_statusbar_logo_loaded = true;
    debuglog(DEBUG_INFO, "TTY: Loaded logo.bmp: %ux%u, %ubpp\n",
             g_statusbar_logo->width, g_statusbar_logo->height, g_statusbar_logo->bpp);
    return true;
}

// Draw scaled logo to framebuffer at specified position
static void tty_draw_logo_scaled(int32_t dest_x, int32_t dest_y, int32_t dest_width, int32_t dest_height) {
    if (!g_statusbar_logo_loaded || !g_statusbar_logo || !g_statusbar_logo->pixel_data) {
        return;
    }

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return;
    }

    // Bounds checking
    if (dest_x < 0 || dest_y < 0 || dest_width <= 0 || dest_height <= 0) {
        return;
    }
    if (dest_x + dest_width > (int32_t)fb->width || dest_y + dest_height > (int32_t)fb->height) {
        return;
    }

    // Get BMP image properties
    uint32_t src_width = g_statusbar_logo->width;
    uint32_t src_height = g_statusbar_logo->height;
    uint8_t bpp = g_statusbar_logo->bpp;
    uint8_t* pixel_data = g_statusbar_logo->pixel_data;
    bool top_down = g_statusbar_logo->top_down;

    // Calculate bytes per pixel in source image
    uint32_t src_bytes_per_pixel = bpp / 8;
    uint32_t src_row_size = src_width * src_bytes_per_pixel;
    uint32_t src_row_padding = (4 - (src_row_size % 4)) % 4;
    uint32_t src_row_size_with_padding = src_row_size + src_row_padding;

    // Calculate scaling factors
    float scale_x = (float)src_width / (float)dest_width;
    float scale_y = (float)src_height / (float)dest_height;

    // Use nearest-neighbor scaling for pixel art look
    uint32_t bytes_per_pixel = (fb->bpp + 7) / 8;
    volatile uint8_t* framebuffer = (volatile uint8_t*)fb->virtual_addr;

    for (int32_t dy = 0; dy < dest_height; dy++) {
        // Calculate source Y coordinate
        int32_t src_y = (int32_t)((float)dy * scale_y);
        if (src_y >= (int32_t)src_height) src_y = src_height - 1;

        // Handle BMP row ordering (bottom-up by default)
        uint32_t bmp_row = top_down ? src_y : (src_height - 1 - src_y);

        for (int32_t dx = 0; dx < dest_width; dx++) {
            // Calculate source X coordinate
            int32_t src_x = (int32_t)((float)dx * scale_x);
            if (src_x >= (int32_t)src_width) src_x = src_width - 1;

            // Get pixel from source image (BGR or BGRA format)
            uint32_t src_idx = (bmp_row * src_row_size_with_padding) + (src_x * src_bytes_per_pixel);

            uint8_t b = pixel_data[src_idx];
            uint8_t g = pixel_data[src_idx + 1];
            uint8_t r = pixel_data[src_idx + 2];
            uint8_t a = (src_bytes_per_pixel == 4) ? pixel_data[src_idx + 3] : 255;

            // Skip fully transparent pixels
            if (a < 128) {
                continue;
            }

            // Calculate destination position
            int32_t fb_x = dest_x + dx;
            int32_t fb_y = dest_y + dy;

            if (fb_x < 0 || fb_x >= (int32_t)fb->width || fb_y < 0 || fb_y >= (int32_t)fb->height) {
                continue;
            }

            size_t offset = ((uint32_t)fb_y * fb->pitch) + ((uint32_t)fb_x * bytes_per_pixel);

            if (a >= 250) {
                // Fully opaque - just write the pixel
                framebuffer[offset] = b;           // Blue
                framebuffer[offset + 1] = g;       // Green
                framebuffer[offset + 2] = r;        // Red
                if (bytes_per_pixel == 4) {
                    framebuffer[offset + 3] = 255; // Alpha
                }
            } else {
                // Alpha blend with background
                float alpha = (float)a / 255.0f;
                float inv_alpha = 1.0f - alpha;

                uint8_t bg_b = framebuffer[offset];
                uint8_t bg_g = framebuffer[offset + 1];
                uint8_t bg_r = framebuffer[offset + 2];

                framebuffer[offset] = (uint8_t)((float)b * alpha + (float)bg_b * inv_alpha);
                framebuffer[offset + 1] = (uint8_t)((float)g * alpha + (float)bg_g * inv_alpha);
                framebuffer[offset + 2] = (uint8_t)((float)r * alpha + (float)bg_r * inv_alpha);
                if (bytes_per_pixel == 4) {
                    framebuffer[offset + 3] = 255;
                }
            }
        }
    }
}

// Draw a colored dot for CPU core indicator
static void tty_draw_cpu_dot(int32_t x, int32_t y, int32_t radius, graphics_color_t color) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return;
    }

    uint32_t bytes_per_pixel = (fb->bpp + 7) / 8;
    volatile uint8_t* framebuffer = (volatile uint8_t*)fb->virtual_addr;
    int32_t radius_sq = radius * radius;

    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dx = -radius; dx <= radius; dx++) {
            if ((dx * dx + dy * dy) <= radius_sq) {
                int32_t fb_x = x + dx;
                int32_t fb_y = y + dy;
                if (fb_x < 0 || fb_x >= (int32_t)fb->width || fb_y < 0 || fb_y >= (int32_t)fb->height) {
                    continue;
                }
                size_t offset = ((uint32_t)fb_y * fb->pitch) + ((uint32_t)fb_x * bytes_per_pixel);
                framebuffer[offset] = color.b;
                framebuffer[offset + 1] = color.g;
                framebuffer[offset + 2] = color.r;
                if (bytes_per_pixel == 4) {
                    framebuffer[offset + 3] = 255;
                }
            }
        }
    }
}

#define CPU_DOT_RADIUS 5
#define CPU_DOT_SPACING 4
static const graphics_color_t cpu_dot_colors[] = {
    {255, 80, 80, 255},
    {255, 180, 60, 255},
    {255, 230, 60, 255},
    {120, 230, 80, 255},
    {60, 220, 160, 255},
    {60, 210, 255, 255},
    {80, 160, 255, 255},
    {100, 100, 255, 255},
    {160, 80, 255, 255},
    {230, 80, 230, 255},
    {255, 80, 160, 255},
    {255, 100, 100, 255},
    {255, 140, 60, 255},
    {230, 200, 60, 255},
    {80, 200, 80, 255},
    {60, 180, 240, 255},
};

// Display CPU core dots on framebuffer
void tty_display_cpu_dots(void) {
    uint32_t cpu_count = smp_get_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    video_mode_t mode;
    if (graphics_get_current_mode(&mode) != GRAPHICS_SUCCESS) {
        return;
    }

    int32_t dot_diameter = CPU_DOT_RADIUS * 2 + 1;
    int32_t total_width = cpu_count * dot_diameter + (cpu_count - 1) * CPU_DOT_SPACING;
    int32_t start_x = mode.width - total_width - 8;
    int32_t start_y = TTY_STATUS_BAR_HEIGHT / 2;

    for (uint32_t i = 0; i < cpu_count && i < sizeof(cpu_dot_colors)/sizeof(cpu_dot_colors[0]); i++) {
        int32_t cx = start_x + i * (dot_diameter + CPU_DOT_SPACING) + CPU_DOT_RADIUS;
        tty_draw_cpu_dot(cx, start_y, CPU_DOT_RADIUS, cpu_dot_colors[i]);
    }
}

// Clear CPU core dots from framebuffer
static void tty_clear_cpu_dots(void) {
    uint32_t cpu_count = smp_get_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    video_mode_t mode;
    if (graphics_get_current_mode(&mode) != GRAPHICS_SUCCESS) {
        return;
    }

    int32_t dot_diameter = CPU_DOT_RADIUS * 2 + 1;
    int32_t total_width = cpu_count * dot_diameter + (cpu_count - 1) * CPU_DOT_SPACING;
    int32_t start_x = mode.width - total_width - 8;
    int32_t start_y = TTY_STATUS_BAR_HEIGHT / 2;

    for (uint32_t i = 0; i < cpu_count && i < sizeof(cpu_dot_colors)/sizeof(cpu_dot_colors[0]); i++) {
        int32_t cx = start_x + i * (dot_diameter + CPU_DOT_SPACING) + CPU_DOT_RADIUS;
        tty_draw_cpu_dot(cx, start_y, CPU_DOT_RADIUS + 1, (graphics_color_t){0, 0, 0, 255});
    }
}

// Helper to render a string using 8x8 font directly to framebuffer
static void tty_render_string_8x8(int32_t x, int32_t y, const char* str, graphics_color_t fg, graphics_color_t bg) {
    if (!str) return;

    tty_font_t* tty_font = NULL;
    if (tty_font_load_builtin("tty-8x8", 8, &tty_font) != TTY_FONT_SUCCESS || !tty_font) {
        return;
    }

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return;
    }

    bool use_back_buffer = fb->double_buffered && fb->back_buffer != 0;

    graphics_surface_t surface;
    surface.pixels = (void*)(use_back_buffer ? fb->back_buffer : fb->virtual_addr);
    surface.width = fb->width;
    surface.height = fb->height;
    surface.pitch = fb->pitch;
    surface.format = fb->format;
    surface.bpp = fb->bpp;

    int32_t px = x;
    for (const char* p = str; *p; p++) {
        tty_font_render_char(tty_font, &surface, px, y, (uint32_t)*p, fg, bg);
        px += 8;  // 8x8 font width
    }
}

static void tty_draw_gradient_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                   graphics_color_t top, graphics_color_t bot) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) return;
    uint32_t bpp = (fb->bpp + 7) / 8;
    volatile uint8_t* fb_mem = (volatile uint8_t*)fb->virtual_addr;

    for (uint32_t row = 0; row < h; row++) {
        uint8_t r = (uint8_t)(top.r + ((int32_t)(bot.r - top.r) * (int32_t)row / (int32_t)h));
        uint8_t g = (uint8_t)(top.g + ((int32_t)(bot.g - top.g) * (int32_t)row / (int32_t)h));
        uint8_t b = (uint8_t)(top.b + ((int32_t)(bot.b - top.b) * (int32_t)row / (int32_t)h));
        int32_t fb_y = y + row;
        if (fb_y < 0 || fb_y >= (int32_t)fb->height) continue;
        for (uint32_t col = 0; col < w; col++) {
            int32_t fb_x = x + col;
            if (fb_x < 0 || fb_x >= (int32_t)fb->width) continue;
            size_t off = ((uint32_t)fb_y * fb->pitch) + ((uint32_t)fb_x * bpp);
            fb_mem[off]     = b;
            fb_mem[off + 1] = g;
            fb_mem[off + 2] = r;
            if (bpp == 4) fb_mem[off + 3] = 255;
        }
    }
}

void tty_draw_fade_overlay(uint8_t opacity) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) return;
    uint32_t bpp = (fb->bpp + 7) / 8;
    /* When double buffering is active (e.g. mid VT switch), draw into the
     * back buffer like every other renderer (WM's wm_present()) does -
     * writing straight to virtual_addr here would race with/get clobbered
     * by the next compositor present, which only ever reads/writes the
     * back buffer. Swap immediately after so the fade step is still visible
     * frame-by-frame rather than only appearing once something else swaps. */
    bool use_back_buffer = fb->double_buffered && fb->back_buffer != 0;
    volatile uint8_t* fb_mem = (volatile uint8_t*)(use_back_buffer ? fb->back_buffer : fb->virtual_addr);
    float alpha = (float)opacity / 255.0f;
    float inv = 1.0f - alpha;

    for (uint32_t y = 0; y < fb->height; y++) {
        volatile uint8_t* row = fb_mem + y * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            size_t off = x * bpp;
            row[off]     = (uint8_t)((float)row[off] * inv);
            row[off + 1] = (uint8_t)((float)row[off + 1] * inv);
            row[off + 2] = (uint8_t)((float)row[off + 2] * inv);
            if (bpp == 4) row[off + 3] = 255;
        }
    }

    if (use_back_buffer) {
        graphics_swap_buffers();
    }
}

static void tty_read_time_string(char* buf, size_t len) {
    cmos_time_t t;
    if (cmos_read_time(&t) != 0 || len < 6) {
        buf[0] = '\0';
        return;
    }
    uint8_t h = t.hours;
    uint8_t m = t.minutes;
    bool pm = false;
    if (!t.hour_24_mode) {
        pm = (h >= 12);
        if (h == 0) h = 12;
        else if (h > 12) h -= 12;
    }
    if (t.hour_24_mode) {
        snprintf(buf, len, "%02d:%02d", h, m);
    } else {
        snprintf(buf, len, "%02d:%02d%s", h, m, pm ? "PM" : "AM");
    }
}

static void tty_query_keyboard_modifiers(void) {
    keyboard_modifier_state_t state;
    if (keyboard_get_modifier_state(&state) == KEYBOARD_SUCCESS) {
        g_kbd_ctrl = state.ctrl_pressed;
        g_kbd_alt = state.alt_pressed;
        g_kbd_shift = state.shift_pressed;
    }
}

static void tty_draw_modifier_indicators(int32_t x, int32_t y) {
    tty_query_keyboard_modifiers();

    graphics_color_t dim = {80, 80, 90, 255};
    graphics_color_t active_ctrl = {255, 100, 100, 255};
    graphics_color_t active_alt = {100, 200, 255, 255};
    graphics_color_t active_shift = {100, 255, 140, 255};
    graphics_color_t status_bg = {20, 20, 25, 255};

    tty_render_string_8x8(x, y, "C", g_kbd_ctrl ? active_ctrl : dim, status_bg);
    tty_render_string_8x8(x + 10, y, "A", g_kbd_alt ? active_alt : dim, status_bg);
    tty_render_string_8x8(x + 20, y, "S", g_kbd_shift ? active_shift : dim, status_bg);
}

void tty_draw_scroll_indicator(void) {
    uint16_t rows = 0, char_height = 0, cursor_y = 0;
    if (!tty_internal_get_scroll_state(&rows, &char_height, &cursor_y)) return;
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) return;

    uint32_t bpp = (fb->bpp + 7) / 8;
    volatile uint8_t* fb_mem = (volatile uint8_t*)fb->virtual_addr;

    int32_t bar_x = (int32_t)fb->width - 3;
    int32_t bar_top = TTY_STATUS_BAR_HEIGHT;
    int32_t bar_h = (int32_t)rows * (int32_t)char_height;
    if (bar_h <= 0) return;

    int32_t thumb_h = bar_h / 4;
    if (thumb_h < 4) thumb_h = 4;
    float cursor_ratio = (rows > 1) ? (float)cursor_y / (float)(rows - 1) : 0.0f;
    int32_t thumb_y = bar_top + (int32_t)(cursor_ratio * (float)(bar_h - thumb_h));

    graphics_color_t dim = {40, 40, 48, 255};
    graphics_color_t thumb = {80, 80, 100, 255};

    for (int32_t row = 0; row < bar_h; row++) {
        int32_t y = bar_top + row;
        if (y < 0 || y >= (int32_t)fb->height) continue;
        bool in_thumb = (row >= thumb_y && row < thumb_y + thumb_h);
        graphics_color_t c = in_thumb ? thumb : dim;
        for (int32_t dx = 0; dx < 3; dx++) {
            int32_t x = bar_x + dx;
            if (x < 0 || x >= (int32_t)fb->width) continue;
            size_t off = ((uint32_t)y * fb->pitch) + ((uint32_t)x * bpp);
            fb_mem[off]     = c.b;
            fb_mem[off + 1] = c.g;
            fb_mem[off + 2] = c.r;
            if (bpp == 4) fb_mem[off + 3] = 255;
        }
    }
}

void tty_draw_status_bar(void) {
    if (!tty_runtime_mutation_allowed() || !status_bar_visible || tty_is_graphics_app_active()) {
        return;
    }

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return;
    }

    graphics_color_t top = {TTY_STATUS_BAR_GRADIENT_TOP_R, TTY_STATUS_BAR_GRADIENT_TOP_G, TTY_STATUS_BAR_GRADIENT_TOP_B, 255};
    graphics_color_t bot = {TTY_STATUS_BAR_GRADIENT_BOT_R, TTY_STATUS_BAR_GRADIENT_BOT_G, TTY_STATUS_BAR_GRADIENT_BOT_B, 255};
    tty_draw_gradient_rect(0, 0, fb->width, TTY_STATUS_BAR_HEIGHT - 2, top, bot);

    graphics_color_t accent = {TTY_ACCENT_R, TTY_ACCENT_G, TTY_ACCENT_B, 255};
    graphics_rect_t bottom_line = {0, TTY_STATUS_BAR_HEIGHT - 2, fb->width, 2};
    graphics_draw_rect(&bottom_line, accent, true);

    uint8_t vt_num = tty_get_current_vt();
    char vt_label[12];
    snprintf(vt_label, sizeof(vt_label), "TTY %d", vt_num);
    graphics_color_t vt_color = {TTY_ACCENT_R, TTY_ACCENT_G, TTY_ACCENT_B, 255};
    graphics_color_t label_bg = top;
    tty_render_string_8x8(6, 7, vt_label, vt_color, label_bg);

    int32_t after_vt_x = 6 + (int32_t)(strlen(vt_label) * 8) + 4;

    if (!g_statusbar_logo_loaded) {
        tty_load_statusbar_logo();
    }

    if (g_statusbar_logo_loaded && g_statusbar_logo && g_statusbar_logo->pixel_data) {
        int32_t max_logo_height = TTY_STATUS_BAR_HEIGHT - 6;
        int32_t max_logo_width = 80;
        float aspect_ratio = (float)g_statusbar_logo->width / (float)g_statusbar_logo->height;
        int32_t logo_width = max_logo_height * aspect_ratio;
        int32_t logo_height = max_logo_height;
        if (logo_width > max_logo_width) {
            logo_width = max_logo_width;
            logo_height = (int32_t)(max_logo_width / aspect_ratio);
        }
        int32_t logo_x = after_vt_x + 4;
        int32_t logo_y = (TTY_STATUS_BAR_HEIGHT - logo_height) / 2;
        tty_draw_logo_scaled(logo_x, logo_y, logo_width, logo_height);
        after_vt_x = logo_x + logo_width + 8;
    } else {
        tty_render_string_8x8(after_vt_x + 4, 7, "Fern", (graphics_color_t){200, 200, 205, 255}, label_bg);
        after_vt_x += 4 + 9 * 8 + 8;
    }

    const char* status_msg = g_status_text[0] ? g_status_text : (g_user_logged_in ? g_current_user : g_login_status);
    uint32_t cpu_count = smp_get_cpu_count();
    if (cpu_count == 0) cpu_count = 1;
    int32_t dot_diameter = CPU_DOT_RADIUS * 2 + 1;
    int32_t dots_total = cpu_count * dot_diameter + (cpu_count - 1) * CPU_DOT_SPACING;
    int32_t dots_right = fb->width - dots_total - 12;
    int32_t status_area_end = dots_right - 20;

    if (after_vt_x + 100 < status_area_end) {
        tty_render_string_8x8(after_vt_x, 7, status_msg, (graphics_color_t){180, 180, 185, 255}, label_bg);
    }

    int32_t mod_x = status_area_end - 34;
    if (mod_x > after_vt_x + 50) {
        tty_draw_modifier_indicators(mod_x, 7);
    }

    tty_display_cpu_dots();

    char time_str[12];
    tty_read_time_string(time_str, sizeof(time_str));
    if (time_str[0]) {
        int32_t time_x = dots_right - (int32_t)(strlen(time_str) * 8) - 10;
        if (time_x > mod_x + 40) {
            tty_render_string_8x8(time_x, 7, time_str, (graphics_color_t){160, 165, 175, 255}, label_bg);
        }
    }

    status_bar_drawn = true;
}

void tty_clear_status_bar(void) {
    if (!tty_runtime_mutation_allowed()) return;
    if (!status_bar_drawn) return;

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return;
    }

    // Clear status bar area
    graphics_color_t black = {0, 0, 0, 255};
    graphics_rect_t clear_rect = {0, 0, fb->width, TTY_STATUS_BAR_HEIGHT};
    graphics_draw_rect(&clear_rect, black, true);

    status_bar_drawn = false;
}

void tty_set_status_bar_visible(bool visible) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    /* No transition: nothing to do. */
    if (status_bar_visible == visible) {
        return;
    }
    status_bar_visible = visible;
    if (!visible) {
        /* Hiding: clear the reserved header area so it does not leave stale
         * pixels behind, then redraw the content region below it. */
        tty_clear_status_bar();
        if (tty_is_ready()) {
            tty_force_redraw();
        }
    } else {
        /* Showing: redraw content first (it is always offset below the
         * header) then paint the header on top of the reserved area so the
         * header never corrupts content cells. */
        if (tty_is_ready()) {
            tty_force_redraw();
            tty_draw_status_bar();
        }
    }
}

bool tty_is_status_bar_visible(void) {
    return status_bar_visible;
}

void tty_set_status_bar_enabled(bool enabled) {
    tty_set_status_bar_visible(enabled);
    if (enabled && tty_is_ready()) {
        tty_draw_status_bar();
    }
}

bool tty_is_status_bar_enabled(void) {
    return status_bar_visible;
}

void tty_set_status_bar_status_text(const char* text) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    const char* src = text ? text : "";
    strncpy(g_status_text, src, sizeof(g_status_text) - 1);
    g_status_text[sizeof(g_status_text) - 1] = '\0';
    if (status_bar_visible && tty_is_ready()) {
        tty_draw_status_bar();
    }
}

void tty_set_status_bar_user_logged_in(bool logged_in) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    g_user_logged_in = logged_in;
    if (!logged_in) {
        g_current_user[0] = '\0';
    }
    if (status_bar_visible && tty_is_ready()) {
        tty_draw_status_bar();
    }
}

void tty_update_status_bar_data(const char* login_text,
                                const char* user_text,
                                const char* status_text,
                                bool user_logged_in) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }

    tty_set_login_status_text(login_text);
    tty_set_current_user_text(user_text);
    tty_set_status_bar_status_text(status_text);
    tty_set_status_bar_user_logged_in(user_logged_in);
}

// Compatibility aliases expected by session/login code paths (weak-extern
// resolved from src/session.c). Only the aliases for the status-text and
// logged-in setters live here, since those setters moved into this file;
// the login-status/current-user text aliases still call setters that stay
// in tty.c and are defined there instead.
void tty_status_set_text(const char* text) { tty_set_status_bar_status_text(text); }
void tty_set_status_text(const char* text) { tty_set_status_bar_status_text(text); }
void tty_status_set_logged_in(bool logged_in) { tty_set_status_bar_user_logged_in(logged_in); }
void tty_set_status_logged_in(bool logged_in) { tty_set_status_bar_user_logged_in(logged_in); }

// Show a brief transition message on the framebuffer during VT switch
void tty_draw_transition_screen(const char* label) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || fb->size == 0) return;

    uint32_t pitch = fb->pitch;
    uint32_t width = fb->width;
    uint32_t height = fb->height;
    uint32_t bpp = fb->bpp;
    uint32_t bytes_pp = bpp / 8;
    /* Same rationale as tty_draw_fade_overlay(): if double buffering is
     * active, target the back buffer so this doesn't race with / get
     * clobbered by the compositor, and swap once at the end to display it. */
    bool use_back_buffer = fb->double_buffered && fb->back_buffer != 0;
    uintptr_t target = use_back_buffer ? fb->back_buffer : fb->virtual_addr;

    // Clear to dark background
    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row = (uint8_t*)target + y * pitch;
        for (uint32_t x = 0; x < width * bytes_pp; x++) {
            row[x] = (bytes_pp == 4) ? 0x1a : 0x00;
        }
    }

    // Draw a simple colored bar at top
    uint32_t bar_height = height / 32;
    if (bar_height < 2) bar_height = 2;
    for (uint32_t y = 0; y < bar_height; y++) {
        uint8_t* row = (uint8_t*)target + y * pitch;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t off = x * bytes_pp;
            if (bytes_pp == 4) {
                row[off + 0] = 0x40;  // B
                row[off + 1] = 0xa0;  // G
                row[off + 2] = 0x20;  // R
                row[off + 3] = 0xff;  // A
            } else if (bytes_pp == 3) {
                row[off + 0] = 0x40;
                row[off + 1] = 0xa0;
                row[off + 2] = 0x20;
            }
        }
    }

    // Render the transition label centered beneath the colored bar
    if (label && label[0] != '\0') {
        int32_t text_width = (int32_t)(strlen(label) * 8);
        int32_t text_x = ((int32_t)width - text_width) / 2;
        if (text_x < 0) text_x = 0;
        int32_t text_y = (int32_t)bar_height + 16;
        graphics_color_t fg = {220, 220, 225, 255};
        graphics_color_t bg = {0x1a, 0x1a, 0x1a, 255};
        tty_render_string_8x8(text_x, text_y, label, fg, bg);
    }

    __asm__ volatile("mfence" ::: "memory");

    if (use_back_buffer) {
        graphics_swap_buffers();
    }
}
