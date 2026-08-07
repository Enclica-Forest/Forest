#ifndef GL_API_IMMEDIATE_H
#define GL_API_IMMEDIATE_H

#include "../include/types.h"
#include "state.h"

typedef struct {
    GLenum mode;
    int vertex_count;
    int capacity;
    gl_immediate_vertex_t *vertices;
    gl_immediate_vertex_t current;
    int in_begin;
} gl_immediate_t;

extern gl_immediate_t g_immediate;

void gl_immediate_init(void);
void gl_immediate_begin(GLenum mode);
void gl_immediate_end(void);
void gl_immediate_vertex(float x, float y, float z);
void gl_immediate_vertex4f(float x, float y, float z, float w);
void gl_immediate_color(float r, float g, float b, float a);
void gl_immediate_color3f(float r, float g, float b);
void gl_immediate_normal(float x, float y, float z);
void gl_immediate_texcoord(float u, float v);

static inline void glBegin(GLenum mode) { gl_immediate_begin(mode); }
static inline void glEnd(void) { gl_immediate_end(); }

#endif
