#ifndef MODE_STATE_H
#define MODE_STATE_H

#include "graphics/graphics_types.h"
#include "display_manager.h"
#include "types.h"
#include <stdint.h>
#include <stdbool.h>

// Maximum state data size per mode (4MB)
#define MODE_STATE_MAX_SIZE (4 * 1024 * 1024)

// Mode state header structure
typedef struct {
    display_mode_t mode;
    uint32_t version;
    uint32_t data_size;
    uint64_t timestamp;
    uint32_t checksum;
    bool is_valid;
} mode_state_header_t;

// TTY state structure
typedef struct {
    // Cursor state
    uint32_t cursor_x, cursor_y;
    bool cursor_visible;
    uint8_t cursor_color;
    
    // Screen content
    uint16_t* screen_buffer;  // VGA text mode buffer
    uint32_t screen_width, screen_height;
    
    // Scrollback buffer
    uint16_t* scrollback_buffer;
    uint32_t scrollback_size;
    uint32_t scrollback_offset;
    uint32_t scrollback_lines;
    
    // Color and attributes
    uint8_t default_color;
    uint8_t current_color;
    
    // Console state
    bool in_escape_sequence;
    char escape_sequence[32];
    uint8_t escape_pos;
    
    // Font and rendering state
    uint8_t font_width, font_height;
    bool double_buffered;
} tty_state_t;

// Forest desktop state structure
typedef struct {
    // Desktop properties
    uint32_t desktop_width, desktop_height;
    graphics_color_t desktop_color;
    char wallpaper_path[256];
    
    // Window manager state
    uint32_t num_windows;
    uint32_t active_window_id;
    uint32_t focused_window_id;
    
    // Window data (simplified)
    struct {
        uint32_t id;
        int32_t x, y;
        uint32_t width, height;
        bool visible;
        bool minimized;
        bool maximized;
        char title[128];
        uint32_t z_order;
    } window_data[64];  // Support up to 64 windows
    
    // Compositor state
    bool compositor_active;
    bool animations_enabled;
    uint32_t animation_speed;
    
    // Panel/taskbar state
    bool panel_visible;
    uint32_t panel_height;
    uint32_t num_panel_items;
    
    // Input state
    uint32_t mouse_x, mouse_y;
    uint32_t mouse_buttons;
    bool keyboard_focus_desktop;
    
} forest_desktop_state_t;

// Application state structure
typedef struct {
    uint32_t pid;
    uint32_t app_id;
    char app_name[64];
    
    // Graphics context
    framebuffer_t* saved_framebuffer;
    graphics_surface_t* saved_surface;
    
    // Input state
    bool has_keyboard_focus;
    bool has_mouse_focus;
    
    // Window state (if applicable)
    uint32_t num_windows;
    struct {
        uint32_t id;
        int32_t x, y;
        uint32_t width, height;
        bool visible;
    } app_windows[16];
    
    // Application-specific state data
    void* custom_state;
    uint32_t custom_state_size;
    
} app_state_t;

// Mode state manager
typedef struct {
    // Current mode states
    tty_state_t tty_state;
    forest_desktop_state_t forest_desktop_state;
    
    // State buffers
    uint8_t* state_buffers[16];  // One buffer per possible mode
    uint32_t buffer_sizes[16];
    bool buffer_valid[16];
    
    // Current context
    display_mode_t current_mode;
    display_mode_t saved_mode;
    
    // Transition state
    bool in_transition;
    uint64_t transition_start_time;
    
    // Statistics
    uint32_t save_count;
    uint32_t restore_count;
    uint32_t total_state_size;
    
} mode_state_manager_t;

// Mode state API
graphics_result_t mode_state_init(void);
graphics_result_t mode_state_shutdown(void);

// State save/restore operations
graphics_result_t mode_state_save(display_mode_t mode);
graphics_result_t mode_state_restore(display_mode_t mode);
graphics_result_t mode_state_save_all(void);
graphics_result_t mode_state_restore_all(void);

// TTY-specific operations
graphics_result_t mode_state_save_tty(void);
graphics_result_t mode_state_restore_tty(void);
graphics_result_t mode_state_get_tty_state(tty_state_t** state);

// Forest desktop-specific operations
graphics_result_t mode_state_save_canopy(void);
graphics_result_t mode_state_restore_canopy(void);
graphics_result_t mode_state_get_canopy_state(forest_desktop_state_t** state);

// Application state operations
graphics_result_t mode_state_save_app(uint32_t pid);
graphics_result_t mode_state_restore_app(uint32_t pid);
graphics_result_t mode_state_get_app_state(uint32_t pid, app_state_t** state);
graphics_result_t mode_state_remove_app_state(uint32_t pid);

// Transition management
graphics_result_t mode_state_start_transition(display_mode_t from_mode, display_mode_t to_mode);
graphics_result_t mode_state_complete_transition(bool success);
graphics_result_t mode_state_rollback_transition(void);

// Buffer management
graphics_result_t mode_state_allocate_buffer(display_mode_t mode, uint32_t size);
graphics_result_t mode_state_free_buffer(display_mode_t mode);
graphics_result_t mode_state_validate_buffer(display_mode_t mode);

// Utility functions
uint32_t mode_state_calculate_checksum(const void* data, uint32_t size);
bool mode_state_is_valid(display_mode_t mode);
graphics_result_t mode_state_get_statistics(uint32_t* total_size, uint32_t* save_count, uint32_t* restore_count);

// Serialization helpers
graphics_result_t mode_state_serialize_tty(const tty_state_t* state, void** buffer, uint32_t* size);
graphics_result_t mode_state_deserialize_tty(const void* buffer, uint32_t size, tty_state_t** state);
graphics_result_t mode_state_serialize_canopy(const forest_desktop_state_t* state, void** buffer, uint32_t* size);
graphics_result_t mode_state_deserialize_canopy(const void* buffer, uint32_t size, forest_desktop_state_t** state);

#endif // MODE_STATE_H