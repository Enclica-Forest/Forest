#include "../include/graphics/app_graphics.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/window_manager.h"
#include "../include/graphics/font_renderer.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/libc/stdlib.h"
#include "../include/debuglog.h"
#include "../include/mm.h"

#define APP_DEFAULT_BG_R 45
#define APP_DEFAULT_BG_G 45
#define APP_DEFAULT_BG_B 55

static struct {
    bool initialized;
    bool subsystems_ready;
    bool pending_init;
    bool main_loop_running;
    app_graphics_context_t* context_list;
    uint32_t context_count;
} app_graphics_state = {
    .initialized = false,
    .subsystems_ready = false,
    .pending_init = false,
    .main_loop_running = false,
    .context_list = NULL,
    .context_count = 0
};

static void app_window_paint_callback(window_t* window, graphics_surface_t* surface);
static void app_window_resize_callback(window_t* window, uint32_t new_width, uint32_t new_height);
static void app_window_close_callback(window_t* window);
static void app_window_input_callback(window_t* window, const input_event_t* event);
static app_graphics_context_t* find_context_by_window(window_handle_t handle);
static graphics_result_t add_context_to_list(app_graphics_context_t* ctx);
static graphics_result_t remove_context_from_list(app_graphics_context_t* ctx);
static bool point_in_clip_rect(app_graphics_context_t* ctx, int32_t x, int32_t y);
static graphics_result_t fill_surface_solid(graphics_surface_t* surface, graphics_color_t color);
static graphics_result_t ensure_graphics_ready(app_graphics_context_t* ctx);
static app_graphics_context_t* create_standalone_window(const app_window_params_t* params);

static void app_window_input_callback(window_t* window, const input_event_t* event) {
    (void)window;
    (void)event;
}

static void fill_surface_default_background(graphics_surface_t* surface) {
    if (!surface || !surface->pixels) return;
    graphics_color_t bg = {APP_DEFAULT_BG_R, APP_DEFAULT_BG_G, APP_DEFAULT_BG_B, 255};
    fill_surface_solid(surface, bg);
}

