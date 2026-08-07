#include "../include/graphics/window_manager.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/font_renderer.h"
#include "../include/graphics/enhanced_cursor.h"
#include "../include/graphics/optimized_blit.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/mm.h"
#include "../include/framebuffer.h"
#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics_init.h"
#include "../include/timer.h"
#include "../include/task.h"
#include "../include/gfx_config.h"

#if HAS_GRAPHICS

#define WM_WATCHDOG_TIMEOUT_TICKS  1000
#define WM_HEARTBEAT_TIMEOUT_TICKS 50

typedef enum {
    WM_RENDER_STATE_NORMAL = 0,
    WM_RENDER_STATE_DEFERRED,
    WM_RENDER_STATE_FALLBACK,
    WM_RENDER_STATE_RECOVERY
} wm_render_state_t;

static struct {
    bool initialized;
    bool wm_ready;
    window_t* window_list;
    window_handle_t next_handle;
    window_handle_t focused_window;
    uint32_t window_count;

    window_t* z_order_array[WM_MAX_WINDOWS];
    uint32_t z_order_count;

    uint32_t desktop_width;
    uint32_t desktop_height;
    graphics_surface_t* desktop_surface;
    graphics_surface_t* wallpaper;

    graphics_surface_t* composition_buffer;
    bool needs_redraw;
    bool comp_buf_separate;
    uint32_t comp_buf_size;

    int32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    bool has_dirty_rect;

    int32_t prev_cursor_x, prev_cursor_y;

    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;

    bool mouse_left_down;
    bool is_dragging;
    window_t* drag_window;
    int32_t drag_offset_x;
    int32_t drag_offset_y;

    bool is_resizing;
    window_t* resize_window;
    uint32_t resize_edge;
    int32_t resize_origin_x;
    int32_t resize_origin_y;
    uint32_t resize_origin_w;
    uint32_t resize_origin_h;
    int32_t resize_start_win_x;
    int32_t resize_start_win_y;

    bool show_snap_preview;
    uint32_t snap_edge;
    int32_t snap_preview_x;
    int32_t snap_preview_y;
    uint32_t snap_preview_w;
    uint32_t snap_preview_h;

    window_manager_config_t config;

    wm_render_state_t render_state;
    uint32_t userspace_map_tick;
    uint32_t last_userspace_flush_tick;
    uint32_t last_kernel_fallback_tick;
    uint32_t last_frame_render_tick;
    bool watchdog_triggered;
} wm_state = {
    .initialized = false,
    .wm_ready = false,
    .window_list = NULL,
    .next_handle = 1,
    .focused_window = INVALID_WINDOW_HANDLE,
    .window_count = 0,
    .z_order_count = 0,
    .desktop_surface = NULL,
    .wallpaper = NULL,
    .composition_buffer = NULL,
    .needs_redraw = true,
    .comp_buf_separate = false,
    .dirty_x0 = 0,
    .dirty_y0 = 0,
    .dirty_x1 = 0,
    .dirty_y1 = 0,
    .has_dirty_rect = false,
    .prev_cursor_x = 0,
    .prev_cursor_y = 0,
    .cursor_x = 0,
    .cursor_y = 0,
    .cursor_visible = false,
    .mouse_left_down = false,
    .is_dragging = false,
    .drag_window = NULL,
    .is_resizing = false,
    .resize_window = NULL,
    .show_snap_preview = false,
    .render_state = WM_RENDER_STATE_NORMAL,
    .userspace_map_tick = 0,
    .last_userspace_flush_tick = 0,
    .last_kernel_fallback_tick = 0,
    .last_frame_render_tick = 0,
    .watchdog_triggered = false
};

static const window_manager_config_t default_config = {
    .title_bar_height = 24,
    .border_width = 2,
    .title_bar_color = {64, 64, 64, 255},
    .border_color = {128, 128, 128, 255},
    .title_text_color = {255, 255, 255, 255},
    .enable_shadows = true,
    .enable_animations = false,
    .animation_duration_ms = 250,
    .snap_threshold = WM_SNAP_THRESHOLD,
    .shadow_offset = WM_SHADOW_OFFSET,
    .shadow_alpha = WM_SHADOW_ALPHA,
    .corner_radius = WM_CORNER_RADIUS
};

static graphics_result_t create_window_surface(window_t* window);
static graphics_result_t destroy_window_surface(window_t* window);
static graphics_result_t draw_window_decorations(window_t* window);
static graphics_result_t composite_windows(void);
static graphics_result_t wm_present(void);
static void wm_draw_cursor_on_fb(framebuffer_t* fb);
static inline void wm_surface_put_pixel(graphics_surface_t* s, uint32_t x, uint32_t y, uint32_t pixel);
static window_t* find_window_by_handle(window_handle_t handle);
static graphics_result_t remove_window_from_list(window_t* window);
static graphics_result_t add_window_to_list(window_t* window);
static void wm_draw_fallback_screen(framebuffer_t* fb);

static void wm_z_order_push(window_t* win);
static void wm_z_order_remove(window_t* win);
static void wm_z_order_to_front(window_t* win);
static void wm_z_order_to_back(window_t* win);
static void wm_z_order_sync(void);

static inline bool wm_is_on_title_bar(window_t* win, int32_t mx, int32_t my);
static uint32_t wm_get_resize_edge(window_t* win, int32_t mx, int32_t my);

static inline uint32_t wm_alpha_blend_pixel(uint32_t dst, uint32_t src_r, uint32_t src_g, uint32_t src_b, uint32_t src_a);
static void wm_fill_rounded_rect(graphics_surface_t* s, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t pixel);
static void wm_draw_shadow(graphics_surface_t* comp, window_t* win);
static void wm_draw_snap_preview(graphics_surface_t* comp);

