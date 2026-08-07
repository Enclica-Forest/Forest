#include "api_vertex_arrays.h"
#include "api_immediate.h"
#include "rasterizer.h"
#include "math.h"
#include <string.h>

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const GLvoid *pointer;
} gl_array_pointer_t;

static gl_array_pointer_t g_vertex_pointer;
static gl_array_pointer_t g_color_pointer;
static gl_array_pointer_t g_normal_pointer;
static gl_array_pointer_t g_texcoord_pointer;

static int get_type_size(GLenum type)
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

static float read_attrib_value(const unsigned char *ptr, GLenum type)
{
    switch (type) {
    case GL_BYTE:           return (float)*(const signed char *)ptr;
    case GL_UNSIGNED_BYTE:  return (float)*(const unsigned char *)ptr;
    case GL_SHORT:          return (float)*(const short *)ptr;
    case GL_UNSIGNED_SHORT: return (float)*(const unsigned short *)ptr;
    case GL_INT:            return (float)*(const int *)ptr;
    case GL_UNSIGNED_INT:   return (float)*(const unsigned int *)ptr;
    case GL_FLOAT:          return *(const float *)ptr;
    default:                return 0.0f;
    }
}

static int read_index(const unsigned char *ptr, GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:  return *ptr;
    case GL_UNSIGNED_SHORT: return *(const unsigned short *)ptr;
    case GL_UNSIGNED_INT:   return *(const unsigned int *)ptr;
    default:                return 0;
    }
}

void gl_vertex_pointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    if (size < 2 || size > 4) return;
    g_vertex_pointer.size = size;
    g_vertex_pointer.type = type;
    g_vertex_pointer.stride = stride;
    g_vertex_pointer.pointer = pointer;
}

void gl_color_pointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    if (size < 3 || size > 4) return;
    g_color_pointer.size = size;
    g_color_pointer.type = type;
    g_color_pointer.stride = stride;
    g_color_pointer.pointer = pointer;
}

void gl_normal_pointer(GLenum type, GLsizei stride, const GLvoid *pointer)
{
    g_normal_pointer.size = 3;
    g_normal_pointer.type = type;
    g_normal_pointer.stride = stride;
    g_normal_pointer.pointer = pointer;
}

void gl_tex_coord_pointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    if (size < 1 || size > 4) return;
    g_texcoord_pointer.size = size;
    g_texcoord_pointer.type = type;
    g_texcoord_pointer.stride = stride;
    g_texcoord_pointer.pointer = pointer;
}

void gl_enable_client_state(GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY:    g_gl_state.vertex_array_enabled = GL_TRUE; break;
    case GL_COLOR_ARRAY:     g_gl_state.color_array_enabled = GL_TRUE; break;
    case GL_NORMAL_ARRAY:    g_gl_state.normal_array_enabled = GL_TRUE; break;
    case GL_TEXTURE_COORD_ARRAY: g_gl_state.texcoord_array_enabled = GL_TRUE; break;
    default: break;
    }
}

void gl_disable_client_state(GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY:    g_gl_state.vertex_array_enabled = GL_FALSE; break;
    case GL_COLOR_ARRAY:     g_gl_state.color_array_enabled = GL_FALSE; break;
    case GL_NORMAL_ARRAY:    g_gl_state.normal_array_enabled = GL_FALSE; break;
    case GL_TEXTURE_COORD_ARRAY: g_gl_state.texcoord_array_enabled = GL_FALSE; break;
    default: break;
    }
}

