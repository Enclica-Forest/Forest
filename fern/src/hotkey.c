#include "include/hotkey.h"
#include "include/display_manager.h"
#include "include/debuglog.h"
#include "include/mm.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/timer.h"
#include "include/ps2_keyboard.h"
#include "include/input_event.h"
#include "include/input_mux.h"
#include "include/tty.h"
#include "include/task.h"
#include "libc/stdio.h"

#define GFP_KERNEL 0x01
#define MAX_HOTKEY_MAPPINGS 32

/*
 * Input multiplexer integration
 * Hotkeys are registered at EXCLUSIVE priority so they receive events first
 * and can consume them before they reach focused applications.
 */
static input_consumer_t hotkey_input_consumer;
static bool hotkey_consumer_registered = false;

/* Track modifier state from input events */
static uint16_t current_input_modifiers = 0;

/*
 * Convert Linux keycode to internal keycode for hotkey matching
 * This maps the Linux evdev keycodes back to scan codes used by hotkey mappings
 */
static uint8_t linux_keycode_to_scancode(uint16_t linux_code) {
    /* Function keys F1-F12: Linux 59-70 -> scan codes 0x3B-0x44, 0x57, 0x58 */
    if (linux_code >= 59 && linux_code <= 68) {
        return 0x3B + (linux_code - 59);  /* F1-F10 */
    }
    if (linux_code == 87) return 0x57;    /* F11 */
    if (linux_code == 88) return 0x58;    /* F12 */

    /* PrintScreen/SysRq: Linux 99 -> scancode 0x54 */
    if (linux_code == 99) return 0x54;

    /* Modifier keys - return 0 (handled separately) */
    if (linux_code == 29 || linux_code == 97) return 0;   /* Ctrl */
    if (linux_code == 56 || linux_code == 100) return 0;  /* Alt */
    if (linux_code == 42 || linux_code == 54) return 0;   /* Shift */
    if (linux_code == 125 || linux_code == 126) return 0; /* Super/GUI */

    return (uint8_t)linux_code;
}

/*
 * Input event filter callback
 * Called for every keyboard event at EXCLUSIVE priority.
 * Returns true to consume the event (prevents delivery to other consumers).
 */
static bool hotkey_input_filter(const input_event_t* event, void* context) {
    (void)context;

    if (!event || event->type != EV_KEY) {
        return false;  /* Only process key events */
    }

    /* Update modifier state */
    bool pressed = (event->value == KEY_PRESS);

    switch (event->code) {
        case 29:   /* KEY_LEFTCTRL */
        case 97:   /* KEY_RIGHTCTRL */
            if (pressed) current_input_modifiers |= HOTKEY_MOD_CTRL;
            else current_input_modifiers &= ~HOTKEY_MOD_CTRL;
            return false;  /* Don't consume modifier-only events */

        case 56:   /* KEY_LEFTALT */
        case 100:  /* KEY_RIGHTALT */
            if (pressed) current_input_modifiers |= HOTKEY_MOD_ALT;
            else current_input_modifiers &= ~HOTKEY_MOD_ALT;
            return false;

        case 42:   /* KEY_LEFTSHIFT */
        case 54:   /* KEY_RIGHTSHIFT */
            if (pressed) current_input_modifiers |= HOTKEY_MOD_SHIFT;
            else current_input_modifiers &= ~HOTKEY_MOD_SHIFT;
            return false;

        case 125:  /* KEY_LEFTMETA */
        case 126:  /* KEY_RIGHTMETA */
            if (pressed) current_input_modifiers |= HOTKEY_MOD_SUPER;
            else current_input_modifiers &= ~HOTKEY_MOD_SUPER;
            return false;

        case 99:    /* KEY_PRINT/SysRq */
            if (pressed) current_input_modifiers |= HOTKEY_MOD_SYSREQ;
            else current_input_modifiers &= ~HOTKEY_MOD_SYSREQ;
            return false;
    }

    /* For non-modifier key presses, check if it's a hotkey */
    if (pressed) {
        uint8_t scancode = linux_keycode_to_scancode(event->code);
        debuglog_printf("[HOTKEY] Key pressed: code=%u, scancode=0x%02X, modifiers=0x%02X\n", 
                       event->code, scancode, current_input_modifiers);
        if (scancode && hotkey_is_hotkey_triggered(current_input_modifiers, scancode)) {
            /* Process the hotkey */
            debuglog_printf("[HOTKEY] Hotkey triggered! Processing...\n");
            hotkey_process_key_event(scancode, true, current_input_modifiers);
            return true;  /* Consume the event - don't pass to other consumers */
        } else {
            debuglog_printf("[HOTKEY] Not a registered hotkey\n");
        }
    }

    return false;  /* Don't consume - let it pass through */
}

