#include "api_state.h"
#include "rasterizer.h"
#include "displaylist.h"
#include <string.h>

void gl_enable(GLenum cap)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_enable(cap);
        return;
    }
    switch (cap) {
    case GL_DEPTH_TEST:    g_gl_state.depth_test = GL_TRUE;    break;
    case GL_BLEND:         g_gl_state.blend = GL_TRUE;         break;
    case GL_CULL_FACE:     g_gl_state.cull_face = GL_TRUE;     break;
    case GL_SCISSOR_TEST:  g_gl_state.scissor_test = GL_TRUE;  break;
    case GL_STENCIL_TEST:  g_gl_state.stencil_test = GL_TRUE;  break;
    case GL_TEXTURE_2D:    g_gl_state.texture_2d = GL_TRUE;    break;
    case GL_LIGHTING:      g_gl_state.lighting = GL_TRUE;      break;
    case GL_NORMALIZE:     g_gl_state.normalize = GL_TRUE;     break;
    case GL_COLOR_MATERIAL: g_gl_state.color_material = GL_TRUE; break;
    case GL_FOG:           g_gl_state.fog_enabled = GL_TRUE;   break;
    case GL_ALPHA_TEST:    g_gl_state.alpha_test_enabled = GL_TRUE; break;
    case GL_COLOR_LOGIC_OP: g_gl_state.color_logic_op_enabled = GL_TRUE; break;
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8)
            g_gl_state.light_enabled[cap - GL_LIGHT0] = GL_TRUE;
        break;
    }
}

void gl_disable(GLenum cap)
{
    if (gl_dl_is_recording()) {
        gl_dl_record_disable(cap);
        return;
    }
    switch (cap) {
    case GL_DEPTH_TEST:    g_gl_state.depth_test = GL_FALSE;    break;
    case GL_BLEND:         g_gl_state.blend = GL_FALSE;         break;
    case GL_CULL_FACE:     g_gl_state.cull_face = GL_FALSE;     break;
    case GL_SCISSOR_TEST:  g_gl_state.scissor_test = GL_FALSE;  break;
    case GL_STENCIL_TEST:  g_gl_state.stencil_test = GL_FALSE;  break;
    case GL_TEXTURE_2D:    g_gl_state.texture_2d = GL_FALSE;    break;
    case GL_LIGHTING:      g_gl_state.lighting = GL_FALSE;      break;
    case GL_NORMALIZE:     g_gl_state.normalize = GL_FALSE;     break;
    case GL_COLOR_MATERIAL: g_gl_state.color_material = GL_FALSE; break;
    case GL_FOG:           g_gl_state.fog_enabled = GL_FALSE;   break;
    case GL_ALPHA_TEST:    g_gl_state.alpha_test_enabled = GL_FALSE; break;
    case GL_COLOR_LOGIC_OP: g_gl_state.color_logic_op_enabled = GL_FALSE; break;
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8)
            g_gl_state.light_enabled[cap - GL_LIGHT0] = GL_FALSE;
        break;
    }
}

void gl_blend_func(GLenum sfactor, GLenum dfactor)
{
    g_gl_state.blend_src = sfactor;
    g_gl_state.blend_dst = dfactor;
}

void gl_depth_func(GLenum func)
{
    g_gl_state.depth_func = func;
}

void gl_depth_mask(GLboolean flag)
{
    (void)flag;
}

void gl_cull_face(GLenum mode)
{
    g_gl_state.cull_face_mode = mode;
}

void gl_front_face(GLenum mode)
{
    g_gl_state.front_face = mode;
}

void gl_scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    g_gl_state.scissor_box[0] = x;
    g_gl_state.scissor_box[1] = y;
    g_gl_state.scissor_box[2] = width;
    g_gl_state.scissor_box[3] = height;
}

void gl_stencil_func(GLenum func, GLint ref, GLuint mask)
{
    g_gl_state.stencil_func = func;
    g_gl_state.stencil_ref = ref;
    g_gl_state.stencil_val_mask = mask;
}

void gl_stencil_op(GLenum sfail, GLenum dpfail, GLenum dppass)
{
    g_gl_state.stencil_sfail = sfail;
    g_gl_state.stencil_dpfail = dpfail;
    g_gl_state.stencil_dppass = dppass;
}

void gl_stencil_mask(GLuint mask)
{
    g_gl_state.stencil_write_mask = mask;
}

void gl_polygon_mode(GLenum face, GLenum mode)
{
    (void)face;
    (void)mode;
}

void gl_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    (void)r;
    (void)g;
    (void)b;
    (void)a;
}

