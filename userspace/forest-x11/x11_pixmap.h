#ifndef X11_PIXMAP_H
#define X11_PIXMAP_H

#include <stdint.h>

#define X11_MAX_PIXMAPS 64

typedef struct {
    int    used;
    uint32_t id;
    uint16_t width, height;
    uint8_t  depth;
    uint8_t* surface;
} x11_pixmap_t;

void x11_pixmap_init(void);
int  x11_pixmap_create(uint32_t id, uint16_t w, uint16_t h, uint8_t depth);
void x11_pixmap_free(uint32_t id);
x11_pixmap_t* x11_pixmap_get(uint32_t id);

#endif
