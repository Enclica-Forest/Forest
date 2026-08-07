#ifndef ENHANCED_CURSOR_H
#define ENHANCED_CURSOR_H

#include "graphics_types.h"

#define CURSOR_WIDTH 24
#define CURSOR_HEIGHT 24
#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0
#define CURSOR_ANIMATION_FRAMES 4
#define CURSOR_ANIMATION_SPEED_MS 100

typedef enum {
    CURSOR_TYPE_ARROW = 0,
    CURSOR_TYPE_MOVE,
    CURSOR_TYPE_RESIZE_NWSE,
    CURSOR_TYPE_RESIZE_NESW,
    CURSOR_TYPE_RESIZE_NS,
    CURSOR_TYPE_RESIZE_EW,
    CURSOR_TYPE_TEXT,
    CURSOR_TYPE_HAND,
    CURSOR_TYPE_BUSY,
    CURSOR_TYPE_CROSSHAIR,
    CURSOR_TYPE_HELP,
    CURSOR_TYPE_POINTER,
    CURSOR_TYPE_CUSTOM
} cursor_type_t;

typedef enum {
    CURSOR_STATE_IDLE,
    CURSOR_STATE_HOVER,
    CURSOR_STATE_ACTIVE,
    CURSOR_STATE_BUSY,
    CURSOR_STATE_DRAGGING
} cursor_state_t;

typedef struct {
    cursor_type_t type;
    cursor_state_t state;
    int32_t x;
    int32_t y;
    int32_t prev_x;
    int32_t prev_y;
    bool visible;
    bool animated;
    uint32_t animation_frame;
    uint64_t last_animation_time;
    
    graphics_surface_t* surface;
    graphics_surface_t* shadow_surface;
    
    graphics_color_t primary_color;
    graphics_color_t secondary_color;
    
    int32_t hotspot_x;
    int32_t hotspot_y;
    
    struct {
        bool enabled;
        int32_t blur_radius;
        int32_t offset_x;
        int32_t offset_y;
        float alpha;
    } shadow;
    
    struct {
        bool enabled;
        float scale;
        uint32_t rotation;
    } smooth;
} enhanced_cursor_t;

typedef struct {
    graphics_color_t primary;
    graphics_color_t secondary;
    bool has_outline;
    graphics_color_t outline;
} cursor_theme_t;

graphics_result_t enhanced_cursor_init(void);
graphics_result_t enhanced_cursor_shutdown(void);

graphics_result_t enhanced_cursor_create(cursor_type_t type,
                                      const cursor_theme_t* theme,
                                      enhanced_cursor_t** cursor);
graphics_result_t enhanced_cursor_destroy(enhanced_cursor_t* cursor);

graphics_result_t enhanced_cursor_set_type(enhanced_cursor_t* cursor, cursor_type_t type);
graphics_result_t enhanced_cursor_set_state(enhanced_cursor_t* cursor, cursor_state_t state);
graphics_result_t enhanced_cursor_set_position(enhanced_cursor_t* cursor, int32_t x, int32_t y);
graphics_result_t enhanced_cursor_set_visible(enhanced_cursor_t* cursor, bool visible);
graphics_result_t enhanced_cursor_set_theme(enhanced_cursor_t* cursor, const cursor_theme_t* theme);

graphics_result_t enhanced_cursor_draw(enhanced_cursor_t* cursor, graphics_surface_t* surface);
graphics_result_t enhanced_cursor_update(enhanced_cursor_t* cursor, uint64_t current_time_ms);

graphics_result_t enhanced_cursor_animate(enhanced_cursor_t* cursor, bool enable);

graphics_result_t enhanced_cursor_set_custom_bitmap(enhanced_cursor_t* cursor,
                                              const uint8_t* bitmap,
                                              uint32_t width,
                                              uint32_t height,
                                              int32_t hotspot_x,
                                              int32_t hotspot_y);

graphics_result_t enhanced_cursor_get_type(cursor_type_t type,
                                          const uint8_t** bitmap,
                                          uint32_t* width,
                                          uint32_t* height);

graphics_result_t enhanced_cursor_rotate(enhanced_cursor_t* cursor, uint32_t degrees);
graphics_result_t enhanced_cursor_scale(enhanced_cursor_t* cursor, float scale);

static inline bool enhanced_cursor_is_animated(const enhanced_cursor_t* cursor) {
    return cursor && cursor->animated;
}

static inline void enhanced_cursor_get_position(const enhanced_cursor_t* cursor,
                                           int32_t* x, int32_t* y) {
    if (x) *x = cursor->x;
    if (y) *y = cursor->y;
}

static inline bool enhanced_cursor_is_visible(const enhanced_cursor_t* cursor) {
    return cursor && cursor->visible;
}

#endif // ENHANCED_CURSOR_H
