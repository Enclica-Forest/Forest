#ifndef X11_WINDOW_H
#define X11_WINDOW_H

#include <stdint.h>

#define X11_MAX_WINDOWS 256
#define X11_MAX_PROPS   32

typedef struct {
    uint32_t atom;
    uint32_t type;
    uint32_t format;
    uint32_t len;
    uint8_t  data[256];
} x11_property_t;

typedef struct {
    int    used;
    uint32_t id;
    uint32_t parent;
    int16_t  x, y;
    uint16_t width, height;
    uint16_t border_width;
    uint32_t event_mask;
    uint32_t bg_pixel;
    int    mapped;
    int    z_order;
    uint8_t* surface;
    int    dirty;
    char   title[64];
    x11_property_t props[X11_MAX_PROPS];
    int    num_props;
} x11_window_t;

extern x11_window_t g_x11_windows[];
void     x11_windows_init(void);
int      x11_window_create(uint32_t id, uint32_t parent, int16_t x, int16_t y,
                           uint16_t w, uint16_t h, uint32_t event_mask, uint32_t bg);
void     x11_window_destroy(uint32_t id);
int      x11_window_map(uint32_t id);
int      x11_window_unmap(uint32_t id);
int      x11_window_configure(uint32_t id, int16_t x, int16_t y, uint16_t w, uint16_t h);
x11_window_t* x11_window_get(uint32_t id);
x11_window_t* x11_window_at(int px, int py);
void     x11_window_set_focus(uint32_t id);
uint32_t   x11_window_get_focus(void);
void     x11_window_set_title(uint32_t id, const char* title, int len);
void     x11_window_set_zorder(uint32_t id, int z);
int      x11_window_set_property(uint32_t id, uint32_t atom, uint32_t type,
                                 uint32_t format, const uint8_t* data, uint32_t len);
int      x11_window_get_property(uint32_t id, uint32_t atom, uint32_t* type,
                                 uint32_t* format, uint8_t* data, uint32_t* len);

#endif
