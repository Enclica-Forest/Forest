#include "include/mode_state.h"
#include "include/display_manager.h"
#include "include/graphics/graphics_manager.h"
#include "include/debuglog.h"
#include "include/mm.h"
#include "include/memory.h" // For kmalloc declaration
#include "include/string.h"
#include "include/spinlock.h"
#include "include/timer.h"

#define GFP_KERNEL 0x01

// Global mode state manager
static mode_state_manager_t g_mode_state = {0};
static bool g_mode_state_initialized = false;
static spinlock_t g_mode_state_lock;

graphics_result_t mode_state_init(void) {
    if (g_mode_state_initialized) {
        return GRAPHICS_SUCCESS;
    }
    
    debuglog_printf("Initializing Mode State Manager...\n");
    
    spinlock_init(&g_mode_state_lock, "mode_state");
    
    // Initialize all state buffers as invalid
    memset(&g_mode_state, 0, sizeof(mode_state_manager_t));
    g_mode_state.current_mode = DISPLAY_MODE_TTY_CONSOLE;
    g_mode_state.saved_mode = DISPLAY_MODE_TTY_CONSOLE;
    
    // Initialize TTY state
    memset(&g_mode_state.tty_state, 0, sizeof(tty_state_t));
    g_mode_state.tty_state.screen_width = 80;
    g_mode_state.tty_state.screen_height = 25;
    g_mode_state.tty_state.default_color = 0x07;  // Light gray on black
    g_mode_state.tty_state.current_color = 0x07;
    g_mode_state.tty_state.font_width = 8;
    g_mode_state.tty_state.font_height = 16;
    
    // Initialize Canopy state
    memset(&g_mode_state.forest_desktop_state, 0, sizeof(forest_desktop_state_t));
    g_mode_state.forest_desktop_state.desktop_width = 1024;
    g_mode_state.forest_desktop_state.desktop_height = 768;
    g_mode_state.forest_desktop_state.desktop_color.r = 50;
    g_mode_state.forest_desktop_state.desktop_color.g = 50;
    g_mode_state.forest_desktop_state.desktop_color.b = 50;
    g_mode_state.forest_desktop_state.desktop_color.a = 255;
    g_mode_state.forest_desktop_state.compositor_active = false;
    g_mode_state.forest_desktop_state.animations_enabled = true;
    g_mode_state.forest_desktop_state.animation_speed = 60;  // 60 FPS default
    g_mode_state.forest_desktop_state.panel_visible = true;
    g_mode_state.forest_desktop_state.panel_height = 32;
    
    // Initialize statistics
    g_mode_state.save_count = 0;
    g_mode_state.restore_count = 0;
    g_mode_state.total_state_size = 0;
    
    g_mode_state_initialized = true;
    debuglog_printf("Mode State Manager initialized successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_shutdown(void) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_SUCCESS;
    }
    
    spin_lock(&g_mode_state_lock);
    
    debuglog_printf("Shutting down Mode State Manager...\n");
    
    // Free all state buffers
    for (uint32_t i = 0; i < 16; i++) {
        if (g_mode_state.state_buffers[i]) {
            kfree(g_mode_state.state_buffers[i]);
            g_mode_state.state_buffers[i] = NULL;
        }
        g_mode_state.buffer_sizes[i] = 0;
        g_mode_state.buffer_valid[i] = false;
    }
    
    g_mode_state_initialized = false;
    
    spin_unlock(&g_mode_state_lock);
    
    debuglog_printf("Mode State Manager shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_save(display_mode_t mode) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    graphics_result_t result = GRAPHICS_SUCCESS;
    
    switch (mode) {
        case DISPLAY_MODE_TTY_CONSOLE:
            result = mode_state_save_tty();
            break;
            
        case DISPLAY_MODE_DESKTOP:
            result = mode_state_save_canopy();
            break;
            
        default:
            result = GRAPHICS_ERROR_NOT_SUPPORTED;
            break;
    }
    
    if (result == GRAPHICS_SUCCESS) {
        g_mode_state.save_count++;
        debuglog_printf("Saved state for mode %d\n", mode);
    } else {
        debuglog_printf("Failed to save state for mode %d: %d\n", mode, result);
    }
    
    spin_unlock(&g_mode_state_lock);
    return result;
}

graphics_result_t mode_state_restore(display_mode_t mode) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    graphics_result_t result = GRAPHICS_SUCCESS;
    
    switch (mode) {
        case DISPLAY_MODE_TTY_CONSOLE:
            result = mode_state_restore_tty();
            break;
            
        case DISPLAY_MODE_DESKTOP:
            result = mode_state_restore_canopy();
            break;
            
        default:
            result = GRAPHICS_ERROR_NOT_SUPPORTED;
            break;
    }
    
    if (result == GRAPHICS_SUCCESS) {
        g_mode_state.restore_count++;
        g_mode_state.current_mode = mode;
        debuglog_printf("Restored state for mode %d\n", mode);
    } else {
        debuglog_printf("Failed to restore state for mode %d: %d\n", mode, result);
    }
    
    spin_unlock(&g_mode_state_lock);
    return result;
}