static void fetch_vertex(int index, gl_immediate_vertex_t *out)
{
    memset(out, 0, sizeof(gl_immediate_vertex_t));
    out->w = 1.0f;
    out->r = 1.0f;
    out->g = 1.0f;
    out->b = 1.0f;
    out->a = 1.0f;

    if (g_gl_state.vertex_array_enabled && g_vertex_pointer.pointer) {
        int stride = g_vertex_pointer.stride;
        if (stride == 0)
            stride = g_vertex_pointer.size * get_type_size(g_vertex_pointer.type);
        const unsigned char *ptr =
            (const unsigned char *)g_vertex_pointer.pointer + index * stride;

        if (g_vertex_pointer.size >= 1) out->x = read_attrib_value(ptr, g_vertex_pointer.type);
        if (g_vertex_pointer.size >= 2) out->y = read_attrib_value(ptr + get_type_size(g_vertex_pointer.type), g_vertex_pointer.type);
        if (g_vertex_pointer.size >= 3) out->z = read_attrib_value(ptr + 2 * get_type_size(g_vertex_pointer.type), g_vertex_pointer.type);
        if (g_vertex_pointer.size >= 4) out->w = read_attrib_value(ptr + 3 * get_type_size(g_vertex_pointer.type), g_vertex_pointer.type);
    }

    if (g_gl_state.color_array_enabled && g_color_pointer.pointer) {
        int stride = g_color_pointer.stride;
        if (stride == 0)
            stride = g_color_pointer.size * get_type_size(g_color_pointer.type);
        const unsigned char *ptr =
            (const unsigned char *)g_color_pointer.pointer + index * stride;

        if (g_color_pointer.size >= 1) out->r = read_attrib_value(ptr, g_color_pointer.type);
        if (g_color_pointer.size >= 2) out->g = read_attrib_value(ptr + get_type_size(g_color_pointer.type), g_color_pointer.type);
        if (g_color_pointer.size >= 3) out->b = read_attrib_value(ptr + 2 * get_type_size(g_color_pointer.type), g_color_pointer.type);
        if (g_color_pointer.size >= 4) out->a = read_attrib_value(ptr + 3 * get_type_size(g_color_pointer.type), g_color_pointer.type);
    }

    if (g_gl_state.normal_array_enabled && g_normal_pointer.pointer) {
        int stride = g_normal_pointer.stride;
        if (stride == 0)
            stride = 3 * get_type_size(g_normal_pointer.type);
        const unsigned char *ptr =
            (const unsigned char *)g_normal_pointer.pointer + index * stride;

        out->nx = read_attrib_value(ptr, g_normal_pointer.type);
        out->ny = read_attrib_value(ptr + get_type_size(g_normal_pointer.type), g_normal_pointer.type);
        out->nz = read_attrib_value(ptr + 2 * get_type_size(g_normal_pointer.type), g_normal_pointer.type);
    }

    if (g_gl_state.texcoord_array_enabled && g_texcoord_pointer.pointer) {
        int stride = g_texcoord_pointer.stride;
        if (stride == 0)
            stride = g_texcoord_pointer.size * get_type_size(g_texcoord_pointer.type);
        const unsigned char *ptr =
            (const unsigned char *)g_texcoord_pointer.pointer + index * stride;

        if (g_texcoord_pointer.size >= 1) out->s = read_attrib_value(ptr, g_texcoord_pointer.type);
        if (g_texcoord_pointer.size >= 2) out->t = read_attrib_value(ptr + get_type_size(g_texcoord_pointer.type), g_texcoord_pointer.type);
    }
}

