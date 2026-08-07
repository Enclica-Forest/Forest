#ifndef GL_OS_H
#define GL_OS_H

#include <stdint.h>
#include <stddef.h>

void* gl_malloc(size_t size);
void  gl_free(void* ptr);

void*    gl_get_framebuffer_ptr(void);
uint32_t gl_get_framebuffer_width(void);
uint32_t gl_get_framebuffer_height(void);
uint32_t gl_get_framebuffer_pitch(void);
uint32_t gl_get_framebuffer_bpp(void);

#endif
