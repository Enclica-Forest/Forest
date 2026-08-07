#include "api_draw.h"
#include "buffer.h"
#include "rasterizer.h"
#include "math.h"
#include "../include/debuglog.h"
#include "../include/string.h"

extern gl_state_t g_gl_state;
extern gl_framebuffer_t *g_gl_framebuffer;

static int type_size(GLenum type)
{
    switch (type) {
    case GL_BYTE:           return 1;
    case GL_UNSIGNED_BYTE:  return 1;
    case GL_SHORT:          return 2;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_INT:            return 4;
    case GL_UNSIGNED_INT:   return 4;
    case GL_FLOAT:          return 4;
    default:                return 4;
    }
}

static float read_component(const GLubyte *ptr, GLenum type)
{
    switch (type) {
    case GL_BYTE:           return (float)(*(const GLbyte *)ptr);
    case GL_UNSIGNED_BYTE:  return (float)(*(const GLubyte *)ptr);
    case GL_SHORT:          return (float)(*(const GLshort *)ptr);
    case GL_UNSIGNED_SHORT: return (float)(*(const GLushort *)ptr);
    case GL_INT:            return (float)(*(const GLint *)ptr);
    case GL_UNSIGNED_INT:   return (float)(*(const GLuint *)ptr);
    case GL_FLOAT:          return *(const float *)ptr;
    default:                return 0.0f;
    }
}

static const GLubyte *attrib_data(gl_attrib_pointer_t *a, GLsizei index)
{
    if (!a || !a->enabled) return NULL;
    int elem_size = a->stride ? a->stride : a->size * type_size(a->type);
    const GLubyte *base = NULL;
    if (a->buffer != 0) {
        gl_buffer_t *buf = gl_buffer_get(a->buffer);
        if (buf) base = buf->data;
    } else {
        base = (const GLubyte *)a->pointer;
    }
    if (!base) return NULL;
    return base + index * elem_size;
}

static void read_vertex(GLsizei index, vec4_t *out_pos, vec4_t *out_color,
                         vec3_t *out_normal, vec2_t *out_texcoord)
{
    out_pos->x = 0.0f; out_pos->y = 0.0f;
    out_pos->z = 0.0f; out_pos->w = 1.0f;
    out_color->x = g_gl_state.current_color[0];
    out_color->y = g_gl_state.current_color[1];
    out_color->z = g_gl_state.current_color[2];
    out_color->w = g_gl_state.current_color[3];
    out_normal->x = g_gl_state.current_normal[0];
    out_normal->y = g_gl_state.current_normal[1];
    out_normal->z = g_gl_state.current_normal[2];
    out_texcoord->x = g_gl_state.current_texcoord[0];
    out_texcoord->y = g_gl_state.current_texcoord[1];

    gl_attrib_pointer_t *va = gl_attrib_get(0);
    if (va && va->enabled) {
        const GLubyte *p = attrib_data(va, index);
        if (p) {
            int n = va->size;
            if (n >= 1) out_pos->x = read_component(p, va->type);
            if (n >= 2) out_pos->y = read_component(p + type_size(va->type), va->type);
            if (n >= 3) out_pos->z = read_component(p + 2 * type_size(va->type), va->type);
            if (n >= 4) out_pos->w = read_component(p + 3 * type_size(va->type), va->type);
        }
    }

    gl_attrib_pointer_t *ca = gl_attrib_get(3);
    if (ca && ca->enabled) {
        const GLubyte *p = attrib_data(ca, index);
        if (p) {
            int n = ca->size;
            if (n >= 1) out_color->x = read_component(p, ca->type);
            if (n >= 2) out_color->y = read_component(p + type_size(ca->type), ca->type);
            if (n >= 3) out_color->z = read_component(p + 2 * type_size(ca->type), ca->type);
            if (n >= 4) out_color->w = read_component(p + 3 * type_size(ca->type), ca->type);
        }
    }

    gl_attrib_pointer_t *na = gl_attrib_get(2);
    if (na && na->enabled) {
        const GLubyte *p = attrib_data(na, index);
        if (p) {
            out_normal->x = read_component(p, na->type);
            out_normal->y = read_component(p + type_size(na->type), na->type);
            out_normal->z = read_component(p + 2 * type_size(na->type), na->type);
        }
    }

    gl_attrib_pointer_t *ta = gl_attrib_get(1);
    if (ta && ta->enabled) {
        const GLubyte *p = attrib_data(ta, index);
        if (p) {
            if (ta->size >= 1) out_texcoord->x = read_component(p, ta->type);
            if (ta->size >= 2) out_texcoord->y = read_component(p + type_size(ta->type), ta->type);
        }
    }
}