// Global hotkey manager
static hotkey_manager_t g_hotkey_manager = {0};
static bool g_hotkey_manager_initialized = false;
static spinlock_t g_hotkey_lock;

uint32_t g_current_tty_session = 1;
static uint16_t g_last_trigger_modifiers = 0;
static uint8_t g_last_trigger_keycode = 0;
static uint32_t g_last_trigger_tick = 0;

graphics_result_t hotkey_init(void) {
    if (g_hotkey_manager_initialized) {
        return GRAPHICS_SUCCESS;
    }

    debuglog_printf("Initializing Hotkey Manager...\n");

    spinlock_init(&g_hotkey_lock, "hotkey");

    // Allocate mappings array
    g_hotkey_manager.mappings = kmalloc(sizeof(hotkey_mapping_t) * MAX_HOTKEY_MAPPINGS);
    if (!g_hotkey_manager.mappings) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    g_hotkey_manager.num_mappings = 0;
    g_hotkey_manager.max_mappings = MAX_HOTKEY_MAPPINGS;
    g_hotkey_manager.enabled = true;
    g_hotkey_manager.current_modifiers = 0;
    memset(g_hotkey_manager.keys_pressed, 0, sizeof(g_hotkey_manager.keys_pressed));

    // Initialize statistics
    g_hotkey_manager.hotkey_triggered_count = 0;
    g_hotkey_manager.last_hotkey_time = 0;

    // Allow registration during init
    g_hotkey_manager_initialized = true;

    // Register default mappings
    graphics_result_t result = hotkey_register_default_mappings();
    if (result != GRAPHICS_SUCCESS) {
        g_hotkey_manager_initialized = false;
        kfree(g_hotkey_manager.mappings);
        return result;
    }

    /*
     * Register with input multiplexer at EXCLUSIVE priority
     * This allows hotkeys to intercept events before focused applications
     */
    if (input_mux_is_initialized() && !hotkey_consumer_registered) {
        input_consumer_init(&hotkey_input_consumer, "hotkey_manager",
                           INPUT_PRIORITY_EXCLUSIVE, INPUT_MASK_KEYBOARD);

        hotkey_input_consumer.filter_callback = hotkey_input_filter;
        hotkey_input_consumer.callback_context = NULL;
        hotkey_input_consumer.has_keyboard_focus = true;

        if (input_mux_register_consumer(&hotkey_input_consumer)) {
            hotkey_consumer_registered = true;
            debuglog_printf("Hotkey manager registered with input multiplexer\n");
        } else {
            debuglog_printf("Warning: Failed to register hotkey manager with input multiplexer\n");
        }
    }

    g_hotkey_manager_initialized = true;
    debuglog_printf("Hotkey Manager initialized successfully (%u mappings)\n",
                   g_hotkey_manager.num_mappings);
    return GRAPHICS_SUCCESS;
}

