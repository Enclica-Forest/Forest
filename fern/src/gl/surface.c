#include <string.h>
#include "surface.h"
#include "os.h"
#include "../include/debuglog.h"

gl_surface_t* gl_create_surface(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;

    gl_surface_t *surface = (gl_surface_t *)gl_malloc(sizeof(gl_surface_t));
    if (!surface) return NULL;

    surface->width = width;
    surface->height = height;
    surface->pixels = (uint32_t *)gl_malloc(width * height * sizeof(uint32_t));
    surface->context = NULL;

    if (!surface->pixels) {
        gl_free(surface);
        return NULL;
    }

    memset(surface->pixels, 0, width * height * sizeof(uint32_t));

    debuglog(DEBUG_INFO, "[GL_SURFACE] created %dx%d\n", width, height);
    return surface;
}

void gl_destroy_surface(gl_surface_t *surface) {
    if (!surface) return;

    if (surface->context) {
        surface->context = NULL;
    }

    if (surface->pixels) {
        gl_free(surface->pixels);
        surface->pixels = NULL;
    }

    debuglog(DEBUG_INFO, "[GL_SURFACE] destroyed\n");
    gl_free(surface);
}

void gl_surface_swap_buffers(gl_surface_t *surface) {
    if (!surface || !surface->pixels) return;
    if (!surface->context) return;

    gl_framebuffer_t *fb = &surface->context->framebuffer;
    if (!fb->color_buffer) return;

    int copy_w = surface->width < fb->width ? surface->width : fb->width;
    int copy_h = surface->height < fb->height ? surface->height : fb->height;

    for (int y = 0; y < copy_h; y++) {
        uint32_t *dst = surface->pixels + y * surface->width;
        unsigned int *src = fb->color_buffer + y * fb->stride;
        for (int x = 0; x < copy_w; x++) {
            dst[x] = src[x];
        }
    }
}

void gl_surface_attach_context(gl_surface_t *surface, gl_context_t *ctx) {
    if (!surface) return;
    surface->context = ctx;
}

void gl_surface_detach_context(gl_surface_t *surface) {
    if (!surface) return;
    surface->context = NULL;
}
