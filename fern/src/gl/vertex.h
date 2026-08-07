#ifndef GL_VERTEX_H
#define GL_VERTEX_H

#include "../include/types.h"
#include "state.h"

// Interpolated vertex for rasterization
typedef struct {
    float x, y, z, w;        // Clip space position
    float screen_x, screen_y; // Screen space position
    float r, g, b, a;         // Color
    float u, v;               // Texture coordinates
    float nx, ny, nz;         // Normal
    float depth;              // NDC depth [0,1]
    float clip_w;             // W from clip space (for perspective correction)
} gl_vertex_attrib_t;

// Processed triangle for rasterization
typedef struct {
    gl_vertex_attrib_t v[3];
    int culled;
} gl_triangle_attrib_t;

void gl_vertex_init(void);

// Transform a single vertex: MVP * position, perspective divide, viewport transform
void gl_vertex_transform(const float *in_pos4, float *out_clip, float *out_screen);

// Set up interpolation for a triangle (perspective-correct attributes)
void gl_vertex_setup_interp(gl_triangle_attrib_t *tri);

// Fetch a single vertex from VBO or client array
void gl_vertex_fetch(GLuint index, float *pos_out, float *color_out,
                     float *texcoord_out, float *normal_out);

// Process an entire triangle (fetch, transform, cull)
void gl_vertex_process_triangle(GLuint idx0, GLuint idx1, GLuint idx2,
                                gl_triangle_attrib_t *out);

#endif /* GL_VERTEX_H */
