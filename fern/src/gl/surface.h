#ifndef GL_SURFACE_H
#define GL_SURFACE_H

#include <stdint.h>
#include "context.h"

typedef struct {
    int width, height;
    uint32_t *pixels;
    gl_context_t *context;
} gl_surface_t;

gl_surface_t* gl_create_surface(int width, int height);
void gl_destroy_surface(gl_surface_t *surface);
void gl_surface_swap_buffers(gl_surface_t *surface);
void gl_surface_attach_context(gl_surface_t *surface, gl_context_t *ctx);
void gl_surface_detach_context(gl_surface_t *surface);

#endif