static gl_vertex_t transform_vertex(vec4_t pos, vec4_t color, vec3_t normal, vec2_t texcoord)
{
    gl_vertex_t v;
    mat4_t mvp = mat4_multiply(g_gl_state.projection_matrix, g_gl_state.modelview_matrix);

    vec4_t clip = mat4_multiply_vec4(mvp, pos);

    v.color.x = color.x;
    v.color.y = color.y;
    v.color.z = color.z;
    v.color.w = color.w;
    v.texcoord = texcoord;
    v.normal = normal;
    v.clip_pos = clip;

    float inv_w = (clip.w != 0.0f) ? 1.0f / clip.w : 1.0f;
    v.world_pos.x = clip.x * inv_w;
    v.world_pos.y = clip.y * inv_w;
    v.world_pos.z = clip.z * inv_w;
    v.eye_z = -clip.z;

    return v;
}

static void emit_point(GLint index)
{
    vec4_t pos, color;
    vec3_t normal;
    vec2_t texcoord;
    read_vertex(index, &pos, &color, &normal, &texcoord);
    gl_vertex_t v = transform_vertex(pos, color, normal, texcoord);
    gl_rasterize_point(&v);
}

static void emit_line(GLint i0, GLint i1)
{
    vec4_t pos0, color0, pos1, color1;
    vec3_t norm0, norm1;
    vec2_t tc0, tc1;
    read_vertex(i0, &pos0, &color0, &norm0, &tc0);
    read_vertex(i1, &pos1, &color1, &norm1, &tc1);
    gl_vertex_t v0 = transform_vertex(pos0, color0, norm0, tc0);
    gl_vertex_t v1 = transform_vertex(pos1, color1, norm1, tc1);
    gl_rasterize_line(&v0, &v1);
}

static void emit_triangle(GLint i0, GLint i1, GLint i2)
{
    vec4_t pos0, color0, pos1, color1, pos2, color2;
    vec3_t norm0, norm1, norm2;
    vec2_t tc0, tc1, tc2;
    read_vertex(i0, &pos0, &color0, &norm0, &tc0);
    read_vertex(i1, &pos1, &color1, &norm1, &tc1);
    read_vertex(i2, &pos2, &color2, &norm2, &tc2);
    gl_vertex_t v0 = transform_vertex(pos0, color0, norm0, tc0);
    gl_vertex_t v1 = transform_vertex(pos1, color1, norm1, tc1);
    gl_vertex_t v2 = transform_vertex(pos2, color2, norm2, tc2);
    gl_triangle_t tri;
    tri.v[0] = v0;
    tri.v[1] = v1;
    tri.v[2] = v2;
    gl_rasterize_triangle(&tri);
}