graphics_result_t hotkey_shutdown(void) {
    if (!g_hotkey_manager_initialized) {
        return GRAPHICS_SUCCESS;
    }

    spin_lock(&g_hotkey_lock);

    debuglog_printf("Shutting down Hotkey Manager...\n");

    /* Unregister from input multiplexer */
    if (hotkey_consumer_registered && input_mux_is_initialized()) {
        input_mux_unregister_consumer(&hotkey_input_consumer);
        hotkey_consumer_registered = false;
        debuglog_printf("Hotkey manager unregistered from input multiplexer\n");
    }

    if (g_hotkey_manager.mappings) {
        kfree(g_hotkey_manager.mappings);
        g_hotkey_manager.mappings = NULL;
    }

    g_hotkey_manager.num_mappings = 0;
    g_hotkey_manager.enabled = false;
    g_hotkey_manager_initialized = false;

    spin_unlock(&g_hotkey_lock);

    debuglog_printf("Hotkey Manager shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t hotkey_enable(bool enable) {
    if (!g_hotkey_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_hotkey_lock);
    g_hotkey_manager.enabled = enable;
    spin_unlock(&g_hotkey_lock);
    
    debuglog_printf("Hotkey manager %s\n", enable ? "enabled" : "disabled");
    return GRAPHICS_SUCCESS;
}

graphics_result_t hotkey_register_mapping(const hotkey_mapping_t* mapping) {
    if (!g_hotkey_manager_initialized || !mapping || !mapping->description) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_hotkey_lock);
    
    if (g_hotkey_manager.num_mappings >= g_hotkey_manager.max_mappings) {
        spin_unlock(&g_hotkey_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    // Check for duplicate mapping
    for (uint32_t i = 0; i < g_hotkey_manager.num_mappings; i++) {
        if (g_hotkey_manager.mappings[i].modifiers == mapping->modifiers &&
            g_hotkey_manager.mappings[i].keycode == mapping->keycode) {
            spin_unlock(&g_hotkey_lock);
            return GRAPHICS_ERROR_DEVICE_BUSY;  // Already mapped
        }
    }
    
    // Add new mapping
    memcpy(&g_hotkey_manager.mappings[g_hotkey_manager.num_mappings], 
           mapping, sizeof(hotkey_mapping_t));
    g_hotkey_manager.num_mappings++;
    
    // Convert scancode to function key number for display
    uint8_t f_key_num = 0;
    if (mapping->keycode >= 0x3B && mapping->keycode <= 0x44) {
        f_key_num = mapping->keycode - 0x3B + 1;  // F1-F10
    } else if (mapping->keycode == 0x57) {
        f_key_num = 11;  // F11
    } else if (mapping->keycode == 0x58) {
        f_key_num = 12;  // F12
    }
    
    if (f_key_num > 0) {
        debuglog_printf("Registered hotkey: %s (%s + F%d)\n", 
                       mapping->description,
                       hotkey_modifiers_to_string(mapping->modifiers),
                       f_key_num);
    } else {
        debuglog_printf("Registered hotkey: %s (%s + 0x%02X)\n", 
                       mapping->description,
                       hotkey_modifiers_to_string(mapping->modifiers),
                       mapping->keycode);
    }
    
    spin_unlock(&g_hotkey_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t hotkey_unregister_mapping(uint16_t modifiers, uint8_t keycode) {
    if (!g_hotkey_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_hotkey_lock);
    
    // Find and remove mapping
    for (uint32_t i = 0; i < g_hotkey_manager.num_mappings; i++) {
        if (g_hotkey_manager.mappings[i].modifiers == modifiers &&
            g_hotkey_manager.mappings[i].keycode == keycode) {
            
            // Shift remaining mappings
            for (uint32_t j = i; j < g_hotkey_manager.num_mappings - 1; j++) {
                memcpy(&g_hotkey_manager.mappings[j], &g_hotkey_manager.mappings[j + 1], 
                       sizeof(hotkey_mapping_t));
            }
            
            g_hotkey_manager.num_mappings--;
            
            debuglog_printf("Unregistered hotkey: modifiers=0x%x, keycode=%d\n", 
                           modifiers, keycode);
            
            spin_unlock(&g_hotkey_lock);
            return GRAPHICS_SUCCESS;
        }
    }
    
    spin_unlock(&g_hotkey_lock);
    return GRAPHICS_ERROR_INVALID_PARAMETER;  // Not found
}

graphics_result_t hotkey_get_mappings(hotkey_mapping_t** mappings, uint32_t* count) {
    if (!g_hotkey_manager_initialized || !mappings || !count) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_hotkey_lock);
    
    // Allocate copy of mappings
    *mappings = kmalloc(sizeof(hotkey_mapping_t) * g_hotkey_manager.num_mappings);
    if (!*mappings) {
        spin_unlock(&g_hotkey_lock);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    memcpy(*mappings, g_hotkey_manager.mappings, 
           sizeof(hotkey_mapping_t) * g_hotkey_manager.num_mappings);
    *count = g_hotkey_manager.num_mappings;
    
    spin_unlock(&g_hotkey_lock);
    return GRAPHICS_SUCCESS;
}

graphics_result_t hotkey_process_key_event(uint8_t keycode, bool pressed, uint16_t modifiers) {
    if (!g_hotkey_manager_initialized || !g_hotkey_manager.enabled) {
        return GRAPHICS_SUCCESS;
    }
    
    spin_lock(&g_hotkey_lock);
    
    // Update key state
    // Note: keycode is already limited to 8-bit range
    g_hotkey_manager.keys_pressed[keycode] = pressed;
    
    g_hotkey_manager.current_modifiers = modifiers;
    
    // Check for hotkey trigger on key press
    if (pressed) {
        for (uint32_t i = 0; i < g_hotkey_manager.num_mappings; i++) {
            const hotkey_mapping_t* mapping = &g_hotkey_manager.mappings[i];
            
            if (mapping->modifiers == modifiers && mapping->keycode == keycode) {
                uint32_t now = timer_get_ticks();
                if (g_last_trigger_keycode == keycode &&
                    g_last_trigger_modifiers == modifiers &&
                    g_last_trigger_tick == now) {
                    spin_unlock(&g_hotkey_lock);
                    return GRAPHICS_SUCCESS;
                }
                g_last_trigger_keycode = keycode;
                g_last_trigger_modifiers = modifiers;
                g_last_trigger_tick = now;

                // Hotkey triggered!
                debuglog_printf("Hotkey triggered: %s\n", mapping->description);
                
                g_hotkey_manager.hotkey_triggered_count++;
                g_hotkey_manager.last_hotkey_time = now;
                
                // Execute action
                bool handled = false;
                switch (mapping->action) {
                    case HOTKEY_ACTION_SWITCH_MODE:
                        display_manager_switch_mode(mapping->target_mode, NULL);
                        handled = true;
                        break;
                        
                    case HOTKEY_ACTION_TOGGLE_DESKTOP:
                        if (mapping->custom_handler) {
                            handled = mapping->custom_handler(mapping->context);
                        } else {
                            handled = hotkey_handler_toggle_desktop(mapping->context);
                        }
                        break;
                        
                    case HOTKEY_ACTION_CUSTOM:
                        if (mapping->custom_handler) {
                            handled = mapping->custom_handler(mapping->context);
                        }
                        break;
                        
                    default:
                        debuglog_printf("Unhandled hotkey action: %d\n", mapping->action);
                        break;
                }
                
                if (handled) {
                    spin_unlock(&g_hotkey_lock);
                    return GRAPHICS_SUCCESS;  // Stop processing further
                }
            }
        }
    }
    
    spin_unlock(&g_hotkey_lock);
    return GRAPHICS_SUCCESS;
}

bool hotkey_is_hotkey_triggered(uint16_t modifiers, uint8_t keycode) {
    if (!g_hotkey_manager_initialized || !g_hotkey_manager.enabled) {
        return false;
    }
    
    bool found = false;
    spin_lock(&g_hotkey_lock);
    
    for (uint32_t i = 0; i < g_hotkey_manager.num_mappings; i++) {
        if (g_hotkey_manager.mappings[i].modifiers == modifiers &&
            g_hotkey_manager.mappings[i].keycode == keycode) {
            found = true;
            break;
        }
    }
    
    spin_unlock(&g_hotkey_lock);
    return found;
}

graphics_result_t hotkey_register_default_mappings(void) {
    graphics_result_t result;

    /*
     * Function key scancodes:
     * F1=0x3B, F2=0x3C, F3=0x3D, F4=0x3E, F5=0x3F, F6=0x40
     * F7=0x41, F8=0x42, F9=0x43, F10=0x44, F11=0x57, F12=0x58
     *
     * New mapping scheme:
     * Ctrl+Alt+F1-F2: Graphical modes (handled by display manager)
     * Ctrl+Alt+F3-F12: TTY Console sessions 3-12 (with separate buffers)
     */

    // Static VT numbers for TTY terminals
    static uint8_t tty_vts[10] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    // Ctrl+Alt+F1: Switch to GUI desktop (VT1 - graphical)
    hotkey_mapping_t vt1_mapping = {
        .modifiers = HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT,
        .keycode = 0x3B,  // F1
        .action = HOTKEY_ACTION_CUSTOM,
        .target_mode = DISPLAY_MODE_DESKTOP,
        .description = "Switch to GUI (VT1)",
        .custom_handler = hotkey_handler_switch_to_vt1,
        .context = NULL
    };
    result = hotkey_register_mapping(&vt1_mapping);
    if (result != GRAPHICS_SUCCESS) return result;

    // Ctrl+Alt+F2: Switch to alternate GUI (VT2 - graphical)
    hotkey_mapping_t vt2_mapping = {
        .modifiers = HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT,
        .keycode = 0x3C,  // F2
        .action = HOTKEY_ACTION_CUSTOM,
        .target_mode = DISPLAY_MODE_DESKTOP,
        .description = "Switch to GUI Alt (VT2)",
        .custom_handler = hotkey_handler_switch_to_vt2,
        .context = NULL
    };
    result = hotkey_register_mapping(&vt2_mapping);
    if (result != GRAPHICS_SUCCESS) return result;

    // Ctrl+Alt+F3-F12: Switch to TTY sessions 3-12
    for (uint8_t i = 0; i < 10; i++) {
        uint8_t f_key_num = 3 + i;  // F3-F12
        uint8_t scancode = (f_key_num <= 10) ? (0x3B + f_key_num - 1) : 
                           (f_key_num == 11) ? 0x57 : 0x58;
        
        // Create description with correct function key number
        static char tty_desc[32];
        snprintf(tty_desc, sizeof(tty_desc), "Switch to TTY (F%d)", f_key_num);
        
        hotkey_mapping_t tty_mapping = {
            .modifiers = HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT,
            .keycode = scancode,
            .action = HOTKEY_ACTION_CUSTOM,
            .target_mode = DISPLAY_MODE_TTY_CONSOLE,
            .description = tty_desc,
            .custom_handler = hotkey_handler_switch_to_tty_vt,
            .context = &tty_vts[i]
        };
        result = hotkey_register_mapping(&tty_mapping);
        if (result != GRAPHICS_SUCCESS) {
            return result;
        }
    }

    // Alt+PrintScreen (SysRq): Emergency switch from GUI to TTY console
    // This provides a quick way to escape from GUI back to text mode
    hotkey_mapping_t sysreq_mapping = {
        .modifiers = HOTKEY_MOD_ALT | HOTKEY_MOD_SYSREQ,
        .keycode = 0x54,  // PrintScreen/SysRq scancode
        .action = HOTKEY_ACTION_CUSTOM,
        .target_mode = DISPLAY_MODE_TTY_CONSOLE,
        .description = "SysRq: Switch to TTY console",
        .custom_handler = hotkey_handler_sysreq_emergency_tty,
        .context = NULL
    };
    result = hotkey_register_mapping(&sysreq_mapping);
    if (result != GRAPHICS_SUCCESS) return result;

    debuglog_printf("Registered %u default hotkey mappings\n", g_hotkey_manager.num_mappings);
    return GRAPHICS_SUCCESS;
}

const char* hotkey_action_to_string(hotkey_action_t action) {
    switch (action) {
        case HOTKEY_ACTION_SWITCH_MODE: return "Switch Mode";
        case HOTKEY_ACTION_TOGGLE_DESKTOP: return "Toggle Desktop";
        case HOTKEY_ACTION_LOCK_SCREEN: return "Lock Screen";
        case HOTKEY_ACTION_LOGOUT: return "Logout";
        case HOTKEY_ACTION_REBOOT: return "Reboot";
        case HOTKEY_ACTION_CUSTOM: return "Custom";
        default: return "Unknown";
    }
}

const char* hotkey_modifiers_to_string(uint16_t modifiers) {
    static char buffer[32];
    buffer[0] = '\0';
    
    if (modifiers & HOTKEY_MOD_CTRL) strcat(buffer, "Ctrl+");
    if (modifiers & HOTKEY_MOD_ALT) strcat(buffer, "Alt+");
    if (modifiers & HOTKEY_MOD_SHIFT) strcat(buffer, "Shift+");
    if (modifiers & HOTKEY_MOD_SUPER) strcat(buffer, "Super+");
    
    // Remove trailing +
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '+') {
        buffer[len - 1] = '\0';
    }
    
    return buffer;
}

graphics_result_t hotkey_get_statistics(uint32_t* triggered_count, uint32_t* last_time) {
    if (!g_hotkey_manager_initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    spin_lock(&g_hotkey_lock);
    if (triggered_count) *triggered_count = g_hotkey_manager.hotkey_triggered_count;
    if (last_time) *last_time = g_hotkey_manager.last_hotkey_time;
    spin_unlock(&g_hotkey_lock);
    
    return GRAPHICS_SUCCESS;
}

// Built-in hotkey handlers
bool hotkey_handler_switch_to_tty(void* context) {
    uint32_t session = context ? *(uint32_t*)context : 1;
    display_mode_t current_mode;

    if (display_manager_get_current_mode(&current_mode) != GRAPHICS_SUCCESS) {
        return false;
    }

    // Check if we're already on the requested TTY session
    if (current_mode == DISPLAY_MODE_TTY_CONSOLE && g_current_tty_session == session) {
        debuglog_printf("Already on TTY session %u\n", session);
        return true;  // Already there, consume the event
    }

    // Update the session number
    //
    // KNOWN GAP: this writes g_current_tty_session directly instead of going
    // through session.c's session_switch_to()/session_perform_transition()
    // pipeline, so that pipeline's framebuffer-snapshot cross-fade and
    // GUI-process SIGSTOP/SIGCONT (session_save_framebuffer/
    // session_restore_framebuffer/session_suspend_gui/session_resume_gui)
    // never run around this switch. See the "KNOWN GAP" comment above
    // session_switch_to() in src/session.c for why this was left as-is
    // rather than risk breaking working VT-switching by wiring it in.
    uint32_t old_session = g_current_tty_session;
    g_current_tty_session = session;

    debuglog_printf("Switching from TTY %u to TTY %u\n", old_session, session);

    if (current_mode != DISPLAY_MODE_TTY_CONSOLE) {
        // Switch to TTY mode from another mode
        display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE, NULL);
    } else {
        // Already in TTY mode, just need to refresh the display
        // Notify TTY to redraw with new session
        extern void tty_force_redraw(void);
        tty_force_redraw();
    }

    return true;
}

bool hotkey_handler_switch_to_desktop(void* context) {
    (void)context;
    display_mode_t current_mode;
    if (display_manager_get_current_mode(&current_mode) == GRAPHICS_SUCCESS) {
        if (current_mode != DISPLAY_MODE_DESKTOP) {
            display_manager_switch_mode(DISPLAY_MODE_DESKTOP, NULL);
            return true;
        }
    }
    return false;
}

bool hotkey_handler_switch_to_fullscreen(void* context) {
    (void)context;
    display_mode_t current_mode;
    if (display_manager_get_current_mode(&current_mode) == GRAPHICS_SUCCESS) {
        if (current_mode != DISPLAY_MODE_FULLSCREEN_APP) {
            display_manager_switch_mode(DISPLAY_MODE_FULLSCREEN_APP, NULL);
            return true;
        }
    }
    return false;
}

bool hotkey_handler_toggle_desktop(void* context) {
    (void)context;
    display_mode_t current_mode;
    if (display_manager_get_current_mode(&current_mode) == GRAPHICS_SUCCESS) {
        if (current_mode == DISPLAY_MODE_DESKTOP) {
            display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE, NULL);
        } else {
            display_manager_switch_mode(DISPLAY_MODE_DESKTOP, NULL);
        }
        return true;
    }
    return false;
}

// Handler for VT1 (F1) - Graphical desktop
bool hotkey_handler_switch_to_vt1(void* context) {
    (void)context;
    debuglog_printf("Hotkey: Switching to VT1 (GUI)\n");

    // Update session to 1 for GUI
    // KNOWN GAP: bypasses session.c's session_switch_to() pipeline (dead
    // code today); see the "KNOWN GAP" comment above session_switch_to()
    // in src/session.c.
    g_current_tty_session = 1;

    // Resume the WM render task so the kernel can paint the desktop
    extern uint32_t task_get_id_by_name_prefix(const char* prefix);
    uint32_t wm_pid = task_get_id_by_name_prefix("wm-");
    if (wm_pid != 0) {
        extern void task_resume(uint32_t pid);
        task_resume(wm_pid);
        debuglog_printf("[HOTKEY] Resumed WM render task (PID %u) for VT1\n", wm_pid);
    }

    // Switch to graphical mode
    debuglog_printf("Hotkey: Calling display_manager_switch_mode(DISPLAY_MODE_DESKTOP)\n");
    display_manager_switch_mode(DISPLAY_MODE_DESKTOP, NULL);
    debuglog_printf("Hotkey: display_manager_switch_mode returned\n");

    return true;
}

// Handler for VT2 (F2) - Alternate graphical mode
bool hotkey_handler_switch_to_vt2(void* context) {
    (void)context;
    debuglog_printf("Hotkey: Switching to VT2 (GUI Alt)\n");

    // Update session to 2 for GUI
    // KNOWN GAP: bypasses session.c's session_switch_to() pipeline (dead
    // code today); see the "KNOWN GAP" comment above session_switch_to()
    // in src/session.c.
    g_current_tty_session = 2;

    // Resume the WM render task so the kernel can paint the desktop
    extern uint32_t task_get_id_by_name_prefix(const char* prefix);
    uint32_t wm_pid = task_get_id_by_name_prefix("wm-");
    if (wm_pid != 0) {
        extern void task_resume(uint32_t pid);
        task_resume(wm_pid);
        debuglog_printf("[HOTKEY] Resumed WM render task (PID %u) for VT2\n", wm_pid);
    }

    // Switch to graphical mode (could be a different desktop environment)
    debuglog_printf("Hotkey: Calling display_manager_switch_mode(DISPLAY_MODE_DESKTOP)\n");
    display_manager_switch_mode(DISPLAY_MODE_DESKTOP, NULL);
    debuglog_printf("Hotkey: display_manager_switch_mode returned\n");

    return true;
}

// Handler for TTY virtual terminals (F3-F12, plus chvt for 13-24)
bool hotkey_handler_switch_to_tty_vt(void* context) {
    uint8_t vt_num = context ? *(uint8_t*)context : 3;

    if (vt_num < TTY_FIRST_TTY_VT || vt_num > TTY_LAST_TTY_VT) {
        debuglog_printf("Hotkey: Invalid VT number %u\n", vt_num);
        return false;
    }

    debuglog_printf("Hotkey: Switching to TTY VT%u (GUI->TTY)\n", vt_num);

    // Update current TTY session to match the VT number
    // VT 3 = Session 1, VT 4 = Session 2, etc.
    // KNOWN GAP: bypasses session.c's session_switch_to() pipeline (dead
    // code today); see the "KNOWN GAP" comment above session_switch_to()
    // in src/session.c.
    uint32_t new_session = vt_num - 2;
    if (new_session >= 1 && new_session <= MAX_TTY_SESSIONS) {
        g_current_tty_session = new_session;
    }

    // Release graphics app ownership before switching
    // This ensures the old GUI app doesn't keep writing to the framebuffer
    tty_set_graphics_app_active(false);

    // Unmap framebuffer from all userspace processes to prevent stale writes
    extern void framebuffer_mmap_unmap_all(void);
    framebuffer_mmap_unmap_all();

    // Resume the WM render task so the kernel can take back control
    extern uint32_t task_get_id_by_name_prefix(const char* prefix);
    uint32_t wm_pid = task_get_id_by_name_prefix("wm-");
    if (wm_pid != 0) {
        extern void task_resume(uint32_t pid);
        task_resume(wm_pid);
        debuglog_printf("[HOTKEY] Resumed WM render task (PID %u)\n", wm_pid);
    }

    // Switch to TTY mode FIRST before anything else
    debuglog_printf("Hotkey: Calling display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE)\n");
    display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE, NULL);
    debuglog_printf("Hotkey: display_manager_switch_mode returned\n");

    // Switch to the specified VT
    tty_switch_vt(vt_num);
    debuglog_printf("Hotkey: tty_switch_vt completed\n");

    return true;
}

// SysRq handler: Alt+PrintScreen to switch from GUI to TTY
// This provides an emergency way to exit GUI and return to text console
bool hotkey_handler_sysreq_emergency_tty(void* context) {
    (void)context;

    display_mode_t current_mode;
    if (display_manager_get_current_mode(&current_mode) != GRAPHICS_SUCCESS) {
        debuglog_printf("[SYSREQ] Failed to get current display mode\n");
        return false;
    }

    // If already in TTY console mode, just force redraw
    if (current_mode == DISPLAY_MODE_TTY_CONSOLE) {
        debuglog_printf("[SYSREQ] Already in TTY console mode, forcing redraw\n");
        extern void tty_force_redraw(void);
        tty_force_redraw();
        return true;
    }

    // Switch from GUI/Canopy to TTY console
    debuglog_printf("[SYSREQ] Emergency switch from GUI to TTY console\n");

    // Release graphics ownership
    tty_set_graphics_app_active(false);

    // Unmap framebuffer from all userspace processes
    extern void framebuffer_mmap_unmap_all(void);
    framebuffer_mmap_unmap_all();

    // Resume the WM render task to take back control of the display
    extern uint32_t task_get_id_by_name_prefix(const char* prefix);
    uint32_t wm_pid = task_get_id_by_name_prefix("wm-");
    if (wm_pid != 0) {
        extern void task_resume(uint32_t pid);
        task_resume(wm_pid);
        debuglog_printf("[SYSREQ] Resumed WM render task (PID %u)\n", wm_pid);
    }

    // Update session to TTY session 1
    // KNOWN GAP: bypasses session.c's session_switch_to() pipeline (dead
    // code today); see the "KNOWN GAP" comment above session_switch_to()
    // in src/session.c.
    g_current_tty_session = 1;

    // Switch display mode to TTY console
    display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE, NULL);

    // Force TTY redraw
    extern void tty_force_redraw(void);
    tty_force_redraw();

    debuglog_printf("[SYSREQ] Successfully switched to TTY console\n");
    return true;
}
