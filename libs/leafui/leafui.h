/*
 * LeafUI - Forest OS Userspace UI Library
 * Modern UI framework for Forest OS applications
 */

#ifndef LEAFUI_H
#define LEAFUI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// Core Types
// ============================================================================

typedef struct {
    void* addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} leafui_fb_t;

typedef struct {
    uint8_t r, g, b, a;
} leafui_color_t;

typedef struct {
    int32_t x, y;
    uint32_t width, height;
} leafui_rect_t;

typedef struct {
    int32_t x, y;
} leafui_point_t;

// ============================================================================
// Colors
// ============================================================================

#define LEAFUI_COLOR_BLACK           0xFF000000
#define LEAFUI_COLOR_WHITE           0xFFFFFFFF
#define LEAFUI_COLOR_RED             0xFFFF0000
#define LEAFUI_COLOR_GREEN           0xFF00FF00
#define LEAFUI_COLOR_BLUE            0xFF0000FF
#define LEAFUI_COLOR_GRAY            0xFF808080
#define LEAFUI_COLOR_LIGHT_GRAY      0xFFC0C0C0
#define LEAFUI_COLOR_DARK_GRAY       0xFF404040

// Modern glass theme colors
#define LEAFUI_COLOR_BG             0xFF1A1A2E
#define LEAFUI_COLOR_BG_TOP         0xFF16213E
#define LEAFUI_COLOR_BG_BOTTOM       0xFF0F3460
#define LEAFUI_COLOR_PANEL          0x40000000
#define LEAFUI_COLOR_PANEL_BORDER   0x60FFFFFF
#define LEAFUI_COLOR_TEXT           0xFFFFFFFF
#define LEAFUI_COLOR_TEXT_SECONDARY  0xDDFFFFFF
#define LEAFUI_COLOR_TEXT_HINT      0x99FFFFFF
#define LEAFUI_COLOR_INPUT_BG       0x50FFFFFF
#define LEAFUI_COLOR_INPUT_BORDER   0x30FFFFFF
#define LEAFUI_COLOR_INPUT_FOCUS    0x80FFFFFF
#define LEAFUI_COLOR_BUTTON_PRIMARY  0xFFFFFFFF
#define LEAFUI_COLOR_BUTTON_HOVER   0xFFE0E0E0
#define LEAFUI_COLOR_BUTTON_PRESSED 0xFFC0C0C0
#define LEAFUI_COLOR_SUCCESS        0xFF62D09B
#define LEAFUI_COLOR_ERROR          0xFFFF6B6B

// ============================================================================
// Drawing Functions
// ============================================================================

// Initialize LeafUI with framebuffer
int leafui_init(leafui_fb_t* fb);

// Core drawing primitives
void leafui_clear(leafui_color_t color);
void leafui_pixel(int32_t x, int32_t y, leafui_color_t color);
void leafui_rect(leafui_rect_t rect, leafui_color_t color);
void leafui_rect_filled(leafui_rect_t rect, leafui_color_t color);
void leafui_circle(int32_t cx, int32_t cy, int32_t radius, leafui_color_t color);
void leafui_circle_filled(int32_t cx, int32_t cy, int32_t radius, leafui_color_t color);

// Anti-aliased drawing
void leafui_circle_aa(int32_t cx, int32_t cy, int32_t radius, leafui_color_t color);
void leafui_rect_rounded_aa(leafui_rect_t rect, int32_t radius, leafui_color_t color);
void leafui_rect_rounded_filled_aa(leafui_rect_t rect, int32_t radius, leafui_color_t color);

// Text rendering
void leafui_set_font_size(uint32_t size);
void leafui_text(int32_t x, int32_t y, const char* text, leafui_color_t color);
void leafui_text_centered(leafui_rect_t rect, const char* text, leafui_color_t color);

// ============================================================================
// Utility Functions
// ============================================================================

// Color utilities
static inline leafui_color_t leafui_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (leafui_color_t){r, g, b, a};
}

static inline leafui_color_t leafui_color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return leafui_color(r, g, b, 255);
}

