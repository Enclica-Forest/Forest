#ifndef HOTKEY_H
#define HOTKEY_H

#include "display_manager.h"
#include "types.h"
#include <stdint.h>
#include <stdbool.h>

// Hotkey modifier flags
#define HOTKEY_MOD_NONE    0x00
#define HOTKEY_MOD_CTRL    0x01
#define HOTKEY_MOD_ALT     0x02
#define HOTKEY_MOD_SHIFT   0x04
#define HOTKEY_MOD_SUPER   0x08
#define HOTKEY_MOD_SYSREQ  0x10  // PrintScreen/SysRq key

// Hotkey action types
typedef enum {
    HOTKEY_ACTION_SWITCH_MODE = 0,
    HOTKEY_ACTION_TOGGLE_DESKTOP,
    HOTKEY_ACTION_LOCK_SCREEN,
    HOTKEY_ACTION_LOGOUT,
    HOTKEY_ACTION_REBOOT,
    HOTKEY_ACTION_CUSTOM
} hotkey_action_t;

// Hotkey mapping structure
typedef struct {
    uint16_t modifiers;           // Bitmask of modifier keys
    uint8_t keycode;              // Main key code
    hotkey_action_t action;       // Action to perform
    display_mode_t target_mode;   // Target mode for switch actions
    const char* description;      // Human-readable description
    bool (*custom_handler)(void* context);  // Custom action handler
    void* context;                // Context for custom handler
} hotkey_mapping_t;

// Hotkey manager state
typedef struct {
    hotkey_mapping_t* mappings;
    uint32_t num_mappings;
    uint32_t max_mappings;
    bool enabled;
    
    // Current key state
    uint16_t current_modifiers;
    bool keys_pressed[256];  // Track pressed keys
    
    // Statistics
    uint32_t hotkey_triggered_count;
    uint32_t last_hotkey_time;
} hotkey_manager_t;

// Hotkey API
graphics_result_t hotkey_init(void);
graphics_result_t hotkey_shutdown(void);
graphics_result_t hotkey_enable(bool enable);

// Hotkey mapping management
graphics_result_t hotkey_register_mapping(const hotkey_mapping_t* mapping);
graphics_result_t hotkey_unregister_mapping(uint16_t modifiers, uint8_t keycode);
graphics_result_t hotkey_get_mappings(hotkey_mapping_t** mappings, uint32_t* count);

// Input processing
graphics_result_t hotkey_process_key_event(uint8_t keycode, bool pressed, uint16_t modifiers);
bool hotkey_is_hotkey_triggered(uint16_t modifiers, uint8_t keycode);

// Default hotkey mappings
graphics_result_t hotkey_register_default_mappings(void);

// Utility functions
const char* hotkey_action_to_string(hotkey_action_t action);
const char* hotkey_modifiers_to_string(uint16_t modifiers);
graphics_result_t hotkey_get_statistics(uint32_t* triggered_count, uint32_t* last_time);

// Built-in hotkey handlers
bool hotkey_handler_switch_to_tty(void* context);
bool hotkey_handler_switch_to_desktop(void* context);
bool hotkey_handler_switch_to_fullscreen(void* context);
bool hotkey_handler_toggle_desktop(void* context);
bool hotkey_handler_switch_to_vt1(void* context);
bool hotkey_handler_switch_to_vt2(void* context);
bool hotkey_handler_switch_to_tty_vt(void* context);
bool hotkey_handler_sysreq_emergency_tty(void* context);

#endif // HOTKEY_H