void gl_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    g_gl_state.clear_color[0] = r;
    g_gl_state.clear_color[1] = g;
    g_gl_state.clear_color[2] = b;
    g_gl_state.clear_color[3] = a;
}

void gl_clear_depth(GLdouble depth)
{
    g_gl_state.clear_depth = depth;
}

void gl_clear(GLbitfield mask)
{
    gl_clear_buffers(mask);
}

void gl_viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    g_gl_state.viewport[0] = x;
    g_gl_state.viewport[1] = y;
    g_gl_state.viewport[2] = width;
    g_gl_state.viewport[3] = height;
}

void gl_polygon_offset(GLfloat factor, GLfloat units)
{
    g_gl_state.polygon_offset_factor = factor;
    g_gl_state.polygon_offset_units = units;
}

void gl_read_pixels(GLint x, GLint y, GLsizei width, GLsizei height,
                    GLenum format, GLenum type, GLvoid *data)
{
    gl_framebuffer_t *fb = g_gl_framebuffer;
    if (!fb || !fb->color_buffer) return;

    unsigned int *dst = (unsigned int *)data;

    for (int j = 0; j < height; j++) {
        int src_y = (fb->height - 1) - (y + j);
        for (int i = 0; i < width; i++) {
            int src_x = x + i;
            if (src_x < 0 || src_x >= fb->width || src_y < 0 || src_y >= fb->height) {
                dst[j * width + i] = 0;
                continue;
            }
            unsigned int raw = fb->color_buffer[src_y * fb->stride + src_x];
            float r = ((raw >> 0)  & 0xFF) / 255.0f;
            float g = ((raw >> 8)  & 0xFF) / 255.0f;
            float b = ((raw >> 16) & 0xFF) / 255.0f;
            float a = ((raw >> 24) & 0xFF) / 255.0f;

            if (format == 0x1908) { /* GL_RGBA */
                if (type == GL_UNSIGNED_BYTE) {
                    unsigned char *p = (unsigned char *)&dst[j * width + i];
                    p[0] = (unsigned char)(r * 255.0f);
                    p[1] = (unsigned char)(g * 255.0f);
                    p[2] = (unsigned char)(b * 255.0f);
                    p[3] = (unsigned char)(a * 255.0f);
                } else if (type == GL_FLOAT) {
                    float *p = (float *)&dst[j * width + i];
                    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
                }
            } else if (format == 0x1907) { /* GL_RGB */
                if (type == GL_UNSIGNED_BYTE) {
                    unsigned char *p = (unsigned char *)&dst[j * width + i];
                    p[0] = (unsigned char)(r * 255.0f);
                    p[1] = (unsigned char)(g * 255.0f);
                    p[2] = (unsigned char)(b * 255.0f);
                } else if (type == GL_FLOAT) {
                    float *p = (float *)&dst[j * width + i];
                    p[0] = r; p[1] = g; p[2] = b;
                }
            } else if (format == 0x1903) { /* GL_RED */
                if (type == GL_UNSIGNED_BYTE) {
                    unsigned char *p = (unsigned char *)&dst[j * width + i];
                    p[0] = (unsigned char)(r * 255.0f);
                } else if (type == GL_FLOAT) {
                    float *p = (float *)&dst[j * width + i];
                    p[0] = r;
                }
            }
        }
    }
}

void gl_flush(void)
{
}

void gl_finish(void)
{
}

void gl_fogf(GLenum pname, GLfloat param)
{
    switch (pname) {
    case 0x0B61: g_gl_state.fog_mode = (GLenum)param;    break; /* GL_FOG_MODE */
    case 0x0B62: g_gl_state.fog_density = param;          break; /* GL_FOG_DENSITY */
    case 0x0B63: g_gl_state.fog_start = param;            break; /* GL_FOG_START */
    case 0x0B64: g_gl_state.fog_end = param;              break; /* GL_FOG_END */
    }
}

void gl_fogfv(GLenum pname, const GLfloat *params)
{
    if (pname == 0x0B66) { /* GL_FOG_COLOR */
        g_gl_state.fog_color[0] = params[0];
        g_gl_state.fog_color[1] = params[1];
        g_gl_state.fog_color[2] = params[2];
        g_gl_state.fog_color[3] = params[3];
    } else {
        gl_fogf(pname, params[0]);
    }
}

void gl_alpha_func(GLenum func, GLclampf ref)
{
    g_gl_state.alpha_func = func;
    g_gl_state.alpha_ref = ref;
}

void gl_logic_op(GLenum opcode)
{
    g_gl_state.logic_op = opcode;
}
