#include "pixelformat.h"

static inline float clamp01f(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline uint8_t float_to_u8(float f)
{
    int v = (int)(clamp01f(f) * 255.0f + 0.5f);
    return (uint8_t)v;
}

static inline float u8_to_float(uint8_t v)
{
    return v / 255.0f;
}

void gl_convert_rgba_to_bgra(uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t p = src[i];
        uint8_t r = (p >> 0)  & 0xFF;
        uint8_t g = (p >> 8)  & 0xFF;
        uint8_t b = (p >> 16) & 0xFF;
        uint8_t a = (p >> 24) & 0xFF;
        dst[i] = ((uint32_t)b) | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
    }
}

void gl_convert_bgra_to_rgba(uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t p = src[i];
        uint8_t b = (p >> 0)  & 0xFF;
        uint8_t g = (p >> 8)  & 0xFF;
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t a = (p >> 24) & 0xFF;
        dst[i] = ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
    }
}

void gl_convert_rgba_to_rgb888(uint8_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t p = src[i];
        dst[i * 3 + 0] = (p >> 0)  & 0xFF;
        dst[i * 3 + 1] = (p >> 8)  & 0xFF;
        dst[i * 3 + 2] = (p >> 16) & 0xFF;
    }
}

void gl_convert_rgba_to_rgb565(uint16_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t p = src[i];
        uint8_t r = (p >> 0)  & 0xFF;
        uint8_t g = (p >> 8)  & 0xFF;
        uint8_t b = (p >> 16) & 0xFF;
        dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

void gl_convert_rgb565_to_rgba(uint32_t *dst, const uint16_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t p = src[i];
        uint8_t r = (uint8_t)(((p >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) << 2);
        uint8_t b = (uint8_t)((p & 0x1F) << 3);
        dst[i] = ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
    }
}

void gl_convert_rgb888_to_rgba(uint32_t *dst, const uint8_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint8_t r = src[i * 3 + 0];
        uint8_t g = src[i * 3 + 1];
        uint8_t b = src[i * 3 + 2];
        dst[i] = ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
    }
}

uint32_t gl_pack_color(float r, float g, float b, float a)
{
    int ri = (int)(clamp01f(r) * 255.0f + 0.5f);
    int gi = (int)(clamp01f(g) * 255.0f + 0.5f);
    int bi = (int)(clamp01f(b) * 255.0f + 0.5f);
    int ai = (int)(clamp01f(a) * 255.0f + 0.5f);
    return (uint32_t)((ai << 24) | (bi << 16) | (gi << 8) | ri);
}

void gl_unpack_color(uint32_t pixel, float *r, float *g, float *b, float *a)
{
    *r = u8_to_float((pixel >> 0)  & 0xFF);
    *g = u8_to_float((pixel >> 8)  & 0xFF);
    *b = u8_to_float((pixel >> 16) & 0xFF);
    *a = u8_to_float((pixel >> 24) & 0xFF);
}

int gl_format_bpp(GLenum format)
{
    switch (format) {
    case GL_RED:          return 8;
    case GL_RGB:          return 24;
    case GL_RGBA:         return 32;
    default:              return 0;
    }
}

int gl_format_components(GLenum format)
{
    switch (format) {
    case GL_RED:          return 1;
    case GL_RGB:          return 3;
    case GL_RGBA:         return 4;
    default:              return 0;
    }
}