static const uint8_t wm_cursor_alpha[19][11] = {
    {255,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    {255,255,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    {255,255,255, 80,  0,  0,  0,  0,  0,  0,  0},
    {255,255,255,255, 80,  0,  0,  0,  0,  0,  0},
    {255,255,255,255,255, 80,  0,  0,  0,  0,  0},
    {255,255,255,255,255,255, 80,  0,  0,  0,  0},
    {255,255,255,255,255,255,255, 80,  0,  0,  0},
    {255,255,255,255,255,255,255,255, 80,  0,  0},
    {255,255,255,255,255,255,255,255,255, 80,  0},
    {255,255,255,255,255,255,255,255,255,255, 80},
    {255,255,255,255,255,255,255,255,255,255,255},
    {255,255,255,255,255,255,255,160,120, 80, 60},
    {255,255,255,255,160,255,255,200,  0,  0,  0},
    {255,255,255,160, 80,200,255,255,120,  0,  0},
    {255,255,160, 80,  0,160,255,255,120,  0,  0},
    {255,200, 80,  0,  0,160,255,255,255,120,  0},
    {255, 80,  0,  0,  0, 80,200,255,255,120,  0},
    { 80,  0,  0,  0,  0,  0,160,255,255,200,  0},
    {  0,  0,  0,  0,  0,  0, 80,200,255,160,  0},
};

static const uint8_t wm_cursor_is_outline[19][11] = {
    {1,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0},
    {1,0,1,0,0,0,0,0,0,0,0},
    {1,0,0,1,0,0,0,0,0,0,0},
    {1,0,0,0,1,0,0,0,0,0,0},
    {1,0,0,0,0,1,0,0,0,0,0},
    {1,0,0,0,0,0,1,0,0,0,0},
    {1,0,0,0,0,0,0,1,0,0,0},
    {1,0,0,0,0,0,0,0,1,0,0},
    {1,0,0,0,0,0,0,0,0,1,0},
    {1,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,1,0,0,0},
    {1,0,0,0,1,0,0,1,0,0,0},
    {1,0,0,1,0,0,0,0,1,0,0},
    {1,0,1,0,0,0,0,0,1,0,0},
    {1,1,0,0,0,0,0,0,0,1,0},
    {1,0,0,0,0,0,0,0,0,1,0},
    {0,0,0,0,0,0,0,1,0,1,0},
    {0,0,0,0,0,0,0,1,1,1,0},
};

static inline void wm_surface_put_pixel(graphics_surface_t* s, uint32_t x, uint32_t y, uint32_t pixel) {
    if (!s || !s->pixels) {
        return;
    }
    if (x >= s->width || y >= s->height) {
        return;
    }

    uint32_t bpp = (s->bpp + 7) / 8;
    if (bpp == 0) {
        bpp = 4;
    }

    uint8_t* addr = (uint8_t*)s->pixels + y * s->pitch + x * bpp;
    switch (bpp) {
        case 4:
            *(uint32_t*)addr = pixel;
            break;
        case 3:
            addr[0] = (uint8_t)(pixel & 0xFF);
            addr[1] = (uint8_t)((pixel >> 8) & 0xFF);
            addr[2] = (uint8_t)((pixel >> 16) & 0xFF);
            break;
        case 2:
            *(uint16_t*)addr = (uint16_t)(pixel & 0xFFFF);
            break;
        case 1:
            *addr = (uint8_t)(pixel & 0xFF);
            break;
        default:
            break;
    }
}

static inline uint32_t wm_get_surface_pixel(graphics_surface_t* s, uint32_t x, uint32_t y) {
    if (!s || !s->pixels || x >= s->width || y >= s->height) return 0;
    uint32_t bpp = (s->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint8_t* addr = (uint8_t*)s->pixels + y * s->pitch + x * bpp;
    switch (bpp) {
        case 4: return *(uint32_t*)addr;
        case 3: return addr[0] | ((uint32_t)addr[1] << 8) | ((uint32_t)addr[2] << 16);
        case 2: return *(uint16_t*)addr;
        case 1: return *addr;
        default: return 0;
    }
}

static inline uint32_t wm_alpha_blend_pixel(uint32_t dst, uint32_t src_r, uint32_t src_g, uint32_t src_b, uint32_t src_a) {
    if (src_a == 0) return dst;
    if (src_a == 255) {
        return src_b | (src_g << 8) | (src_r << 16) | 0xFF000000u;
    }
    uint32_t dst_b = dst & 0xFF;
    uint32_t dst_g = (dst >> 8) & 0xFF;
    uint32_t dst_r = (dst >> 16) & 0xFF;
    uint32_t inv = 255 - src_a;
    uint32_t r = (src_r * src_a + dst_r * inv) / 255;
    uint32_t g = (src_g * src_a + dst_g * inv) / 255;
    uint32_t b = (src_b * src_a + dst_b * inv) / 255;
    return b | (g << 8) | (r << 16) | 0xFF000000u;
}

static inline void wm_surface_blend_pixel(graphics_surface_t* s, uint32_t x, uint32_t y,
                                           uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (!s || !s->pixels || x >= s->width || y >= s->height || a == 0) return;
    if (a == 255) {
        wm_surface_put_pixel(s, x, y, b | (g << 8) | (r << 16) | 0xFF000000u);
        return;
    }
    uint32_t bpp = (s->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint8_t* addr = (uint8_t*)s->pixels + y * s->pitch + x * bpp;
    uint32_t dst = 0;
    switch (bpp) {
        case 4: dst = *(uint32_t*)addr; break;
        case 3: dst = addr[0] | ((uint32_t)addr[1] << 8) | ((uint32_t)addr[2] << 16); break;
        case 2: dst = *(uint16_t*)addr; break;
        case 1: dst = *addr; break;
        default: return;
    }
    uint32_t blended = wm_alpha_blend_pixel(dst, r, g, b, a);
    switch (bpp) {
        case 4: *(uint32_t*)addr = blended; break;
        case 3: addr[0] = blended & 0xFF; addr[1] = (blended >> 8) & 0xFF; addr[2] = (blended >> 16) & 0xFF; break;
        case 2: *(uint16_t*)addr = (uint16_t)(blended & 0xFFFF); break;
        case 1: *addr = (uint8_t)(blended & 0xFF); break;
    }
}

static void wm_fill_rounded_rect(graphics_surface_t* s, int32_t rx, int32_t ry,
                                  uint32_t rw, uint32_t rh, uint32_t radius, uint32_t pixel) {
    if (!s || !s->pixels || radius == 0) {
        for (uint32_t py = 0; py < rh; py++) {
            for (uint32_t px = 0; px < rw; px++) {
                wm_surface_put_pixel(s, rx + px, ry + py, pixel);
            }
        }
        return;
    }
    uint32_t r2 = radius * radius;
    for (uint32_t py = 0; py < rh; py++) {
        for (uint32_t px = 0; px < rw; px++) {
            bool inside = true;
            if (px < radius && py < radius) {
                uint32_t dx = radius - px;
                uint32_t dy = radius - py;
                if (dx * dx + dy * dy > r2) inside = false;
            } else if (px >= rw - radius && py < radius) {
                uint32_t dx = px - (rw - radius - 1);
                uint32_t dy = radius - py;
                if (dx * dx + dy * dy > r2) inside = false;
            } else if (px < radius && py >= rh - radius) {
                uint32_t dx = radius - px;
                uint32_t dy = py - (rh - radius - 1);
                if (dx * dx + dy * dy > r2) inside = false;
            } else if (px >= rw - radius && py >= rh - radius) {
                uint32_t dx = px - (rw - radius - 1);
                uint32_t dy = py - (rh - radius - 1);
                if (dx * dx + dy * dy > r2) inside = false;
            }
            if (inside) {
                wm_surface_put_pixel(s, rx + px, ry + py, pixel);
            }
        }
    }
}

static void wm_draw_shadow(graphics_surface_t* comp, window_t* win) {
    if (!comp || !comp->pixels || !win || !win->visible) return;
    if (win->state == WINDOW_STATE_MINIMIZED) return;

    uint32_t off = wm_state.config.shadow_offset;
    if (off == 0) return;

    uint32_t comp_bpp = (comp->bpp + 7) / 8;
    if (comp_bpp == 0) comp_bpp = 4;
    uint8_t* comp_pixels = (uint8_t*)comp->pixels;
    uint8_t sa = wm_state.config.shadow_alpha;

    int32_t sx = win->x - (int32_t)off;
    int32_t sy = win->y - (int32_t)off;
    uint32_t sw = win->width + off * 2;
    uint32_t sh = win->height + off * 2;

    for (uint32_t py = 0; py < sh; py++) {
        int32_t dy = sy + (int32_t)py;
        if (dy < 0 || (uint32_t)dy >= comp->height) continue;
        for (uint32_t px = 0; px < sw; px++) {
            int32_t dx = sx + (int32_t)px;
            if (dx < 0 || (uint32_t)dx >= comp->width) continue;
            bool in_win = (dx >= win->x && dx < win->x + (int32_t)win->width &&
                           dy >= win->y && dy < win->y + (int32_t)win->height);
            if (in_win) continue;
            uint8_t* dp = comp_pixels + (uint32_t)dy * comp->pitch + (uint32_t)dx * comp_bpp;
            uint32_t existing = 0;
            switch (comp_bpp) {
                case 4: existing = *(uint32_t*)dp; break;
                case 3: existing = dp[0] | ((uint32_t)dp[1] << 8) | ((uint32_t)dp[2] << 16); break;
                case 2: existing = *(uint16_t*)dp; break;
                case 1: existing = *dp; break;
            }
            uint32_t blended = wm_alpha_blend_pixel(existing, 0, 0, 0, sa);
            switch (comp_bpp) {
                case 4: *(uint32_t*)dp = blended; break;
                case 3: dp[0] = blended & 0xFF; dp[1] = (blended >> 8) & 0xFF; dp[2] = (blended >> 16) & 0xFF; break;
                case 2: *(uint16_t*)dp = (uint16_t)(blended & 0xFFFF); break;
                case 1: *dp = (uint8_t)(blended & 0xFF); break;
            }
        }
    }
}

static void wm_draw_snap_preview(graphics_surface_t* comp) {
    if (!comp || !comp->pixels || !wm_state.show_snap_preview) return;

    uint32_t comp_bpp = (comp->bpp + 7) / 8;
    if (comp_bpp == 0) comp_bpp = 4;
    uint8_t* comp_pixels = (uint8_t*)comp->pixels;
    uint8_t sa = 100;

    int32_t px0 = wm_state.snap_preview_x;
    int32_t py0 = wm_state.snap_preview_y;
    uint32_t pw = wm_state.snap_preview_w;
    uint32_t ph = wm_state.snap_preview_h;

    for (uint32_t py = 0; py < ph; py++) {
        int32_t dy = py0 + (int32_t)py;
        if (dy < 0 || (uint32_t)dy >= comp->height) continue;
        for (uint32_t px = 0; px < pw; px++) {
            int32_t dx = px0 + (int32_t)px;
            if (dx < 0 || (uint32_t)dx >= comp->width) continue;
            bool on_border = (py < 2 || py >= ph - 2 || px < 2 || px >= pw - 2);
            uint8_t* dp = comp_pixels + (uint32_t)dy * comp->pitch + (uint32_t)dx * comp_bpp;
            uint32_t existing = 0;
            switch (comp_bpp) {
                case 4: existing = *(uint32_t*)dp; break;
                case 3: existing = dp[0] | ((uint32_t)dp[1] << 8) | ((uint32_t)dp[2] << 16); break;
                case 2: existing = *(uint16_t*)dp; break;
                case 1: existing = *dp; break;
            }
            uint32_t blended;
            if (on_border) {
                blended = wm_alpha_blend_pixel(existing, 100, 149, 237, 200);
            } else {
                blended = wm_alpha_blend_pixel(existing, 100, 149, 237, sa);
            }
            switch (comp_bpp) {
                case 4: *(uint32_t*)dp = blended; break;
                case 3: dp[0] = blended & 0xFF; dp[1] = (blended >> 8) & 0xFF; dp[2] = (blended >> 16) & 0xFF; break;
                case 2: *(uint16_t*)dp = (uint16_t)(blended & 0xFFFF); break;
                case 1: *dp = (uint8_t)(blended & 0xFF); break;
            }
        }
    }
}

static inline bool wm_is_on_title_bar(window_t* win, int32_t mx, int32_t my) {
    if (!win) return false;
    return (mx >= win->x && mx < win->x + (int32_t)win->width &&
            my >= win->y && my < win->y + (int32_t)wm_state.config.title_bar_height);
}

static uint32_t wm_get_resize_edge(window_t* win, int32_t mx, int32_t my) {
    if (!win) return RESIZE_EDGE_NONE;
    if (!(win->flags & WINDOW_FLAG_RESIZABLE)) return RESIZE_EDGE_NONE;

    uint32_t edge = RESIZE_EDGE_NONE;
    int32_t bw = (int32_t)wm_state.config.border_width + 4;

    if (mx < win->x + bw) edge |= RESIZE_EDGE_LEFT;
    else if (mx >= win->x + (int32_t)win->width - bw) edge |= RESIZE_EDGE_RIGHT;

    if (my < win->y + bw) edge |= RESIZE_EDGE_TOP;
    else if (my >= win->y + (int32_t)win->height - bw) edge |= RESIZE_EDGE_BOTTOM;

    return edge;
}

static inline uint32_t wm_cursor_shape_at(int32_t row, int32_t col) {
    if (row < 0 || row >= 19 || col < 0 || col >= 11) return 0;
    return wm_cursor_is_outline[row][col];
}

static void wm_z_order_push(window_t* win) {
    if (!win || wm_state.z_order_count >= WM_MAX_WINDOWS) return;
    for (uint32_t i = wm_state.z_order_count; i > 0; i--) {
        wm_state.z_order_array[i] = wm_state.z_order_array[i - 1];
    }
    wm_state.z_order_array[0] = win;
    wm_state.z_order_count++;
    for (uint32_t i = 0; i < wm_state.z_order_count; i++) {
        wm_state.z_order_array[i]->z_order = (int32_t)i;
    }
}

static void wm_z_order_remove(window_t* win) {
    if (!win) return;
    for (uint32_t i = 0; i < wm_state.z_order_count; i++) {
        if (wm_state.z_order_array[i] == win) {
            for (uint32_t j = i; j < wm_state.z_order_count - 1; j++) {
                wm_state.z_order_array[j] = wm_state.z_order_array[j + 1];
            }
            wm_state.z_order_count--;
            for (uint32_t k = 0; k < wm_state.z_order_count; k++) {
                wm_state.z_order_array[k]->z_order = (int32_t)k;
            }
            return;
        }
    }
}

static void wm_z_order_to_front(window_t* win) {
    wm_z_order_remove(win);
    wm_z_order_push(win);
}

static void wm_z_order_to_back(window_t* win) {
    wm_z_order_remove(win);
    if (wm_state.z_order_count < WM_MAX_WINDOWS) {
        wm_state.z_order_array[wm_state.z_order_count++] = win;
    }
    for (uint32_t i = 0; i < wm_state.z_order_count; i++) {
        wm_state.z_order_array[i]->z_order = (int32_t)i;
    }
}

__attribute__((unused))
static void wm_z_order_sync(void) {
    wm_state.z_order_count = 0;
    int32_t max_z = -1;
    window_t* cur = wm_state.window_list;
    while (cur) {
        if (cur->z_order > max_z) max_z = cur->z_order;
        cur = cur->next;
    }
    for (int32_t z = 0; z <= max_z; z++) {
        cur = wm_state.window_list;
        while (cur) {
            if (cur->z_order == z && wm_state.z_order_count < WM_MAX_WINDOWS) {
                wm_state.z_order_array[wm_state.z_order_count++] = cur;
                break;
            }
            cur = cur->next;
        }
    }
    for (uint32_t i = 0; i < wm_state.z_order_count; i++) {
        wm_state.z_order_array[i]->z_order = (int32_t)i;
    }
}

static void wm_mark_dirty_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    if (!wm_state.has_dirty_rect) {
        wm_state.dirty_x0 = x;
        wm_state.dirty_y0 = y;
        wm_state.dirty_x1 = x + w;
        wm_state.dirty_y1 = y + h;
        wm_state.has_dirty_rect = true;
    } else {
        if (x < wm_state.dirty_x0) wm_state.dirty_x0 = x;
        if (y < wm_state.dirty_y0) wm_state.dirty_y0 = y;
        if (x + w > wm_state.dirty_x1) wm_state.dirty_x1 = x + w;
        if (y + h > wm_state.dirty_y1) wm_state.dirty_y1 = y + h;
    }
}

static void wm_mark_full_screen_dirty(void) {
    wm_state.dirty_x0 = 0;
    wm_state.dirty_y0 = 0;
    wm_state.dirty_x1 = (int32_t)wm_state.desktop_width;
    wm_state.dirty_y1 = (int32_t)wm_state.desktop_height;
    wm_state.has_dirty_rect = true;
}

static inline void wm_reset_dirty_rect(void) {
    wm_state.has_dirty_rect = false;
}

static void desktop_draw_gradient(graphics_surface_t* surface) {
    if (!surface || !surface->pixels) return;
    uint32_t w = surface->width;
    uint32_t h = surface->height;
    uint32_t bpp = (surface->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = (uint8_t*)surface->pixels + y * surface->pitch;
        uint32_t ratio = (y * 256) / (h ? h : 1);
        /* Visible blue-teal gradient: top=0x3366AA, bottom=0x1A4477 */
        uint8_t r = (uint8_t)(51  - (ratio * 20) / 256);
        uint8_t g = (uint8_t)(102 - (ratio * 36) / 256);
        uint8_t b = (uint8_t)(170 - (ratio * 51) / 256);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* addr = row + x * bpp;
            if (bpp == 4) {
                uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                *(uint32_t*)addr = color;
            } else if (bpp == 3) {
                /* BGR order for PIXEL_FORMAT_BGR_888 */
                addr[0] = b;
                addr[1] = g;
                addr[2] = r;
            } else if (bpp == 2) {
                uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                *(uint16_t*)addr = c;
            } else {
                *addr = (uint8_t)(((r + g + b) / 3) & 0xFF);
            }
        }
    }
}

static void wm_paint_fallback_background(void) {
    gfx_framebuffer_t* fb = NULL;
    if (gfx_get_framebuffer(&fb) != GFX_OK || !fb || !fb->virt_addr) return;
    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t* row = (uint8_t*)fb->virt_addr + y * fb->pitch;
        uint32_t ratio = (y * 256) / (fb->height ? fb->height : 1);
        uint8_t r = (uint8_t)(10 + (ratio * 5) / 256);
        uint8_t g = (uint8_t)(40 + (ratio * 20) / 256);
        uint8_t b = (uint8_t)(20 + (ratio * 10) / 256);
        for (uint32_t x = 0; x < fb->width; x++) {
            uint8_t* addr = row + x * bpp;
            if (bpp == 4) {
                uint32_t color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                *(uint32_t*)addr = color;
            } else if (bpp == 3) {
                addr[0] = b; addr[1] = g; addr[2] = r;
            }
        }
    }
}

/* Write a short message to VGA text buffer at 0xB8000 so there is always
 * something visible on screen even when the framebuffer is not ready.
 * Each cell is two bytes: attribute byte (0x07 = light grey on black)
 * followed by ASCII character. */
static void wm_vga_text_puts(const char* msg) {
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    for (int i = 0; msg[i] && i < 80; i++) {
        vga[i] = (uint16_t)(0x0700 | (uint8_t)msg[i]);
    }
}

#define WM_FALLBACK_WIDTH  1024u
#define WM_FALLBACK_HEIGHT  768u

graphics_result_t window_manager_init(void) {
    debuglog(DEBUG_INFO, "Initializing window manager...\n");

    if (wm_state.wm_ready) {
        debuglog(DEBUG_WARN, "Window manager already initialized\n");
        return GRAPHICS_SUCCESS;
    }

    if (!graphics_is_initialized()) {
        if (kernel_framebuffer_disabled()) {
            debuglog(DEBUG_INFO, "Window manager: nofb mode, cannot initialize\n");
            return GRAPHICS_ERROR_GENERIC;
        }
        debuglog(DEBUG_INFO, "Window manager: graphics not initialized, attempting init...\n");
        graphics_result_t init_result = graphics_init();
        if (init_result != GRAPHICS_SUCCESS) {
            debuglog(DEBUG_ERROR, "Window manager: failed to initialize graphics: %s\n",
                    graphics_get_error_string(init_result));
            wm_vga_text_puts("WM: NO FB");
            return init_result;
        }
    }

    video_mode_t current_mode;
    graphics_result_t mode_result = graphics_get_current_mode(&current_mode);
    if (mode_result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "Failed to get current video mode: %s\n",
                graphics_get_error_string(mode_result));
        wm_vga_text_puts("WM: NO FB");
        return GRAPHICS_ERROR_GENERIC;
    }

    debuglog(DEBUG_INFO, "Window manager: video mode %ux%ux%u\n",
            current_mode.width, current_mode.height, current_mode.bpp);

    /* If the driver reported 0x0 dimensions the framebuffer is not really
     * ready.  Fall back to a safe default so layout calculations below
     * don't divide by zero or allocate 0-byte surfaces. */
    if (current_mode.width == 0 || current_mode.height == 0) {
        debuglog(DEBUG_WARN, "WM: video mode returned 0x0 — framebuffer not ready, "
                 "using fallback %ux%u\n", WM_FALLBACK_WIDTH, WM_FALLBACK_HEIGHT);
        wm_vga_text_puts("WM: NO FB");
        current_mode.width  = WM_FALLBACK_WIDTH;
        current_mode.height = WM_FALLBACK_HEIGHT;
    }

    wm_state.desktop_width = current_mode.width;
    wm_state.desktop_height = current_mode.height;

    debuglog(DEBUG_INFO, "WM: Creating desktop surface...\n");
    uint32_t free_before = kheap_get_free_memory();
    debuglog(DEBUG_INFO, "WM: Memory before surfaces: free=%u KB\n", free_before / 1024);

    graphics_result_t result = graphics_create_surface(
        wm_state.desktop_width, wm_state.desktop_height,
        current_mode.format, &wm_state.desktop_surface
    );
    if (result != GRAPHICS_SUCCESS || !wm_state.desktop_surface) {
        debuglog(DEBUG_WARN, "WM: Desktop surface alloc failed (%ux%u), degraded mode\n",
                 wm_state.desktop_width, wm_state.desktop_height);
        wm_state.desktop_surface = NULL;
        wm_state.composition_buffer = NULL;
        wm_paint_fallback_background();
        wm_state.config = default_config;
        wm_state.cursor_x = (int32_t)(wm_state.desktop_width / 2);
        wm_state.cursor_y = (int32_t)(wm_state.desktop_height / 2);
        wm_state.cursor_visible = false;
        wm_state.initialized = true;
        /* Mark wm_ready so the render loop can still call wm_present() and
         * draw the fallback background directly to the framebuffer. */
        wm_state.wm_ready = true;
        wm_state.needs_redraw = true;
        wm_mark_full_screen_dirty();
        return GRAPHICS_SUCCESS;
    }
    debuglog(DEBUG_INFO, "WM: Desktop surface created, free=%u KB\n",
             kheap_get_free_memory() / 1024);

    wm_state.comp_buf_separate = false;
    wm_state.comp_buf_size = 0;

    uint32_t comp_full_size = wm_state.desktop_width * wm_state.desktop_height *
                              ((current_mode.bpp + 7) / 8);
    uint32_t free_now = kheap_get_free_memory();

    if (free_now < comp_full_size + 256 * 1024) {
        debuglog(DEBUG_WARN, "WM: Memory pressure: need %u KB for comp buf, free=%u KB\n",
                 comp_full_size / 1024, free_now / 1024);
    }

    result = graphics_create_surface(
        wm_state.desktop_width, wm_state.desktop_height,
        current_mode.format, &wm_state.composition_buffer
    );
    if (result == GRAPHICS_SUCCESS) {
        wm_state.comp_buf_separate = true;
        wm_state.comp_buf_size = wm_state.composition_buffer->pitch * wm_state.desktop_height;
        debuglog(DEBUG_INFO, "WM: Full composition buffer allocated (%ux%u, %u bytes)\n",
                 wm_state.desktop_width, wm_state.desktop_height, wm_state.comp_buf_size);
    } else {
        debuglog(DEBUG_WARN, "WM: Full composition buffer alloc failed, trying quarter-size...\n");

        uint32_t q_w = wm_state.desktop_width / 2;
        uint32_t q_h = wm_state.desktop_height / 2;
        if (q_w >= 320 && q_h >= 240) {
            result = graphics_create_surface(q_w, q_h, current_mode.format, &wm_state.composition_buffer);
            if (result == GRAPHICS_SUCCESS) {
                wm_state.comp_buf_separate = true;
                wm_state.comp_buf_size = wm_state.composition_buffer->pitch * q_h;
                debuglog(DEBUG_INFO, "WM: Quarter composition buffer allocated (%ux%u, %u bytes)\n",
                         q_w, q_h, wm_state.comp_buf_size);
            }
        }

        if (!wm_state.comp_buf_separate) {
            debuglog(DEBUG_WARN, "WM: Quarter-size alloc failed, trying minimal 640x480...\n");

            uint32_t m_w = 640;
            uint32_t m_h = 480;
            if (m_w > wm_state.desktop_width) m_w = wm_state.desktop_width;
            if (m_h > wm_state.desktop_height) m_h = wm_state.desktop_height;

            result = graphics_create_surface(m_w, m_h, current_mode.format, &wm_state.composition_buffer);
            if (result == GRAPHICS_SUCCESS) {
                wm_state.comp_buf_separate = true;
                wm_state.comp_buf_size = wm_state.composition_buffer->pitch * m_h;
                debuglog(DEBUG_INFO, "WM: Minimal composition buffer allocated (%ux%u, %u bytes)\n",
                         m_w, m_h, wm_state.comp_buf_size);
            }
        }

        if (!wm_state.comp_buf_separate) {
            debuglog(DEBUG_WARN, "WM: All composition buffer allocs failed, using dirty-rect fallback\n");
            wm_state.composition_buffer = wm_state.desktop_surface;
            wm_state.comp_buf_size = 0;
            wm_mark_full_screen_dirty();
        }
    }

    desktop_draw_gradient(wm_state.desktop_surface);

    wm_state.config = default_config;
    wm_state.z_order_count = 0;

    wm_state.cursor_x = (int32_t)(wm_state.desktop_width / 2);
    wm_state.cursor_y = (int32_t)(wm_state.desktop_height / 2);
    wm_state.cursor_visible = false;

    wm_state.initialized = true;
    wm_state.wm_ready = true;
    wm_mark_full_screen_dirty();
    wm_state.needs_redraw = true;

    debuglog(DEBUG_INFO, "Window manager initialized (%ux%u), cursor at (%d,%d)\n",
            wm_state.desktop_width, wm_state.desktop_height,
            wm_state.cursor_x, wm_state.cursor_y);

    return GRAPHICS_SUCCESS;
}

