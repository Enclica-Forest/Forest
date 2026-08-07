#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "graphics/graphics_types.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DISPLAY_MODE_TTY_CONSOLE = 0,
    DISPLAY_MODE_DESKTOP = 1,
    DISPLAY_MODE_FULLSCREEN_APP = 2,
    DISPLAY_MODE_TRANSITION = 255
} display_mode_t;

#define MAX_TTY_SESSIONS 22
extern uint32_t g_current_tty_session;

#define DM_MAX_OVERLAYS 4
#define DM_MAX_DIRTY_REGIONS 16

/* Forward declaration so overlay_client_t can reference display_client_t. */
typedef struct display_client display_client_t;

typedef struct overlay_client {
    display_client_t* client;
    int32_t z_order;
    graphics_rect_t bounds;
    uint8_t opacity;
    bool visible;
    struct overlay_client* next;
} overlay_client_t;

typedef struct {
    graphics_rect_t rect;
    bool valid;
} dirty_region_t;

typedef struct display_client {
    const char* name;
    uint32_t priority;
    bool active;
    display_mode_t mode;

    framebuffer_t* framebuffer;
    /*
     * Set to true by dm_suspend_current_client() after it copies the live
     * framebuffer into the offscreen buffer.  dm_resume_client() only restores
     * the offscreen content when this flag is set, preventing a freshly-
     * registered (zeroed) offscreen buffer from overwriting the screen.
     */
    bool offscreen_valid;

    graphics_result_t (*suspend)(void* context);
    graphics_result_t (*resume)(void* context);
    graphics_result_t (*frame_callback)(framebuffer_t* fb, void* context);
    graphics_result_t (*input_callback)(const input_event_t* event, void* context);

    void* context;
    struct display_client* next;
} display_client_t;

typedef struct {
    display_client_t* clients;
    display_client_t* active_client;
    display_mode_t current_mode;
    display_mode_t pending_mode;
    display_mode_t default_mode;
    bool in_transition;

    framebuffer_t* master_fb;
    framebuffer_t* offscreen_fbs[8];
    uint32_t num_offscreen_fbs;
    framebuffer_t* transition_fb;

    uint32_t transition_start_time;
    uint32_t transition_duration_ms;
    bool transition_fade_enabled;

    bool hotkey_enabled;
    uint32_t hotkey_modifiers;

    overlay_client_t* overlays;
    uint32_t num_overlays;

    dirty_region_t dirty_regions[DM_MAX_DIRTY_REGIONS];
    uint32_t num_dirty_regions;
    bool full_redraw_pending;

    uint32_t vsync_counter;

    uint32_t mode_switch_count;
    uint32_t last_switch_time;
} display_manager_state_t;

graphics_result_t display_manager_init(void);
graphics_result_t display_manager_shutdown(void);

graphics_result_t display_manager_register_client(const display_client_t* client);
graphics_result_t display_manager_unregister_client(const char* name);
graphics_result_t display_manager_activate_client(const char* name);
graphics_result_t display_manager_get_active_client(display_client_t** client);

graphics_result_t display_manager_switch_mode(display_mode_t mode, void* params);
graphics_result_t display_manager_get_current_mode(display_mode_t* mode);
graphics_result_t display_manager_get_available_modes(display_mode_t** modes, uint32_t* count);

graphics_result_t display_manager_process_frame(void);
graphics_result_t display_manager_process_input(const input_event_t* event);

graphics_result_t display_manager_start_transition(display_mode_t from_mode, display_mode_t to_mode, uint32_t duration_ms);
graphics_result_t display_manager_update_transition(void);
bool display_manager_is_in_transition(void);

graphics_result_t display_manager_set_default_mode(display_mode_t mode);
graphics_result_t display_manager_enable_hotkeys(bool enable);
graphics_result_t display_manager_set_transition_fade(bool enable, uint32_t duration_ms);

graphics_result_t display_manager_add_overlay(const char* client_name, int32_t z_order, const graphics_rect_t* bounds, uint8_t opacity);
graphics_result_t display_manager_remove_overlay(const char* client_name);
graphics_result_t display_manager_set_overlay_z_order(const char* client_name, int32_t z_order);
graphics_result_t display_manager_set_overlay_opacity(const char* client_name, uint8_t opacity);
graphics_result_t display_manager_set_overlay_visible(const char* client_name, bool visible);

const char* display_mode_to_string(display_mode_t mode);
display_mode_t display_string_to_mode(const char* mode_str);
graphics_result_t display_manager_get_statistics(uint32_t* switches, uint32_t* last_switch_time);

graphics_result_t display_manager_acquire_framebuffer(framebuffer_t** fb);
graphics_result_t display_manager_release_framebuffer(framebuffer_t* fb);
graphics_result_t display_manager_invalidate_region(const graphics_rect_t* region);
graphics_result_t display_manager_invalidate_full(void);

void display_manager_set_tty_session(uint32_t session);
uint32_t display_manager_get_tty_session(void);

#endif // DISPLAY_MANAGER_H
