#ifndef X11_GC_H
#define X11_GC_H

#include <stdint.h>

#define X11_MAX_GCS 64

typedef struct {
    int    used;
    uint32_t id;
    uint32_t fg;
    uint32_t bg;
    uint8_t  function;
    uint32_t plane_mask;
    uint16_t line_width;
    uint32_t cap_style;
    uint32_t join_style;
    uint32_t fill_style;
} x11_gc_t;

void x11_gc_init(void);
int  x11_gc_create(uint32_t id, const uint8_t* vals, int num_vals);
int  x11_gc_change(uint32_t id, const uint8_t* vals, int num_vals);
void x11_gc_free(uint32_t id);
x11_gc_t* x11_gc_get(uint32_t id);

#endif