graphics_result_t window_manager_shutdown(void) {
    if (!wm_state.initialized) {
        return GRAPHICS_SUCCESS;
    }

    debuglog(DEBUG_INFO, "Shutting down window manager...\n");

    window_t* current = wm_state.window_list;
    while (current) {
        window_t* next = current->next;
        window_destroy(current->handle);
        current = next;
    }

    if (wm_state.comp_buf_separate && wm_state.composition_buffer &&
        wm_state.composition_buffer != wm_state.desktop_surface) {
        if (wm_state.composition_buffer->pixels) {
            kheap_graphics_free(wm_state.composition_buffer->pixels);
        }
        kfree(wm_state.composition_buffer);
        wm_state.composition_buffer = NULL;
    } else {
        wm_state.composition_buffer = NULL;
    }

    if (wm_state.desktop_surface) {
        graphics_destroy_surface(wm_state.desktop_surface);
        wm_state.desktop_surface = NULL;
    }

    if (wm_state.wallpaper) {
        graphics_destroy_surface(wm_state.wallpaper);
        wm_state.wallpaper = NULL;
    }

    wm_state.z_order_count = 0;
    wm_state.initialized = false;
    wm_state.wm_ready = false;
    debuglog(DEBUG_INFO, "Window manager shutdown complete\n");

    return GRAPHICS_SUCCESS;
}

bool window_manager_is_initialized(void) {
    return wm_state.wm_ready;
}

