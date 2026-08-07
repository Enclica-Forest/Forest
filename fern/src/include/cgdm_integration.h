#ifndef CGDM_INTEGRATION_H
#define CGDM_INTEGRATION_H

// CGDM Integration Interface
// Provides integration points for TTY and Forest desktop systems

#include "display_manager.h"
#include "mode_state.h"
#include "graphics/graphics_driver_v2.h"

// TTY CGDM integration
graphics_result_t tty_cgdm_init(void);
graphics_result_t tty_cgdm_suspend(void* context);
graphics_result_t tty_cgdm_resume(void* context);
graphics_result_t tty_cgdm_frame_callback(framebuffer_t* fb, void* context);
graphics_result_t tty_cgdm_input_callback(const input_event_t* event, void* context);

// Forest desktop CGDM integration
graphics_result_t forest_cgdm_init(void);
graphics_result_t forest_cgdm_suspend(void* context);
graphics_result_t forest_cgdm_resume(void* context);
graphics_result_t forest_cgdm_frame_callback(framebuffer_t* fb, void* context);
graphics_result_t forest_cgdm_input_callback(const input_event_t* event, void* context);

// Session management
graphics_result_t cgdm_session_init(void);
graphics_result_t cgdm_auto_start_desktop(void);
graphics_result_t cgdm_switch_to_default_mode(void);

#endif // CGDM_INTEGRATION_H