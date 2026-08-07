#include "gl.h"
#include "../arch/framebuffer.h"
#include "rasterizer.h"
#include "state.h"
#include "framebuffer.h"
#include "texture.h"
#include "api_shader.h"
#include "buffer.h"
#include "vertex.h"
#include "fragment.h"
#include "api_matrix.h"
#include "api_state.h"
#include "../include/debuglog.h"
#include <stdint.h>
#include <stddef.h>
#include "os.h"

static gl_framebuffer_t g_gl_fb;
static int g_gl_initialized = 0;

void gl_init(void) {
    if (g_gl_initialized) return;

    gl_rasterizer_init();
    gl_state_init();
    gl_immediate_init();
    gl_framebuffer_init();
    gl_texture_init();
    gl_shader_init();
    gl_buffer_init();
    gl_vertex_init();
    gl_context_init();

    g_gl_fragment_shader = gl_default_fragment_shader;

    g_gl_initialized = 1;
    debuglog(DEBUG_INFO, "[GL] Software renderer initialized\n");
}

void gl_init_with_framebuffer(void) {
    if (!framebuffer_is_available()) {
        debuglog(DEBUG_WARN, "[GL] No framebuffer available, skipping init\n");
        return;
    }

    gl_init();

    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    framebuffer_get_info(&fb_width, &fb_height, &fb_pitch, &fb_bpp);

    if (fb_width == 0 || fb_height == 0) {
        debuglog(DEBUG_WARN, "[GL] Invalid framebuffer: %ux%u\n", fb_width, fb_height);
        return;
    }

    uint32_t stride = (fb_pitch + 3) / 4;

    uint32_t color_size = stride * fb_height * sizeof(unsigned int);
    uint32_t depth_size = stride * fb_height * sizeof(float);
    uint32_t stencil_size = stride * fb_height * sizeof(unsigned char);

    g_gl_fb.color_buffer = (unsigned int*)gl_malloc(color_size);
    g_gl_fb.depth_buffer = (float*)gl_malloc(depth_size);
    g_gl_fb.stencil_buffer = (unsigned char*)gl_malloc(stencil_size);

    if (!g_gl_fb.color_buffer || !g_gl_fb.depth_buffer || !g_gl_fb.stencil_buffer) {
        debuglog(DEBUG_ERROR, "[GL] Failed to allocate framebuffer buffers (%ux%u)\n",
                 fb_width, fb_height);
        gl_free(g_gl_fb.color_buffer);
        gl_free(g_gl_fb.depth_buffer);
        gl_free(g_gl_fb.stencil_buffer);
        g_gl_fb.color_buffer = 0;
        g_gl_fb.depth_buffer = 0;
        g_gl_fb.stencil_buffer = 0;
        return;
    }

    g_gl_fb.width = (int)fb_width;
    g_gl_fb.height = (int)fb_height;
    g_gl_fb.stride = (int)stride;

    gl_rasterizer_set_framebuffer(&g_gl_fb);

    gl_viewport(0, 0, (GLsizei)fb_width, (GLsizei)fb_height);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, fb_width, fb_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    debuglog(DEBUG_INFO, "[GL] Framebuffer ready: %ux%u pitch=%u bpp=%u\n",
             fb_width, fb_height, fb_pitch, fb_bpp);
}

void gl_shutdown(void) {
    if (!g_gl_initialized) return;

    if (g_gl_fb.color_buffer) {
        gl_free(g_gl_fb.color_buffer);
        g_gl_fb.color_buffer = 0;
    }
    if (g_gl_fb.depth_buffer) {
        gl_free(g_gl_fb.depth_buffer);
        g_gl_fb.depth_buffer = 0;
    }
    if (g_gl_fb.stencil_buffer) {
        gl_free(g_gl_fb.stencil_buffer);
        g_gl_fb.stencil_buffer = 0;
    }

    gl_rasterizer_set_framebuffer(0);

    g_gl_initialized = 0;
    debuglog(DEBUG_INFO, "[GL] Software renderer shut down\n");
}

gl_framebuffer_t* gl_get_framebuffer(void) {
    if (!g_gl_initialized) return 0;
    return &g_gl_fb;
}

int gl_is_initialized(void) {
    return g_gl_initialized;
}