window_handle_t window_create(int32_t x, int32_t y, uint32_t width, uint32_t height,
                             const char* title, uint32_t flags) {
    if (!wm_state.wm_ready) {
        debuglog(DEBUG_ERROR, "Window manager not ready\n");
        return INVALID_WINDOW_HANDLE;
    }

    if (width == 0 || height == 0) {
        debuglog(DEBUG_ERROR, "Invalid window dimensions: %ux%u\n", width, height);
        return INVALID_WINDOW_HANDLE;
    }

    if (wm_state.window_count >= WM_MAX_WINDOWS) {
        debuglog(DEBUG_ERROR, "Maximum window count reached\n");
        return INVALID_WINDOW_HANDLE;
    }

    window_t* window = kmalloc(sizeof(window_t));
    if (!window) {
        debuglog(DEBUG_ERROR, "Failed to allocate memory for window\n");
        return INVALID_WINDOW_HANDLE;
    }

    memset(window, 0, sizeof(window_t));

    window->handle = wm_state.next_handle++;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    window->min_width = 100;
    window->min_height = 50;
    window->max_width = wm_state.desktop_width;
    window->max_height = wm_state.desktop_height;
    window->flags = flags;
    window->state = WINDOW_STATE_NORMAL;
    window->z_order = 0;
    window->visible = true;
    window->focused = false;
    window->dirty = true;

    if (title) {
        strncpy(window->title, title, sizeof(window->title) - 1);
        window->title[sizeof(window->title) - 1] = '\0';
    } else {
        strcpy(window->title, "Untitled Window");
    }

    if (create_window_surface(window) != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "Failed to create window surface\n");
        kfree(window);
        return INVALID_WINDOW_HANDLE;
    }

    add_window_to_list(window);
    wm_state.window_count++;
    wm_z_order_push(window);

    window_focus(window->handle);

    wm_state.needs_redraw = true;
    wm_mark_dirty_rect(x, y, width, height);

    debuglog(DEBUG_INFO, "Created window '%s' (handle: %u, %dx%d at %d,%d)\n",
            window->title, window->handle, width, height, x, y);

    return window->handle;
}

graphics_result_t window_destroy(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    debuglog(DEBUG_INFO, "Destroying window '%s' (handle: %u)\n", window->title, handle);

    if (window->on_close) {
        window->on_close(window);
    }

    if (wm_state.focused_window == handle) {
        wm_state.focused_window = INVALID_WINDOW_HANDLE;

        window_t* best_candidate = NULL;
        for (uint32_t i = wm_state.z_order_count; i > 0; i--) {
            window_t* scan = wm_state.z_order_array[i - 1];
            if (scan->handle != handle && scan->visible &&
                scan->state != WINDOW_STATE_MINIMIZED) {
                best_candidate = scan;
                break;
            }
        }
        if (best_candidate) {
            window_focus(best_candidate->handle);
        }
    }

    wm_z_order_remove(window);
    destroy_window_surface(window);
    remove_window_from_list(window);
    wm_state.window_count--;

    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    kfree(window);
    wm_state.needs_redraw = true;

    return GRAPHICS_SUCCESS;
}

window_t* window_get(window_handle_t handle) {
    return find_window_by_handle(handle);
}

graphics_result_t window_set_title(window_handle_t handle, const char* title) {
    window_t* window = find_window_by_handle(handle);
    if (!window || !title) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    strncpy(window->title, title, sizeof(window->title) - 1);
    window->title[sizeof(window->title) - 1] = '\0';
    window->dirty = true;
    wm_mark_dirty_rect(window->x, window->y, window->width, wm_state.config.title_bar_height);
    wm_state.needs_redraw = true;

    return GRAPHICS_SUCCESS;
}

