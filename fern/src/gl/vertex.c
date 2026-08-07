#include "vertex.h"
#include "buffer.h"
#include "math.h"
#include "../include/memory.h"
#include "../include/string.h"

void gl_vertex_init(void) {
}

static int get_type_size(GLenum type) {
    switch (type) {
        case GL_FLOAT:          return 4;
        case GL_UNSIGNED_BYTE:  return 1;
        case GL_BYTE:           return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_SHORT:          return 2;
        case GL_UNSIGNED_INT:   return 4;
        case GL_INT:            return 4;
        default:                return 4;
    }
}

static void read_attrib_value(const unsigned char *ptr, GLenum type, float *out) {
    switch (type) {
        case GL_FLOAT:
            *out = *(const float*)ptr;
            break;
        case GL_UNSIGNED_BYTE:
            *out = (float)(*ptr) / 255.0f;
            break;
        case GL_BYTE:
            *out = (float)(*(signed char*)ptr) / 127.0f;
            break;
        case GL_UNSIGNED_SHORT:
            *out = (float)(*(const unsigned short*)ptr) / 65535.0f;
            break;
        case GL_SHORT:
            *out = (float)(*(const short*)ptr) / 32767.0f;
            break;
        case GL_UNSIGNED_INT:
            *out = (float)(*(const unsigned int*)ptr);
            break;
        case GL_INT:
            *out = (float)(*(const int*)ptr);
            break;
        default:
            *out = 0.0f;
            break;
    }
}

static void fetch_attrib(GLuint index, GLuint vertex_index, float *out) {
    gl_attrib_pointer_t *attr = gl_attrib_get(index);
    if (!attr || !attr->enabled) return;

    const unsigned char *base = 0;

    if (attr->buffer != 0) {
        gl_buffer_t *buf = gl_buffer_get(attr->buffer);
        if (!buf || !buf->data) return;
        base = (const unsigned char*)buf->data;
    } else {
        base = (const unsigned char*)attr->pointer;
    }

    if (!base) return;

    int type_size = get_type_size(attr->type);
    int elem_size = attr->stride ? attr->stride : attr->size * type_size;
    const unsigned char *ptr = base + vertex_index * elem_size;

    for (int i = 0; i < attr->size && i < 4; i++) {
        int offset = i * (attr->type == GL_FLOAT ? 4 : type_size);
        read_attrib_value(ptr + offset, attr->type, &out[i]);
    }
}

void gl_vertex_fetch(GLuint index, float *pos_out, float *color_out,
                     float *texcoord_out, float *normal_out) {
    float pos[4] = {0, 0, 0, 1};
    float color[4] = {1, 1, 1, 1};
    float texcoord[2] = {0, 0};
    float normal[3] = {0, 0, 1};

    fetch_attrib(0, index, pos);
    fetch_attrib(3, index, color);
    fetch_attrib(8, index, texcoord);
    fetch_attrib(2, index, normal);

    if (pos_out) {
        pos_out[0] = pos[0];
        pos_out[1] = pos[1];
        pos_out[2] = pos[2];
        pos_out[3] = pos[3];
    }
    if (color_out) {
        color_out[0] = color[0];
        color_out[1] = color[1];
        color_out[2] = color[2];
        color_out[3] = color[3];
    }
    if (texcoord_out) {
        texcoord_out[0] = texcoord[0];
        texcoord_out[1] = texcoord[1];
    }
    if (normal_out) {
        normal_out[0] = normal[0];
        normal_out[1] = normal[1];
        normal_out[2] = normal[2];
    }
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void clip_to_screen(const float *clip, float *screen) {
    if (clip[3] == 0.0f) {
        screen[0] = 0;
        screen[1] = 0;
        screen[2] = 0;
        return;
    }
    float ndc_x = clip[0] / clip[3];
    float ndc_y = clip[1] / clip[3];
    float ndc_z = clip[2] / clip[3];

    float vx = (float)g_gl_state.viewport[0];
    float vy = (float)g_gl_state.viewport[1];
    float vw = (float)g_gl_state.viewport[2];
    float vh = (float)g_gl_state.viewport[3];

    screen[0] = vx + (ndc_x + 1.0f) * 0.5f * vw;
    screen[1] = vy + (1.0f - ndc_y) * 0.5f * vh;
    screen[2] = (ndc_z + 1.0f) * 0.5f;
}

void gl_vertex_transform(const float *in_pos4, float *out_clip, float *out_screen) {
    mat4_t mvp = mat4_multiply(g_gl_state.projection_matrix, g_gl_state.modelview_matrix);
    vec4_t in_v = {in_pos4[0], in_pos4[1], in_pos4[2], in_pos4[3]};
    vec4_t clip = mat4_multiply_vec4(mvp, in_v);

    out_clip[0] = clip.x;
    out_clip[1] = clip.y;
    out_clip[2] = clip.z;
    out_clip[3] = clip.w;

    clip_to_screen(clip.x == clip.x ? (float*)&clip : out_clip, out_screen);
}

void gl_vertex_setup_interp(gl_triangle_attrib_t *tri) {
    for (int i = 0; i < 3; i++) {
        tri->v[i].clip_w = tri->v[i].w;
        if (tri->v[i].clip_w == 0.0f) tri->v[i].clip_w = 1.0f;
        tri->v[i].depth = (tri->v[i].z / tri->v[i].clip_w + 1.0f) * 0.5f;
        tri->v[i].depth = clampf(tri->v[i].depth, 0.0f, 1.0f);
    }
}

static float edge_function(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static int check_winding(gl_vertex_attrib_t *v0, gl_vertex_attrib_t *v1, gl_vertex_attrib_t *v2) {
    float area = edge_function(v0->screen_x, v0->screen_y,
                               v1->screen_x, v1->screen_y,
                               v2->screen_x, v2->screen_y);
    if (g_gl_state.front_face == GL_CCW) {
        return area > 0.0f;
    }
    return area < 0.0f;
}

void gl_vertex_process_triangle(GLuint idx0, GLuint idx1, GLuint idx2,
                                gl_triangle_attrib_t *out) {
    float pos[4], color[4], texcoord[2], normal[3];
    float clip[4], screen[3];

    memset(out, 0, sizeof(gl_triangle_attrib_t));

    GLuint indices[3] = {idx0, idx1, idx2};
    for (int i = 0; i < 3; i++) {
        gl_vertex_fetch(indices[i], pos, color, texcoord, normal);

        gl_vertex_transform(pos, clip, screen);

        out->v[i].x = clip[0];
        out->v[i].y = clip[1];
        out->v[i].z = clip[2];
        out->v[i].w = clip[3];
        out->v[i].screen_x = screen[0];
        out->v[i].screen_y = screen[1];
        out->v[i].r = color[0];
        out->v[i].g = color[1];
        out->v[i].b = color[2];
        out->v[i].a = color[3];
        out->v[i].u = texcoord[0];
        out->v[i].v = texcoord[1];
        out->v[i].nx = normal[0];
        out->v[i].ny = normal[1];
        out->v[i].nz = normal[2];
    }

    gl_vertex_setup_interp(out);

    out->culled = 0;
    if (g_gl_state.cull_face) {
        int front = check_winding(&out->v[0], &out->v[1], &out->v[2]);
        if (g_gl_state.cull_face_mode == GL_BACK && !front) {
            out->culled = 1;
        } else if (g_gl_state.cull_face_mode == GL_FRONT && front) {
            out->culled = 1;
        } else if (g_gl_state.cull_face_mode == GL_FRONT_AND_BACK) {
            out->culled = 1;
        }
    }
}