static uint32_t read_index(const GLvoid *indices, GLsizei i, GLenum type)
{
    const GLubyte *p = (const GLubyte *)indices;
    switch (type) {
    case GL_UNSIGNED_BYTE:  return p[i];
    case GL_UNSIGNED_SHORT: return ((const GLushort *)p)[i];
    case GL_UNSIGNED_INT:   return ((const GLuint *)p)[i];
    default:                return 0;
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0) return;

    switch (mode) {
    case GL_POINTS:
        for (GLint i = first; i < first + count; i++)
            emit_point(i);
        break;

    case GL_LINES:
        for (GLint i = first; i < first + count - 1; i += 2)
            emit_line(i, i + 1);
        break;

    case GL_LINE_STRIP:
        for (GLint i = first; i < first + count - 1; i++)
            emit_line(i, i + 1);
        break;

    case GL_LINE_LOOP:
        for (GLint i = first; i < first + count - 1; i++)
            emit_line(i, i + 1);
        if (count > 1)
            emit_line(first + count - 1, first);
        break;

    case GL_TRIANGLES:
        for (GLint i = first; i < first + count - 2; i += 3)
            emit_triangle(i, i + 1, i + 2);
        break;

    case GL_TRIANGLE_STRIP:
        for (GLint i = first; i < first + count - 2; i++) {
            if ((i - first) % 2 == 0)
                emit_triangle(i, i + 1, i + 2);
            else
                emit_triangle(i + 1, i, i + 2);
        }
        break;

    case GL_TRIANGLE_FAN:
        for (GLint i = first + 1; i < first + count - 1; i++)
            emit_triangle(first, i, i + 1);
        break;
    }
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    if (count <= 0 || !indices) return;

    const GLubyte *ibo_data = NULL;
    if (g_gl_state.bound_element_buffer != 0) {
        gl_buffer_t *buf = gl_buffer_get(g_gl_state.bound_element_buffer);
        if (buf) ibo_data = buf->data;
    }

    const GLvoid *idx = indices;
    if (ibo_data)
        idx = ibo_data + (uintptr_t)indices;

    switch (mode) {
    case GL_POINTS:
        for (GLsizei i = 0; i < count; i++)
            emit_point((GLint)read_index(idx, i, type));
        break;

    case GL_LINES:
        for (GLsizei i = 0; i < count - 1; i += 2)
            emit_line((GLint)read_index(idx, i, type),
                      (GLint)read_index(idx, i + 1, type));
        break;

    case GL_LINE_STRIP:
        for (GLsizei i = 0; i < count - 1; i++)
            emit_line((GLint)read_index(idx, i, type),
                      (GLint)read_index(idx, i + 1, type));
        break;

    case GL_LINE_LOOP:
        for (GLsizei i = 0; i < count - 1; i++)
            emit_line((GLint)read_index(idx, i, type),
                      (GLint)read_index(idx, i + 1, type));
        if (count > 1)
            emit_line((GLint)read_index(idx, count - 1, type),
                      (GLint)read_index(idx, 0, type));
        break;

    case GL_TRIANGLES:
        for (GLsizei i = 0; i < count - 2; i += 3)
            emit_triangle((GLint)read_index(idx, i, type),
                          (GLint)read_index(idx, i + 1, type),
                          (GLint)read_index(idx, i + 2, type));
        break;

    case GL_TRIANGLE_STRIP:
        for (GLsizei i = 0; i < count - 2; i++) {
            if (i % 2 == 0)
                emit_triangle((GLint)read_index(idx, i, type),
                              (GLint)read_index(idx, i + 1, type),
                              (GLint)read_index(idx, i + 2, type));
            else
                emit_triangle((GLint)read_index(idx, i + 1, type),
                              (GLint)read_index(idx, i, type),
                              (GLint)read_index(idx, i + 2, type));
        }
        break;

    case GL_TRIANGLE_FAN:
        for (GLsizei i = 1; i < count - 1; i++)
            emit_triangle((GLint)read_index(idx, 0, type),
                          (GLint)read_index(idx, i, type),
                          (GLint)read_index(idx, i + 1, type));
        break;
    }
}

void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount)
{
    for (GLsizei inst = 0; inst < instancecount; inst++)
        glDrawArrays(mode, first, count);
}
