#include "api_immediate.h"
#include "rasterizer.h"
#include "math.h"
#include <string.h>

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

#define GL_IMMEDIATE_INITIAL_CAPACITY 256

gl_immediate_t g_immediate;

void gl_immediate_init(void)
{
    memset(&g_immediate, 0, sizeof(gl_immediate_t));
    g_immediate.capacity = GL_IMMEDIATE_INITIAL_CAPACITY;
    g_immediate.vertices = (gl_immediate_vertex_t *)kmalloc(
        g_immediate.capacity * sizeof(gl_immediate_vertex_t));
    if (g_immediate.vertices)
        memset(g_immediate.vertices, 0,
               g_immediate.capacity * sizeof(gl_immediate_vertex_t));

    g_immediate.current.r = 1.0f;
    g_immediate.current.g = 1.0f;
    g_immediate.current.b = 1.0f;
    g_immediate.current.a = 1.0f;
    g_immediate.current.w = 1.0f;
}

void gl_immediate_begin(GLenum mode)
{
    if (g_immediate.in_begin) return;
    g_immediate.mode = mode;
    g_immediate.vertex_count = 0;
    g_immediate.in_begin = 1;
}

static void emit_vertex(gl_immediate_vertex_t *v)
{
    if (g_immediate.vertex_count >= g_immediate.capacity) {
        int new_cap = g_immediate.capacity * 2;
        gl_immediate_vertex_t *new_buf =
            (gl_immediate_vertex_t *)kmalloc(new_cap * sizeof(gl_immediate_vertex_t));
        if (!new_buf) return;
        memcpy(new_buf, g_immediate.vertices,
               g_immediate.vertex_count * sizeof(gl_immediate_vertex_t));
        memset(new_buf + g_immediate.capacity, 0,
               (new_cap - g_immediate.capacity) * sizeof(gl_immediate_vertex_t));
        kfree(g_immediate.vertices);
        g_immediate.vertices = new_buf;
        g_immediate.capacity = new_cap;
    }
    g_immediate.vertices[g_immediate.vertex_count++] = *v;
}

static void emit_triangle(gl_immediate_vertex_t *a,
                           gl_immediate_vertex_t *b,
                           gl_immediate_vertex_t *c)
{
    mat4_t mvp = mat4_multiply(g_gl_state.projection_matrix,
                                g_gl_state.modelview_matrix);

    gl_vertex_t verts[3];
    gl_immediate_vertex_t *src[3] = { a, b, c };

    for (int i = 0; i < 3; i++) {
        vec4_t pos = { src[i]->x, src[i]->y, src[i]->z, src[i]->w };
        vec4_t clip = mat4_multiply_vec4(mvp, pos);

        if (clip.w != 0.0f) {
            clip.x /= clip.w;
            clip.y /= clip.w;
            clip.z /= clip.w;
        }

        verts[i].clip_pos = clip;
        verts[i].world_pos = (vec3_t){ src[i]->x, src[i]->y, src[i]->z };
        verts[i].color = (vec4_t){ src[i]->r, src[i]->g, src[i]->b, src[i]->a };
        verts[i].texcoord = (vec2_t){ src[i]->s, src[i]->t };
        verts[i].normal = (vec3_t){ src[i]->nx, src[i]->ny, src[i]->nz };
        verts[i].eye_z = clip.z;
    }

    gl_triangle_t tri;
    tri.v[0] = verts[0];
    tri.v[1] = verts[1];
    tri.v[2] = verts[2];
    gl_rasterize_triangle(&tri);
}

static void emit_line(gl_immediate_vertex_t *a, gl_immediate_vertex_t *b)
{
    mat4_t mvp = mat4_multiply(g_gl_state.projection_matrix,
                                g_gl_state.modelview_matrix);

    gl_vertex_t verts[2];
    gl_immediate_vertex_t *src[2] = { a, b };

    for (int i = 0; i < 2; i++) {
        vec4_t pos = { src[i]->x, src[i]->y, src[i]->z, src[i]->w };
        vec4_t clip = mat4_multiply_vec4(mvp, pos);

        if (clip.w != 0.0f) {
            clip.x /= clip.w;
            clip.y /= clip.w;
            clip.z /= clip.w;
        }

        verts[i].clip_pos = clip;
        verts[i].world_pos = (vec3_t){ src[i]->x, src[i]->y, src[i]->z };
        verts[i].color = (vec4_t){ src[i]->r, src[i]->g, src[i]->b, src[i]->a };
        verts[i].texcoord = (vec2_t){ src[i]->s, src[i]->t };
        verts[i].normal = (vec3_t){ src[i]->nx, src[i]->ny, src[i]->nz };
        verts[i].eye_z = clip.z;
    }

    gl_rasterize_line(&verts[0], &verts[1]);
}

