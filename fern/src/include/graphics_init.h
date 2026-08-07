#ifndef GRAPHICS_INIT_H
#define GRAPHICS_INIT_H

#include "graphics/graphics_types.h"
#include "graphics/window_manager.h"

graphics_result_t initialize_graphics_subsystem(void);
graphics_result_t shutdown_graphics_subsystem(void);

/* Returns true when the kernel was booted with "nofb" (text-only VGA console).
 * All subsystems that might lazily initialize graphics MUST check this first. */
bool kernel_framebuffer_disabled(void);

void wm_start_render_loop_task(void);

graphics_result_t test_graphics_functionality(void);
graphics_result_t test_window_manager(void);

bool graphics_is_initialized_v2_compat(void);
framebuffer_t* graphics_get_framebuffer_v2_compat(void);
graphics_device_t* graphics_get_primary_device_v2_compat(void);
graphics_result_t graphics_set_mode_v2_compat(uint32_t width, uint32_t height,
                                             uint32_t bpp, uint32_t refresh_rate);
graphics_result_t graphics_clear_screen_v2_compat(graphics_color_t color);

bool graphics_is_display_ready(void);

bool graphics_health_check(void);
graphics_result_t graphics_recover_subsystem(void);

#endif // GRAPHICS_INIT_H
