#include "../include/graphics/multi_monitor.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/window_manager.h"
#include "../include/debuglog.h"
#include "../include/mm.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/spinlock.h"
#include "../include/timer.h"

#define GFP_KERNEL 0x01

static multi_monitor_state_t g_multi_monitor = {0};
static bool g_multi_monitor_initialized = false;
static spinlock_t g_multi_monitor_lock;

static void mm_update_totals(void);
static graphics_result_t mm_setup_default_monitor(void);
static bool mm_id_exists(uint32_t id);

graphics_result_t multi_monitor_init(void) {
    if (g_multi_monitor_initialized) {
        return GRAPHICS_SUCCESS;
    }

    debuglog(DEBUG_INFO, "Initializing Multi-Monitor subsystem...\n");

    spinlock_init(&g_multi_monitor_lock, "multi_monitor");

    memset(&g_multi_monitor, 0, sizeof(multi_monitor_state_t));

    g_multi_monitor.virtual_x_offset = 0;
    g_multi_monitor.virtual_y_offset = 0;

    graphics_result_t result = mm_setup_default_monitor();
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "Failed to setup default monitor\n");
        return result;
    }

    g_multi_monitor_initialized = true;

    debuglog(DEBUG_INFO, "Multi-Monitor initialized: %u monitor(s), virtual desktop %ux%u\n",
             g_multi_monitor.num_monitors, g_multi_monitor.total_width, g_multi_monitor.total_height);

    return GRAPHICS_SUCCESS;
}

