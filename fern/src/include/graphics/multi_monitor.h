#ifndef MULTI_MONITOR_H
#define MULTI_MONITOR_H

#include "graphics_types.h"
#include "window_manager.h"
#include <stdbool.h>

#define MAX_MONITORS 4

typedef struct monitor_info {
    uint32_t id;
    uint32_t x, y;
    uint32_t width, height;
    uint32_t bpp;
    uint32_t refresh_rate;
    bool active;
    bool primary;
    framebuffer_t* framebuffer;
    char name[64];
} monitor_info_t;

typedef struct {
    uint32_t total_width;
    uint32_t total_height;
    uint32_t num_monitors;
    monitor_info_t monitors[MAX_MONITORS];
    int32_t virtual_x_offset;
    int32_t virtual_y_offset;
} multi_monitor_state_t;

graphics_result_t multi_monitor_init(void);
graphics_result_t multi_monitor_shutdown(void);
bool multi_monitor_is_initialized(void);

graphics_result_t multi_monitor_get_monitor(uint32_t index, monitor_info_t** info);
graphics_result_t multi_monitor_get_primary(monitor_info_t** info);
graphics_result_t multi_monitor_get_total_size(uint32_t* width, uint32_t* height);
uint32_t multi_monitor_point_to_monitor(int32_t x, int32_t y);
graphics_result_t multi_monitor_move_window_to(window_handle_t handle, uint32_t monitor_id);
graphics_result_t multi_monitor_set_primary(uint32_t id);
graphics_result_t multi_monitor_get_count(uint32_t* count);

graphics_result_t multi_monitor_set_mode(uint32_t monitor_id, uint32_t width,
                                          uint32_t height, uint32_t bpp,
                                          uint32_t refresh_rate);
graphics_result_t multi_monitor_get_mode(uint32_t monitor_id, video_mode_t* mode);

graphics_result_t multi_monitor_enable_monitor(uint32_t id, bool enable);
graphics_result_t multi_monitor_get_state(multi_monitor_state_t* state);

#endif // MULTI_MONITOR_H
