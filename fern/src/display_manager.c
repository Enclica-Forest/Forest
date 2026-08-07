#include "include/display_manager.h"
#include "include/mode_state.h"
#include "include/graphics/graphics_manager.h"
#include "include/gfx_config.h"
#include "include/debuglog.h"
#include "include/mm.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/timer.h"
#include "include/input_mux.h"

#if HAS_GRAPHICS

#define GFP_KERNEL 0x01

static display_manager_state_t g_display_manager = {0};
static bool g_display_manager_initialized = false;
static spinlock_t g_display_manager_lock;

extern uint32_t g_current_tty_session;

static graphics_result_t dm_create_offscreen_framebuffer(uint32_t width, uint32_t height,
                                                        pixel_format_t format, framebuffer_t** fb);
static graphics_result_t dm_destroy_offscreen_framebuffer(framebuffer_t* fb);
static graphics_result_t dm_perform_transition_fade(uint32_t alpha_256);
static display_client_t* dm_find_client_by_name(const char* name);
static display_client_t* dm_find_client_by_mode(display_mode_t mode);
static graphics_result_t dm_suspend_current_client(void);
static graphics_result_t dm_resume_client(display_client_t* client);
static void dm_add_dirty_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);
static void dm_merge_dirty_regions(void);
static void dm_clear_dirty_regions(void);
static void dm_process_overlays(void);
static inline uint8_t dm_alpha_blend_channel(uint8_t src, uint8_t dst, uint32_t alpha_256);
static bool dm_rects_intersect(const graphics_rect_t* a, const graphics_rect_t* b);
static void dm_clip_rect_to_fb(const graphics_rect_t* src, graphics_rect_t* dst,
                               uint32_t fb_w, uint32_t fb_h);
static void dm_blit_region_alpha(framebuffer_t* dst, framebuffer_t* src,
                                 const graphics_rect_t* region, uint8_t opacity);
static void dm_blit_region_opaque(framebuffer_t* dst, framebuffer_t* src,
                                  const graphics_rect_t* region);
static void dm_fade_region(framebuffer_t* dst, framebuffer_t* from_fb,
                           framebuffer_t* to_fb, const graphics_rect_t* region,
                           uint32_t alpha_256);
static void dm_save_transition_state(void);