graphics_result_t multi_monitor_shutdown(void) {
    if (!g_multi_monitor_initialized) {
        return GRAPHICS_SUCCESS;
    }

    spin_lock(&g_multi_monitor_lock);

    debuglog(DEBUG_INFO, "Shutting down Multi-Monitor subsystem...\n");

    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        monitor_info_t* mon = &g_multi_monitor.monitors[i];
        if (mon->framebuffer) {
            if (mon->framebuffer->virtual_addr) {
                kfree((void*)mon->framebuffer->virtual_addr);
            }
            kfree(mon->framebuffer);
            mon->framebuffer = NULL;
        }
        mon->active = false;
    }

    g_multi_monitor.num_monitors = 0;
    g_multi_monitor.total_width = 0;
    g_multi_monitor.total_height = 0;

    g_multi_monitor_initialized = false;

    spin_unlock(&g_multi_monitor_lock);

    debuglog(DEBUG_INFO, "Multi-Monitor shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

bool multi_monitor_is_initialized(void) {
    return g_multi_monitor_initialized;
}

graphics_result_t multi_monitor_get_monitor(uint32_t index, monitor_info_t** info) {
    if (!g_multi_monitor_initialized || !info) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    if (index >= g_multi_monitor.num_monitors) {
        spin_unlock(&g_multi_monitor_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    *info = &g_multi_monitor.monitors[index];

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t multi_monitor_get_primary(monitor_info_t** info) {
    if (!g_multi_monitor_initialized || !info) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].primary && g_multi_monitor.monitors[i].active) {
            *info = &g_multi_monitor.monitors[i];
            spin_unlock(&g_multi_monitor_lock);
            return GRAPHICS_SUCCESS;
        }
    }

    *info = NULL;
    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t multi_monitor_get_total_size(uint32_t* width, uint32_t* height) {
    if (!g_multi_monitor_initialized || !width || !height) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    *width = g_multi_monitor.total_width;
    *height = g_multi_monitor.total_height;

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_SUCCESS;
}

uint32_t multi_monitor_point_to_monitor(int32_t x, int32_t y) {
    if (!g_multi_monitor_initialized) {
        return 0;
    }

    spin_lock(&g_multi_monitor_lock);

    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        monitor_info_t* mon = &g_multi_monitor.monitors[i];
        if (!mon->active) continue;

        int32_t mx = (int32_t)mon->x;
        int32_t my = (int32_t)mon->y;
        int32_t mw = (int32_t)mon->width;
        int32_t mh = (int32_t)mon->height;

        if (x >= mx && x < mx + mw && y >= my && y < my + mh) {
            spin_unlock(&g_multi_monitor_lock);
            return mon->id;
        }
    }

    spin_unlock(&g_multi_monitor_lock);
    return 0;
}

graphics_result_t multi_monitor_move_window_to(window_handle_t handle, uint32_t monitor_id) {
    if (!g_multi_monitor_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    window_t* window = window_get(handle);
    if (!window) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    monitor_info_t* target = NULL;
    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].id == monitor_id && g_multi_monitor.monitors[i].active) {
            target = &g_multi_monitor.monitors[i];
            break;
        }
    }

    if (!target) {
        spin_unlock(&g_multi_monitor_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    int32_t new_x = (int32_t)target->x + (int32_t)(target->width / 2) - (int32_t)(window->width / 2);
    int32_t new_y = (int32_t)target->y + (int32_t)(target->height / 2) - (int32_t)(window->height / 2);

    if (new_x < (int32_t)target->x) new_x = (int32_t)target->x;
    if (new_y < (int32_t)target->y) new_y = (int32_t)target->y;
    if (new_x + (int32_t)window->width > (int32_t)(target->x + target->width)) {
        new_x = (int32_t)(target->x + target->width) - (int32_t)window->width;
    }
    if (new_y + (int32_t)window->height > (int32_t)(target->y + target->height)) {
        new_y = (int32_t)(target->y + target->height) - (int32_t)window->height;
    }

    spin_unlock(&g_multi_monitor_lock);

    return window_set_position(handle, new_x, new_y);
}

graphics_result_t multi_monitor_set_primary(uint32_t id) {
    if (!g_multi_monitor_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    bool found = false;
    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].id == id && g_multi_monitor.monitors[i].active) {
            g_multi_monitor.monitors[i].primary = true;
            found = true;
        } else {
            g_multi_monitor.monitors[i].primary = false;
        }
    }

    if (!found) {
        spin_unlock(&g_multi_monitor_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    debuglog(DEBUG_INFO, "Primary monitor set to %u\n", id);

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t multi_monitor_get_count(uint32_t* count) {
    if (!g_multi_monitor_initialized || !count) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);
    *count = g_multi_monitor.num_monitors;
    spin_unlock(&g_multi_monitor_lock);

    return GRAPHICS_SUCCESS;
}

graphics_result_t multi_monitor_set_mode(uint32_t monitor_id, uint32_t width,
                                          uint32_t height, uint32_t bpp,
                                          uint32_t refresh_rate) {
    if (!g_multi_monitor_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    monitor_info_t* mon = NULL;
    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].id == monitor_id) {
            mon = &g_multi_monitor.monitors[i];
            break;
        }
    }

    if (!mon) {
        spin_unlock(&g_multi_monitor_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (mon->framebuffer) {
        if (mon->framebuffer->virtual_addr) {
            kfree((void*)mon->framebuffer->virtual_addr);
        }
        kfree(mon->framebuffer);
        mon->framebuffer = NULL;
    }

    uint32_t bytes_per_pixel = (bpp + 7) / 8;
    if (bytes_per_pixel == 0) bytes_per_pixel = 4;
    uint32_t pitch = width * bytes_per_pixel;

    framebuffer_t* fb = kmalloc(sizeof(framebuffer_t));
    if (!fb) {
        spin_unlock(&g_multi_monitor_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    size_t fb_size = (size_t)pitch * height;
    void* fb_mem = kmalloc(fb_size);
    if (!fb_mem) {
        kfree(fb);
        spin_unlock(&g_multi_monitor_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    memset(fb, 0, sizeof(framebuffer_t));
    memset(fb_mem, 0, fb_size);

    fb->virtual_addr = (uintptr_t)fb_mem;
    fb->physical_addr = (uintptr_t)fb_mem;
    fb->size = fb_size;
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->bpp = bpp;
    fb->double_buffered = false;
    fb->hw_cursor_available = false;

    if (bpp == 32) {
        fb->format = PIXEL_FORMAT_BGRA_8888;
    } else if (bpp == 24) {
        fb->format = PIXEL_FORMAT_BGR_888;
    } else if (bpp == 16) {
        fb->format = PIXEL_FORMAT_RGB_565;
    } else {
        fb->format = PIXEL_FORMAT_INDEXED_8;
    }

    mon->framebuffer = fb;
    mon->width = width;
    mon->height = height;
    mon->bpp = bpp;
    mon->refresh_rate = refresh_rate;

    mm_update_totals();

    debuglog(DEBUG_INFO, "Monitor %u mode set to %ux%u@%uHz\n",
             monitor_id, width, height, refresh_rate);

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t multi_monitor_get_mode(uint32_t monitor_id, video_mode_t* mode) {
    if (!g_multi_monitor_initialized || !mode) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].id == monitor_id) {
            monitor_info_t* mon = &g_multi_monitor.monitors[i];
            mode->width = mon->width;
            mode->height = mon->height;
            mode->bpp = mon->bpp;
            mode->refresh_rate = mon->refresh_rate;
            mode->pitch = mon->framebuffer ? mon->framebuffer->pitch : 0;
            mode->format = mon->framebuffer ? mon->framebuffer->format : PIXEL_FORMAT_BGRA_8888;
            mode->is_text_mode = false;
            mode->mode_number = monitor_id;
            mode->hw_data = NULL;
            spin_unlock(&g_multi_monitor_lock);
            return GRAPHICS_SUCCESS;
        }
    }

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t multi_monitor_enable_monitor(uint32_t id, bool enable) {
    if (!g_multi_monitor_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].id == id) {
            if (!enable && g_multi_monitor.monitors[i].primary) {
                spin_unlock(&g_multi_monitor_lock);
                return GRAPHICS_ERROR_INVALID_PARAMETER;
            }
            g_multi_monitor.monitors[i].active = enable;
            mm_update_totals();
            debuglog(DEBUG_INFO, "Monitor %u %s\n", id, enable ? "enabled" : "disabled");
            spin_unlock(&g_multi_monitor_lock);
            return GRAPHICS_SUCCESS;
        }
    }

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t multi_monitor_get_state(multi_monitor_state_t* state) {
    if (!g_multi_monitor_initialized || !state) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_multi_monitor_lock);

    memcpy(state, &g_multi_monitor, sizeof(multi_monitor_state_t));

    spin_unlock(&g_multi_monitor_lock);
    return GRAPHICS_SUCCESS;
}

static void mm_update_totals(void) {
    uint32_t max_x = 0;
    uint32_t max_y = 0;

    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        monitor_info_t* mon = &g_multi_monitor.monitors[i];
        if (!mon->active) continue;

        uint32_t right = mon->x + mon->width;
        uint32_t bottom = mon->y + mon->height;

        if (right > max_x) max_x = right;
        if (bottom > max_y) max_y = bottom;
    }

    g_multi_monitor.total_width = max_x;
    g_multi_monitor.total_height = max_y;
}

static graphics_result_t mm_setup_default_monitor(void) {
    graphics_device_t* dev = graphics_get_primary_device();
    if (!dev) {
        debuglog(DEBUG_WARN, "No graphics device found, using fallback monitor\n");

        g_multi_monitor.num_monitors = 1;

        monitor_info_t* mon = &g_multi_monitor.monitors[0];
        memset(mon, 0, sizeof(monitor_info_t));
        mon->id = 1;
        mon->x = 0;
        mon->y = 0;
        mon->width = 1024;
        mon->height = 768;
        mon->bpp = 32;
        mon->refresh_rate = 60;
        mon->active = true;
        mon->primary = true;
        strncpy(mon->name, "LVDS-1", sizeof(mon->name) - 1);

        framebuffer_t* fb = kmalloc(sizeof(framebuffer_t));
        if (fb) {
            uint32_t pitch = mon->width * 4;
            size_t fb_size = (size_t)pitch * mon->height;
            void* fb_mem = kmalloc(fb_size);
            if (fb_mem) {
                memset(fb, 0, sizeof(framebuffer_t));
                memset(fb_mem, 0, fb_size);
                fb->virtual_addr = (uintptr_t)fb_mem;
                fb->physical_addr = (uintptr_t)fb_mem;
                fb->size = fb_size;
                fb->width = mon->width;
                fb->height = mon->height;
                fb->pitch = pitch;
                fb->bpp = mon->bpp;
                fb->format = PIXEL_FORMAT_BGRA_8888;
                fb->double_buffered = false;
                fb->hw_cursor_available = false;
                mon->framebuffer = fb;
            } else {
                kfree(fb);
            }
        }
    } else {
        video_mode_t current_mode;
        graphics_result_t mode_result = graphics_get_current_mode(&current_mode);

        g_multi_monitor.num_monitors = 1;

        monitor_info_t* mon = &g_multi_monitor.monitors[0];
        memset(mon, 0, sizeof(monitor_info_t));
        mon->id = 1;
        mon->x = 0;
        mon->y = 0;

        if (mode_result == GRAPHICS_SUCCESS) {
            mon->width = current_mode.width;
            mon->height = current_mode.height;
            mon->bpp = current_mode.bpp;
            mon->refresh_rate = current_mode.refresh_rate;
        } else {
            mon->width = 1024;
            mon->height = 768;
            mon->bpp = 32;
            mon->refresh_rate = 60;
        }

        mon->active = true;
        mon->primary = true;
        strncpy(mon->name, "Primary", sizeof(mon->name) - 1);

        framebuffer_t* hw_fb = graphics_get_framebuffer();
        if (hw_fb) {
            framebuffer_t* fb = kmalloc(sizeof(framebuffer_t));
            if (fb) {
                uint32_t pitch = mon->width * ((mon->bpp + 7) / 8);
                size_t fb_size = (size_t)pitch * mon->height;
                void* fb_mem = kmalloc(fb_size);
                if (fb_mem) {
                    memset(fb, 0, sizeof(framebuffer_t));
                    memcpy(fb_mem, (void*)hw_fb->virtual_addr, fb_size < hw_fb->size ? fb_size : hw_fb->size);
                    fb->virtual_addr = (uintptr_t)fb_mem;
                    fb->physical_addr = (uintptr_t)fb_mem;
                    fb->size = fb_size;
                    fb->width = mon->width;
                    fb->height = mon->height;
                    fb->pitch = pitch;
                    fb->bpp = mon->bpp;
                    fb->format = hw_fb->format;
                    fb->double_buffered = false;
                    fb->hw_cursor_available = false;
                    mon->framebuffer = fb;
                } else {
                    kfree(fb);
                }
            }
        }
    }

    mm_update_totals();

    debuglog(DEBUG_INFO, "Default monitor: %ux%u@%uHz (primary)\n",
             g_multi_monitor.monitors[0].width,
             g_multi_monitor.monitors[0].height,
             g_multi_monitor.monitors[0].refresh_rate);

    return GRAPHICS_SUCCESS;
}

__attribute__((unused)) static bool mm_id_exists(uint32_t id) {
    for (uint32_t i = 0; i < g_multi_monitor.num_monitors; i++) {
        if (g_multi_monitor.monitors[i].id == id) {
            return true;
        }
    }
    return false;
}