graphics_result_t mode_state_save_tty(void) {
    tty_state_t* tty = &g_mode_state.tty_state;
    
    debuglog_printf("Saving TTY state...\n");
    
    // Save cursor position
    // For now, use current text mode cursor position
    tty->cursor_visible = true;
    tty->cursor_color = tty->current_color;
    
    // Save screen buffer if we have access to it
    // This would interface with the TTY system
    if (tty->screen_buffer == NULL) {
        size_t buffer_size = tty->screen_width * tty->screen_height * 2;  // 2 bytes per character
        tty->screen_buffer = kmalloc(buffer_size);
        if (!tty->screen_buffer) {
            return GRAPHICS_ERROR_OUT_OF_MEMORY;
        }
    }
    
    // Save scrollback buffer (simplified - allocate if needed)
    if (tty->scrollback_buffer == NULL) {
        tty->scrollback_size = 1024 * 80 * 2;  // 1024 lines of 80 chars
        tty->scrollback_buffer = kmalloc(tty->scrollback_size);
        if (!tty->scrollback_buffer) {
            return GRAPHICS_ERROR_OUT_OF_MEMORY;
        }
        tty->scrollback_lines = 0;
        tty->scrollback_offset = 0;
    }
    
    // Mark TTY state as valid
    g_mode_state.buffer_valid[DISPLAY_MODE_TTY_CONSOLE] = true;
    
    debuglog_printf("TTY state saved successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_restore_tty(void) {
    tty_state_t* tty = &g_mode_state.tty_state;
    
    debuglog_printf("Restoring TTY state...\n");
    
    if (!g_mode_state.buffer_valid[DISPLAY_MODE_TTY_CONSOLE]) {
        debuglog_printf("No valid TTY state available\n");
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Restore cursor position
    // This would interface with the TTY system to restore cursor
    
    // Restore screen buffer if available
    if (tty->screen_buffer) {
        // Restore to actual screen buffer
        // This would need to interface with the TTY system
    }
    
    // Restore scrollback buffer if available
    if (tty->scrollback_buffer) {
        // Restore scrollback state
        // This would interface with the TTY system
    }
    
    debuglog_printf("TTY state restored successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_save_canopy(void) {
    forest_desktop_state_t* canopy = &g_mode_state.forest_desktop_state;
    
    debuglog_printf("Saving Canopy desktop state...\n");
    
    // Save current desktop dimensions from graphics system
    graphics_device_t* graphics_dev = graphics_get_primary_device();
    if (graphics_dev && graphics_dev->current_fb) {
        canopy->desktop_width = graphics_dev->current_fb->width;
        canopy->desktop_height = graphics_dev->current_fb->height;
    }
    
    // Save window states
    // This would interface with the Canopy window manager
    // For now, just clear the window data
    memset(canopy->window_data, 0, sizeof(canopy->window_data));
    canopy->num_windows = 0;
    canopy->active_window_id = 0;
    canopy->focused_window_id = 0;
    
    // Save compositor state
    // This would interface with the Canopy compositor
    canopy->compositor_active = true;  // Assume active if being saved
    
    // Save panel/taskbar state
    // This would interface with the Canopy panel system
    canopy->num_panel_items = 0;
    
    // Save input state
    // This would interface with the input system
    canopy->mouse_x = 0;
    canopy->mouse_y = 0;
    canopy->mouse_buttons = 0;
    canopy->keyboard_focus_desktop = true;
    
    // Mark Canopy state as valid
    g_mode_state.buffer_valid[DISPLAY_MODE_DESKTOP] = true;
    
    debuglog_printf("Canopy state saved successfully (resolution: %dx%d)\n", 
                   canopy->desktop_width, canopy->desktop_height);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_restore_canopy(void) {
    forest_desktop_state_t* canopy = &g_mode_state.forest_desktop_state;
    
    debuglog_printf("Restoring Canopy desktop state...\n");
    
    if (!g_mode_state.buffer_valid[DISPLAY_MODE_DESKTOP]) {
        debuglog_printf("No valid Canopy state available\n");
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Restore desktop dimensions
    // This would interface with the graphics system
    
    // Restore window states
    // This would interface with the Canopy window manager
    
    // Restore compositor state
    // This would interface with the Canopy compositor
    
    // Restore panel/taskbar state
    // This would interface with the Canopy panel system
    
    // Restore input state
    // This would interface with the input system
    
    debuglog_printf("Canopy state restored successfully (resolution: %dx%d)\n", 
                   canopy->desktop_width, canopy->desktop_height);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_get_tty_state(tty_state_t** state) {
    if (!g_mode_state_initialized || !state) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    *state = &g_mode_state.tty_state;
    spin_unlock(&g_mode_state_lock);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_get_forest_desktop_state(forest_desktop_state_t** state) {
    if (!g_mode_state_initialized || !state) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    *state = &g_mode_state.forest_desktop_state;
    spin_unlock(&g_mode_state_lock);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_start_transition(display_mode_t from_mode, display_mode_t to_mode) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    g_mode_state.saved_mode = from_mode;
    g_mode_state.in_transition = true;
    g_mode_state.transition_start_time = timer_get_ticks();
    
    debuglog_printf("Starting transition from mode %d to %d\n", from_mode, to_mode);
    
    spin_unlock(&g_mode_state_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_complete_transition(bool success) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    if (success) {
        debuglog_printf("Transition completed successfully\n");
    } else {
        debuglog_printf("Transition failed, rolling back\n");
        mode_state_rollback_transition();
    }
    
    g_mode_state.in_transition = false;
    
    spin_unlock(&g_mode_state_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_rollback_transition(void) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    display_mode_t saved_mode = g_mode_state.saved_mode;
    
    debuglog_printf("Rolling back to mode %d\n", saved_mode);
    
    // Restore the saved mode state
    return mode_state_restore(saved_mode);
}

graphics_result_t mode_state_get_statistics(uint32_t* total_size, uint32_t* save_count, uint32_t* restore_count) {
    if (!g_mode_state_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    if (total_size) *total_size = g_mode_state.total_state_size;
    if (save_count) *save_count = g_mode_state.save_count;
    if (restore_count) *restore_count = g_mode_state.restore_count;
    
    spin_unlock(&g_mode_state_lock);
    
    return GRAPHICS_SUCCESS;
}

uint32_t mode_state_calculate_checksum(const void* data, uint32_t size) {
    if (!data || size == 0) {
        return 0;
    }
    
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t checksum = 0;
    
    for (uint32_t i = 0; i < size; i++) {
        checksum += bytes[i];
        checksum = (checksum << 1) | (checksum >> 31);  // Rotate left
    }
    
    return checksum;
}

bool mode_state_is_valid(display_mode_t mode) {
    if (!g_mode_state_initialized || mode >= 16) {
        return false;
    }
    
    bool valid;
    spin_lock(&g_mode_state_lock);
    valid = g_mode_state.buffer_valid[mode];
    spin_unlock(&g_mode_state_lock);
    
    return valid;
}

graphics_result_t mode_state_save_app(uint32_t pid) {
    // Placeholder for application state saving
    (void)pid;
    debuglog_printf("Application state saving not yet implemented for PID %u\n", pid);
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t mode_state_restore_app(uint32_t pid) {
    // Placeholder for application state restoration
    (void)pid;
    debuglog_printf("Application state restoration not yet implemented for PID %u\n", pid);
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t mode_state_get_app_state(uint32_t pid, app_state_t** state) {
    // Placeholder for getting application state
    (void)pid;
    (void)state;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t mode_state_remove_app_state(uint32_t pid) {
    // Placeholder for removing application state
    (void)pid;
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t mode_state_save_all(void) {
    graphics_result_t result = GRAPHICS_SUCCESS;
    
    // Save TTY state
    if (mode_state_is_valid(DISPLAY_MODE_TTY_CONSOLE) || 
        g_mode_state.current_mode == DISPLAY_MODE_TTY_CONSOLE) {
        graphics_result_t tty_result = mode_state_save_tty();
        if (tty_result != GRAPHICS_SUCCESS) {
            result = tty_result;
        }
    }
    
    // Save Canopy state
    if (mode_state_is_valid(DISPLAY_MODE_DESKTOP) || 
        g_mode_state.current_mode == DISPLAY_MODE_DESKTOP) {
        graphics_result_t canopy_result = mode_state_save_canopy();
        if (canopy_result != GRAPHICS_SUCCESS) {
            result = canopy_result;
        }
    }
    
    return result;
}

graphics_result_t mode_state_restore_all(void) {
    // Restore the current mode
    return mode_state_restore(g_mode_state.current_mode);
}

graphics_result_t mode_state_allocate_buffer(display_mode_t mode, uint32_t size) {
    if (mode >= 16 || size == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    if (g_mode_state.state_buffers[mode]) {
        kfree(g_mode_state.state_buffers[mode]);
    }
    
    uint8_t* buffer = kmalloc(size);
    if (!buffer) {
        spin_unlock(&g_mode_state_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    g_mode_state.state_buffers[mode] = buffer;
    g_mode_state.buffer_sizes[mode] = size;
    g_mode_state.buffer_valid[mode] = false;
    g_mode_state.total_state_size += size;
    
    spin_unlock(&g_mode_state_lock);
    
    debuglog_printf("Allocated %u bytes for mode %d\n", size, mode);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_free_buffer(display_mode_t mode) {
    if (mode >= 16) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    if (g_mode_state.state_buffers[mode]) {
        g_mode_state.total_state_size -= g_mode_state.buffer_sizes[mode];
        kfree(g_mode_state.state_buffers[mode]);
        g_mode_state.state_buffers[mode] = NULL;
        g_mode_state.buffer_sizes[mode] = 0;
        g_mode_state.buffer_valid[mode] = false;
    }
    
    spin_unlock(&g_mode_state_lock);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_validate_buffer(display_mode_t mode) {
    if (mode >= 16) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_mode_state_lock);
    
    bool valid = g_mode_state.buffer_valid[mode];
    if (valid && g_mode_state.state_buffers[mode] && g_mode_state.buffer_sizes[mode] > 0) {
        // Validate checksum if available
        // This would require storing checksums in the buffer header
    }
    
    spin_unlock(&g_mode_state_lock);
    
    return valid ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_INVALID_PARAMETER;
}

graphics_result_t mode_state_serialize_tty(const tty_state_t* state, void** buffer, uint32_t* size) {
    if (!state || !buffer || !size) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Calculate total size needed
    uint32_t total_size = sizeof(mode_state_header_t) + sizeof(tty_state_t);
    
    // Add size for dynamic buffers if they exist
    if (state->screen_buffer) {
        total_size += state->screen_width * state->screen_height * 2;  // 2 bytes per char
    }
    if (state->scrollback_buffer && state->scrollback_size > 0) {
        total_size += state->scrollback_size;
    }
    
    // Allocate buffer
    uint8_t* data = kmalloc(total_size);
    if (!data) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    uint32_t offset = 0;
    
    // Write header
    mode_state_header_t* header = (mode_state_header_t*)data;
    header->mode = DISPLAY_MODE_TTY_CONSOLE;
    header->version = 1;
    header->data_size = total_size - sizeof(mode_state_header_t);
    header->timestamp = timer_get_ticks();
    header->is_valid = true;
    offset += sizeof(mode_state_header_t);
    
    // Write TTY state structure
    memcpy(data + offset, state, sizeof(tty_state_t));
    offset += sizeof(tty_state_t);
    
    // Write screen buffer if it exists
    tty_state_t* mutable_state = (tty_state_t*)(data + sizeof(mode_state_header_t));
    if (state->screen_buffer && state->screen_width > 0 && state->screen_height > 0) {
        uint32_t screen_size = state->screen_width * state->screen_height * 2;
        memcpy(data + offset, state->screen_buffer, screen_size);
        mutable_state->screen_buffer = (uint16_t*)(offset);  // Store offset
        offset += screen_size;
    } else {
        mutable_state->screen_buffer = NULL;
    }
    
    // Write scrollback buffer if it exists
    if (state->scrollback_buffer && state->scrollback_size > 0) {
        memcpy(data + offset, state->scrollback_buffer, state->scrollback_size);
        mutable_state->scrollback_buffer = (uint16_t*)(offset);  // Store offset
        offset += state->scrollback_size;
    } else {
        mutable_state->scrollback_buffer = NULL;
    }
    
    // Calculate and store checksum
    header->checksum = mode_state_calculate_checksum(data + sizeof(mode_state_header_t), 
                                                  total_size - sizeof(mode_state_header_t));
    
    *buffer = data;
    *size = total_size;
    
    debuglog_printf("TTY state serialized: %u bytes\n", total_size);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_deserialize_tty(const void* buffer, uint32_t size, tty_state_t** state) {
    if (!buffer || size < sizeof(mode_state_header_t) || !state) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    const uint8_t* data = (const uint8_t*)buffer;
    uint32_t offset = 0;
    
    // Read and validate header
    const mode_state_header_t* header = (const mode_state_header_t*)data;
    if (header->mode != DISPLAY_MODE_TTY_CONSOLE || !header->is_valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    if (size != sizeof(mode_state_header_t) + header->data_size) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Verify checksum
    uint32_t calculated_checksum = mode_state_calculate_checksum(data + sizeof(mode_state_header_t), 
                                                              header->data_size);
    if (calculated_checksum != header->checksum) {
        debuglog_printf("TTY state checksum mismatch: expected 0x%x, got 0x%x\n", 
                      header->checksum, calculated_checksum);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    offset += sizeof(mode_state_header_t);
    
    // Allocate TTY state structure
    tty_state_t* tty_state = kmalloc(sizeof(tty_state_t));
    if (!tty_state) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    // Copy TTY state structure
    memcpy(tty_state, data + offset, sizeof(tty_state_t));
    offset += sizeof(tty_state_t);
    
    // Restore screen buffer if it was saved
    if (tty_state->screen_buffer != NULL) {
        uint32_t screen_size = tty_state->screen_width * tty_state->screen_height * 2;
        uint16_t* screen_buffer = kmalloc(screen_size);
        if (!screen_buffer) {
            kfree(tty_state);
            return GRAPHICS_ERROR_OUT_OF_MEMORY;
        }
        
        memcpy(screen_buffer, data + (uintptr_t)tty_state->screen_buffer, screen_size);
        tty_state->screen_buffer = screen_buffer;
    }
    
    // Restore scrollback buffer if it was saved
    if (tty_state->scrollback_buffer != NULL && tty_state->scrollback_size > 0) {
        uint16_t* scrollback_buffer = kmalloc(tty_state->scrollback_size);
        if (!scrollback_buffer) {
            if (tty_state->screen_buffer) kfree(tty_state->screen_buffer);
            kfree(tty_state);
            return GRAPHICS_ERROR_OUT_OF_MEMORY;
        }
        
        memcpy(scrollback_buffer, data + (uintptr_t)tty_state->scrollback_buffer, 
               tty_state->scrollback_size);
        tty_state->scrollback_buffer = scrollback_buffer;
    }
    
    *state = tty_state;
    
    debuglog_printf("TTY state deserialized successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_serialize_canopy(const forest_desktop_state_t* state, void** buffer, uint32_t* size) {
    if (!state || !buffer || !size) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Calculate total size needed
    uint32_t total_size = sizeof(mode_state_header_t) + sizeof(forest_desktop_state_t);
    
    // Add size for wallpaper path if it exists
    uint32_t wallpaper_len = 0;
    if (state->wallpaper_path[0] != '\0') {
        wallpaper_len = strlen(state->wallpaper_path) + 1;
        total_size += wallpaper_len;
    }
    
    // Add size for panel items if they exist
    if (state->num_panel_items > 0) {
        // Simplified: assume each panel item is 64 bytes
        total_size += state->num_panel_items * 64;
    }
    
    // Allocate buffer
    uint8_t* data = kmalloc(total_size);
    if (!data) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    uint32_t offset = 0;
    
    // Write header
    mode_state_header_t* header = (mode_state_header_t*)data;
    header->mode = DISPLAY_MODE_DESKTOP;
    header->version = 1;
    header->data_size = total_size - sizeof(mode_state_header_t);
    header->timestamp = timer_get_ticks();
    header->is_valid = true;
    offset += sizeof(mode_state_header_t);
    
    // Write Canopy state structure
    memcpy(data + offset, state, sizeof(forest_desktop_state_t));
    offset += sizeof(forest_desktop_state_t);
    
    // Write wallpaper path if it exists
    forest_desktop_state_t* mutable_state = (forest_desktop_state_t*)(data + sizeof(mode_state_header_t));
    if (wallpaper_len > 0) {
        memcpy(data + offset, state->wallpaper_path, wallpaper_len);
        mutable_state->wallpaper_path[0] = (char)(offset & 0xFF);  // Store offset as indicator
        mutable_state->wallpaper_path[1] = (char)((offset >> 8) & 0xFF);
        offset += wallpaper_len;
    } else {
        mutable_state->wallpaper_path[0] = '\0';
    }
    
    // Write panel items if they exist
    if (state->num_panel_items > 0) {
        // For now, just write placeholder data
        memset(data + offset, 0, state->num_panel_items * 64);
        offset += state->num_panel_items * 64;
    }
    
    // Calculate and store checksum
    header->checksum = mode_state_calculate_checksum(data + sizeof(mode_state_header_t), 
                                                  total_size - sizeof(mode_state_header_t));
    
    *buffer = data;
    *size = total_size;
    
    debuglog_printf("Canopy state serialized: %u bytes (%u windows)\n", 
                   total_size, state->num_windows);
    return GRAPHICS_SUCCESS;
}

graphics_result_t mode_state_deserialize_canopy(const void* buffer, uint32_t size, forest_desktop_state_t** state) {
    if (!buffer || size < sizeof(mode_state_header_t) || !state) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    const uint8_t* data = (const uint8_t*)buffer;
    uint32_t offset = 0;
    
    // Read and validate header
    const mode_state_header_t* header = (const mode_state_header_t*)data;
    if (header->mode != DISPLAY_MODE_DESKTOP || !header->is_valid) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    if (size != sizeof(mode_state_header_t) + header->data_size) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Verify checksum
    uint32_t calculated_checksum = mode_state_calculate_checksum(data + sizeof(mode_state_header_t), 
                                                              header->data_size);
    if (calculated_checksum != header->checksum) {
        debuglog_printf("Canopy state checksum mismatch: expected 0x%x, got 0x%x\n", 
                      header->checksum, calculated_checksum);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    offset += sizeof(mode_state_header_t);
    
    // Allocate Canopy state structure
    forest_desktop_state_t* forest_desktop_state = kmalloc(sizeof(forest_desktop_state_t));
    if (!forest_desktop_state) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    // Copy Canopy state structure
    memcpy(forest_desktop_state, data + offset, sizeof(forest_desktop_state_t));
    offset += sizeof(forest_desktop_state_t);
    
    // Restore wallpaper path if it was saved
    if (forest_desktop_state->wallpaper_path[0] != '\0' && forest_desktop_state->wallpaper_path[1] != '\0') {
        // Calculate the offset from the first two bytes
        uint32_t wallpaper_offset = (uint8_t)forest_desktop_state->wallpaper_path[0] | 
                                  ((uint8_t)forest_desktop_state->wallpaper_path[1] << 8);
        
        if (wallpaper_offset < size) {
            const char* wallpaper_path = (const char*)(data + wallpaper_offset);
            size_t wallpaper_len = strlen(wallpaper_path);
            if (wallpaper_len < sizeof(forest_desktop_state->wallpaper_path)) {
                strcpy(forest_desktop_state->wallpaper_path, wallpaper_path);
            }
        }
    } else {
        forest_desktop_state->wallpaper_path[0] = '\0';
    }
    
    // Skip panel items data
    if (forest_desktop_state->num_panel_items > 0) {
        offset += forest_desktop_state->num_panel_items * 64;
    }
    
    *state = forest_desktop_state;
    
    debuglog_printf("Canopy state deserialized successfully (%u windows)\n", 
                   forest_desktop_state->num_windows);
    return GRAPHICS_SUCCESS;
}