static void emit_point(gl_immediate_vertex_t *a)
{
    mat4_t mvp = mat4_multiply(g_gl_state.projection_matrix,
                                g_gl_state.modelview_matrix);

    vec4_t pos = { a->x, a->y, a->z, a->w };
    vec4_t clip = mat4_multiply_vec4(mvp, pos);

    if (clip.w != 0.0f) {
        clip.x /= clip.w;
        clip.y /= clip.w;
        clip.z /= clip.w;
    }

    gl_vertex_t v;
    v.clip_pos = clip;
    v.world_pos = (vec3_t){ a->x, a->y, a->z };
    v.color = (vec4_t){ a->r, a->g, a->b, a->a };
    v.texcoord = (vec2_t){ a->s, a->t };
    v.normal = (vec3_t){ a->nx, a->ny, a->nz };
    v.eye_z = clip.z;

    gl_rasterize_point(&v);
}

void gl_immediate_end(void)
{
    if (!g_immediate.in_begin) return;
    g_immediate.in_begin = 0;

    int n = g_immediate.vertex_count;
    gl_immediate_vertex_t *v = g_immediate.vertices;

    switch (g_immediate.mode) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3)
            emit_triangle(&v[i], &v[i+1], &v[i+2]);
        break;

    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1)
                emit_triangle(&v[i+1], &v[i], &v[i+2]);
            else
                emit_triangle(&v[i], &v[i+1], &v[i+2]);
        }
        break;

    case GL_TRIANGLE_FAN:
        for (int i = 1; i + 1 < n; i++)
            emit_triangle(&v[0], &v[i], &v[i+1]);
        break;

    case GL_LINES:
        for (int i = 0; i + 1 < n; i += 2)
            emit_line(&v[i], &v[i+1]);
        break;

    case GL_LINE_STRIP:
        for (int i = 0; i + 1 < n; i++)
            emit_line(&v[i], &v[i+1]);
        break;

    case GL_LINE_LOOP:
        for (int i = 0; i + 1 < n; i++)
            emit_line(&v[i], &v[i+1]);
        if (n > 2)
            emit_line(&v[n-1], &v[0]);
        break;

    case GL_POINTS:
        for (int i = 0; i < n; i++)
            emit_point(&v[i]);
        break;

    default:
        break;
    }

    g_immediate.vertex_count = 0;
}

void gl_immediate_vertex(float x, float y, float z)
{
    if (!g_immediate.in_begin) return;
    gl_immediate_vertex_t vert = g_immediate.current;
    vert.x = x;
    vert.y = y;
    vert.z = z;
    vert.w = 1.0f;
    emit_vertex(&vert);
}

void gl_immediate_vertex4f(float x, float y, float z, float w)
{
    if (!g_immediate.in_begin) return;
    gl_immediate_vertex_t vert = g_immediate.current;
    vert.x = x;
    vert.y = y;
    vert.z = z;
    vert.w = w;
    emit_vertex(&vert);
}

void gl_immediate_color(float r, float g, float b, float a)
{
    g_immediate.current.r = r;
    g_immediate.current.g = g;
    g_immediate.current.b = b;
    g_immediate.current.a = a;
    g_gl_state.current_color[0] = r;
    g_gl_state.current_color[1] = g;
    g_gl_state.current_color[2] = b;
    g_gl_state.current_color[3] = a;
}

void gl_immediate_color3f(float r, float g, float b)
{
    gl_immediate_color(r, g, b, 1.0f);
}

void gl_immediate_normal(float x, float y, float z)
{
    g_immediate.current.nx = x;
    g_immediate.current.ny = y;
    g_immediate.current.nz = z;
    g_gl_state.current_normal[0] = x;
    g_gl_state.current_normal[1] = y;
    g_gl_state.current_normal[2] = z;
}

void gl_immediate_texcoord(float u, float v)
{
    g_immediate.current.s = u;
    g_immediate.current.t = v;
    g_gl_state.current_texcoord[0] = u;
    g_gl_state.current_texcoord[1] = v;
}