static inline uint32_t leafui_color_to_pixel(leafui_color_t color) {
    return (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
}

// Rectangle utilities
static inline bool leafui_point_in_rect(leafui_point_t point, leafui_rect_t rect) {
    return point.x >= rect.x && point.x < rect.x + (int32_t)rect.width &&
           point.y >= rect.y && point.y < rect.y + (int32_t)rect.height;
}

// ============================================================================
// Widget System
// ============================================================================

typedef enum {
    LEAFUI_WIDGET_BUTTON,
    LEAFUI_WIDGET_LABEL,
    LEAFUI_WIDGET_INPUT,
    LEAFUI_WIDGET_PANEL,
    LEAFUI_WIDGET_PROGRESS
} leafui_widget_type_t;

typedef struct leafui_widget leafui_widget_t;

// Widget callback function
typedef void (*leafui_widget_callback_t)(leafui_widget_t* widget, void* user_data);

struct leafui_widget {
    leafui_widget_type_t type;
    leafui_rect_t rect;
    char* text;
    bool visible;
    bool enabled;
    bool hovered;
    bool pressed;
    leafui_color_t bg_color;
    leafui_color_t text_color;
    leafui_color_t border_color;
    uint32_t border_width;
    int32_t border_radius;
    leafui_widget_callback_t callback;
    void* user_data;
    struct leafui_widget* parent;
    struct leafui_widget* first_child;
    struct leafui_widget* next_sibling;
};

// Widget management
leafui_widget_t* leafui_widget_create(leafui_widget_type_t type, leafui_rect_t rect);
void leafui_widget_destroy(leafui_widget_t* widget);
void leafui_widget_add_child(leafui_widget_t* parent, leafui_widget_t* child);
void leafui_widget_set_text(leafui_widget_t* widget, const char* text);
void leafui_widget_set_callback(leafui_widget_t* widget, leafui_widget_callback_t callback, void* user_data);

// Widget drawing
void leafui_widget_draw(leafui_widget_t* widget);

// High-level UI elements
void leafui_draw_button(leafui_widget_t* button);
void leafui_draw_input(leafui_widget_t* input);
void leafui_draw_panel(leafui_widget_t* panel);
void leafui_draw_label(leafui_widget_t* label);

// ============================================================================
// Layout System
// ============================================================================

typedef enum {
    LEAFUI_LAYOUT_VERTICAL,
    LEAFUI_LAYOUT_HORIZONTAL,
    LEAFUI_LAYOUT_GRID
} leafui_layout_type_t;

typedef struct {
    leafui_layout_type_t type;
    leafui_rect_t rect;
    int32_t spacing;
    int32_t padding;
    leafui_widget_t* first_child;
} leafui_layout_t;

// Layout management
leafui_layout_t* leafui_layout_create(leafui_layout_type_t type, leafui_rect_t rect);
void leafui_layout_destroy(leafui_layout_t* layout);
void leafui_layout_add_widget(leafui_layout_t* layout, leafui_widget_t* widget);
void leafui_layout_update(leafui_layout_t* layout);
void leafui_layout_draw(leafui_layout_t* layout);

// ============================================================================
// Input Handling
// ============================================================================

typedef enum {
    LEAFUI_EVENT_MOUSE_MOVE,
    LEAFUI_EVENT_MOUSE_DOWN,
    LEAFUI_EVENT_MOUSE_UP,
    LEAFUI_EVENT_KEY_DOWN,
    LEAFUI_EVENT_KEY_UP,
    LEAFUI_EVENT_CLICK
} leafui_event_type_t;

typedef struct {
    leafui_event_type_t type;
    union {
        struct {
            int32_t x, y;
            uint32_t buttons;
        } mouse;
        struct {
            uint32_t key_code;
            uint32_t modifiers;
        } key;
    } data;
} leafui_event_t;

// Input management
void leafui_handle_mouse_event(leafui_event_t* event);
void leafui_handle_key_event(leafui_event_t* event);
leafui_widget_t* leafui_get_focused_widget(void);
void leafui_set_focused_widget(leafui_widget_t* widget);

// ============================================================================
// Animation System
// ============================================================================

typedef struct {
    float start_time;
    float duration;
    float start_value;
    float end_value;
    bool (*update_callback)(float value, void* user_data);
    void* user_data;
} leafui_animation_t;

// Animation management
leafui_animation_t* leafui_animation_create(float duration, float start_value, float end_value, 
                                        bool (*callback)(float value, void* user_data), void* user_data);
void leafui_animation_destroy(leafui_animation_t* animation);
bool leafui_animation_update(leafui_animation_t* animation, float current_time);
void leafui_draw_all_animations(void);

// ============================================================================
// Main Loop
// ============================================================================

// Frame control
void leafui_begin_frame(void);
void leafui_end_frame(void);
void leafui_present(void);

// Main event processing
void leafui_process_events(void);
bool leafui_should_quit(void);

#endif // LEAFUI_H