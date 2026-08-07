#ifndef GL_PIXELFORMAT_H
#define GL_PIXELFORMAT_H

#include "state.h"
#include <stdint.h>

void gl_convert_rgba_to_bgra(uint32_t *dst, const uint32_t *src, int count);
void gl_convert_bgra_to_rgba(uint32_t *dst, const uint32_t *src, int count);
void gl_convert_rgba_to_rgb888(uint8_t *dst, const uint32_t *src, int count);
void gl_convert_rgba_to_rgb565(uint16_t *dst, const uint32_t *src, int count);
void gl_convert_rgb565_to_rgba(uint32_t *dst, const uint16_t *src, int count);
void gl_convert_rgb888_to_rgba(uint32_t *dst, const uint8_t *src, int count);

uint32_t gl_pack_color(float r, float g, float b, float a);
void gl_unpack_color(uint32_t pixel, float *r, float *g, float *b, float *a);

int gl_format_bpp(GLenum format);
int gl_format_components(GLenum format);

#endif
