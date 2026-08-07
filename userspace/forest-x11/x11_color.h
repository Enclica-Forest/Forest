#ifndef X11_COLOR_H
#define X11_COLOR_H

#include <stdint.h>

typedef struct {
    uint16_t r, g, b;
    uint32_t pixel;
} x11_color_t;

void     x11_color_init(void);
uint32_t   x11_alloc_color(uint16_t r, uint16_t g, uint16_t b);
int      x11_alloc_named_color(const char* name, int len, uint16_t* r, uint16_t* g, uint16_t* b);
uint32_t   x11_get_pixel(uint32_t cmap, uint16_t r, uint16_t g, uint16_t b);

#endif
