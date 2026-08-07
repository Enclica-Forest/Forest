#ifndef X11_DRAW_H
#define X11_DRAW_H

#include <stdint.h>

void x11_draw_rect(uint8_t* surf, int sw, int sh, int x, int y, int w, int h,
                   uint32_t color);
void x11_draw_hline(uint8_t* surf, int sw, int sh, int x, int y, int w,
                    uint32_t color);
void x11_draw_vline(uint8_t* surf, int sw, int sh, int x, int y, int h,
                    uint32_t color);
void x11_draw_line(uint8_t* surf, int sw, int sh, int x0, int y0, int x1, int y1,
                   uint32_t color);
void x11_draw_circle(uint8_t* surf, int sw, int sh, int cx, int cy, int r,
                     uint32_t color, int filled);
void x11_draw_arc(uint8_t* surf, int sw, int sh, int cx, int cy, int rx, int ry,
                  int angle1, int angle2, uint32_t color);
void x11_draw_text(uint8_t* surf, int sw, int sh, int x, int y,
                   const char* text, int len, uint32_t color, int font_h);
void x11_put_image(uint8_t* surf, int sw, int sh, int x, int y, int w, int h,
                   const uint8_t* data, int depth);
void x11_copy_area(uint8_t* dst, int dw, int dh, int dx, int dy,
                   const uint8_t* src, int sw, int sh, int sx, int sy, int w, int h);
void x11_fill_polygon(uint8_t* surf, int sw, int sh, const int* pts, int npts,
                      uint32_t color);

#endif