graphics_result_t app_graphics_init(void) {
    debuglog(DEBUG_INFO, "Initializing application graphics API...\n");

    if (app_graphics_state.initialized) {
        if (!app_graphics_state.subsystems_ready) {
            app_graphics_try_complete_init();
        }
        return GRAPHICS_SUCCESS;
    }

    app_graphics_state.initialized = true;
    app_graphics_state.main_loop_running = false;
    app_graphics_state.context_list = NULL;
    app_graphics_state.context_count = 0;
    app_graphics_state.subsystems_ready = false;
    app_graphics_state.pending_init = false;

    app_graphics_try_complete_init();

    debuglog(DEBUG_INFO, "Application graphics API initialized\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_graphics_try_complete_init(void) {
    if (app_graphics_state.subsystems_ready) {
        return GRAPHICS_SUCCESS;
    }

    bool gm_ok = graphics_is_initialized();
    bool wm_ok = window_manager_is_initialized();
    bool fr_ok = font_renderer_is_initialized();

    if (wm_ok) {
        app_graphics_state.subsystems_ready = true;
        app_graphics_state.pending_init = false;
        debuglog(DEBUG_INFO, "app_graphics: all subsystems ready\n");

        app_graphics_context_t* ctx = app_graphics_state.context_list;
        while (ctx) {
            if (ctx->pending_wm_reconnect) {
                window_handle_t handle = window_create(
                    ctx->pending_x, ctx->pending_y,
                    ctx->pending_width, ctx->pending_height,
                    ctx->pending_title, ctx->pending_flags);
                if (handle != INVALID_WINDOW_HANDLE) {
                    ctx->window_handle = handle;
                    ctx->window = window_get(handle);
                    if (ctx->window && window_get_surface(handle, &ctx->surface) == GRAPHICS_SUCCESS) {
                        ctx->pending_wm_reconnect = false;
                        fill_surface_default_background(ctx->surface);
                        ctx->valid = true;
                        ctx->needs_redraw = true;
                        window_set_paint_callback(handle, app_window_paint_callback);
                        window_set_resize_callback(handle, app_window_resize_callback);
                        ctx->window->on_close = app_window_close_callback;
                        ctx->window->on_input = app_window_input_callback;
                        add_context_to_list(ctx);
                        debuglog(DEBUG_INFO, "app_graphics: deferred window '%s' created\n", ctx->pending_title);
                    }
                }
            }
            ctx = (app_graphics_context_t*)ctx->user_data;
        }
        return GRAPHICS_SUCCESS;
    }

    if (!gm_ok) {
        debuglog(DEBUG_WARN, "app_graphics: graphics manager not ready\n");
    }
    if (!wm_ok) {
        debuglog(DEBUG_WARN, "app_graphics: window manager not ready\n");
    }
    if (!fr_ok) {
        debuglog(DEBUG_WARN, "app_graphics: font renderer not ready\n");
    }

    app_graphics_state.pending_init = true;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

bool app_graphics_are_subsystems_ready(void) {
    return app_graphics_state.subsystems_ready;
}

graphics_result_t app_graphics_shutdown(void) {
    if (!app_graphics_state.initialized) {
        return GRAPHICS_SUCCESS;
    }

    debuglog(DEBUG_INFO, "Shutting down application graphics API...\n");

    app_graphics_cleanup();

    app_graphics_state.initialized = false;
    app_graphics_state.main_loop_running = false;
    app_graphics_state.context_list = NULL;
    app_graphics_state.context_count = 0;
    app_graphics_state.subsystems_ready = false;
    app_graphics_state.pending_init = false;

    debuglog(DEBUG_INFO, "Application graphics API shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_graphics_cleanup(void) {
    app_graphics_context_t* ctx = app_graphics_state.context_list;
    while (ctx) {
        app_graphics_context_t* next = (app_graphics_context_t*)ctx->user_data;
        if (ctx->valid && ctx->window_handle != INVALID_WINDOW_HANDLE) {
            if (ctx->on_close) {
                ctx->on_close(ctx);
            }
            if (window_manager_is_initialized()) {
                window_destroy(ctx->window_handle);
            }
        } else {
            if (ctx->on_close) {
                ctx->on_close(ctx);
            }
        }
        ctx->valid = false;
        ctx->window_handle = INVALID_WINDOW_HANDLE;
        ctx->window = NULL;
        ctx->surface = NULL;
        ctx = next;
    }
    app_graphics_state.context_list = NULL;
    app_graphics_state.context_count = 0;
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_graphics_reconnect_wm(void) {
    debuglog(DEBUG_INFO, "app_graphics: attempting WM reconnect...\n");

    if (!window_manager_is_initialized()) {
        graphics_result_t result = window_manager_init();
        if (result != GRAPHICS_SUCCESS) {
            debuglog(DEBUG_WARN, "app_graphics: WM reconnect failed: %s\n",
                     graphics_get_error_string(result));
            return result;
        }
    }

    app_graphics_state.subsystems_ready = true;
    app_graphics_state.pending_init = false;

    app_graphics_context_t* ctx = app_graphics_state.context_list;
    while (ctx) {
        if (ctx->pending_wm_reconnect) {
            window_handle_t handle = window_create(
                ctx->pending_x, ctx->pending_y,
                ctx->pending_width, ctx->pending_height,
                ctx->pending_title, ctx->pending_flags);
            if (handle != INVALID_WINDOW_HANDLE) {
                ctx->window_handle = handle;
                ctx->window = window_get(handle);
                if (ctx->window && window_get_surface(handle, &ctx->surface) == GRAPHICS_SUCCESS) {
                    ctx->pending_wm_reconnect = false;
                    fill_surface_default_background(ctx->surface);
                    ctx->valid = true;
                    ctx->needs_redraw = true;
                    window_set_paint_callback(handle, app_window_paint_callback);
                    window_set_resize_callback(handle, app_window_resize_callback);
                    ctx->window->on_close = app_window_close_callback;
                    ctx->window->on_input = app_window_input_callback;
                    add_context_to_list(ctx);
                    debuglog(DEBUG_INFO, "app_graphics: reconnected window '%s'\n", ctx->pending_title);
                }
            }
        }
        ctx = (app_graphics_context_t*)ctx->user_data;
    }

    return GRAPHICS_SUCCESS;
}

bool app_graphics_is_initialized(void) {
    return app_graphics_state.initialized;
}

static graphics_result_t ensure_graphics_ready(app_graphics_context_t* ctx) {
    (void)ctx;
    if (!app_graphics_state.subsystems_ready) {
        app_graphics_try_complete_init();
    }
    return app_graphics_state.subsystems_ready ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_NOT_SUPPORTED;
}

static graphics_result_t fill_surface_solid(graphics_surface_t* surface, graphics_color_t color) {
    if (!surface || !surface->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    uint32_t pixel_value = graphics_color_to_pixel(color, surface->format);

    switch (surface->format) {
        case PIXEL_FORMAT_RGB_565:
        case PIXEL_FORMAT_RGB_555: {
            uint16_t* pixels = (uint16_t*)surface->pixels;
            uint32_t pixel_count = (surface->pitch / 2) * surface->height;
            for (uint32_t i = 0; i < pixel_count; i++) {
                pixels[i] = (uint16_t)pixel_value;
            }
            break;
        }
        case PIXEL_FORMAT_RGB_888:
        case PIXEL_FORMAT_BGR_888: {
            uint8_t* pixels = (uint8_t*)surface->pixels;
            for (uint32_t y = 0; y < surface->height; y++) {
                uint8_t* row = pixels + y * surface->pitch;
                for (uint32_t x = 0; x < surface->width; x++) {
                    row[x * 3 + 0] = (pixel_value >> 0) & 0xFF;
                    row[x * 3 + 1] = (pixel_value >> 8) & 0xFF;
                    row[x * 3 + 2] = (pixel_value >> 16) & 0xFF;
                }
            }
            break;
        }
        case PIXEL_FORMAT_RGBA_8888:
        case PIXEL_FORMAT_BGRA_8888: {
            uint32_t* pixels = (uint32_t*)surface->pixels;
            uint32_t pixel_count = (surface->pitch / 4) * surface->height;
            for (uint32_t i = 0; i < pixel_count; i++) {
                pixels[i] = pixel_value;
            }
            break;
        }
        default:
            return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    return GRAPHICS_SUCCESS;
}

static app_graphics_context_t* create_standalone_window(const app_window_params_t* params) {
    if (!graphics_is_initialized()) {
        return NULL;
    }

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return NULL;
    }

    video_mode_t mode;
    if (graphics_get_current_mode(&mode) != GRAPHICS_SUCCESS) {
        return NULL;
    }

    app_graphics_context_t* ctx = kmalloc(sizeof(app_graphics_context_t));
    if (!ctx) {
        return NULL;
    }
    memset(ctx, 0, sizeof(app_graphics_context_t));

    graphics_surface_t* surface = kmalloc(sizeof(graphics_surface_t));
    if (!surface) {
        kfree(ctx);
        return NULL;
    }

    surface->width = params->width;
    surface->height = params->height;
    surface->format = mode.format;
    uint32_t bpp_bytes = 4;
    switch (mode.format) {
        case PIXEL_FORMAT_RGB_565:
        case PIXEL_FORMAT_RGB_555:
            bpp_bytes = 2;
            break;
        case PIXEL_FORMAT_RGB_888:
        case PIXEL_FORMAT_BGR_888:
            bpp_bytes = 3;
            break;
        case PIXEL_FORMAT_RGBA_8888:
        case PIXEL_FORMAT_BGRA_8888:
        default:
            bpp_bytes = 4;
            break;
    }
    surface->bpp = bpp_bytes * 8;
    surface->pitch = surface->width * bpp_bytes;

    uint32_t surface_size = surface->pitch * surface->height;
    surface->pixels = kmalloc(surface_size);
    if (!surface->pixels) {
        kfree(surface);
        kfree(ctx);
        return NULL;
    }
    memset(surface->pixels, 0, surface_size);

    ctx->window_handle = INVALID_WINDOW_HANDLE;
    ctx->window = NULL;
    ctx->surface = surface;
    ctx->foreground_color = COLOR_WHITE;
    ctx->background_color = (graphics_color_t){APP_DEFAULT_BG_R, APP_DEFAULT_BG_G, APP_DEFAULT_BG_B, 255};
    ctx->draw_mode = DRAW_MODE_IMMEDIATE;
    ctx->clipping_enabled = false;
    ctx->clip_rect.x = 0;
    ctx->clip_rect.y = 0;
    ctx->clip_rect.width = params->width;
    ctx->clip_rect.height = params->height;
    ctx->transform.xx = 1.0f;
    ctx->transform.xy = 0.0f;
    ctx->transform.yx = 0.0f;
    ctx->transform.yy = 1.0f;
    ctx->transform.x0 = 0.0f;
    ctx->transform.y0 = 0.0f;
    font_get_system_font(&ctx->current_font);
    ctx->text_style = (text_style_t)DEFAULT_TEXT_STYLE;
    ctx->on_paint = params->on_paint;
    ctx->on_resize = params->on_resize;
    ctx->on_close = params->on_close;
    ctx->on_key = params->on_key;
    ctx->on_mouse = params->on_mouse;
    ctx->user_data = params->user_data;
    ctx->needs_redraw = true;
    ctx->valid = true;
    ctx->pending_wm_reconnect = false;

    fill_surface_default_background(surface);

    debuglog(DEBUG_INFO, "Created standalone window '%s' (%ux%u)\n",
            params->title, params->width, params->height);
    return ctx;
}

app_graphics_context_t* app_create_window(const app_window_params_t* params) {
    if (!params) {
        debuglog(DEBUG_ERROR, "Invalid parameters for window creation\n");
        return NULL;
    }

    if (!app_graphics_state.initialized) {
        debuglog(DEBUG_ERROR, "app_graphics not initialized\n");
        return NULL;
    }

    if (!app_graphics_state.subsystems_ready) {
        app_graphics_try_complete_init();
    }

    if (window_manager_is_initialized()) {
        window_handle_t handle = window_create(params->x, params->y, params->width, params->height,
                                              params->title, params->flags);
        if (handle == INVALID_WINDOW_HANDLE) {
            debuglog(DEBUG_WARN, "WM window_create failed, trying standalone\n");
            app_graphics_context_t* ctx = create_standalone_window(params);
            if (ctx) {
                add_context_to_list(ctx);
            }
            return ctx;
        }

        app_graphics_context_t* ctx = kmalloc(sizeof(app_graphics_context_t));
        if (!ctx) {
            debuglog(DEBUG_ERROR, "Failed to allocate memory for graphics context\n");
            window_destroy(handle);
            return NULL;
        }

        memset(ctx, 0, sizeof(app_graphics_context_t));

        ctx->window_handle = handle;
        ctx->window = window_get(handle);
        if (!ctx->window) {
            debuglog(DEBUG_ERROR, "Failed to get window object\n");
            window_destroy(handle);
            kfree(ctx);
            return NULL;
        }

        if (window_get_surface(handle, &ctx->surface) != GRAPHICS_SUCCESS) {
            debuglog(DEBUG_ERROR, "Failed to get window surface\n");
            window_destroy(handle);
            kfree(ctx);
            return NULL;
        }

        ctx->foreground_color = COLOR_WHITE;
        ctx->background_color = (graphics_color_t){APP_DEFAULT_BG_R, APP_DEFAULT_BG_G, APP_DEFAULT_BG_B, 255};
        ctx->draw_mode = DRAW_MODE_IMMEDIATE;
        ctx->clipping_enabled = false;
        ctx->clip_rect.x = 0;
        ctx->clip_rect.y = 0;
        ctx->clip_rect.width = params->width;
        ctx->clip_rect.height = params->height;
        ctx->transform.xx = 1.0f;
        ctx->transform.xy = 0.0f;
        ctx->transform.yx = 0.0f;
        ctx->transform.yy = 1.0f;
        ctx->transform.x0 = 0.0f;
        ctx->transform.y0 = 0.0f;
        font_get_system_font(&ctx->current_font);
        ctx->text_style = (text_style_t)DEFAULT_TEXT_STYLE;
        ctx->on_paint = params->on_paint;
        ctx->on_resize = params->on_resize;
        ctx->on_close = params->on_close;
        ctx->on_key = params->on_key;
        ctx->on_mouse = params->on_mouse;
        ctx->user_data = params->user_data;
        ctx->needs_redraw = true;
        ctx->valid = true;
        ctx->pending_wm_reconnect = false;

        fill_surface_default_background(ctx->surface);

        window_set_paint_callback(handle, app_window_paint_callback);
        window_set_resize_callback(handle, app_window_resize_callback);
        ctx->window->on_close = app_window_close_callback;
        ctx->window->on_input = app_window_input_callback;

        add_context_to_list(ctx);

        debuglog(DEBUG_INFO, "Created application window '%s' (%ux%u)\n",
                params->title, params->width, params->height);
        return ctx;
    }

    debuglog(DEBUG_WARN, "WM unavailable, creating standalone window\n");
    app_graphics_context_t* ctx = create_standalone_window(params);
    if (ctx) {
        add_context_to_list(ctx);
    }
    return ctx;
}

graphics_result_t app_destroy_window(app_graphics_context_t* ctx) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    debuglog(DEBUG_INFO, "Destroying application window\n");

    if (ctx->on_close) {
        ctx->on_close(ctx);
    }

    remove_context_from_list(ctx);

    if (ctx->window_handle != INVALID_WINDOW_HANDLE && window_manager_is_initialized()) {
        window_destroy(ctx->window_handle);
    }

    if (ctx->surface && ctx->window_handle == INVALID_WINDOW_HANDLE) {
        if (ctx->surface->pixels) {
            kfree(ctx->surface->pixels);
        }
        kfree(ctx->surface);
    }

    ctx->valid = false;
    ctx->window_handle = INVALID_WINDOW_HANDLE;
    ctx->window = NULL;
    ctx->surface = NULL;

    kfree(ctx);

    return GRAPHICS_SUCCESS;
}

graphics_result_t app_begin_drawing(app_graphics_context_t* ctx) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (!app_graphics_state.subsystems_ready) {
        ensure_graphics_ready(ctx);
    }

    if (!ctx->surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t app_end_drawing(app_graphics_context_t* ctx) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (ctx->draw_mode == DRAW_MODE_BUFFERED) {
        return app_flush_drawing(ctx);
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t app_flush_drawing(app_graphics_context_t* ctx) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (ctx->window_handle != INVALID_WINDOW_HANDLE && window_manager_is_initialized()) {
        window_present(ctx->window_handle);
        compositor_update();
    } else if (ctx->surface && graphics_is_initialized()) {
        framebuffer_t* fb = graphics_get_framebuffer();
        if (fb && fb->virtual_addr) {
            video_mode_t mode;
            if (graphics_get_current_mode(&mode) == GRAPHICS_SUCCESS) {
                uint32_t bpp_bytes = (fb->bpp + 7) / 8;
                if (bpp_bytes == 0) bpp_bytes = 4;
                uint32_t src_bpp = (ctx->surface->bpp + 7) / 8;
                if (src_bpp == 0) src_bpp = 4;

                uint32_t copy_w = ctx->surface->width;
                uint32_t copy_h = ctx->surface->height;
                if (copy_w > fb->width) copy_w = fb->width;
                if (copy_h > fb->height) copy_h = fb->height;

                for (uint32_t y = 0; y < copy_h; y++) {
                    uint8_t* src_row = (uint8_t*)ctx->surface->pixels + y * ctx->surface->pitch;
                    uint8_t* dst_row = (uint8_t*)fb->virtual_addr + y * fb->pitch;
                    memcpy(dst_row, src_row, copy_w * (src_bpp < bpp_bytes ? src_bpp : bpp_bytes));
                }
            }
        }
    }

    ctx->needs_redraw = false;
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_set_foreground_color(app_graphics_context_t* ctx, graphics_color_t color) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    ctx->foreground_color = color;
    ctx->text_style.foreground = color;

    return GRAPHICS_SUCCESS;
}

graphics_result_t app_set_background_color(app_graphics_context_t* ctx, graphics_color_t color) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    ctx->background_color = color;
    ctx->text_style.background = color;

    return GRAPHICS_SUCCESS;
}

graphics_result_t app_clear(app_graphics_context_t* ctx) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    return app_clear_with_color(ctx, ctx->background_color);
}

graphics_result_t app_clear_with_color(app_graphics_context_t* ctx, graphics_color_t color) {
    if (!ctx || !ctx->valid || !ctx->surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    return fill_surface_solid(ctx->surface, color);
}

graphics_result_t app_draw_pixel(app_graphics_context_t* ctx, int32_t x, int32_t y) {
    return app_draw_pixel_with_color(ctx, x, y, ctx->foreground_color);
}

graphics_result_t app_draw_pixel_with_color(app_graphics_context_t* ctx, int32_t x, int32_t y, graphics_color_t color) {
    if (!ctx || !ctx->valid || !ctx->surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (ctx->clipping_enabled && !point_in_clip_rect(ctx, x, y)) {
        return GRAPHICS_SUCCESS;
    }

    if (x < 0 || x >= (int32_t)ctx->surface->width || y < 0 || y >= (int32_t)ctx->surface->height) {
        return GRAPHICS_SUCCESS;
    }

    uint32_t pixel_value = graphics_color_to_pixel(color, ctx->surface->format);

    switch (ctx->surface->format) {
        case PIXEL_FORMAT_RGB_565:
        case PIXEL_FORMAT_RGB_555: {
            uint16_t* pixels = (uint16_t*)ctx->surface->pixels;
            pixels[y * (ctx->surface->pitch / 2) + x] = (uint16_t)pixel_value;
            break;
        }
        case PIXEL_FORMAT_RGB_888:
        case PIXEL_FORMAT_BGR_888: {
            uint8_t* pixels = (uint8_t*)ctx->surface->pixels;
            uint8_t* row = pixels + y * ctx->surface->pitch;
            row[x * 3 + 0] = (pixel_value >> 0) & 0xFF;
            row[x * 3 + 1] = (pixel_value >> 8) & 0xFF;
            row[x * 3 + 2] = (pixel_value >> 16) & 0xFF;
            break;
        }
        case PIXEL_FORMAT_RGBA_8888:
        case PIXEL_FORMAT_BGRA_8888: {
            uint32_t* pixels = (uint32_t*)ctx->surface->pixels;
            pixels[y * (ctx->surface->pitch / 4) + x] = pixel_value;
            break;
        }
        default:
            return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    ctx->needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_draw_rect(app_graphics_context_t* ctx, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    graphics_rect_t rect = {x, y, width, height};
    return graphics_draw_rect(&rect, ctx->foreground_color, false);
}

graphics_result_t app_fill_rect(app_graphics_context_t* ctx, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    for (uint32_t dy = 0; dy < height; dy++) {
        for (uint32_t dx = 0; dx < width; dx++) {
            app_draw_pixel_with_color(ctx, x + dx, y + dy, ctx->foreground_color);
        }
    }

    ctx->needs_redraw = true;
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_draw_line(app_graphics_context_t* ctx, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    int32_t dx = abs(x2 - x1);
    int32_t dy = abs(y2 - y1);
    int32_t x_inc = (x1 < x2) ? 1 : -1;
    int32_t y_inc = (y1 < y2) ? 1 : -1;
    int32_t error = dx - dy;

    int32_t x = x1;
    int32_t y = y1;

    while (true) {
        app_draw_pixel(ctx, x, y);

        if (x == x2 && y == y2) {
            break;
        }

        int32_t error2 = error * 2;
        if (error2 > -dy) {
            error -= dy;
            x += x_inc;
        }
        if (error2 < dx) {
            error += dx;
            y += y_inc;
        }
    }

    return GRAPHICS_SUCCESS;
}

graphics_result_t app_set_font(app_graphics_context_t* ctx, font_t* font) {
    if (!ctx || !ctx->valid || !font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    ctx->current_font = font;
    return GRAPHICS_SUCCESS;
}

graphics_result_t app_draw_text(app_graphics_context_t* ctx, int32_t x, int32_t y, const char* text) {
    if (!ctx || !ctx->valid || !text || !ctx->current_font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    return font_render_text(ctx->current_font, ctx->surface, x, y, text, &ctx->text_style);
}

graphics_result_t app_measure_text(app_graphics_context_t* ctx, const char* text, uint32_t* width, uint32_t* height) {
    if (!ctx || !ctx->valid || !text || !width || !height || !ctx->current_font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    return font_measure_text(ctx->current_font, text, width, height);
}

graphics_result_t app_invalidate_window(app_graphics_context_t* ctx) {
    if (!ctx || !ctx->valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    ctx->needs_redraw = true;
    if (ctx->window_handle != INVALID_WINDOW_HANDLE && window_manager_is_initialized()) {
        return window_invalidate(ctx->window_handle);
    }
    return GRAPHICS_SUCCESS;
}

static void app_window_paint_callback(window_t* window, graphics_surface_t* surface) {
    (void)surface;
    if (!window) {
        return;
    }

    app_graphics_context_t* ctx = find_context_by_window(window->handle);
    if (ctx && ctx->on_paint) {
        ctx->on_paint(ctx);
    }
}

static void app_window_resize_callback(window_t* window, uint32_t new_width, uint32_t new_height) {
    if (!window) {
        return;
    }

    app_graphics_context_t* ctx = find_context_by_window(window->handle);
    if (ctx) {
        if (!ctx->clipping_enabled) {
            ctx->clip_rect.width = new_width;
            ctx->clip_rect.height = new_height;
        }

        if (ctx->on_resize) {
            ctx->on_resize(ctx, new_width, new_height);
        }
    }
}

static void app_window_close_callback(window_t* window) {
    if (!window) {
        return;
    }

    app_graphics_context_t* ctx = find_context_by_window(window->handle);
    if (ctx && ctx->on_close) {
        ctx->on_close(ctx);
    }
}

static app_graphics_context_t* find_context_by_window(window_handle_t handle) {
    app_graphics_context_t* current = app_graphics_state.context_list;
    while (current) {
        if (current->window_handle == handle) {
            return current;
        }
        current = (app_graphics_context_t*)current->user_data;
    }
    return NULL;
}

static graphics_result_t add_context_to_list(app_graphics_context_t* ctx) {
    if (!ctx) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    ctx->user_data = app_graphics_state.context_list;
    app_graphics_state.context_list = ctx;
    app_graphics_state.context_count++;

    return GRAPHICS_SUCCESS;
}

static graphics_result_t remove_context_from_list(app_graphics_context_t* ctx) {
    if (!ctx) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (app_graphics_state.context_list == ctx) {
        app_graphics_state.context_list = (app_graphics_context_t*)ctx->user_data;
    } else {
        app_graphics_context_t* current = app_graphics_state.context_list;
        while (current && (app_graphics_context_t*)current->user_data != ctx) {
            current = (app_graphics_context_t*)current->user_data;
        }
        if (current) {
            current->user_data = ctx->user_data;
        }
    }

    app_graphics_state.context_count--;
    return GRAPHICS_SUCCESS;
}

static bool point_in_clip_rect(app_graphics_context_t* ctx, int32_t x, int32_t y) {
    if (!ctx) {
        return false;
    }

    return (x >= ctx->clip_rect.x &&
            x < ctx->clip_rect.x + (int32_t)ctx->clip_rect.width &&
            y >= ctx->clip_rect.y &&
            y < ctx->clip_rect.y + (int32_t)ctx->clip_rect.height);
}
