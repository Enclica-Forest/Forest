#include "gl.h"
#include "error.h"
#include "texture.h"
#include "buffer.h"
#include <string.h>

#define GL_VIEWPORT                    0x0BA2
#define GL_MODELVIEW_MATRIX            0x0BA6
#define GL_PROJECTION_MATRIX           0x0BA7
#define GL_TEXTURE_MATRIX              0x0BA8
#define GL_MATRIX_MODE                 0x0BA0
#define GL_MODELVIEW_STACK_DEPTH       0x0BA3
#define GL_PROJECTION_STACK_DEPTH      0x0BA4
#define GL_TEXTURE_STACK_DEPTH         0x0BA5
#define GL_CURRENT_COLOR               0x0B00
#define GL_CURRENT_NORMAL              0x0B02
#define GL_CURRENT_TEXTURE_COORDS      0x0B03
#define GL_DEPTH_RANGE                 0x0B70
#define GL_LINE_WIDTH                  0x0B21
#define GL_POINT_SIZE                  0x0B11
#define GL_POLYGON_MODE                0x0B40
#define GL_COLOR_MATERIAL_FACE         0x0B55
#define GL_COLOR_MATERIAL_PARAMETER    0x0B56
#define GL_MAX_VIEWPORT_DIMS           0x0D3A
#define GL_MAX_LIGHTS                  0x0D31
#define GL_MAX_TEXTURE_UNITS           0x84E2
#define GL_VERTEX_ARRAY_SIZE           0x807A
#define GL_VERTEX_ARRAY_TYPE           0x807B
#define GL_VERTEX_ARRAY_STRIDE         0x807C
#define GL_COLOR_ARRAY_SIZE            0x807E
#define GL_COLOR_ARRAY_TYPE            0x807F
#define GL_COLOR_ARRAY_STRIDE          0x8080
#define GL_NORMAL_ARRAY_TYPE           0x8078
#define GL_NORMAL_ARRAY_STRIDE         0x8079
#define GL_TEXTURE_COORD_ARRAY_SIZE    0x807D
#define GL_TEXTURE_COORD_ARRAY_TYPE    0x8089
#define GL_TEXTURE_COORD_ARRAY_STRIDE  0x808A
#define GL_ARRAY_BUFFER_BINDING        0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_ACTIVE_TEXTURE_ARB          0x84C0
#define GL_CLIENT_ACTIVE_TEXTURE       0x84E1
#define GL_TEXTURE_BINDING_2D          0x8069
#define GL_BLEND_SRC_RGB               0x80C9
#define GL_BLEND_DST_RGB               0x80C8
#define GL_BLEND_SRC_ALPHA             0x80CB
#define GL_BLEND_DST_ALPHA             0x80CC
#define GL_UNPACK_ALIGNMENT            0x0CF5
#define GL_PACK_ALIGNMENT              0x0D05
#define GL_DEPTH_BITS                  0x0D56
#define GL_STENCIL_BITS                0x0D57
#define GL_RED_BITS                    0x0D52
#define GL_GREEN_BITS                  0x0D53
#define GL_BLUE_BITS                   0x0D54
#define GL_ALPHA_BITS                  0x0D55
#define GL_LIGHT1                      0x4001
#define GL_LIGHT2                      0x4002
#define GL_LIGHT3                      0x4003
#define GL_LIGHT4                      0x4004
#define GL_LIGHT5                      0x4005
#define GL_LIGHT6                      0x4006
#define GL_LIGHT7                      0x4007
#define GL_BUFFER_SIZE                 0x8764
#define GL_BUFFER_USAGE                0x8765
#define GL_TEXTURE_WIDTH               0x1000
#define GL_TEXTURE_HEIGHT              0x1001
#define GL_TEXTURE_INTERNAL_FORMAT     0x1003
#define GL_CLEAR_STENCIL               0x0C71
#define GL_COLOR_CLEAR_VALUE           0x0C22
#define GL_DEPTH_CLEAR_VALUE           0x0B74
#define GL_DRAW_BUFFER                 0x0C01
#define GL_READ_BUFFER                 0x0C02
#define GL_DEPTH_FUNC                  0x0B74
#define GL_BLEND_SRC                   0x0BE1
#define GL_BLEND_DST                   0x0BE0
#define GL_CULL_FACE_MODE              0x0B45
#define GL_FRONT_FACE                  0x0B46
#define GL_STENCIL_FUNC                0x0B92
#define GL_STENCIL_VALUE_MASK          0x0B93
#define GL_STENCIL_WRITE_MASK         0x0B98
#define GL_STENCIL_FAIL                0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL    0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS    0x0B96
#define GL_STENCIL_REF                 0x0B97
#define GL_LOGIC_OP                    0x0BF1
#define GL_ALPHA_FUNC                  0x0BC0
#define GL_ALPHA_TEST_REF              0x0BC2
#define GL_CURRENT_PROGRAM             0x8B8D