graphics_result_t display_manager_init(void) {
    if (g_display_manager_initialized) {
        return GRAPHICS_SUCCESS;
    }

    debuglog_printf("Initializing Display Manager...\n");

    spinlock_init(&g_display_manager_lock, "display_manager");

    graphics_device_t* graphics_dev = graphics_get_primary_device();

    /*
     * In the V2-bridge path, graphics_state.device.current_fb is never
     * populated (it is only filled by the old disabled driver), so fall
     * back to graphics_get_framebuffer() which always returns the live
     * compat framebuffer (including the multiboot identity-map fallback).
     * Only hard-fail if we cannot get any framebuffer at all.
     */
    framebuffer_t* master_fb = (graphics_dev && graphics_dev->current_fb)
                                   ? graphics_dev->current_fb
                                   : graphics_get_framebuffer();

    if (!master_fb) {
        debuglog_printf("ERROR: No framebuffer available for Display Manager\n");
        return GRAPHICS_ERROR_HARDWARE_FAULT;
    }
    if (!master_fb->virtual_addr) {
        debuglog_printf("ERROR: Display Manager framebuffer has no virtual address\n");
        return GRAPHICS_ERROR_HARDWARE_FAULT;
    }

    g_display_manager.master_fb = master_fb;

    memset(g_display_manager.offscreen_fbs, 0, sizeof(g_display_manager.offscreen_fbs));
    g_display_manager.num_offscreen_fbs = 0;

    g_display_manager.current_mode = DISPLAY_MODE_TTY_CONSOLE;
    g_display_manager.pending_mode = DISPLAY_MODE_TTY_CONSOLE;
    g_display_manager.default_mode = DISPLAY_MODE_TTY_CONSOLE;
    g_display_manager.active_client = NULL;
    g_display_manager.clients = NULL;
    g_display_manager.in_transition = false;

    g_display_manager.transition_fb = NULL;
    g_display_manager.transition_duration_ms = 300;
    g_display_manager.transition_fade_enabled = true;
    g_display_manager.hotkey_enabled = true;
    g_display_manager.hotkey_modifiers = 0;

    g_display_manager.overlays = NULL;
    g_display_manager.num_overlays = 0;

    g_display_manager.num_dirty_regions = 0;
    g_display_manager.full_redraw_pending = true;
    g_display_manager.vsync_counter = 0;

    g_display_manager.mode_switch_count = 0;
    g_display_manager.last_switch_time = timer_get_ticks();

    g_display_manager_initialized = true;

    debuglog_printf("Display Manager initialized successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_shutdown(void) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_SUCCESS;
    }

    spin_lock(&g_display_manager_lock);

    debuglog_printf("Shutting down Display Manager...\n");

    if (g_display_manager.active_client) {
        dm_suspend_current_client();
    }

    overlay_client_t* ov = g_display_manager.overlays;
    while (ov) {
        overlay_client_t* next = ov->next;
        kfree(ov);
        ov = next;
    }
    g_display_manager.overlays = NULL;
    g_display_manager.num_overlays = 0;

    for (uint32_t i = 0; i < g_display_manager.num_offscreen_fbs; i++) {
        if (g_display_manager.offscreen_fbs[i]) {
            dm_destroy_offscreen_framebuffer(g_display_manager.offscreen_fbs[i]);
        }
    }

    if (g_display_manager.transition_fb) {
        dm_destroy_offscreen_framebuffer(g_display_manager.transition_fb);
        g_display_manager.transition_fb = NULL;
    }

    g_display_manager.clients = NULL;
    g_display_manager.active_client = NULL;

    g_display_manager_initialized = false;

    spin_unlock(&g_display_manager_lock);

    debuglog_printf("Display Manager shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_register_client(const display_client_t* client) {
    if (!g_display_manager_initialized || !client || !client->name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    if (dm_find_client_by_name(client->name)) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_DEVICE_BUSY;
    }

    display_client_t* new_client = kmalloc(sizeof(display_client_t));
    if (!new_client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    memcpy(new_client, client, sizeof(display_client_t));
    new_client->next = NULL;
    new_client->active = false;

    graphics_result_t result = dm_create_offscreen_framebuffer(
        g_display_manager.master_fb->width,
        g_display_manager.master_fb->height,
        g_display_manager.master_fb->format,
        &new_client->framebuffer
    );

    if (result != GRAPHICS_SUCCESS) {
        kfree(new_client);
        spin_unlock(&g_display_manager_lock);
        return result;
    }

    new_client->next = g_display_manager.clients;
    g_display_manager.clients = new_client;

    debuglog_printf("Registered display client: %s (mode %d)\n", new_client->name, new_client->mode);

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_unregister_client(const char* name) {
    if (!g_display_manager_initialized || !name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* prev = NULL;
    display_client_t* client = g_display_manager.clients;

    while (client) {
        if (strcmp(client->name, name) == 0) {
            if (client->active) {
                dm_suspend_current_client();
                g_display_manager.active_client = NULL;
            }

            overlay_client_t** ov_ptr = &g_display_manager.overlays;
            while (*ov_ptr) {
                if ((*ov_ptr)->client == client) {
                    overlay_client_t* found = *ov_ptr;
                    *ov_ptr = found->next;
                    kfree(found);
                    g_display_manager.num_overlays--;
                } else {
                    ov_ptr = &(*ov_ptr)->next;
                }
            }

            if (prev) {
                prev->next = client->next;
            } else {
                g_display_manager.clients = client->next;
            }

            if (client->framebuffer) {
                dm_destroy_offscreen_framebuffer(client->framebuffer);
            }

            kfree(client);

            debuglog_printf("Unregistered display client: %s\n", name);
            spin_unlock(&g_display_manager_lock);
            return GRAPHICS_SUCCESS;
        }

        prev = client;
        client = client->next;
    }

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t display_manager_activate_client(const char* name) {
    if (!g_display_manager_initialized || !name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* client = dm_find_client_by_name(name);
    if (!client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (g_display_manager.active_client == client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_SUCCESS;
    }

    g_display_manager.pending_mode = client->mode;

    graphics_result_t result = GRAPHICS_SUCCESS;
    if (g_display_manager.active_client) {
        dm_save_transition_state();
        result = dm_suspend_current_client();
        if (result != GRAPHICS_SUCCESS) {
            spin_unlock(&g_display_manager_lock);
            return result;
        }
    }

    result = dm_resume_client(client);
    if (result != GRAPHICS_SUCCESS) {
        spin_unlock(&g_display_manager_lock);
        return result;
    }

    g_display_manager.active_client = client;
    g_display_manager.current_mode = client->mode;

    if (input_mux_is_initialized()) {
        input_mux_set_active_mode(client->mode);
    }

    g_display_manager.mode_switch_count++;
    g_display_manager.last_switch_time = timer_get_ticks();
    g_display_manager.full_redraw_pending = true;

    debuglog_printf("Activated display client: %s (mode %d)\n", client->name, client->mode);

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_get_active_client(display_client_t** client) {
    if (!g_display_manager_initialized || !client) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    *client = g_display_manager.active_client;
    spin_unlock(&g_display_manager_lock);

    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_switch_mode(display_mode_t mode, void* params) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    display_client_t* client = dm_find_client_by_mode(mode);
    if (!client) {
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    (void)params;
    return display_manager_activate_client(client->name);
}

graphics_result_t display_manager_get_current_mode(display_mode_t* mode) {
    if (!g_display_manager_initialized || !mode) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    *mode = g_display_manager.current_mode;
    spin_unlock(&g_display_manager_lock);

    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_get_available_modes(display_mode_t** modes, uint32_t* count) {
    if (!g_display_manager_initialized || !modes || !count) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    uint32_t num_clients = 0;
    display_client_t* c = g_display_manager.clients;
    while (c) {
        num_clients++;
        c = c->next;
    }

    if (num_clients == 0) {
        *count = 0;
        *modes = NULL;
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_SUCCESS;
    }

    display_mode_t* mode_list = kmalloc(num_clients * sizeof(display_mode_t));
    if (!mode_list) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    uint32_t idx = 0;
    c = g_display_manager.clients;
    while (c && idx < num_clients) {
        mode_list[idx++] = c->mode;
        c = c->next;
    }

    *count = num_clients;
    *modes = mode_list;

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_set_default_mode(display_mode_t mode) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    g_display_manager.default_mode = mode;
    spin_unlock(&g_display_manager_lock);

    debuglog_printf("Default display mode set to %d\n", mode);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_enable_hotkeys(bool enable) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    g_display_manager.hotkey_enabled = enable;
    spin_unlock(&g_display_manager_lock);

    debuglog_printf("Hotkeys %s\n", enable ? "enabled" : "disabled");
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_set_transition_fade(bool enable, uint32_t duration_ms) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    g_display_manager.transition_fade_enabled = enable;
    if (duration_ms > 0) {
        g_display_manager.transition_duration_ms = duration_ms;
    }
    spin_unlock(&g_display_manager_lock);

    debuglog_printf("Transition fade: enabled=%d, duration=%dms\n", enable, duration_ms);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_add_overlay(const char* client_name, int32_t z_order,
                                              const graphics_rect_t* bounds, uint8_t opacity) {
    if (!g_display_manager_initialized || !client_name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* client = dm_find_client_by_name(client_name);
    if (!client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (g_display_manager.num_overlays >= DM_MAX_OVERLAYS) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_DEVICE_BUSY;
    }

    overlay_client_t* ov = g_display_manager.overlays;
    while (ov) {
        if (ov->client == client) {
            ov->z_order = z_order;
            if (bounds) {
                ov->bounds = *bounds;
            }
            ov->opacity = opacity;
            ov->visible = true;
            spin_unlock(&g_display_manager_lock);
            return GRAPHICS_SUCCESS;
        }
        ov = ov->next;
    }

    overlay_client_t* new_ov = kmalloc(sizeof(overlay_client_t));
    if (!new_ov) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    new_ov->client = client;
    new_ov->z_order = z_order;
    if (bounds) {
        new_ov->bounds = *bounds;
    } else {
        new_ov->bounds.x = 0;
        new_ov->bounds.y = 0;
        new_ov->bounds.width = g_display_manager.master_fb->width;
        new_ov->bounds.height = g_display_manager.master_fb->height;
    }
    new_ov->opacity = opacity;
    new_ov->visible = true;
    new_ov->next = NULL;

    overlay_client_t** insert = &g_display_manager.overlays;
    while (*insert && (*insert)->z_order <= z_order) {
        insert = &(*insert)->next;
    }
    new_ov->next = *insert;
    *insert = new_ov;
    g_display_manager.num_overlays++;

    debuglog_printf("Added overlay: %s (z=%d, opacity=%d)\n", client_name, z_order, opacity);

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_remove_overlay(const char* client_name) {
    if (!g_display_manager_initialized || !client_name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* client = dm_find_client_by_name(client_name);
    if (!client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    overlay_client_t** ptr = &g_display_manager.overlays;
    while (*ptr) {
        if ((*ptr)->client == client) {
            overlay_client_t* found = *ptr;
            *ptr = found->next;
            kfree(found);
            g_display_manager.num_overlays--;
            g_display_manager.full_redraw_pending = true;
            debuglog_printf("Removed overlay: %s\n", client_name);
            spin_unlock(&g_display_manager_lock);
            return GRAPHICS_SUCCESS;
        }
        ptr = &(*ptr)->next;
    }

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t display_manager_set_overlay_z_order(const char* client_name, int32_t z_order) {
    if (!g_display_manager_initialized || !client_name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* client = dm_find_client_by_name(client_name);
    if (!client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    overlay_client_t* ov = g_display_manager.overlays;
    while (ov) {
        if (ov->client == client) {
            ov->z_order = z_order;
            g_display_manager.full_redraw_pending = true;
            spin_unlock(&g_display_manager_lock);
            return GRAPHICS_SUCCESS;
        }
        ov = ov->next;
    }

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t display_manager_set_overlay_opacity(const char* client_name, uint8_t opacity) {
    if (!g_display_manager_initialized || !client_name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* client = dm_find_client_by_name(client_name);
    if (!client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    overlay_client_t* ov = g_display_manager.overlays;
    while (ov) {
        if (ov->client == client) {
            ov->opacity = opacity;
            spin_unlock(&g_display_manager_lock);
            return GRAPHICS_SUCCESS;
        }
        ov = ov->next;
    }

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t display_manager_set_overlay_visible(const char* client_name, bool visible) {
    if (!g_display_manager_initialized || !client_name) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    display_client_t* client = dm_find_client_by_name(client_name);
    if (!client) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    overlay_client_t* ov = g_display_manager.overlays;
    while (ov) {
        if (ov->client == client) {
            ov->visible = visible;
            g_display_manager.full_redraw_pending = true;
            spin_unlock(&g_display_manager_lock);
            return GRAPHICS_SUCCESS;
        }
        ov = ov->next;
    }

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t display_manager_process_frame(void) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    if (g_display_manager.in_transition) {
        display_manager_update_transition();
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_SUCCESS;
    }

    if (g_display_manager.active_client && g_display_manager.active_client->frame_callback) {
        if (g_display_manager.full_redraw_pending) {
            g_display_manager.active_client->frame_callback(
                g_display_manager.master_fb,
                g_display_manager.active_client->context
            );
            g_display_manager.full_redraw_pending = false;
            dm_clear_dirty_regions();
        } else if (g_display_manager.num_dirty_regions > 0) {
            for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
                if (g_display_manager.dirty_regions[i].valid) {
                    g_display_manager.active_client->frame_callback(
                        g_display_manager.master_fb,
                        g_display_manager.active_client->context
                    );
                    break;
                }
            }
            dm_clear_dirty_regions();
        }
    }

    dm_process_overlays();

    g_display_manager.vsync_counter++;

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_process_input(const input_event_t* event) {
    if (!g_display_manager_initialized || !event) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    if (g_display_manager.active_client && g_display_manager.active_client->input_callback) {
        graphics_result_t result = g_display_manager.active_client->input_callback(
            event,
            g_display_manager.active_client->context
        );

        if (result != GRAPHICS_SUCCESS) {
            debuglog_printf("Input callback failed for client %s: %d\n",
                          g_display_manager.active_client->name, result);
        }
    }

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_start_transition(display_mode_t from_mode, display_mode_t to_mode, uint32_t duration_ms) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    if (g_display_manager.in_transition) {
        spin_unlock(&g_display_manager_lock);
        return GRAPHICS_ERROR_DEVICE_BUSY;
    }

    dm_save_transition_state();

    display_client_t* target = dm_find_client_by_mode(to_mode);
    if (target && target->frame_callback) {
        target->frame_callback(target->framebuffer, target->context);
    }

    g_display_manager.pending_mode = to_mode;
    g_display_manager.transition_start_time = timer_get_ticks();
    g_display_manager.transition_duration_ms = duration_ms;
    g_display_manager.in_transition = true;

    debuglog_printf("Starting transition from mode %d to %d (%dms)\n",
                   from_mode, to_mode, duration_ms);

    spin_unlock(&g_display_manager_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_update_transition(void) {
    if (!g_display_manager_initialized || !g_display_manager.in_transition) {
        return GRAPHICS_SUCCESS;
    }

    uint64_t current_time = timer_get_ticks();
    uint64_t elapsed = current_time - g_display_manager.transition_start_time;

    if (elapsed >= g_display_manager.transition_duration_ms) {
        g_display_manager.current_mode = g_display_manager.pending_mode;
        g_display_manager.in_transition = false;

        display_client_t* client = dm_find_client_by_mode(g_display_manager.current_mode);
        if (client) {
            dm_resume_client(client);
            g_display_manager.active_client = client;
        }

        if (g_display_manager.transition_fb) {
            dm_destroy_offscreen_framebuffer(g_display_manager.transition_fb);
            g_display_manager.transition_fb = NULL;
        }

        g_display_manager.full_redraw_pending = true;

        debuglog_printf("Transition complete, now in mode %d\n", g_display_manager.current_mode);
    } else if (g_display_manager.transition_fade_enabled) {
        uint32_t alpha_256 = (elapsed * 256) / g_display_manager.transition_duration_ms;
        dm_perform_transition_fade(alpha_256);
    }

    return GRAPHICS_SUCCESS;
}

bool display_manager_is_in_transition(void) {
    if (!g_display_manager_initialized) {
        return false;
    }

    bool in_transition;
    spin_lock(&g_display_manager_lock);
    in_transition = g_display_manager.in_transition;
    spin_unlock(&g_display_manager_lock);

    return in_transition;
}

graphics_result_t display_manager_acquire_framebuffer(framebuffer_t** fb) {
    if (!g_display_manager_initialized || !fb) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    *fb = g_display_manager.master_fb;
    spin_unlock(&g_display_manager_lock);

    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_release_framebuffer(framebuffer_t* fb) {
    (void)fb;
    return GRAPHICS_SUCCESS;
}

graphics_result_t display_manager_invalidate_region(const graphics_rect_t* region) {
    if (!g_display_manager_initialized || !region) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);

    dm_add_dirty_rect(region->x, region->y, region->width, region->height);
    dm_merge_dirty_regions();

    spin_unlock(&g_display_manager_lock);

    return display_manager_process_frame();
}

graphics_result_t display_manager_invalidate_full(void) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    g_display_manager.full_redraw_pending = true;
    spin_unlock(&g_display_manager_lock);

    return display_manager_process_frame();
}

static graphics_result_t dm_create_offscreen_framebuffer(uint32_t width, uint32_t height,
                                                        pixel_format_t format, framebuffer_t** fb) {
    if (!fb) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    *fb = kmalloc(sizeof(framebuffer_t));
    if (!*fb) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    uint32_t bpp = (format == PIXEL_FORMAT_RGBA_8888 || format == PIXEL_FORMAT_BGRA_8888) ? 32 :
                   (format == PIXEL_FORMAT_RGB_888 || format == PIXEL_FORMAT_BGR_888) ? 24 :
                   (format == PIXEL_FORMAT_RGB_565) ? 16 : 8;

    uint32_t pitch = (width * bpp + 7) / 8;
    size_t fb_size = pitch * height;

    void* fb_memory = kmalloc(fb_size);
    if (!fb_memory) {
        kfree(*fb);
        *fb = NULL;
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    memset(*fb, 0, sizeof(framebuffer_t));
    (*fb)->virtual_addr = (uintptr_t)fb_memory;
    (*fb)->physical_addr = (uintptr_t)fb_memory;
    (*fb)->size = fb_size;
    (*fb)->width = width;
    (*fb)->height = height;
    (*fb)->pitch = pitch;
    (*fb)->format = format;
    (*fb)->bpp = bpp;
    (*fb)->double_buffered = false;
    (*fb)->hw_cursor_available = false;

    return GRAPHICS_SUCCESS;
}

static graphics_result_t dm_destroy_offscreen_framebuffer(framebuffer_t* fb) {
    if (!fb) {
        return GRAPHICS_SUCCESS;
    }

    if (fb->virtual_addr) {
        kfree((void*)fb->virtual_addr);
    }

    kfree(fb);
    return GRAPHICS_SUCCESS;
}

static inline uint8_t dm_alpha_blend_channel(uint8_t src, uint8_t dst, uint32_t alpha_256) {
    uint32_t result = (src * alpha_256 + dst * (256 - alpha_256)) / 256;
    if (result > 255) result = 255;
    return (uint8_t)result;
}

static bool dm_rects_intersect(const graphics_rect_t* a, const graphics_rect_t* b) {
    if (a->x >= (int32_t)(b->x + b->width) || b->x >= (int32_t)(a->x + a->width)) {
        return false;
    }
    if (a->y >= (int32_t)(b->y + b->height) || b->y >= (int32_t)(a->y + a->height)) {
        return false;
    }
    return true;
}

static void dm_clip_rect_to_fb(const graphics_rect_t* src, graphics_rect_t* dst,
                               uint32_t fb_w, uint32_t fb_h) {
    int32_t x0 = src->x < 0 ? 0 : src->x;
    int32_t y0 = src->y < 0 ? 0 : src->y;
    int32_t x1 = (int32_t)(src->x + src->width);
    int32_t y1 = (int32_t)(src->y + src->height);

    if (x1 > (int32_t)fb_w) x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;

    dst->x = x0;
    dst->y = y0;
    dst->width = (x1 > x0) ? (uint32_t)(x1 - x0) : 0;
    dst->height = (y1 > y0) ? (uint32_t)(y1 - y0) : 0;
}

static void dm_blit_region_alpha(framebuffer_t* dst, framebuffer_t* src,
                                 const graphics_rect_t* region, uint8_t opacity) {
    if (!dst || !src || !region || region->width == 0 || region->height == 0) {
        return;
    }
    if (!dst->virtual_addr || !src->virtual_addr) {
        return;
    }

    uint8_t bpp = dst->bpp;
    if (bpp != 32) {
        dm_blit_region_opaque(dst, src, region);
        return;
    }

    uint32_t alpha_256 = ((uint32_t)opacity * 256) / 255;

    for (uint32_t y = 0; y < region->height; y++) {
        uint32_t src_y = (uint32_t)region->y + y;
        uint32_t dst_y = (uint32_t)region->y + y;
        if (src_y >= src->height || dst_y >= dst->height) continue;

        uint32_t src_row_offset = src_y * src->pitch + (uint32_t)region->x * 4;
        uint32_t dst_row_offset = dst_y * dst->pitch + (uint32_t)region->x * 4;

        uint8_t* src_row = (uint8_t*)src->virtual_addr + src_row_offset;
        uint8_t* dst_row = (uint8_t*)dst->virtual_addr + dst_row_offset;

        for (uint32_t x = 0; x < region->width; x++) {
            uint32_t src_x = (uint32_t)region->x + x;
            if (src_x >= src->width) break;

            uint8_t sb = src_row[x * 4 + 0];
            uint8_t sg = src_row[x * 4 + 1];
            uint8_t sr = src_row[x * 4 + 2];

            uint8_t db = dst_row[x * 4 + 0];
            uint8_t dg = dst_row[x * 4 + 1];
            uint8_t dr = dst_row[x * 4 + 2];

            dst_row[x * 4 + 0] = dm_alpha_blend_channel(sb, db, alpha_256);
            dst_row[x * 4 + 1] = dm_alpha_blend_channel(sg, dg, alpha_256);
            dst_row[x * 4 + 2] = dm_alpha_blend_channel(sr, dr, alpha_256);
        }
    }
}

static void dm_blit_region_opaque(framebuffer_t* dst, framebuffer_t* src,
                                  const graphics_rect_t* region) {
    if (!dst || !src || !region || region->width == 0 || region->height == 0) {
        return;
    }
    if (!dst->virtual_addr || !src->virtual_addr) {
        return;
    }

    for (uint32_t y = 0; y < region->height; y++) {
        uint32_t src_y = (uint32_t)region->y + y;
        uint32_t dst_y = (uint32_t)region->y + y;
        if (src_y >= src->height || dst_y >= dst->height) continue;

        size_t copy_len = region->width * (dst->bpp / 8);
        uint32_t src_row_offset = src_y * src->pitch + (uint32_t)region->x * (dst->bpp / 8);
        uint32_t dst_row_offset = dst_y * dst->pitch + (uint32_t)region->x * (dst->bpp / 8);

        void* src_ptr = (void*)(src->virtual_addr + src_row_offset);
        void* dst_ptr = (void*)(dst->virtual_addr + dst_row_offset);

        memcpy(dst_ptr, src_ptr, copy_len);
    }
}

static void dm_fade_region(framebuffer_t* dst, framebuffer_t* from_fb,
                           framebuffer_t* to_fb, const graphics_rect_t* region,
                           uint32_t alpha_256) {
    if (!dst || !region || region->width == 0 || region->height == 0) {
        return;
    }
    if (!dst->virtual_addr) {
        return;
    }

    if (dst->bpp != 32) {
        if (alpha_256 >= 128 && to_fb) {
            dm_blit_region_opaque(dst, to_fb, region);
        } else if (from_fb) {
            dm_blit_region_opaque(dst, from_fb, region);
        }
        return;
    }

    for (uint32_t y = 0; y < region->height; y++) {
        uint32_t fy = (uint32_t)region->y + y;
        uint32_t ty = (uint32_t)region->y + y;
        uint32_t dy = (uint32_t)region->y + y;
        if (fy >= from_fb->height || ty >= to_fb->height || dy >= dst->height) continue;

        uint32_t row_offset = fy * from_fb->pitch + (uint32_t)region->x * 4;

        uint8_t* from_row = from_fb ? (uint8_t*)from_fb->virtual_addr + row_offset : NULL;
        uint8_t* to_row = to_fb ? (uint8_t*)to_fb->virtual_addr + row_offset : NULL;
        uint8_t* dst_row = (uint8_t*)dst->virtual_addr + row_offset;

        for (uint32_t x = 0; x < region->width; x++) {
            uint32_t px = (uint32_t)region->x + x;
            if (px >= from_fb->width) break;

            uint32_t idx = x * 4;

            uint8_t fb = from_row ? from_row[idx + 0] : 0;
            uint8_t fg = from_row ? from_row[idx + 1] : 0;
            uint8_t fr = from_row ? from_row[idx + 2] : 0;

            uint8_t tb = to_row ? to_row[idx + 0] : 0;
            uint8_t tg = to_row ? to_row[idx + 1] : 0;
            uint8_t tr = to_row ? to_row[idx + 2] : 0;

            uint32_t inv = 256 - alpha_256;
            dst_row[idx + 0] = (uint8_t)((fb * inv + tb * alpha_256) / 256);
            dst_row[idx + 1] = (uint8_t)((fg * inv + tg * alpha_256) / 256);
            dst_row[idx + 2] = (uint8_t)((fr * inv + tr * alpha_256) / 256);
        }
    }
}

static void dm_save_transition_state(void) {
    if (!g_display_manager.master_fb || !g_display_manager.master_fb->virtual_addr) {
        return;
    }

    if (!g_display_manager.transition_fb) {
        dm_create_offscreen_framebuffer(
            g_display_manager.master_fb->width,
            g_display_manager.master_fb->height,
            g_display_manager.master_fb->format,
            &g_display_manager.transition_fb
        );
    }

    if (g_display_manager.transition_fb && g_display_manager.transition_fb->virtual_addr) {
        memcpy((void*)g_display_manager.transition_fb->virtual_addr,
               (void*)g_display_manager.master_fb->virtual_addr,
               g_display_manager.transition_fb->size);
    }
}

static graphics_result_t dm_perform_transition_fade(uint32_t alpha_256) {
    if (!g_display_manager.master_fb) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    framebuffer_t* from_fb = g_display_manager.transition_fb;
    framebuffer_t* to_fb = NULL;

    display_client_t* target = dm_find_client_by_mode(g_display_manager.pending_mode);
    if (target) {
        to_fb = target->framebuffer;
    }

    if (!from_fb && !to_fb) {
        return GRAPHICS_SUCCESS;
    }

    if (g_display_manager.num_dirty_regions > 0 && !g_display_manager.full_redraw_pending) {
        for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
            if (!g_display_manager.dirty_regions[i].valid) continue;

            graphics_rect_t clipped;
            dm_clip_rect_to_fb(&g_display_manager.dirty_regions[i].rect, &clipped,
                               g_display_manager.master_fb->width,
                               g_display_manager.master_fb->height);

            if (clipped.width > 0 && clipped.height > 0) {
                dm_fade_region(g_display_manager.master_fb, from_fb, to_fb,
                              &clipped, alpha_256);
            }
        }
    } else {
        graphics_rect_t full = {0, 0,
            g_display_manager.master_fb->width,
            g_display_manager.master_fb->height};
        dm_fade_region(g_display_manager.master_fb, from_fb, to_fb, &full, alpha_256);
    }

    return GRAPHICS_SUCCESS;
}

static display_client_t* dm_find_client_by_name(const char* name) {
    display_client_t* client = g_display_manager.clients;
    while (client) {
        if (strcmp(client->name, name) == 0) {
            return client;
        }
        client = client->next;
    }
    return NULL;
}

static display_client_t* dm_find_client_by_mode(display_mode_t mode) {
    display_client_t* client = g_display_manager.clients;
    while (client) {
        if (client->mode == mode) {
            return client;
        }
        client = client->next;
    }
    return NULL;
}

static graphics_result_t dm_suspend_current_client(void) {
    if (!g_display_manager.active_client) {
        return GRAPHICS_SUCCESS;
    }

    display_client_t* client = g_display_manager.active_client;
    client->active = false;

    if (client->suspend) {
        graphics_result_t result = client->suspend(client->context);
        if (result != GRAPHICS_SUCCESS) {
            debuglog_printf("Client suspend failed for %s: %d\n", client->name, result);
            return result;
        }
    }

    if (client->framebuffer && client->framebuffer->virtual_addr &&
        g_display_manager.master_fb && g_display_manager.master_fb->virtual_addr) {
        if (g_display_manager.num_dirty_regions > 0) {
            for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
                if (!g_display_manager.dirty_regions[i].valid) continue;

                graphics_rect_t clipped;
                dm_clip_rect_to_fb(&g_display_manager.dirty_regions[i].rect, &clipped,
                                   g_display_manager.master_fb->width,
                                   g_display_manager.master_fb->height);

                if (clipped.width > 0 && clipped.height > 0) {
                    dm_blit_region_opaque(client->framebuffer, g_display_manager.master_fb, &clipped);
                }
            }
        } else {
            memcpy((void*)client->framebuffer->virtual_addr,
                   (void*)g_display_manager.master_fb->virtual_addr,
                   client->framebuffer->size);
        }
        /* Mark that this client's offscreen buffer now has real content. */
        client->offscreen_valid = true;
    }

    debuglog_printf("Suspended display client: %s\n", client->name);
    return GRAPHICS_SUCCESS;
}

static graphics_result_t dm_resume_client(display_client_t* client) {
    if (!client) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    debuglog_printf("dm_resume_client: switching to mode %d\n", client->mode);

    /*
     * Only clear the framebuffer to black for TTY mode when we are actually
     * transitioning FROM another active client (i.e. when the previous GUI
     * content is stale and would bleed through).  On the very first activation
     * (active_client == NULL, no previous content) we must NOT clear: at that
     * point the splash screen is still visible and erasing it produces the
     * "black screen after splash" symptom.
     *
     * Also guard against virtual_addr == 0 which occurs during degraded-mode
     * init where the fallback framebuffer address may still be 0.
     */
    if (client->mode == DISPLAY_MODE_TTY_CONSOLE &&
        g_display_manager.active_client != NULL) {
        if (g_display_manager.master_fb && g_display_manager.master_fb->virtual_addr) {
            memset((void*)g_display_manager.master_fb->virtual_addr, 0,
                   g_display_manager.master_fb->size);
            debuglog_printf("dm_resume_client: cleared framebuffer for TTY (client switch)\n");
        }
    }

    /*
     * For GUI clients, restore their saved offscreen content ONLY when the
     * offscreen buffer actually contains a previously-suspended frame
     * (offscreen_valid == true, set by dm_suspend_current_client).
     * Skip the memcpy for a freshly-registered client whose offscreen buffer
     * is still zeroed — copying it would paint the screen black, destroying
     * the splash or whatever was there.
     */
    if (client->offscreen_valid &&
        client->framebuffer && g_display_manager.master_fb &&
        g_display_manager.master_fb->virtual_addr &&
        client->framebuffer->virtual_addr &&
        client->framebuffer->size > 0 &&
        client->mode != DISPLAY_MODE_TTY_CONSOLE) {
        memcpy((void*)g_display_manager.master_fb->virtual_addr,
               (void*)client->framebuffer->virtual_addr,
               client->framebuffer->size);
    }

    if (client->resume) {
        graphics_result_t result = client->resume(client->context);
        if (result != GRAPHICS_SUCCESS) {
            debuglog_printf("Client resume failed for %s: %d\n", client->name, result);
            return result;
        }
    }

    client->active = true;
    debuglog_printf("Resumed display client: %s\n", client->name);
    return GRAPHICS_SUCCESS;
}

static void dm_add_dirty_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (g_display_manager.num_dirty_regions >= DM_MAX_DIRTY_REGIONS) {
        g_display_manager.full_redraw_pending = true;
        return;
    }

    for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
        if (!g_display_manager.dirty_regions[i].valid) {
            g_display_manager.dirty_regions[i].rect.x = x;
            g_display_manager.dirty_regions[i].rect.y = y;
            g_display_manager.dirty_regions[i].rect.width = w;
            g_display_manager.dirty_regions[i].rect.height = h;
            g_display_manager.dirty_regions[i].valid = true;
            return;
        }
    }

    if (g_display_manager.num_dirty_regions < DM_MAX_DIRTY_REGIONS) {
        g_display_manager.dirty_regions[g_display_manager.num_dirty_regions].rect.x = x;
        g_display_manager.dirty_regions[g_display_manager.num_dirty_regions].rect.y = y;
        g_display_manager.dirty_regions[g_display_manager.num_dirty_regions].rect.width = w;
        g_display_manager.dirty_regions[g_display_manager.num_dirty_regions].rect.height = h;
        g_display_manager.dirty_regions[g_display_manager.num_dirty_regions].valid = true;
        g_display_manager.num_dirty_regions++;
    }
}

static void dm_merge_dirty_regions(void) {
    bool merged = true;

    while (merged) {
        merged = false;

        for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
            if (!g_display_manager.dirty_regions[i].valid) continue;

            for (uint32_t j = i + 1; j < g_display_manager.num_dirty_regions; j++) {
                if (!g_display_manager.dirty_regions[j].valid) continue;

                graphics_rect_t* a = &g_display_manager.dirty_regions[i].rect;
                graphics_rect_t* b = &g_display_manager.dirty_regions[j].rect;

                if (dm_rects_intersect(a, b)) {
                    int32_t x0 = a->x < b->x ? a->x : b->x;
                    int32_t y0 = a->y < b->y ? a->y : b->y;
                    int32_t x1_a = (int32_t)(a->x + a->width);
                    int32_t y1_a = (int32_t)(a->y + a->height);
                    int32_t x1_b = (int32_t)(b->x + b->width);
                    int32_t y1_b = (int32_t)(b->y + b->height);
                    int32_t x1 = x1_a > x1_b ? x1_a : x1_b;
                    int32_t y1 = y1_a > y1_b ? y1_a : y1_b;

                    a->x = x0;
                    a->y = y0;
                    a->width = (uint32_t)(x1 - x0);
                    a->height = (uint32_t)(y1 - y0);

                    g_display_manager.dirty_regions[j].valid = false;
                    merged = true;
                }
            }
        }
    }

    if (g_display_manager.master_fb) {
        uint32_t total_area = 0;
        uint32_t fb_area = g_display_manager.master_fb->width * g_display_manager.master_fb->height;

        for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
            if (g_display_manager.dirty_regions[i].valid) {
                total_area += g_display_manager.dirty_regions[i].rect.width *
                              g_display_manager.dirty_regions[i].rect.height;
            }
        }

        if (total_area > (fb_area / 2)) {
            g_display_manager.full_redraw_pending = true;
            dm_clear_dirty_regions();
        }
    }
}

static void dm_clear_dirty_regions(void) {
    for (uint32_t i = 0; i < g_display_manager.num_dirty_regions; i++) {
        g_display_manager.dirty_regions[i].valid = false;
    }
    g_display_manager.num_dirty_regions = 0;
}

static void dm_process_overlays(void) {
    if (g_display_manager.num_overlays == 0 || !g_display_manager.master_fb) {
        return;
    }

    overlay_client_t* ov = g_display_manager.overlays;
    while (ov) {
        if (ov->visible && ov->client && ov->client->framebuffer) {
            if (ov->opacity == 255) {
                dm_blit_region_opaque(g_display_manager.master_fb, ov->client->framebuffer,
                                     &ov->bounds);
            } else {
                dm_blit_region_alpha(g_display_manager.master_fb, ov->client->framebuffer,
                                    &ov->bounds, ov->opacity);
            }
        }
        ov = ov->next;
    }
}

const char* display_mode_to_string(display_mode_t mode) {
    switch (mode) {
        case DISPLAY_MODE_TTY_CONSOLE: return "TTY Console";
        case DISPLAY_MODE_DESKTOP: return "Desktop";
        case DISPLAY_MODE_FULLSCREEN_APP: return "Fullscreen App";
        case DISPLAY_MODE_TRANSITION: return "Transition";
        default: return "Unknown";
    }
}

display_mode_t display_string_to_mode(const char* mode_str) {
    if (!mode_str) return DISPLAY_MODE_TTY_CONSOLE;

    if (strcmp(mode_str, "tty") == 0 || strcmp(mode_str, "console") == 0) {
        return DISPLAY_MODE_TTY_CONSOLE;
    } else if (strcmp(mode_str, "desktop") == 0 || strcmp(mode_str, "canopy") == 0) {
        return DISPLAY_MODE_DESKTOP;
    } else if (strcmp(mode_str, "fullscreen") == 0 || strcmp(mode_str, "app") == 0) {
        return DISPLAY_MODE_FULLSCREEN_APP;
    }

    return DISPLAY_MODE_TTY_CONSOLE;
}

graphics_result_t display_manager_get_statistics(uint32_t* switches, uint32_t* last_switch_time) {
    if (!g_display_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    spin_lock(&g_display_manager_lock);
    if (switches) *switches = g_display_manager.mode_switch_count;
    if (last_switch_time) *last_switch_time = g_display_manager.last_switch_time;
    spin_unlock(&g_display_manager_lock);

    return GRAPHICS_SUCCESS;
}

void display_manager_set_tty_session(uint32_t session) {
    if (session >= 1 && session <= MAX_TTY_SESSIONS) {
        g_current_tty_session = session;
    }
}

uint32_t display_manager_get_tty_session(void) {
    return g_current_tty_session;
}

#else /* !HAS_GRAPHICS */

/* No-framebuffer display manager stubs. The kernel stays in TTY console
 * mode (the default); all DM calls succeed vacuously or report that the
 * requested mode is unavailable. g_current_tty_session is owned by
 * hotkey.c and only referenced here. */

extern uint32_t g_current_tty_session;

graphics_result_t display_manager_init(void)            { return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_shutdown(void)        { return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_register_client(const display_client_t* c) { (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_unregister_client(const char* n) { (void)n; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_activate_client(const char* n) { (void)n; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_get_active_client(display_client_t** c) { if (c) *c = NULL; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_switch_mode(display_mode_t mode, void* params) { (void)mode; (void)params; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_get_current_mode(display_mode_t* mode) { if (mode) *mode = DISPLAY_MODE_TTY_CONSOLE; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_get_available_modes(display_mode_t** modes, uint32_t* count) {
    if (modes)  *modes  = NULL;
    if (count)  *count  = 0;
    return GRAPHICS_SUCCESS;
}
graphics_result_t display_manager_process_frame(void)   { return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_process_input(const input_event_t* e) { (void)e; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_start_transition(display_mode_t f, display_mode_t t, uint32_t d) { (void)f; (void)t; (void)d; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_update_transition(void) { return GRAPHICS_SUCCESS; }
bool display_manager_is_in_transition(void)              { return false; }
graphics_result_t display_manager_set_default_mode(display_mode_t mode) { (void)mode; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_enable_hotkeys(bool enable) { (void)enable; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_set_transition_fade(bool enable, uint32_t duration_ms) { (void)enable; (void)duration_ms; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_add_overlay(const char* n, int32_t z, const graphics_rect_t* b, uint8_t o) { (void)n; (void)z; (void)b; (void)o; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_remove_overlay(const char* n) { (void)n; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_set_overlay_z_order(const char* n, int32_t z) { (void)n; (void)z; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_set_overlay_opacity(const char* n, uint8_t o) { (void)n; (void)o; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_set_overlay_visible(const char* n, bool v) { (void)n; (void)v; return GRAPHICS_ERROR_NOT_SUPPORTED; }
const char* display_mode_to_string(display_mode_t mode) { (void)mode; return "tty"; }
display_mode_t display_string_to_mode(const char* s)    { (void)s; return DISPLAY_MODE_TTY_CONSOLE; }
graphics_result_t display_manager_get_statistics(uint32_t* switches, uint32_t* last) {
    if (switches) *switches = 0;
    if (last)     *last = 0;
    return GRAPHICS_SUCCESS;
}
graphics_result_t display_manager_acquire_framebuffer(framebuffer_t** fb) { if (fb) *fb = NULL; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t display_manager_release_framebuffer(framebuffer_t* fb) { (void)fb; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_invalidate_region(const graphics_rect_t* r) { (void)r; return GRAPHICS_SUCCESS; }
graphics_result_t display_manager_invalidate_full(void)  { return GRAPHICS_SUCCESS; }
void display_manager_set_tty_session(uint32_t session)  { if (session >= 1 && session <= MAX_TTY_SESSIONS) g_current_tty_session = session; }
uint32_t display_manager_get_tty_session(void)           { return g_current_tty_session; }

#endif /* HAS_GRAPHICS */