graphics_result_t window_set_position(window_handle_t handle, int32_t x, int32_t y) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + (int32_t)window->width > (int32_t)wm_state.desktop_width) {
        x = (int32_t)wm_state.desktop_width - (int32_t)window->width;
    }
    if (y + (int32_t)window->height > (int32_t)wm_state.desktop_height) {
        y = (int32_t)wm_state.desktop_height - (int32_t)window->height;
    }

    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->x = x;
    window->y = y;
    wm_mark_dirty_rect(x, y, window->width, window->height);
    window->dirty = true;
    wm_state.needs_redraw = true;

    if (window->on_move) {
        window->on_move(window, x, y);
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t window_focus(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (wm_state.focused_window != INVALID_WINDOW_HANDLE) {
        window_t* old_focused = find_window_by_handle(wm_state.focused_window);
        if (old_focused) {
            old_focused->focused = false;
            old_focused->dirty = true;
            wm_mark_dirty_rect(old_focused->x, old_focused->y,
                              old_focused->width, wm_state.config.title_bar_height);
            if (old_focused->on_focus) {
                old_focused->on_focus(old_focused, false);
            }
        }
    }

    wm_state.focused_window = handle;
    window->focused = true;
    window->dirty = true;
    wm_mark_dirty_rect(window->x, window->y, window->width, wm_state.config.title_bar_height);
    wm_state.needs_redraw = true;

    if (window->on_focus) {
        window->on_focus(window, true);
    }

    wm_z_order_to_front(window);

    return GRAPHICS_SUCCESS;
}

window_handle_t window_get_focused(void) {
    return wm_state.focused_window;
}

graphics_result_t window_set_paint_callback(
        window_handle_t handle,
        void (*callback)(window_t* window, graphics_surface_t* surface)) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    window->on_paint = callback;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_set_resize_callback(
        window_handle_t handle,
        void (*callback)(window_t* window, uint32_t new_width, uint32_t new_height)) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    window->on_resize = callback;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_set_input_callback(
        window_handle_t handle,
        void (*callback)(window_t* window, const input_event_t* event)) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    window->on_input = callback;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_get_surface(window_handle_t handle, graphics_surface_t** surface) {
    window_t* window = find_window_by_handle(handle);
    if (!window || !surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    *surface = window->surface;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_invalidate(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    window->dirty = true;
    wm_state.needs_redraw = true;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_invalidate_rect(window_handle_t handle, const graphics_rect_t* rect) {
    (void)rect;
    return window_invalidate(handle);
}

graphics_result_t window_present(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    wm_state.needs_redraw = true;
    return compositor_update();
}

graphics_result_t compositor_update(void) {
    if (!wm_state.wm_ready) {
        return GRAPHICS_SUCCESS;
    }

    if (!wm_state.needs_redraw) {
        return GRAPHICS_SUCCESS;
    }

    graphics_result_t result = composite_windows();
    if (result != GRAPHICS_SUCCESS) {
        return result;
    }

    return wm_present();
}

graphics_result_t wm_render_loop_tick(void) {
    if (!wm_state.wm_ready) {
        return GRAPHICS_SUCCESS;
    }
    task_mark_active();
    wm_state.needs_redraw = true;
    return compositor_update();
}

graphics_result_t wm_update_cursor(int32_t x, int32_t y) {
    if (!wm_state.initialized) {
        return GRAPHICS_SUCCESS;
    }
    wm_mark_dirty_rect(wm_state.cursor_x - 1, wm_state.cursor_y - 1, 13, 21);
    wm_state.cursor_x = x;
    wm_state.cursor_y = y;
    wm_mark_dirty_rect(x - 1, y - 1, 13, 21);
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t wm_enable_cursor(bool visible) {
    if (!wm_state.initialized) {
        return GRAPHICS_SUCCESS;
    }
    if (wm_state.cursor_visible != visible) {
        wm_state.cursor_visible = visible;
        wm_state.needs_redraw = true;
        debuglog(DEBUG_INFO, "[WM] Cursor %s\n", visible ? "enabled" : "disabled");
    }
    return GRAPHICS_SUCCESS;
}

graphics_result_t wm_handle_input_event(const input_event_t* event) {
    if (!wm_state.initialized || !event) {
        return GRAPHICS_SUCCESS;
    }

    if (event->type == EV_REL) {
        if (event->code == REL_X) {
            wm_state.cursor_x += event->value;
            if (wm_state.cursor_x < 0) wm_state.cursor_x = 0;
            if (wm_state.cursor_x >= (int32_t)wm_state.desktop_width) {
                wm_state.cursor_x = (int32_t)wm_state.desktop_width - 1;
            }
        } else if (event->code == REL_Y) {
            wm_state.cursor_y += event->value;
            if (wm_state.cursor_y < 0) wm_state.cursor_y = 0;
            if (wm_state.cursor_y >= (int32_t)wm_state.desktop_height) {
                wm_state.cursor_y = (int32_t)wm_state.desktop_height - 1;
            }
        } else if (event->code == REL_WHEEL) {
            window_handle_t h = window_at_point(wm_state.cursor_x, wm_state.cursor_y);
            if (h != INVALID_WINDOW_HANDLE) {
                window_t* win = find_window_by_handle(h);
                if (win && win->on_input) {
                    win->on_input(win, event);
                }
            }
            wm_state.needs_redraw = true;
            return GRAPHICS_SUCCESS;
        }

        if (wm_state.is_dragging && wm_state.drag_window) {
            int32_t new_x = wm_state.cursor_x - wm_state.drag_offset_x;
            int32_t new_y = wm_state.cursor_y - wm_state.drag_offset_y;

            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;

            uint32_t thresh = wm_state.config.snap_threshold;
            wm_state.show_snap_preview = false;
            wm_state.snap_edge = SNAP_EDGE_NONE;

            if ((uint32_t)wm_state.cursor_x < thresh) {
                wm_state.snap_edge = SNAP_EDGE_LEFT;
                wm_state.snap_preview_x = 0;
                wm_state.snap_preview_y = 0;
                wm_state.snap_preview_w = wm_state.desktop_width / 2;
                wm_state.snap_preview_h = wm_state.desktop_height;
                wm_state.show_snap_preview = true;
            } else if ((uint32_t)wm_state.cursor_x >= wm_state.desktop_width - thresh) {
                wm_state.snap_edge = SNAP_EDGE_RIGHT;
                wm_state.snap_preview_x = (int32_t)(wm_state.desktop_width / 2);
                wm_state.snap_preview_y = 0;
                wm_state.snap_preview_w = wm_state.desktop_width / 2;
                wm_state.snap_preview_h = wm_state.desktop_height;
                wm_state.show_snap_preview = true;
            } else if ((uint32_t)wm_state.cursor_y < thresh) {
                wm_state.snap_edge = SNAP_EDGE_TOP;
                wm_state.snap_preview_x = 0;
                wm_state.snap_preview_y = 0;
                wm_state.snap_preview_w = wm_state.desktop_width;
                wm_state.snap_preview_h = wm_state.desktop_height;
                wm_state.show_snap_preview = true;
            }

            wm_state.drag_window->x = new_x;
            wm_state.drag_window->y = new_y;
            wm_state.drag_window->dirty = true;
            wm_mark_dirty_rect(new_x, new_y, wm_state.drag_window->width, wm_state.drag_window->height);
            if (wm_state.drag_window->on_move) {
                wm_state.drag_window->on_move(wm_state.drag_window, new_x, new_y);
            }
            wm_state.needs_redraw = true;
        }

        if (wm_state.is_resizing && wm_state.resize_window) {
            window_t* win = wm_state.resize_window;
            int32_t dx = wm_state.cursor_x - wm_state.resize_origin_x;
            int32_t dy = wm_state.cursor_y - wm_state.resize_origin_y;

            uint32_t new_w = wm_state.resize_origin_w;
            uint32_t new_h = wm_state.resize_origin_h;
            int32_t new_x = wm_state.resize_start_win_x;
            int32_t new_y = wm_state.resize_start_win_y;

            if (wm_state.resize_edge & RESIZE_EDGE_LEFT) {
                if (dx > 0 && (uint32_t)dx < wm_state.resize_origin_w - win->min_width) {
                    new_w = wm_state.resize_origin_w - (uint32_t)dx;
                    new_x = wm_state.resize_start_win_x + dx;
                }
            }
            if (wm_state.resize_edge & RESIZE_EDGE_RIGHT) {
                new_w = wm_state.resize_origin_w + (uint32_t)dx;
            }
            if (wm_state.resize_edge & RESIZE_EDGE_TOP) {
                if (dy > 0 && (uint32_t)dy < wm_state.resize_origin_h - win->min_height) {
                    new_h = wm_state.resize_origin_h - (uint32_t)dy;
                    new_y = wm_state.resize_start_win_y + dy;
                }
            }
            if (wm_state.resize_edge & RESIZE_EDGE_BOTTOM) {
                new_h = wm_state.resize_origin_h + (uint32_t)dy;
            }

            if (new_w < win->min_width) new_w = win->min_width;
            if (new_h < win->min_height) new_h = win->min_height;
            if (new_w > win->max_width) new_w = win->max_width;
            if (new_h > win->max_height) new_h = win->max_height;

            if (new_x < 0) { new_w += new_x; new_x = 0; }
            if (new_y < 0) { new_h += new_y; new_y = 0; }

            if (new_w >= win->min_width && new_h >= win->min_height) {
                win->x = new_x;
                win->y = new_y;
                win->width = new_w;
                win->height = new_h;
                win->dirty = true;
                wm_mark_dirty_rect(new_x, new_y, new_w, new_h);
                if (win->on_resize) {
                    win->on_resize(win, new_w, new_h);
                }
                wm_state.needs_redraw = true;
            }
        }

        wm_state.needs_redraw = true;
        return GRAPHICS_SUCCESS;
    }

    if (event->type == EV_KEY) {
        if (event->code == BTN_LEFT) {
            if (event->value == 1) {
                wm_state.mouse_left_down = true;
                window_handle_t h = window_at_point(wm_state.cursor_x, wm_state.cursor_y);

                if (h != INVALID_WINDOW_HANDLE) {
                    window_t* win = find_window_by_handle(h);
                    if (win) {
                        if (wm_is_on_title_bar(win, wm_state.cursor_x, wm_state.cursor_y)) {
                            if ((win->flags & WINDOW_FLAG_MOVABLE) &&
                                win->state != WINDOW_STATE_MAXIMIZED &&
                                win->state != WINDOW_STATE_FULLSCREEN) {
                                wm_state.is_dragging = true;
                                wm_state.drag_window = win;
                                wm_state.drag_offset_x = wm_state.cursor_x - win->x;
                                wm_state.drag_offset_y = wm_state.cursor_y - win->y;
                            }
                        } else {
                            uint32_t edge = wm_get_resize_edge(win, wm_state.cursor_x, wm_state.cursor_y);
                            if (edge != RESIZE_EDGE_NONE) {
                                wm_state.is_resizing = true;
                                wm_state.resize_window = win;
                                wm_state.resize_edge = edge;
                                wm_state.resize_origin_x = wm_state.cursor_x;
                                wm_state.resize_origin_y = wm_state.cursor_y;
                                wm_state.resize_origin_w = win->width;
                                wm_state.resize_origin_h = win->height;
                                wm_state.resize_start_win_x = win->x;
                                wm_state.resize_start_win_y = win->y;
                            }
                        }

                        window_focus(h);
                        wm_state.needs_redraw = true;
                    }
                } else {
                    if (wm_state.focused_window != INVALID_WINDOW_HANDLE) {
                        window_t* old = find_window_by_handle(wm_state.focused_window);
                        if (old) {
                            old->focused = false;
                            old->dirty = true;
                            if (old->on_focus) old->on_focus(old, false);
                        }
                        wm_state.focused_window = INVALID_WINDOW_HANDLE;
                        wm_state.needs_redraw = true;
                    }
                }
            } else {
                if (wm_state.is_dragging && wm_state.drag_window) {
                    if (wm_state.show_snap_preview) {
                        window_t* win = wm_state.drag_window;
                        wm_mark_dirty_rect(win->x, win->y, win->width, win->height);
                        win->x = wm_state.snap_preview_x;
                        win->y = wm_state.snap_preview_y;
                        win->width = wm_state.snap_preview_w;
                        win->height = wm_state.snap_preview_h;
                        win->dirty = true;
                        wm_mark_dirty_rect(win->x, win->y, win->width, win->height);
                        if (win->on_resize) {
                            win->on_resize(win, win->width, win->height);
                        }
                        if (wm_state.snap_edge == SNAP_EDGE_TOP) {
                            win->state = WINDOW_STATE_MAXIMIZED;
                        }
                    }
                }

                wm_state.is_dragging = false;
                wm_state.drag_window = NULL;
                wm_state.show_snap_preview = false;
                wm_state.snap_edge = SNAP_EDGE_NONE;
                wm_state.is_resizing = false;
                wm_state.resize_window = NULL;
                wm_state.mouse_left_down = false;
                wm_state.needs_redraw = true;
            }
        }
    }

    return GRAPHICS_SUCCESS;
}

static graphics_result_t wm_present(void) {
    framebuffer_t* hw_fb = graphics_get_framebuffer();
    if (!hw_fb || !hw_fb->virtual_addr) {
        static bool _warned = false;
        if (!_warned) {
            _warned = true;
            debuglog(DEBUG_WARN, "[WM] wm_present: hw_fb=%p virt=%p — framebuffer not ready\n",
                     hw_fb, hw_fb ? (void*)hw_fb->virtual_addr : NULL);
            /* Write to VGA text buffer so the user sees something on screen
             * even though the linear framebuffer is unavailable. */
            wm_vga_text_puts("WM: NO FB");
            if (!kernel_framebuffer_disabled()) {
                graphics_init();
            }
        }
        return GRAPHICS_SUCCESS;
    }
    static bool _logged_fb = false;
    if (!_logged_fb) {
        _logged_fb = true;
        debuglog(DEBUG_INFO, "[WM] wm_present: virt=0x%08x %ux%u bpp=%u pitch=%u\n",
                 (uint32_t)hw_fb->virtual_addr, hw_fb->width, hw_fb->height,
                 hw_fb->bpp, hw_fb->pitch);
    }

    uint32_t now = timer_get_ticks();

    if (framebuffer_has_userspace_mapping()) {
        if (wm_state.render_state == WM_RENDER_STATE_NORMAL ||
            wm_state.render_state == WM_RENDER_STATE_RECOVERY) {
            wm_state.userspace_map_tick = now;
            wm_state.last_userspace_flush_tick = now;
            wm_state.render_state = WM_RENDER_STATE_DEFERRED;
            wm_state.watchdog_triggered = false;
            debuglog(DEBUG_INFO, "[WM] Userspace mapped FB, deferring\n");
        }

        if (wm_state.render_state == WM_RENDER_STATE_DEFERRED) {
            uint32_t last_flush = framebuffer_get_userspace_last_flush();
            if (last_flush != 0) {
                wm_state.last_userspace_flush_tick = last_flush;
            }
            uint32_t since_flush = now - wm_state.last_userspace_flush_tick;
            if (since_flush < WM_WATCHDOG_TIMEOUT_TICKS) {
                return GRAPHICS_SUCCESS;
            }
            debuglog(DEBUG_WARN, "[WM] Userspace stalled %u ticks, kernel fallback\n",
                     since_flush);
            wm_state.render_state = WM_RENDER_STATE_FALLBACK;
            wm_state.watchdog_triggered = true;
            gfx_panic_display("Desktop environment unresponsive\nKernel taking over display");
        }

        if (wm_state.render_state == WM_RENDER_STATE_FALLBACK) {
            if (now - wm_state.last_kernel_fallback_tick < 30) {
                return GRAPHICS_SUCCESS;
            }
            wm_state.last_kernel_fallback_tick = now;
            wm_draw_fallback_screen(hw_fb);
            if (wm_state.cursor_visible) {
                wm_draw_cursor_on_fb(hw_fb);
            }
            return GRAPHICS_SUCCESS;
        }

        return GRAPHICS_SUCCESS;
    }

    if (wm_state.render_state == WM_RENDER_STATE_DEFERRED ||
        wm_state.render_state == WM_RENDER_STATE_FALLBACK) {
        debuglog(DEBUG_INFO, "[WM] Userspace unmapped FB, kernel WM taking back control\n");
        wm_state.render_state = WM_RENDER_STATE_NORMAL;
        wm_state.needs_redraw = true;
    }

    uint32_t hw_bpp = (hw_fb->bpp + 7) / 8;
    if (hw_bpp == 0) hw_bpp = 4;
    bool use_back_buffer = (hw_fb->double_buffered && hw_fb->back_buffer != 0);
    uint8_t* present_base = (uint8_t*)(use_back_buffer ? hw_fb->back_buffer : hw_fb->virtual_addr);

    if (wm_state.composition_buffer && wm_state.composition_buffer->pixels) {
        bool full_buffer = wm_state.comp_buf_separate &&
                           wm_state.composition_buffer->height >= wm_state.desktop_height;
        bool small_buffer = wm_state.comp_buf_separate && !full_buffer;

        if (small_buffer) {
            graphics_rect_t src_rect = {0, 0,
                wm_state.composition_buffer->width,
                wm_state.composition_buffer->height};
            graphics_surface_t present_surf;
            present_surf.pixels = (void*)(use_back_buffer ? hw_fb->back_buffer : hw_fb->virtual_addr);
            present_surf.width  = hw_fb->width;
            present_surf.height = hw_fb->height;
            present_surf.pitch  = hw_fb->pitch;
            present_surf.bpp    = hw_fb->bpp;
            present_surf.format = (pixel_format_t)hw_fb->format;
            optimized_blit_surface_scaled(wm_state.composition_buffer, &present_surf,
                                           &src_rect, 0, 0,
                                           hw_fb->width, hw_fb->height);
        } else if (full_buffer || !wm_state.has_dirty_rect) {
            uint32_t comp_bpp = (wm_state.composition_buffer->bpp + 7) / 8;
            if (comp_bpp == 0) comp_bpp = hw_bpp;
            uint32_t copy_w = wm_state.composition_buffer->width;
            if (copy_w > hw_fb->width) copy_w = hw_fb->width;
            uint32_t copy_h = wm_state.composition_buffer->height;
            if (copy_h > hw_fb->height) copy_h = hw_fb->height;
            if (comp_bpp == hw_bpp) {
                /* Same bpp: fast row-by-row copy respecting each surface's pitch */
                uint32_t row_bytes = copy_w * hw_bpp;
                for (uint32_t y = 0; y < copy_h; y++) {
                    uint8_t* src = (uint8_t*)wm_state.composition_buffer->pixels +
                                   y * wm_state.composition_buffer->pitch;
                    uint8_t* dst = present_base + y * hw_fb->pitch;
                    memcpy(dst, src, row_bytes);
                }
            } else {
                /* Bpp mismatch (e.g. comp=32bpp, hw=24bpp): pixel-by-pixel conversion */
                for (uint32_t y = 0; y < copy_h; y++) {
                    uint8_t* src_row = (uint8_t*)wm_state.composition_buffer->pixels +
                                       y * wm_state.composition_buffer->pitch;
                    uint8_t* dst_row = present_base + y * hw_fb->pitch;
                    for (uint32_t x = 0; x < copy_w; x++) {
                        uint8_t* sp = src_row + x * comp_bpp;
                        uint8_t* dp = dst_row + x * hw_bpp;
                        /* Extract BGR from source (works for both BGR_888 and BGRA_8888) */
                        uint8_t b = sp[0];
                        uint8_t g = sp[1];
                        uint8_t r = (comp_bpp >= 3) ? sp[2] : 0;
                        if (hw_bpp == 3) {
                            dp[0] = b; dp[1] = g; dp[2] = r;
                        } else if (hw_bpp == 4) {
                            dp[0] = b; dp[1] = g; dp[2] = r; dp[3] = 0xFF;
                        } else if (hw_bpp == 2) {
                            *(uint16_t*)dp = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        } else {
                            *dp = (uint8_t)((r + g + b) / 3);
                        }
                    }
                }
            }
        } else {
            int32_t rx0 = wm_state.dirty_x0 < 0 ? 0 : wm_state.dirty_x0;
            int32_t ry0 = wm_state.dirty_y0 < 0 ? 0 : wm_state.dirty_y0;
            int32_t rx1 = wm_state.dirty_x1 > (int32_t)hw_fb->width ? (int32_t)hw_fb->width : wm_state.dirty_x1;
            int32_t ry1 = wm_state.dirty_y1 > (int32_t)hw_fb->height ? (int32_t)hw_fb->height : wm_state.dirty_y1;
            if (rx1 > rx0 && ry1 > ry0) {
                uint32_t comp_bpp = (wm_state.composition_buffer->bpp + 7) / 8;
                if (comp_bpp == 0) comp_bpp = hw_bpp;
                if (comp_bpp == hw_bpp) {
                    for (int32_t y = ry0; y < ry1; y++) {
                        uint8_t* src = (uint8_t*)wm_state.composition_buffer->pixels +
                                       y * wm_state.composition_buffer->pitch + rx0 * comp_bpp;
                        uint8_t* dst = present_base + y * hw_fb->pitch + rx0 * hw_bpp;
                        memcpy(dst, src, (rx1 - rx0) * hw_bpp);
                    }
                } else {
                    for (int32_t y = ry0; y < ry1; y++) {
                        uint8_t* src_row = (uint8_t*)wm_state.composition_buffer->pixels +
                                           y * wm_state.composition_buffer->pitch;
                        uint8_t* dst_row = present_base + y * hw_fb->pitch;
                        for (int32_t x = rx0; x < rx1; x++) {
                            uint8_t* sp = src_row + x * comp_bpp;
                            uint8_t* dp = dst_row + x * hw_bpp;
                            uint8_t b = sp[0];
                            uint8_t g = sp[1];
                            uint8_t r = (comp_bpp >= 3) ? sp[2] : 0;
                            if (hw_bpp == 3) {
                                dp[0] = b; dp[1] = g; dp[2] = r;
                            } else if (hw_bpp == 4) {
                                dp[0] = b; dp[1] = g; dp[2] = r; dp[3] = 0xFF;
                            } else if (hw_bpp == 2) {
                                *(uint16_t*)dp = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                            } else {
                                *dp = (uint8_t)((r + g + b) / 3);
                            }
                        }
                    }
                }
            }
        }
        wm_reset_dirty_rect();
    } else {
        /* No composition buffer — paint a solid background directly to the
         * framebuffer.  Only do this if the framebuffer actually has a valid
         * size; a 0x0 framebuffer means hardware init is incomplete. */
        if (present_base && hw_fb->width > 0 && hw_fb->height > 0) {
            video_mode_t fb_mode;
            pixel_format_t fb_fmt = PIXEL_FORMAT_BGRA_8888;
            if (graphics_get_current_mode(&fb_mode) == GRAPHICS_SUCCESS) {
                fb_fmt = fb_mode.format;
            }
            uint32_t bg_pixel = graphics_color_to_pixel(
                (graphics_color_t){64, 40, 16, 255}, fb_fmt);
            for (uint32_t y = 0; y < hw_fb->height; y++) {
                uint8_t* row = present_base + y * hw_fb->pitch;
                for (uint32_t x = 0; x < hw_fb->width; x++) {
                    uint8_t* addr = row + x * hw_bpp;
                    switch (hw_bpp) {
                        case 4: *(uint32_t*)addr = bg_pixel; break;
                        case 3: addr[0] = bg_pixel & 0xFF; addr[1] = (bg_pixel >> 8) & 0xFF; addr[2] = (bg_pixel >> 16) & 0xFF; break;
                        case 2: *(uint16_t*)addr = (uint16_t)(bg_pixel & 0xFFFF); break;
                        case 1: *addr = (uint8_t)(bg_pixel & 0xFF); break;
                    }
                }
            }
        }
    }

    if (wm_state.cursor_visible) {
        if (use_back_buffer) {
            framebuffer_t present_fb = *hw_fb;
            present_fb.virtual_addr = hw_fb->back_buffer;
            wm_draw_cursor_on_fb(&present_fb);
        } else {
            wm_draw_cursor_on_fb(hw_fb);
        }
    }

    if (use_back_buffer) {
        graphics_result_t swap_res = graphics_swap_buffers();
        if (swap_res != GRAPHICS_SUCCESS) {
            (void)gfx_flush_framebuffer();
        }
    }

    wm_state.last_frame_render_tick = now;

    return GRAPHICS_SUCCESS;
}

static void wm_draw_fallback_screen(framebuffer_t* fb) {
    if (!fb || !fb->virtual_addr) return;

    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;

    video_mode_t mode;
    pixel_format_t fmt = PIXEL_FORMAT_BGRA_8888;
    if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS) {
        fmt = mode.format;
    }

    uint32_t bg = graphics_color_to_pixel((graphics_color_t){30, 30, 46, 255}, fmt);

    uint8_t* pixels = (uint8_t*)fb->virtual_addr;
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t* row = pixels + y * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            uint8_t* addr = row + x * bpp;
            switch (bpp) {
                case 4: *(uint32_t*)addr = bg; break;
                case 3: addr[0] = bg & 0xFF; addr[1] = (bg >> 8) & 0xFF; addr[2] = (bg >> 16) & 0xFF; break;
                case 2: *(uint16_t*)addr = (uint16_t)(bg & 0xFFFF); break;
                case 1: *addr = (uint8_t)(bg & 0xFF); break;
            }
        }
    }

    font_t* font = NULL;
    font_get_system_font(&font);
    if (font) {
        text_style_t style = {
            .foreground = (graphics_color_t){205, 214, 244, 255},
            .background = (graphics_color_t){30, 30, 46, 255},
            .has_background = false,
            .underline = false,
            .strikethrough = false
        };

        uint32_t text_w = 0, text_h = 0;
        const char* msg = "Waiting for desktop environment...";
        font_measure_text(font, msg, &text_w, &text_h);

        int32_t tx = ((int32_t)fb->width - (int32_t)text_w) / 2;
        int32_t ty = ((int32_t)fb->height - (int32_t)text_h) / 2;

        graphics_surface_t tmp_surface;
        memset(&tmp_surface, 0, sizeof(tmp_surface));
        tmp_surface.width = fb->width;
        tmp_surface.height = fb->height;
        tmp_surface.pitch = fb->pitch;
        tmp_surface.bpp = fb->bpp;
        tmp_surface.format = fmt;
        tmp_surface.pixels = (void*)fb->virtual_addr;

        font_render_text(font, &tmp_surface, tx, ty, msg, &style);
    }
}

static void wm_draw_cursor_on_fb(framebuffer_t* fb) {
    if (!fb || !fb->virtual_addr) return;

    int32_t cx = wm_state.cursor_x;
    int32_t cy = wm_state.cursor_y;
    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint8_t* fb_pixels = (uint8_t*)fb->virtual_addr;

    for (int32_t row = 0; row < 19; row++) {
        for (int32_t col = 0; col < 11; col++) {
            int32_t px = cx + col;
            int32_t py = cy + row;

            if (px < 0 || py < 0 ||
                (uint32_t)px >= fb->width || (uint32_t)py >= fb->height) {
                continue;
            }

            uint8_t alpha = wm_cursor_alpha[row][col];
            if (alpha == 0) continue;

            uint32_t cr, cg, cb;
            if (wm_cursor_is_outline[row][col]) {
                cr = 0; cg = 0; cb = 0;
            } else {
                cr = 255; cg = 255; cb = 255;
            }

            uint8_t* addr = fb_pixels + py * fb->pitch + px * bpp;

            if (alpha == 255) {
                uint32_t pixel_val = cb | (cg << 8) | (cr << 16);
                switch (bpp) {
                    case 4: *(uint32_t*)addr = pixel_val; break;
                    case 3: addr[0] = (uint8_t)(cb); addr[1] = (uint8_t)(cg); addr[2] = (uint8_t)(cr); break;
                    case 2: *(uint16_t*)addr = (uint16_t)(pixel_val & 0xFFFF); break;
                    case 1: *addr = (uint8_t)(pixel_val & 0xFF); break;
                }
            } else {
                uint32_t dst = 0;
                switch (bpp) {
                    case 4: dst = *(uint32_t*)addr; break;
                    case 3: dst = addr[0] | ((uint32_t)addr[1] << 8) | ((uint32_t)addr[2] << 16); break;
                    case 2: dst = *(uint16_t*)addr; break;
                    case 1: dst = *addr; break;
                }
                uint32_t blended = wm_alpha_blend_pixel(dst, cr, cg, cb, alpha);
                switch (bpp) {
                    case 4: *(uint32_t*)addr = blended; break;
                    case 3: addr[0] = blended & 0xFF; addr[1] = (blended >> 8) & 0xFF; addr[2] = (blended >> 16) & 0xFF; break;
                    case 2: *(uint16_t*)addr = (uint16_t)(blended & 0xFFFF); break;
                    case 1: *addr = (uint8_t)(blended & 0xFF); break;
                }
            }
        }
    }
}

static graphics_result_t create_window_surface(window_t* window) {
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    video_mode_t current_mode;
    if (graphics_get_current_mode(&current_mode) != GRAPHICS_SUCCESS) {
        return GRAPHICS_ERROR_GENERIC;
    }

    graphics_result_t result = graphics_create_surface(
        window->width, window->height, current_mode.format, &window->surface
    );
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "WM: surface alloc failed for window '%s' (%ux%u): result=%d\n",
                 window->title, window->width, window->height, (int)result);
        window->surface = NULL;
        return result;
    }

    /* Paranoia: driver succeeded but returned a NULL pointer or NULL pixels. */
    if (!window->surface || !window->surface->pixels) {
        debuglog(DEBUG_ERROR, "WM: surface alloc returned NULL for window '%s' (%ux%u)\n",
                 window->title, window->width, window->height);
        window->surface = NULL;
        return GRAPHICS_ERROR_GENERIC;
    }

    memset(window->surface->pixels, 0,
           window->surface->pitch * window->surface->height);

    return GRAPHICS_SUCCESS;
}

