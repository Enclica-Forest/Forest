#include "include/cgdm_integration.h"
#include "include/display_manager.h"
#include "include/mode_state.h"
#include "include/graphics/graphics_manager.h"
#include "include/gfx_config.h"
#include "include/tty.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/input_event.h"
// Note: canopy/canopy.h is a userspace header, not available in kernel
// The canopy_is_running() function is declared as extern where needed

#if HAS_GRAPHICS

#define GFP_KERNEL 0x01

// TTY display client context
typedef struct {
    bool active;
    uint32_t ref_count;
} tty_cgdm_context_t;

static tty_cgdm_context_t g_tty_context = {0};
static display_client_t g_tty_client = {0};

graphics_result_t tty_cgdm_suspend(void* context) {
    (void)context;
    
    debuglog_printf("TTY CGDM suspend called\n");
    
    // Clear TTY status bar from screen
    extern void tty_clear_status_bar(void);
    tty_clear_status_bar();
    
    // Save TTY state
    graphics_result_t result = mode_state_save_tty();
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Failed to save TTY state: %d\n", result);
        return result;
    }
    
    // Mark TTY as inactive
    g_tty_context.active = false;
    
    debuglog_printf("TTY suspended successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t tty_cgdm_resume(void* context) {
    (void)context;
    
    debuglog_printf("TTY CGDM resume called\n");
    
    // Restore TTY state
    graphics_result_t result = mode_state_restore_tty();
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Failed to restore TTY state: %d\n", result);
        return result;
    }
    
    // Mark TTY as active
    g_tty_context.active = true;
    
    // IMPORTANT: Disable graphics app mode so TTY will render
    // This is necessary when switching from GUI to TTY
    extern void tty_set_graphics_app_active(bool active);
    tty_set_graphics_app_active(false);
    
    // CRITICAL: Clear the framebuffer to remove GUI content
    // The GUI content stays in video memory and TTY cells render "on top"
    // but text rendering doesn't clear the entire background
    extern void tty_clear_framebuffer_raw(void);
    tty_clear_framebuffer_raw();
    
    // Clear TTY screen to prepare for fresh content
    extern void tty_clear(void);
    tty_clear();
    
    // Draw TTY status bar with current session number
    extern void tty_draw_status_bar(void);
    tty_draw_status_bar();
    
    // Force redraw of TTY content
    extern void tty_force_redraw(void);
    tty_force_redraw();
    
    debuglog_printf("TTY resumed successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t tty_cgdm_frame_callback(framebuffer_t* fb, void* context) {
    (void)context;
    
    if (!g_tty_context.active || !fb) {
        return GRAPHICS_SUCCESS;
    }
    
    // TTY renders directly to framebuffer, no need to clear or redraw here
    // The TTY system handles its own rendering via tty_force_redraw() and
    // individual cell updates. Clearing here would wipe the TTY content.
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t tty_cgdm_input_callback(const input_event_t* event, void* context) {
    (void)context;
    
    if (!g_tty_context.active || !event) {
        return GRAPHICS_SUCCESS;
    }
    
    // Forward input to TTY system
    // input_event uses EV_KEY (0x01) for type, code for keycode, value (1=press, 0=release)
    if (event->type == 0x01) {  // EV_KEY
        // This would interface with TTY input handling
        debuglog_printf("TTY key event: code=%u, pressed=%d\n", 
                       event->code, event->value == 1);  // KEY_PRESS = 1
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t tty_cgdm_init(void) {
    debuglog_printf("Initializing TTY CGDM integration...\n");
    
    // Initialize TTY context
    memset(&g_tty_context, 0, sizeof(tty_cgdm_context_t));
    g_tty_context.active = true;
    g_tty_context.ref_count = 1;
    
    // Initialize TTY display client
    memset(&g_tty_client, 0, sizeof(display_client_t));
    g_tty_client.name = "TTY_Console";
    g_tty_client.priority = 100;  // High priority for TTY
    g_tty_client.active = true;
    g_tty_client.mode = DISPLAY_MODE_TTY_CONSOLE;
    g_tty_client.suspend = tty_cgdm_suspend;
    g_tty_client.resume = tty_cgdm_resume;
    g_tty_client.frame_callback = tty_cgdm_frame_callback;
    g_tty_client.input_callback = tty_cgdm_input_callback;
    g_tty_client.context = &g_tty_context;
    
    // Register TTY client with display manager
    graphics_result_t result = display_manager_register_client(&g_tty_client);
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Failed to register TTY client: %d\n", result);
        return result;
    }
    
    debuglog_printf("TTY CGDM integration initialized successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t forest_cgdm_suspend(void* context) {
    (void)context;
    
    debuglog_printf("Canopy CGDM suspend called\n");
    
    // Save Canopy state
    graphics_result_t result = mode_state_save_canopy();
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Failed to save Canopy state: %d\n", result);
        return result;
    }
    
    debuglog_printf("Canopy suspended successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t forest_cgdm_resume(void* context) {
    (void)context;
    
    debuglog_printf("Canopy CGDM resume called\n");
    
    // Restore Canopy state
    graphics_result_t result = mode_state_restore_canopy();
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Failed to restore Canopy state: %d\n", result);
        return result;
    }
    
    // IMPORTANT: Enable graphics app mode so TTY doesn't render over GUI
    // This is necessary when switching from TTY to GUI
    extern void tty_set_graphics_app_active(bool active);
    tty_set_graphics_app_active(true);
    
    debuglog_printf("Canopy resumed successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t forest_cgdm_frame_callback(framebuffer_t* fb, void* context) {
    (void)context;
    
    if (!fb) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Canopy frame rendering - interface with Canopy compositor
    // Only render if Canopy is actually running (not suspended)
    extern bool canopy_is_running(void);
    if (canopy_is_running()) {
        // This callback is invoked by display_manager_process_frame()
        // Let the main Canopy loop handle rendering, just validate framebuffer
        debuglog_printf("Canopy frame callback - compositor running normally\n");
    } else {
        // Canopy is not running, nothing to render
        debuglog_printf("Canopy frame callback - compositor not running, skipping\n");
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t forest_cgdm_input_callback(const input_event_t* event, void* context) {
    (void)context;
    
    if (!event) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Forward input to Canopy system
    debuglog_printf("Canopy input event: type=%d\n", event->type);
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t forest_cgdm_init(void) {
    debuglog_printf("Initializing Canopy CGDM integration...\n");
    
    // Initialize Canopy display client
    display_client_t canopy_client = {0};
    canopy_client.name = "Canopy_Desktop";
    canopy_client.priority = 50;  // Medium priority
    canopy_client.active = false;  // Initially inactive
    canopy_client.mode = DISPLAY_MODE_DESKTOP;
    canopy_client.suspend = forest_cgdm_suspend;
    canopy_client.resume = forest_cgdm_resume;
    canopy_client.frame_callback = forest_cgdm_frame_callback;
    canopy_client.input_callback = forest_cgdm_input_callback;
    canopy_client.context = NULL;
    
    // Register Canopy client with display manager
    graphics_result_t result = display_manager_register_client(&canopy_client);
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Failed to register Canopy client: %d\n", result);
        return result;
    }
    
    debuglog_printf("Canopy CGDM integration initialized successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t cgdm_session_init(void) {
    debuglog_printf("Initializing CGDM session management...\n");
    
    // Initialize TTY CGDM integration
    graphics_result_t result = tty_cgdm_init();
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("TTY CGDM init failed: %d\n", result);
        return result;
    }
    
    // Initialize Canopy CGDM integration
    result = forest_cgdm_init();
    if (result != GRAPHICS_SUCCESS) {
        debuglog_printf("Canopy CGDM init failed: %d\n", result);
        return result;
    }
    
    debuglog_printf("CGDM session management initialized successfully\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t cgdm_auto_start_desktop(void) {
    debuglog_printf("Checking for auto-start desktop...\n");
    
    // Check if we should auto-start Canopy
    // This could be based on configuration, hardware capabilities, etc.
    graphics_device_t* graphics_dev = graphics_get_primary_device();
    if (!graphics_dev) {
        debuglog_printf("No graphics device available, staying in TTY\n");
        return GRAPHICS_ERROR_HARDWARE_FAULT;
    }
    
    // Simple heuristic: start Canopy if we have sufficient graphics capabilities
    if (graphics_dev->caps.max_resolution_x >= 1024 && 
        graphics_dev->caps.max_resolution_y >= 768) {
        
        debuglog_printf("Graphics capabilities sufficient, switching to Canopy desktop\n");
        return display_manager_switch_mode(DISPLAY_MODE_DESKTOP, NULL);
    } else {
        debuglog_printf("Insufficient graphics capabilities, staying in TTY\n");
        return display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE, NULL);
    }
}

graphics_result_t cgdm_switch_to_default_mode(void) {
    debuglog_printf("Switching to default display mode...\n");

    // Default to TTY console
    return display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE, NULL);
}

#else /* !HAS_GRAPHICS */

/* No-framebuffer CGDM integration stubs. TTY console is the only display
 * mode; desktop/compositor integration is unavailable. */

graphics_result_t tty_cgdm_init(void)                              { return GRAPHICS_SUCCESS; }
graphics_result_t tty_cgdm_suspend(void* context)                  { (void)context; return GRAPHICS_SUCCESS; }
graphics_result_t tty_cgdm_resume(void* context)                   { (void)context; return GRAPHICS_SUCCESS; }
graphics_result_t tty_cgdm_frame_callback(framebuffer_t* fb, void* context) { (void)fb; (void)context; return GRAPHICS_SUCCESS; }
graphics_result_t tty_cgdm_input_callback(const input_event_t* e, void* context) { (void)e; (void)context; return GRAPHICS_SUCCESS; }
graphics_result_t forest_cgdm_init(void)                           { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t forest_cgdm_suspend(void* context)               { (void)context; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t forest_cgdm_resume(void* context)                { (void)context; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t forest_cgdm_frame_callback(framebuffer_t* fb, void* context) { (void)fb; (void)context; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t forest_cgdm_input_callback(const input_event_t* e, void* context) { (void)e; (void)context; return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t cgdm_session_init(void)                          { return GRAPHICS_SUCCESS; }
graphics_result_t cgdm_auto_start_desktop(void)                    { return GRAPHICS_SUCCESS; }
graphics_result_t cgdm_switch_to_default_mode(void)               { return GRAPHICS_SUCCESS; }

#endif /* HAS_GRAPHICS */