static void transform_and_rasterize(gl_immediate_vertex_t *verts, int count, GLenum mode)
{
    mat4_t mvp = mat4_multiply(g_gl_state.projection_matrix,
                                g_gl_state.modelview_matrix);

    int i;

    switch (mode) {
    case GL_TRIANGLES:
        for (i = 0; i + 2 < count; i += 3) {
            gl_vertex_t tri_verts[3];
            for (int j = 0; j < 3; j++) {
                vec4_t pos = { verts[i+j].x, verts[i+j].y, verts[i+j].z, verts[i+j].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                tri_verts[j].clip_pos = clip;
                tri_verts[j].world_pos = (vec3_t){ verts[i+j].x, verts[i+j].y, verts[i+j].z };
                tri_verts[j].color = (vec4_t){ verts[i+j].r, verts[i+j].g, verts[i+j].b, verts[i+j].a };
                tri_verts[j].texcoord = (vec2_t){ verts[i+j].s, verts[i+j].t };
                tri_verts[j].normal = (vec3_t){ verts[i+j].nx, verts[i+j].ny, verts[i+j].nz };
                tri_verts[j].eye_z = clip.z;
            }
            gl_triangle_t tri;
            tri.v[0] = tri_verts[0];
            tri.v[1] = tri_verts[1];
            tri.v[2] = tri_verts[2];
            gl_rasterize_triangle(&tri);
        }
        break;

    case GL_TRIANGLE_STRIP:
        for (i = 0; i + 2 < count; i++) {
            gl_vertex_t tri_verts[3];
            int indices[3];
            if (i & 1) {
                indices[0] = i + 1;
                indices[1] = i;
                indices[2] = i + 2;
            } else {
                indices[0] = i;
                indices[1] = i + 1;
                indices[2] = i + 2;
            }
            for (int j = 0; j < 3; j++) {
                int idx = indices[j];
                vec4_t pos = { verts[idx].x, verts[idx].y, verts[idx].z, verts[idx].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                tri_verts[j].clip_pos = clip;
                tri_verts[j].world_pos = (vec3_t){ verts[idx].x, verts[idx].y, verts[idx].z };
                tri_verts[j].color = (vec4_t){ verts[idx].r, verts[idx].g, verts[idx].b, verts[idx].a };
                tri_verts[j].texcoord = (vec2_t){ verts[idx].s, verts[idx].t };
                tri_verts[j].normal = (vec3_t){ verts[idx].nx, verts[idx].ny, verts[idx].nz };
                tri_verts[j].eye_z = clip.z;
            }
            gl_triangle_t tri;
            tri.v[0] = tri_verts[0];
            tri.v[1] = tri_verts[1];
            tri.v[2] = tri_verts[2];
            gl_rasterize_triangle(&tri);
        }
        break;

    case GL_TRIANGLE_FAN:
        for (i = 1; i + 1 < count; i++) {
            gl_vertex_t tri_verts[3];
            int indices[3] = { 0, i, i + 1 };
            for (int j = 0; j < 3; j++) {
                int idx = indices[j];
                vec4_t pos = { verts[idx].x, verts[idx].y, verts[idx].z, verts[idx].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                tri_verts[j].clip_pos = clip;
                tri_verts[j].world_pos = (vec3_t){ verts[idx].x, verts[idx].y, verts[idx].z };
                tri_verts[j].color = (vec4_t){ verts[idx].r, verts[idx].g, verts[idx].b, verts[idx].a };
                tri_verts[j].texcoord = (vec2_t){ verts[idx].s, verts[idx].t };
                tri_verts[j].normal = (vec3_t){ verts[idx].nx, verts[idx].ny, verts[idx].nz };
                tri_verts[j].eye_z = clip.z;
            }
            gl_triangle_t tri;
            tri.v[0] = tri_verts[0];
            tri.v[1] = tri_verts[1];
            tri.v[2] = tri_verts[2];
            gl_rasterize_triangle(&tri);
        }
        break;

    case GL_LINES:
        for (i = 0; i + 1 < count; i += 2) {
            gl_vertex_t line_verts[2];
            for (int j = 0; j < 2; j++) {
                int idx = i + j;
                vec4_t pos = { verts[idx].x, verts[idx].y, verts[idx].z, verts[idx].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                line_verts[j].clip_pos = clip;
                line_verts[j].world_pos = (vec3_t){ verts[idx].x, verts[idx].y, verts[idx].z };
                line_verts[j].color = (vec4_t){ verts[idx].r, verts[idx].g, verts[idx].b, verts[idx].a };
                line_verts[j].texcoord = (vec2_t){ verts[idx].s, verts[idx].t };
                line_verts[j].normal = (vec3_t){ verts[idx].nx, verts[idx].ny, verts[idx].nz };
                line_verts[j].eye_z = clip.z;
            }
            gl_rasterize_line(&line_verts[0], &line_verts[1]);
        }
        break;

    case GL_LINE_STRIP:
        for (i = 0; i + 1 < count; i++) {
            gl_vertex_t line_verts[2];
            for (int j = 0; j < 2; j++) {
                int idx = i + j;
                vec4_t pos = { verts[idx].x, verts[idx].y, verts[idx].z, verts[idx].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                line_verts[j].clip_pos = clip;
                line_verts[j].world_pos = (vec3_t){ verts[idx].x, verts[idx].y, verts[idx].z };
                line_verts[j].color = (vec4_t){ verts[idx].r, verts[idx].g, verts[idx].b, verts[idx].a };
                line_verts[j].texcoord = (vec2_t){ verts[idx].s, verts[idx].t };
                line_verts[j].normal = (vec3_t){ verts[idx].nx, verts[idx].ny, verts[idx].nz };
                line_verts[j].eye_z = clip.z;
            }
            gl_rasterize_line(&line_verts[0], &line_verts[1]);
        }
        break;

    case GL_LINE_LOOP:
        for (i = 0; i + 1 < count; i++) {
            gl_vertex_t line_verts[2];
            for (int j = 0; j < 2; j++) {
                int idx = (j == 0) ? i : (i + 1 < count) ? i + 1 : 0;
                vec4_t pos = { verts[idx].x, verts[idx].y, verts[idx].z, verts[idx].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                line_verts[j].clip_pos = clip;
                line_verts[j].world_pos = (vec3_t){ verts[idx].x, verts[idx].y, verts[idx].z };
                line_verts[j].color = (vec4_t){ verts[idx].r, verts[idx].g, verts[idx].b, verts[idx].a };
                line_verts[j].texcoord = (vec2_t){ verts[idx].s, verts[idx].t };
                line_verts[j].normal = (vec3_t){ verts[idx].nx, verts[idx].ny, verts[idx].nz };
                line_verts[j].eye_z = clip.z;
            }
            gl_rasterize_line(&line_verts[0], &line_verts[1]);
        }
        if (count > 2) {
            gl_vertex_t line_verts[2];
            int indices[2] = { count - 1, 0 };
            for (int j = 0; j < 2; j++) {
                int idx = indices[j];
                vec4_t pos = { verts[idx].x, verts[idx].y, verts[idx].z, verts[idx].w };
                vec4_t clip = mat4_multiply_vec4(mvp, pos);
                if (clip.w != 0.0f) {
                    clip.x /= clip.w;
                    clip.y /= clip.w;
                    clip.z /= clip.w;
                }
                line_verts[j].clip_pos = clip;
                line_verts[j].world_pos = (vec3_t){ verts[idx].x, verts[idx].y, verts[idx].z };
                line_verts[j].color = (vec4_t){ verts[idx].r, verts[idx].g, verts[idx].b, verts[idx].a };
                line_verts[j].texcoord = (vec2_t){ verts[idx].s, verts[idx].t };
                line_verts[j].normal = (vec3_t){ verts[idx].nx, verts[idx].ny, verts[idx].nz };
                line_verts[j].eye_z = clip.z;
            }
            gl_rasterize_line(&line_verts[0], &line_verts[1]);
        }
        break;

    case GL_POINTS:
        for (i = 0; i < count; i++) {
            vec4_t pos = { verts[i].x, verts[i].y, verts[i].z, verts[i].w };
            vec4_t clip = mat4_multiply_vec4(mvp, pos);
            if (clip.w != 0.0f) {
                clip.x /= clip.w;
                clip.y /= clip.w;
                clip.z /= clip.w;
            }
            gl_vertex_t v;
            v.clip_pos = clip;
            v.world_pos = (vec3_t){ verts[i].x, verts[i].y, verts[i].z };
            v.color = (vec4_t){ verts[i].r, verts[i].g, verts[i].b, verts[i].a };
            v.texcoord = (vec2_t){ verts[i].s, verts[i].t };
            v.normal = (vec3_t){ verts[i].nx, verts[i].ny, verts[i].nz };
            v.eye_z = clip.z;
            gl_rasterize_point(&v);
        }
        break;

    default:
        break;
    }
}

void gl_draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0 || !g_gl_state.vertex_array_enabled) return;
    if (!g_vertex_pointer.pointer) return;

    gl_immediate_vertex_t *verts =
        (gl_immediate_vertex_t *)kmalloc(count * sizeof(gl_immediate_vertex_t));
    if (!verts) return;

    for (int i = 0; i < count; i++)
        fetch_vertex(first + i, &verts[i]);

    transform_and_rasterize(verts, count, mode);
    kfree(verts);
}

void gl_draw_elements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    if (count <= 0 || !indices || !g_gl_state.vertex_array_enabled) return;
    if (!g_vertex_pointer.pointer) return;

    gl_immediate_vertex_t *verts =
        (gl_immediate_vertex_t *)kmalloc(count * sizeof(gl_immediate_vertex_t));
    if (!verts) return;

    const unsigned char *idx_ptr = (const unsigned char *)indices;
    int idx_size = get_type_size(type);

    for (int i = 0; i < count; i++) {
        int idx = read_index(idx_ptr + i * idx_size, type);
        fetch_vertex(idx, &verts[i]);
    }

    transform_and_rasterize(verts, count, mode);
    kfree(verts);
}