static graphics_result_t destroy_window_surface(window_t* window) {
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (window->surface) {
        graphics_destroy_surface(window->surface);
        window->surface = NULL;
    }

    if (window->back_buffer) {
        graphics_destroy_surface(window->back_buffer);
        window->back_buffer = NULL;
    }

    return GRAPHICS_SUCCESS;
}

static graphics_result_t composite_windows(void) {
    if (!wm_state.composition_buffer || !wm_state.composition_buffer->pixels) {
        return GRAPHICS_ERROR_GENERIC;
    }

    graphics_surface_t* comp = wm_state.composition_buffer;
    uint32_t comp_bpp_bytes = (comp->bpp + 7) / 8;
    if (comp_bpp_bytes == 0) comp_bpp_bytes = 4;
    uint8_t* comp_pixels = (uint8_t*)comp->pixels;

    bool full_buffer = wm_state.comp_buf_separate &&
                       comp->height >= wm_state.desktop_height;

    int32_t clear_x0 = 0, clear_y0 = 0;
    int32_t clear_x1 = (int32_t)comp->width;
    int32_t clear_y1 = (int32_t)comp->height;

    if (!full_buffer) {
        if (!wm_state.has_dirty_rect) {
            wm_state.needs_redraw = false;
            return GRAPHICS_SUCCESS;
        }
        clear_x0 = wm_state.dirty_x0 < 0 ? 0 : wm_state.dirty_x0;
        clear_y0 = wm_state.dirty_y0 < 0 ? 0 : wm_state.dirty_y0;
        clear_x1 = wm_state.dirty_x1 > (int32_t)comp->width ? (int32_t)comp->width : wm_state.dirty_x1;
        clear_y1 = wm_state.dirty_y1 > (int32_t)comp->height ? (int32_t)comp->height : wm_state.dirty_y1;
        if (clear_x1 <= clear_x0 || clear_y1 <= clear_y0) {
            wm_state.needs_redraw = false;
            return GRAPHICS_SUCCESS;
        }
    }

    if (wm_state.wallpaper && wm_state.wallpaper->pixels) {
        uint32_t wp_bpp = (wm_state.wallpaper->bpp + 7) / 8;
        if (wp_bpp == 0) wp_bpp = comp_bpp_bytes;
        uint32_t blit_w = wm_state.wallpaper->width;
        uint32_t blit_h = wm_state.wallpaper->height;
        if (blit_w > comp->width) blit_w = comp->width;
        if (blit_h > comp->height) blit_h = comp->height;

        int32_t wp_y0 = full_buffer ? 0 : clear_y0;
        int32_t wp_y1 = full_buffer ? (int32_t)blit_h : clear_y1;
        int32_t wp_x0 = full_buffer ? 0 : clear_x0;
        int32_t wp_x1 = full_buffer ? (int32_t)blit_w : clear_x1;
        if (wp_y1 > (int32_t)blit_h) wp_y1 = (int32_t)blit_h;
        if (wp_x1 > (int32_t)blit_w) wp_x1 = (int32_t)blit_w;

        for (int32_t y = wp_y0; y < wp_y1; y++) {
            uint8_t* src_row = (uint8_t*)wm_state.wallpaper->pixels + y * wm_state.wallpaper->pitch;
            uint8_t* dst_row = comp_pixels + (uint32_t)y * comp->pitch;
            uint32_t row_bytes = (wp_x1 - wp_x0) * comp_bpp_bytes;
            memcpy(dst_row + wp_x0 * comp_bpp_bytes, src_row + wp_x0 * comp_bpp_bytes, row_bytes);
        }
    } else {
        if (full_buffer) {
            desktop_draw_gradient(comp);
        } else {
            for (int32_t y = clear_y0; y < clear_y1; y++) {
                uint8_t* row = comp_pixels + (uint32_t)y * comp->pitch;
                memset(row + clear_x0 * comp_bpp_bytes, 0x00,
                       (clear_x1 - clear_x0) * comp_bpp_bytes);
                if (comp_bpp_bytes == 4) {
                    for (int32_t x = clear_x0; x < clear_x1; x++) {
                        row[x * 4 + 3] = 0xFF;
                    }
                }
            }
        }
    }

    for (uint32_t i = wm_state.z_order_count; i > 0; i--) {
        window_t* win = wm_state.z_order_array[i - 1];
        if (!win->visible || win->state == WINDOW_STATE_MINIMIZED) continue;

        if (wm_state.config.enable_shadows && win->state != WINDOW_STATE_FULLSCREEN) {
            wm_draw_shadow(comp, win);
        }

        if (win->flags & WINDOW_FLAG_DECORATED) {
            draw_window_decorations(win);
        }

        if (win->dirty && win->on_paint && win->surface) {
            win->on_paint(win, win->surface);
            win->dirty = false;
        }

        if (!win->surface || !win->surface->pixels) continue;

        uint32_t win_bpp = (win->surface->bpp + 7) / 8;
        if (win_bpp == 0) win_bpp = comp_bpp_bytes;

        int32_t dst_x0 = win->x;
        int32_t dst_y0 = win->y;

        for (uint32_t sy = 0; sy < win->height; sy++) {
            int32_t dy = dst_y0 + (int32_t)sy;
            if (dy < 0 || (uint32_t)dy >= comp->height) continue;

            uint8_t* src_row = (uint8_t*)win->surface->pixels + sy * win->surface->pitch;
            uint8_t* dst_row = comp_pixels + (uint32_t)dy * comp->pitch;

            for (uint32_t sx = 0; sx < win->width; sx++) {
                int32_t dx = dst_x0 + (int32_t)sx;
                if (dx < 0 || (uint32_t)dx >= comp->width) continue;
                uint8_t* sp = src_row + sx * win_bpp;
                uint8_t* dp = dst_row + (uint32_t)dx * comp_bpp_bytes;

                if (win_bpp >= 3 && comp_bpp_bytes >= 3) {
                    uint8_t src_b = sp[0];
                    uint8_t src_g = sp[1];
                    uint8_t src_r = sp[2];
                    uint8_t src_a = (win_bpp == 4) ? sp[3] : 255;

                    if (src_a == 0) continue;
                    if (src_a == 255) {
                        uint32_t n = win_bpp < comp_bpp_bytes ? win_bpp : comp_bpp_bytes;
                        for (uint32_t k = 0; k < n; k++) dp[k] = sp[k];
                        if (comp_bpp_bytes == 4 && win_bpp == 3) dp[3] = 0xFF;
                    } else {
                        uint32_t dst_b = dp[0];
                        uint32_t dst_g = dp[1];
                        uint32_t dst_r = dp[2];
                        uint32_t inv = 255 - src_a;
                        dp[0] = (src_b * src_a + dst_b * inv) / 255;
                        dp[1] = (src_g * src_a + dst_g * inv) / 255;
                        dp[2] = (src_r * src_a + dst_r * inv) / 255;
                        if (comp_bpp_bytes == 4) dp[3] = 0xFF;
                    }
                } else {
                    uint32_t n = win_bpp < comp_bpp_bytes ? win_bpp : comp_bpp_bytes;
                    for (uint32_t k = 0; k < n; k++) dp[k] = sp[k];
                    if (comp_bpp_bytes == 4 && win_bpp == 3) dp[3] = 0xFF;
                }
            }
        }
    }

    wm_draw_snap_preview(comp);

    wm_state.needs_redraw = false;
    return GRAPHICS_SUCCESS;
}

