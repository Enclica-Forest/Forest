#ifndef GL_CONTEXT_H
#define GL_CONTEXT_H

#include "state.h"
#include "rasterizer.h"

#define GL_CONTEXT_POOL_SIZE 16

#define GL_CONTEXT_PROFILE_CORE          0
#define GL_CONTEXT_PROFILE_COMPATIBILITY 1

#define GL_CONTEXT_VERSION_1_1 0x0101
#define GL_CONTEXT_VERSION_2_0 0x0200

typedef struct {
    int version;
    int profile;
    int red_bits, green_bits, blue_bits, alpha_bits;
    int depth_bits;
    int stencil_bits;
    int double_buffer;
    int width, height;
    gl_framebuffer_t *framebuffer;
} gl_context_attribs_t;

typedef struct {
    gl_state_t state;
    gl_framebuffer_t framebuffer;
    int width, height;
    gl_context_attribs_t attribs;
    GLboolean current;
    int id;
} gl_context_t;

void gl_context_init(void);

gl_context_t* gl_create_context(const gl_context_attribs_t *attribs);
void gl_destroy_context(gl_context_t *ctx);
void gl_make_current(gl_context_t *ctx);
gl_context_t* gl_get_current_context(void);

void gl_context_set_surface_size(gl_context_t *ctx, int width, int height);
int  gl_context_get_id(gl_context_t *ctx);

#endif
