#ifndef GL_RASTERIZER_H
#define GL_RASTERIZER_H

#include "math.h"

typedef struct {
    vec4_t clip_pos;
    vec3_t world_pos;
    vec4_t color;
    vec2_t texcoord;
    vec3_t normal;
    float  eye_z;
} gl_vertex_t;

typedef struct {
    gl_vertex_t v[3];
} gl_triangle_t;

typedef struct {
    unsigned int *color_buffer;
    float        *depth_buffer;
    unsigned char *stencil_buffer;
    int width, height;
    int stride;
} gl_framebuffer_t;

typedef struct {
    float x, y;
    float r, g, b, a;
    float u, v;
    float nx, ny, nz;
    float depth;
    int   discard;
} gl_fragment_t;

typedef void (*fragment_shader_fn)(gl_fragment_t *frag);

extern fragment_shader_fn g_gl_fragment_shader;
extern gl_framebuffer_t  *g_gl_framebuffer;

void gl_rasterizer_init(void);
void gl_rasterizer_set_framebuffer(gl_framebuffer_t *fb);
void gl_rasterize_triangle(gl_triangle_t *tri);
void gl_rasterize_line(gl_vertex_t *v0, gl_vertex_t *v1);
void gl_rasterize_point(gl_vertex_t *v);
void gl_clear_buffers(unsigned int mask);

#endif