static graphics_result_t draw_window_decorations(window_t* window) {
    if (!window || !window->surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    graphics_color_t title_color = window->focused ?
        wm_state.config.title_bar_color : COLOR_GRAY;

    if (window->surface->pixels) {
        uint32_t title_pixel = graphics_color_to_pixel(title_color, window->surface->format);
        uint32_t radius = wm_state.config.corner_radius;
        wm_fill_rounded_rect(window->surface, 0, 0, window->width,
                             wm_state.config.title_bar_height, radius, title_pixel);

        if (window->focused) {
            uint32_t accent_pixel = graphics_color_to_pixel(
                graphics_make_color(80, 130, 220, 255), window->surface->format);
            for (uint32_t x = radius; x < window->width - radius && x < window->width; x++) {
                wm_surface_put_pixel(window->surface, x, wm_state.config.title_bar_height - 1, accent_pixel);
            }
        }
    }

    if (window->title[0] != '\0' && window->surface->pixels) {
        text_style_t title_style = {
            .foreground = wm_state.config.title_text_color,
            .background = title_color,
            .has_background = false,
            .underline = false,
            .strikethrough = false
        };

        font_t* font = NULL;
        font_get_system_font(&font);
        if (font) {
            int32_t text_x = 8;
            int32_t text_y = (wm_state.config.title_bar_height - 8) / 2;
            font_render_text(font, window->surface, text_x, text_y, window->title, &title_style);
        }
    }

    if (window->surface->pixels && window->width > 60) {
        uint32_t btn_size = wm_state.config.title_bar_height - 6;
        uint32_t btn_y = 3;
        uint32_t radius = 3;

        uint32_t close_x = window->width - btn_size - 4;
        graphics_color_t close_bg = graphics_make_color(200, 60, 60, 255);
        uint32_t close_pixel = graphics_color_to_pixel(close_bg, window->surface->format);
        uint32_t white_pixel = graphics_color_to_pixel(COLOR_WHITE, window->surface->format);

        wm_fill_rounded_rect(window->surface, close_x, btn_y, btn_size, btn_size,
                             radius, close_pixel);

        for (uint32_t i = 3; i < btn_size - 3; i++) {
            uint32_t y1 = btn_y + i;
            uint32_t x1 = close_x + i;
            uint32_t x2 = close_x + btn_size - 1 - i;
            wm_surface_put_pixel(window->surface, x1, y1, white_pixel);
            wm_surface_put_pixel(window->surface, x2, y1, white_pixel);
        }

        uint32_t max_x = close_x - btn_size - 2;
        graphics_color_t max_bg = graphics_make_color(100, 100, 100, 255);
        uint32_t max_pixel = graphics_color_to_pixel(max_bg, window->surface->format);

        wm_fill_rounded_rect(window->surface, max_x, btn_y, btn_size, btn_size,
                             radius, max_pixel);

        for (uint32_t i = 4; i < btn_size - 4; i++) {
            uint32_t top_y = btn_y + 4;
            uint32_t bot_y = btn_y + btn_size - 5;
            uint32_t left_x = max_x + 4;
            uint32_t right_x = max_x + btn_size - 5;
            wm_surface_put_pixel(window->surface, max_x + i, top_y, white_pixel);
            wm_surface_put_pixel(window->surface, max_x + i, bot_y, white_pixel);
            wm_surface_put_pixel(window->surface, left_x, btn_y + i, white_pixel);
            wm_surface_put_pixel(window->surface, right_x, btn_y + i, white_pixel);
        }

        uint32_t min_x = max_x - btn_size - 2;
        graphics_color_t min_bg = graphics_make_color(100, 100, 100, 255);
        uint32_t min_pixel = graphics_color_to_pixel(min_bg, window->surface->format);

        wm_fill_rounded_rect(window->surface, min_x, btn_y, btn_size, btn_size,
                             radius, min_pixel);

        uint32_t line_y = btn_y + btn_size / 2;
        for (uint32_t i = 4; i < btn_size - 4; i++) {
            wm_surface_put_pixel(window->surface, min_x + i, line_y, white_pixel);
        }
    }

    if (window->surface->pixels) {
        uint32_t border_pixel = graphics_color_to_pixel(
            window->focused ? wm_state.config.border_color : COLOR_GRAY,
            window->surface->format);
        uint32_t bw = wm_state.config.border_width;
        uint32_t rad = wm_state.config.corner_radius;

        for (uint32_t x = 0; x < window->width; x++) {
            for (uint32_t y = 0; y < bw; y++) {
                bool skip = false;
                if (x < rad && y < rad) {
                    uint32_t dx = rad - x;
                    uint32_t dy = rad - y;
                    if (dx * dx + dy * dy > rad * rad) skip = true;
                } else if (x >= window->width - rad && y < rad) {
                    uint32_t dx = x - (window->width - rad - 1);
                    uint32_t dy = rad - y;
                    if (dx * dx + dy * dy > rad * rad) skip = true;
                }
                if (!skip) wm_surface_put_pixel(window->surface, x, y, border_pixel);
            }
        }
        for (uint32_t y = wm_state.config.title_bar_height; y < window->height; y++) {
            for (uint32_t x = 0; x < bw; x++) {
                wm_surface_put_pixel(window->surface, x, y, border_pixel);
            }
            for (uint32_t x = window->width - bw; x < window->width; x++) {
                wm_surface_put_pixel(window->surface, x, y, border_pixel);
            }
        }
        for (uint32_t x = 0; x < window->width; x++) {
            for (uint32_t y = window->height - bw; y < window->height; y++) {
                wm_surface_put_pixel(window->surface, x, y, border_pixel);
            }
        }
    }

    return GRAPHICS_SUCCESS;
}

static window_t* find_window_by_handle(window_handle_t handle) {
    window_t* current = wm_state.window_list;
    while (current) {
        if (current->handle == handle) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

static graphics_result_t add_window_to_list(window_t* window) {
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    window->next = wm_state.window_list;
    wm_state.window_list = window;

    return GRAPHICS_SUCCESS;
}

static graphics_result_t remove_window_from_list(window_t* window) {
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (wm_state.window_list == window) {
        wm_state.window_list = window->next;
    } else {
        window_t* current = wm_state.window_list;
        while (current && current->next != window) {
            current = current->next;
        }
        if (current) {
            current->next = window->next;
        }
    }

    window->next = NULL;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_bring_to_front(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    wm_z_order_to_front(window);
    wm_state.needs_redraw = true;

    return GRAPHICS_SUCCESS;
}

graphics_result_t compositor_force_redraw(void) {
    if (!wm_state.wm_ready) {
        return GRAPHICS_SUCCESS;
    }
    wm_mark_full_screen_dirty();
    wm_state.needs_redraw = true;
    return compositor_update();
}

graphics_result_t compositor_enable_vsync(bool enable) {
    (void)enable;
    return GRAPHICS_SUCCESS;
}

graphics_result_t desktop_set_wallpaper(const graphics_surface_t* wallpaper) {
    if (!wm_state.initialized) return GRAPHICS_ERROR_GENERIC;

    if (wm_state.wallpaper) {
        graphics_destroy_surface(wm_state.wallpaper);
        wm_state.wallpaper = NULL;
    }

    if (!wallpaper) {
        wm_state.needs_redraw = true;
        return GRAPHICS_SUCCESS;
    }

    graphics_result_t result = graphics_create_surface(
        wallpaper->width, wallpaper->height, wallpaper->format, &wm_state.wallpaper
    );
    if (result != GRAPHICS_SUCCESS) {
        return result;
    }

    uint32_t bpp = (wallpaper->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;
    uint32_t row_bytes = wallpaper->width * bpp;
    if (row_bytes > wallpaper->pitch) row_bytes = wallpaper->pitch;
    if (row_bytes > wm_state.wallpaper->pitch) row_bytes = wm_state.wallpaper->pitch;

    for (uint32_t y = 0; y < wallpaper->height && y < wm_state.wallpaper->height; y++) {
        uint8_t* src = (uint8_t*)wallpaper->pixels + y * wallpaper->pitch;
        uint8_t* dst = (uint8_t*)wm_state.wallpaper->pixels + y * wm_state.wallpaper->pitch;
        memcpy(dst, src, row_bytes);
    }

    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t desktop_get_size(uint32_t* width, uint32_t* height) {
    if (!width || !height) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    *width  = wm_state.desktop_width;
    *height = wm_state.desktop_height;
    return GRAPHICS_SUCCESS;
}

graphics_result_t desktop_invalidate(void) {
    wm_mark_full_screen_dirty();
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_get_title(window_handle_t handle, char* title, size_t size) {
    window_t* window = find_window_by_handle(handle);
    if (!window || !title || size == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    strncpy(title, window->title, size - 1);
    title[size - 1] = '\0';
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_get_position(window_handle_t handle, int32_t* x, int32_t* y) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    if (x) *x = window->x;
    if (y) *y = window->y;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_set_size(window_handle_t handle, uint32_t width, uint32_t height) {
    window_t* window = find_window_by_handle(handle);
    if (!window || width == 0 || height == 0) return GRAPHICS_ERROR_INVALID_PARAMETER;
    if (width < window->min_width) width = window->min_width;
    if (height < window->min_height) height = window->min_height;
    if (width > window->max_width) width = window->max_width;
    if (height > window->max_height) height = window->max_height;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->width  = width;
    window->height = height;
    wm_mark_dirty_rect(window->x, window->y, width, height);
    window->dirty  = true;
    wm_state.needs_redraw = true;
    if (window->on_resize) window->on_resize(window, width, height);
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_get_size(window_handle_t handle, uint32_t* width, uint32_t* height) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    if (width)  *width  = window->width;
    if (height) *height = window->height;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_set_constraints(window_handle_t handle,
                                          uint32_t min_width, uint32_t min_height,
                                          uint32_t max_width, uint32_t max_height) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    window->min_width  = min_width;
    window->min_height = min_height;
    window->max_width  = max_width  ? max_width  : wm_state.desktop_width;
    window->max_height = max_height ? max_height : wm_state.desktop_height;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_show(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    window->visible = true;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_hide(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->visible = false;
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_minimize(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->state = WINDOW_STATE_MINIMIZED;
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_maximize(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->state  = WINDOW_STATE_MAXIMIZED;
    window->x      = 0;
    window->y      = 0;
    window->width  = wm_state.desktop_width;
    window->height = wm_state.desktop_height;
    window->dirty  = true;
    wm_mark_dirty_rect(0, 0, window->width, window->height);
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_restore(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->state = WINDOW_STATE_NORMAL;
    window->dirty = true;
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_set_fullscreen(window_handle_t handle, bool fullscreen) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    window->state = fullscreen ? WINDOW_STATE_FULLSCREEN : WINDOW_STATE_NORMAL;
    window->dirty = true;
    if (fullscreen) {
        wm_mark_dirty_rect(0, 0, wm_state.desktop_width, wm_state.desktop_height);
    } else {
        wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    }
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_send_to_back(window_handle_t handle) {
    window_t* window = find_window_by_handle(handle);
    if (!window) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_mark_dirty_rect(window->x, window->y, window->width, window->height);
    wm_z_order_to_back(window);
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_enumerate(window_handle_t* handles, uint32_t* count) {
    if (!count) return GRAPHICS_ERROR_INVALID_PARAMETER;
    uint32_t n = 0;
    for (uint32_t i = 0; i < wm_state.z_order_count; i++) {
        window_t* w = wm_state.z_order_array[i];
        if (handles && n < *count) handles[n] = w->handle;
        n++;
    }
    *count = n;
    return GRAPHICS_SUCCESS;
}

window_handle_t window_find_by_title(const char* title) {
    if (!title) return INVALID_WINDOW_HANDLE;
    window_t* current = wm_state.window_list;
    while (current) {
        if (strcmp(current->title, title) == 0) return current->handle;
        current = current->next;
    }
    return INVALID_WINDOW_HANDLE;
}

window_handle_t window_at_point(int32_t x, int32_t y) {
    for (uint32_t i = wm_state.z_order_count; i > 0; i--) {
        window_t* current = wm_state.z_order_array[i - 1];
        if (current->visible && current->state != WINDOW_STATE_MINIMIZED &&
            x >= current->x && x < current->x + (int32_t)current->width &&
            y >= current->y && y < current->y + (int32_t)current->height) {
            return current->handle;
        }
    }
    return INVALID_WINDOW_HANDLE;
}

graphics_result_t window_manager_get_config(window_manager_config_t* config) {
    if (!config) return GRAPHICS_ERROR_INVALID_PARAMETER;
    *config = wm_state.config;
    return GRAPHICS_SUCCESS;
}

graphics_result_t window_manager_set_config(const window_manager_config_t* config) {
    if (!config) return GRAPHICS_ERROR_INVALID_PARAMETER;
    wm_state.config = *config;
    wm_state.needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

#else /* !HAS_GRAPHICS */

/* No-framebuffer window manager stubs. Windowing requires a framebuffer;
 * without one all calls report not-supported and return INVALID_WINDOW_HANDLE. */

graphics_result_t window_manager_init(void)                              { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_manager_shutdown(void)                          { return GRAPHICS_SUCCESS; }
bool window_manager_is_initialized(void)                                 { return false; }
window_handle_t window_create(int32_t x, int32_t y, uint32_t w, uint32_t h, const char* t, uint32_t f) {
    (void)x; (void)y; (void)w; (void)h; (void)t; (void)f; return INVALID_WINDOW_HANDLE;
}
graphics_result_t window_destroy(window_handle_t h)                      { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
window_t* window_get(window_handle_t h)                                  { (void)h; return NULL; }
graphics_result_t window_set_title(window_handle_t h, const char* t)     { (void)h; (void)t; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_get_title(window_handle_t h, char* t, size_t s) { (void)h; (void)t; (void)s; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_position(window_handle_t h, int32_t x, int32_t y) { (void)h; (void)x; (void)y; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_get_position(window_handle_t h, int32_t* x, int32_t* y) { (void)h; if (x) *x=0; if (y) *y=0; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_size(window_handle_t h, uint32_t w, uint32_t hh) { (void)h; (void)w; (void)hh; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_get_size(window_handle_t h, uint32_t* w, uint32_t* hh) { (void)h; if (w) *w=0; if (hh) *hh=0; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_constraints(window_handle_t h, uint32_t mw, uint32_t mh, uint32_t Mw, uint32_t Mh) { (void)h; (void)mw; (void)mh; (void)Mw; (void)Mh; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_show(window_handle_t h)                         { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_hide(window_handle_t h)                         { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_minimize(window_handle_t h)                     { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_maximize(window_handle_t h)                     { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_restore(window_handle_t h)                      { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_fullscreen(window_handle_t h, bool f)       { (void)h; (void)f; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_focus(window_handle_t h)                        { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
window_handle_t window_get_focused(void)                                 { return INVALID_WINDOW_HANDLE; }
graphics_result_t window_bring_to_front(window_handle_t h)               { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_send_to_back(window_handle_t h)                 { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_paint_callback(window_handle_t h, void (*c)(window_t*, graphics_surface_t*)) { (void)h; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_resize_callback(window_handle_t h, void (*c)(window_t*, uint32_t, uint32_t)) { (void)h; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_set_input_callback(window_handle_t h, void (*c)(window_t*, const input_event_t*)) { (void)h; (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_invalidate(window_handle_t h)                   { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_invalidate_rect(window_handle_t h, const graphics_rect_t* r) { (void)h; (void)r; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_get_surface(window_handle_t h, graphics_surface_t** s) { (void)h; if (s) *s=NULL; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_present(window_handle_t h)                      { (void)h; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_enumerate(window_handle_t* h, uint32_t* c)      { if (h) *h=INVALID_WINDOW_HANDLE; if (c) *c=0; return GRAPHICS_SUCCESS; }
window_handle_t window_find_by_title(const char* t)                      { (void)t; return INVALID_WINDOW_HANDLE; }
window_handle_t window_at_point(int32_t x, int32_t y)                    { (void)x; (void)y; return INVALID_WINDOW_HANDLE; }
graphics_result_t compositor_update(void)                                { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t compositor_force_redraw(void)                          { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t compositor_enable_vsync(bool e)                        { (void)e; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t wm_render_loop_tick(void)                              { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t wm_update_cursor(int32_t x, int32_t y)                 { (void)x; (void)y; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t wm_enable_cursor(bool v)                               { (void)v; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t wm_handle_input_event(const input_event_t* e)          { (void)e; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t desktop_set_wallpaper(const graphics_surface_t* w)     { (void)w; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t desktop_get_size(uint32_t* w, uint32_t* h)             { if (w) *w=0; if (h) *h=0; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t desktop_invalidate(void)                               { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_manager_get_config(window_manager_config_t* c)  { if (c) memset(c,0,sizeof(*c)); return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t window_manager_set_config(const window_manager_config_t* c) { (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }

#endif /* HAS_GRAPHICS */