extern gl_state_t g_gl_state;

static void get_matrix_floatv(const mat4_t *m, GLfloat *params) {
    for (int i = 0; i < 16; i++)
        params[i] = ((const float *)m)[i];
}

static void get_matrix_doublev(const mat4_t *m, GLdouble *params) {
    for (int i = 0; i < 16; i++)
        params[i] = (GLdouble)((const float *)m)[i];
}

void glGetIntegerv(GLenum pname, GLint *params) {
    if (!params) return;
    gl_state_t *s = &g_gl_state;
    switch (pname) {
    case GL_VIEWPORT:
        params[0] = s->viewport[0];
        params[1] = s->viewport[1];
        params[2] = s->viewport[2];
        params[3] = s->viewport[3];
        break;
    case GL_CLEAR_STENCIL:
        params[0] = s->clear_stencil;
        break;
    case GL_MATRIX_MODE:
        if (s->matrix_mode_ptr == &s->modelview_matrix)
            params[0] = GL_MODELVIEW;
        else if (s->matrix_mode_ptr == &s->projection_matrix)
            params[0] = GL_PROJECTION;
        else
            params[0] = GL_TEXTURE;
        break;
    case GL_MODELVIEW_STACK_DEPTH:
        params[0] = s->modelview_stack_top + 1;
        break;
    case GL_PROJECTION_STACK_DEPTH:
        params[0] = s->projection_stack_top + 1;
        break;
    case GL_DEPTH_FUNC:
        params[0] = s->depth_func;
        break;
    case GL_BLEND_SRC:
        params[0] = s->blend_src;
        break;
    case GL_BLEND_DST:
        params[0] = s->blend_dst;
        break;
    case GL_CULL_FACE_MODE:
        params[0] = s->cull_face_mode;
        break;
    case GL_FRONT_FACE:
        params[0] = s->front_face;
        break;
    case GL_STENCIL_FUNC:
        params[0] = s->stencil_func;
        break;
    case GL_STENCIL_VALUE_MASK:
        params[0] = s->stencil_val_mask;
        break;
    case GL_STENCIL_WRITE_MASK:
        params[0] = s->stencil_write_mask;
        break;
    case GL_STENCIL_FAIL:
        params[0] = s->stencil_sfail;
        break;
    case GL_STENCIL_PASS_DEPTH_FAIL:
        params[0] = s->stencil_dpfail;
        break;
    case GL_STENCIL_PASS_DEPTH_PASS:
        params[0] = s->stencil_dppass;
        break;
    case GL_LOGIC_OP:
        params[0] = s->logic_op;
        break;
    case GL_FOG_MODE:
        params[0] = s->fog_mode;
        break;
    case GL_ALPHA_FUNC:
        params[0] = s->alpha_func;
        break;
    case GL_ACTIVE_TEXTURE_ARB:
        params[0] = s->active_texture;
        break;
    case GL_ARRAY_BUFFER_BINDING:
        params[0] = s->bound_array_buffer;
        break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
        params[0] = s->bound_element_buffer;
        break;
    case GL_MAX_TEXTURE_SIZE:
        params[0] = 4096;
        break;
    case GL_MAX_LIGHTS:
        params[0] = 8;
        break;
    case GL_MAX_TEXTURE_UNITS:
        params[0] = 8;
        break;
    case GL_POLYGON_MODE:
        params[0] = GL_FILL;
        params[1] = GL_FILL;
        break;
    case GL_COLOR_MATERIAL_FACE:
        params[0] = GL_FRONT_AND_BACK;
        break;
    case GL_COLOR_MATERIAL_PARAMETER:
        params[0] = GL_AMBIENT_AND_DIFFUSE;
        break;
    case GL_UNPACK_ALIGNMENT:
        params[0] = 4;
        break;
    case GL_PACK_ALIGNMENT:
        params[0] = 4;
        break;
    case GL_DEPTH_BITS:
        params[0] = 24;
        break;
    case GL_STENCIL_BITS:
        params[0] = 8;
        break;
    case GL_RED_BITS:
        params[0] = 8;
        break;
    case GL_GREEN_BITS:
        params[0] = 8;
        break;
    case GL_BLUE_BITS:
        params[0] = 8;
        break;
    case GL_ALPHA_BITS:
        params[0] = 8;
        break;
    case GL_TEXTURE_STACK_DEPTH:
        params[0] = s->texture_stack_top[0] + 1;
        break;
    case GL_CURRENT_PROGRAM:
        params[0] = s->current_program;
        break;
    case GL_STENCIL_REF:
        params[0] = s->stencil_ref;
        break;
    case GL_DRAW_BUFFER:
        params[0] = GL_BACK;
        break;
    case GL_READ_BUFFER:
        params[0] = GL_BACK;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *params) {
    if (!params) return;
    gl_state_t *s = &g_gl_state;
    switch (pname) {
    case GL_VIEWPORT:
        params[0] = (GLfloat)s->viewport[0];
        params[1] = (GLfloat)s->viewport[1];
        params[2] = (GLfloat)s->viewport[2];
        params[3] = (GLfloat)s->viewport[3];
        break;
    case GL_COLOR_CLEAR_VALUE:
        params[0] = s->clear_color[0];
        params[1] = s->clear_color[1];
        params[2] = s->clear_color[2];
        params[3] = s->clear_color[3];
        break;
    case GL_DEPTH_CLEAR_VALUE:
        params[0] = (GLfloat)s->clear_depth;
        break;
    case GL_DEPTH_RANGE:
        params[0] = 0.0f;
        params[1] = 1.0f;
        break;
    case GL_MODELVIEW_MATRIX:
        get_matrix_floatv(&s->modelview_matrix, params);
        break;
    case GL_PROJECTION_MATRIX:
        get_matrix_floatv(&s->projection_matrix, params);
        break;
    case GL_TEXTURE_MATRIX:
        get_matrix_floatv(&s->texture_matrix[0], params);
        break;
    case GL_CURRENT_COLOR:
        params[0] = s->current_color[0];
        params[1] = s->current_color[1];
        params[2] = s->current_color[2];
        params[3] = s->current_color[3];
        break;
    case GL_CURRENT_NORMAL:
        params[0] = s->current_normal[0];
        params[1] = s->current_normal[1];
        params[2] = s->current_normal[2];
        break;
    case GL_CURRENT_TEXTURE_COORDS:
        params[0] = s->current_texcoord[0];
        params[1] = s->current_texcoord[1];
        break;
    case GL_LINE_WIDTH:
        params[0] = 1.0f;
        break;
    case GL_POINT_SIZE:
        params[0] = 1.0f;
        break;
    case GL_POLYGON_OFFSET_FACTOR:
        params[0] = s->polygon_offset_factor;
        break;
    case GL_POLYGON_OFFSET_UNITS:
        params[0] = s->polygon_offset_units;
        break;
    case GL_FOG_START:
        params[0] = s->fog_start;
        break;
    case GL_FOG_END:
        params[0] = s->fog_end;
        break;
    case GL_FOG_DENSITY:
        params[0] = s->fog_density;
        break;
    case GL_FOG_COLOR:
        params[0] = s->fog_color[0];
        params[1] = s->fog_color[1];
        params[2] = s->fog_color[2];
        params[3] = s->fog_color[3];
        break;
    case GL_ALPHA_TEST_REF:
        params[0] = s->alpha_ref;
        break;
    case GL_STENCIL_REF:
        params[0] = (GLfloat)s->stencil_ref;
        break;
    case GL_MAX_VIEWPORT_DIMS:
        params[0] = 4096.0f;
        params[1] = 4096.0f;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glGetBooleanv(GLenum pname, GLboolean *params) {
    if (!params) return;
    GLint ival;
    glGetIntegerv(pname, &ival);
    *params = ival ? GL_TRUE : GL_FALSE;
}

void glGetDoublev(GLenum pname, GLdouble *params) {
    if (!params) return;
    GLfloat fval;
    glGetFloatv(pname, &fval);
    *params = (GLdouble)fval;
}

GLboolean glIsEnabled(GLenum cap) {
    gl_state_t *s = &g_gl_state;
    switch (cap) {
    case GL_DEPTH_TEST:    return s->depth_test;
    case GL_BLEND:         return s->blend;
    case GL_CULL_FACE:     return s->cull_face;
    case GL_SCISSOR_TEST:  return s->scissor_test;
    case GL_STENCIL_TEST:  return s->stencil_test;
    case GL_TEXTURE_2D:    return s->texture_2d;
    case GL_LIGHTING:      return s->lighting;
    case GL_NORMALIZE:     return s->normalize;
    case GL_COLOR_MATERIAL: return s->color_material;
    case GL_FOG:           return s->fog_enabled;
    case GL_ALPHA_TEST:    return s->alpha_test_enabled;
    case GL_COLOR_LOGIC_OP: return s->color_logic_op_enabled;
    case GL_LIGHT0:        return s->light_enabled[0];
    case GL_LIGHT1:        return s->light_enabled[1];
    case GL_LIGHT2:        return s->light_enabled[2];
    case GL_LIGHT3:        return s->light_enabled[3];
    case GL_LIGHT4:        return s->light_enabled[4];
    case GL_LIGHT5:        return s->light_enabled[5];
    case GL_LIGHT6:        return s->light_enabled[6];
    case GL_LIGHT7:        return s->light_enabled[7];
    case GL_VERTEX_ARRAY:        return s->vertex_array_enabled;
    case GL_COLOR_ARRAY:         return s->color_array_enabled;
    case GL_NORMAL_ARRAY:        return s->normal_array_enabled;
    case GL_TEXTURE_COORD_ARRAY: return s->texcoord_array_enabled;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return GL_FALSE;
    }
}

void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) {
    if (!pixels) return;
    if (target != GL_TEXTURE_2D) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    GLuint tex_name = g_gl_state.bound_textures[g_gl_state.active_texture - GL_TEXTURE0];
    gl_texture_t *tex = gl_texture_get(tex_name);
    if (!tex || !tex->used || !tex->data) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    if (level < 0 || level >= tex->mip_levels) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    int mip_w = tex->mip_width[level];
    int mip_h = tex->mip_height[level];
    if (mip_w <= 0 || mip_h <= 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    int src_bpp = gl_texture_bpp(tex);
    int src_offset = 0;
    for (int i = 0; i < level; i++)
        src_offset += tex->mip_width[i] * tex->mip_height[i] * src_bpp;

    int dst_bpp;
    switch (format) {
    case GL_RGBA: dst_bpp = 4; break;
    case GL_RGB:  dst_bpp = 3; break;
    case GL_RED:  dst_bpp = 1; break;
    default: gl_set_error(GL_INVALID_ENUM); return;
    }

    int elem_size;
    switch (type) {
    case GL_UNSIGNED_BYTE: elem_size = 1; break;
    case GL_FLOAT:         elem_size = 4; break;
    default: gl_set_error(GL_INVALID_ENUM); return;
    }

    GLubyte *dst = (GLubyte *)pixels;
    GLubyte *src = tex->data + src_offset;
    int dst_stride = mip_w * dst_bpp * elem_size;

    for (int y = 0; y < mip_h; y++) {
        for (int x = 0; x < mip_w; x++) {
            GLubyte r = src[(y * mip_w + x) * src_bpp + 0];
            GLubyte g = src_bpp >= 3 ? src[(y * mip_w + x) * src_bpp + 1] : r;
            GLubyte b = src_bpp >= 3 ? src[(y * mip_w + x) * src_bpp + 2] : r;
            GLubyte a = src_bpp >= 4 ? src[(y * mip_w + x) * src_bpp + 3] : 255;

            int off = (y * mip_w + x) * dst_bpp * elem_size;
            if (type == GL_UNSIGNED_BYTE) {
                if (format == GL_RGBA) {
                    dst[off+0] = r; dst[off+1] = g; dst[off+2] = b; dst[off+3] = a;
                } else if (format == GL_RGB) {
                    dst[off+0] = r; dst[off+1] = g; dst[off+2] = b;
                } else {
                    dst[off] = r;
                }
            } else {
                float *fdst = (float *)(dst + off);
                if (format == GL_RGBA) {
                    fdst[0] = r/255.0f; fdst[1] = g/255.0f;
                    fdst[2] = b/255.0f; fdst[3] = a/255.0f;
                } else if (format == GL_RGB) {
                    fdst[0] = r/255.0f; fdst[1] = g/255.0f; fdst[2] = b/255.0f;
                } else {
                    fdst[0] = r/255.0f;
                }
            }
        }
    }
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) {
    if (!params) return;
    if (target != GL_TEXTURE_2D) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    GLuint tex_name = g_gl_state.bound_textures[g_gl_state.active_texture - GL_TEXTURE0];
    gl_texture_t *tex = gl_texture_get(tex_name);
    if (!tex || !tex->used) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    if (level < 0 || level >= tex->mip_levels) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    switch (pname) {
    case GL_TEXTURE_WIDTH:        params[0] = tex->mip_width[level]; break;
    case GL_TEXTURE_HEIGHT:       params[0] = tex->mip_height[level]; break;
    case GL_TEXTURE_INTERNAL_FORMAT: params[0] = tex->internal_format; break;
    case GL_TEXTURE_MAX_LEVEL:    params[0] = tex->mip_levels - 1; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) {
    if (!params || target != GL_TEXTURE_2D) {
        if (target != GL_TEXTURE_2D) gl_set_error(GL_INVALID_ENUM);
        return;
    }
    GLuint tex_name = g_gl_state.bound_textures[g_gl_state.active_texture - GL_TEXTURE0];
    gl_texture_t *tex = gl_texture_get(tex_name);
    if (!tex || !tex->used) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: params[0] = (GLfloat)tex->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: params[0] = (GLfloat)tex->mag_filter; break;
    case GL_TEXTURE_WRAP_S:     params[0] = (GLfloat)tex->wrap_s; break;
    case GL_TEXTURE_WRAP_T:     params[0] = (GLfloat)tex->wrap_t; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) {
    if (!params || target != GL_TEXTURE_2D) {
        if (target != GL_TEXTURE_2D) gl_set_error(GL_INVALID_ENUM);
        return;
    }
    GLuint tex_name = g_gl_state.bound_textures[g_gl_state.active_texture - GL_TEXTURE0];
    gl_texture_t *tex = gl_texture_get(tex_name);
    if (!tex || !tex->used) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: params[0] = tex->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: params[0] = tex->mag_filter; break;
    case GL_TEXTURE_WRAP_S:     params[0] = tex->wrap_s; break;
    case GL_TEXTURE_WRAP_T:     params[0] = tex->wrap_t; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    if (!params) return;
    GLuint buf_name = 0;
    if (target == GL_ARRAY_BUFFER)
        buf_name = g_gl_state.bound_array_buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        buf_name = g_gl_state.bound_element_buffer;
    else {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    gl_buffer_t *buf = gl_buffer_get(buf_name);
    if (!buf || !buf->used) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    switch (pname) {
    case GL_BUFFER_SIZE:  params[0] = (GLint)buf->size; break;
    case GL_BUFFER_USAGE: params[0] = buf->usage; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}
