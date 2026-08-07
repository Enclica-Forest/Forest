#include "api_matrix.h"
#include "displaylist.h"
#include <math.h>
#include <string.h>

static void mat4_copy(mat4_t *dst, const mat4_t *src)
{
    memcpy(dst, src, sizeof(mat4_t));
}

static void mat4_mul(mat4_t *result, const mat4_t *a, const mat4_t *b)
{
    mat4_t tmp;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a->m[k * 4 + row] * b->m[c * 4 + k];
            }
            tmp.m[c * 4 + row] = sum;
        }
    }
    *result = tmp;
}

void glMatrixMode(GLenum mode)
{
    switch (mode) {
    case GL_MODELVIEW:
        g_gl_state.matrix_mode_ptr = &g_gl_state.modelview_matrix;
        break;
    case GL_PROJECTION:
        g_gl_state.matrix_mode_ptr = &g_gl_state.projection_matrix;
        break;
    case GL_TEXTURE:
        g_gl_state.matrix_mode_ptr =
            &g_gl_state.texture_matrix[g_gl_state.active_texture - GL_TEXTURE0];
        break;
    }
}

void glLoadIdentity(void)
{
    memset(g_gl_state.matrix_mode_ptr, 0, sizeof(mat4_t));
    g_gl_state.matrix_mode_ptr->m[0]  = 1.0f;
    g_gl_state.matrix_mode_ptr->m[5]  = 1.0f;
    g_gl_state.matrix_mode_ptr->m[10] = 1.0f;
    g_gl_state.matrix_mode_ptr->m[15] = 1.0f;
}

void glLoadMatrixf(const GLfloat *m)
{
    if (gl_dl_is_recording()) {
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_load_matrix(mode, m);
        return;
    }
    for (int i = 0; i < 16; i++)
        g_gl_state.matrix_mode_ptr->m[i] = m[i];
}

void glLoadMatrixd(const GLdouble *m)
{
    if (gl_dl_is_recording()) {
        GLfloat fm[16];
        for (int i = 0; i < 16; i++)
            fm[i] = (GLfloat)m[i];
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_load_matrix(mode, fm);
        return;
    }
    for (int i = 0; i < 16; i++)
        g_gl_state.matrix_mode_ptr->m[i] = (GLfloat)m[i];
}

void glMultMatrixf(const GLfloat *m)
{
    if (gl_dl_is_recording()) {
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_mult_matrix(mode, m);
        return;
    }
    mat4_t src;
    for (int i = 0; i < 16; i++)
        src.m[i] = m[i];
    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &src);
}

void glMultMatrixd(const GLdouble *m)
{
    if (gl_dl_is_recording()) {
        GLfloat fm[16];
        for (int i = 0; i < 16; i++)
            fm[i] = (GLfloat)m[i];
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_mult_matrix(mode, fm);
        return;
    }
    mat4_t src;
    for (int i = 0; i < 16; i++)
        src.m[i] = (GLfloat)m[i];
    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &src);
}

void glPushMatrix(void)
{
    GLenum mode = GL_MODELVIEW;
    if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
        mode = GL_PROJECTION;
    else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
        mode = GL_TEXTURE;

    switch (mode) {
    case GL_MODELVIEW:
        if (g_gl_state.modelview_stack_top < GL_STACK_MAX_DEPTH - 1) {
            mat4_copy(&g_gl_state.modelview_stack[g_gl_state.modelview_stack_top],
                       &g_gl_state.modelview_matrix);
            g_gl_state.modelview_stack_top++;
        }
        break;
    case GL_PROJECTION:
        if (g_gl_state.projection_stack_top < GL_STACK_MAX_DEPTH - 1) {
            mat4_copy(&g_gl_state.projection_stack[g_gl_state.projection_stack_top],
                       &g_gl_state.projection_matrix);
            g_gl_state.projection_stack_top++;
        }
        break;
    case GL_TEXTURE: {
        int tex_idx = g_gl_state.active_texture - GL_TEXTURE0;
        if (g_gl_state.texture_stack_top[tex_idx] < GL_STACK_MAX_DEPTH - 1) {
            mat4_copy(&g_gl_state.texture_stack[tex_idx][g_gl_state.texture_stack_top[tex_idx]],
                       &g_gl_state.texture_matrix[tex_idx]);
            g_gl_state.texture_stack_top[tex_idx]++;
        }
        break;
    }
    }
}

void glPopMatrix(void)
{
    GLenum mode = GL_MODELVIEW;
    if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
        mode = GL_PROJECTION;
    else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
        mode = GL_TEXTURE;

    switch (mode) {
    case GL_MODELVIEW:
        if (g_gl_state.modelview_stack_top > 0) {
            g_gl_state.modelview_stack_top--;
            mat4_copy(&g_gl_state.modelview_matrix,
                       &g_gl_state.modelview_stack[g_gl_state.modelview_stack_top]);
        }
        break;
    case GL_PROJECTION:
        if (g_gl_state.projection_stack_top > 0) {
            g_gl_state.projection_stack_top--;
            mat4_copy(&g_gl_state.projection_matrix,
                       &g_gl_state.projection_stack[g_gl_state.projection_stack_top]);
        }
        break;
    case GL_TEXTURE: {
        int tex_idx = g_gl_state.active_texture - GL_TEXTURE0;
        if (g_gl_state.texture_stack_top[tex_idx] > 0) {
            g_gl_state.texture_stack_top[tex_idx]--;
            mat4_copy(&g_gl_state.texture_matrix[tex_idx],
                       &g_gl_state.texture_stack[tex_idx][g_gl_state.texture_stack_top[tex_idx]]);
        }
        break;
    }
    }
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    if (gl_dl_is_recording()) {
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_translate(mode, x, y, z);
        return;
    }
    mat4_t t;
    memset(&t, 0, sizeof(mat4_t));
    t.m[0]  = 1.0f;
    t.m[5]  = 1.0f;
    t.m[10] = 1.0f;
    t.m[15] = 1.0f;
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &t);
}

void glScaled(GLdouble x, GLdouble y, GLdouble z)
{
    if (gl_dl_is_recording()) {
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_scale(mode, (GLfloat)x, (GLfloat)y, (GLfloat)z);
        return;
    }
    mat4_t s;
    memset(&s, 0, sizeof(mat4_t));
    s.m[0]  = (GLfloat)x;
    s.m[5]  = (GLfloat)y;
    s.m[10] = (GLfloat)z;
    s.m[15] = 1.0f;
    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &s);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    if (gl_dl_is_recording()) {
        GLenum mode = GL_MODELVIEW;
        if (g_gl_state.matrix_mode_ptr == &g_gl_state.projection_matrix)
            mode = GL_PROJECTION;
        else if (g_gl_state.matrix_mode_ptr != &g_gl_state.modelview_matrix)
            mode = GL_TEXTURE;
        gl_dl_record_rotate(mode, angle, x, y, z);
        return;
    }
    GLfloat rad = angle * 3.14159265358979323846f / 180.0f;
    GLfloat c = cosf(rad);
    GLfloat s = sinf(rad);
    GLfloat len = sqrtf(x * x + y * y + z * z);

    if (len < 1e-6f)
        return;

    x /= len;
    y /= len;
    z /= len;

    mat4_t r;
    memset(&r, 0, sizeof(mat4_t));
    r.m[15] = 1.0f;

    r.m[0]  = x * x * (1.0f - c) + c;
    r.m[1]  = x * y * (1.0f - c) + z * s;
    r.m[2]  = x * z * (1.0f - c) - y * s;

    r.m[4]  = y * x * (1.0f - c) - z * s;
    r.m[5]  = y * y * (1.0f - c) + c;
    r.m[6]  = y * z * (1.0f - c) + x * s;

    r.m[8]  = z * x * (1.0f - c) + y * s;
    r.m[9]  = z * y * (1.0f - c) - x * s;
    r.m[10] = z * z * (1.0f - c) + c;

    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &r);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val)
{
    mat4_t m;
    memset(&m, 0, sizeof(mat4_t));

    m.m[0]  = (GLfloat)(2.0 / (right - left));
    m.m[5]  = (GLfloat)(2.0 / (top - bottom));
    m.m[10] = (GLfloat)(-2.0 / (far_val - near_val));
    m.m[12] = (GLfloat)(-(right + left) / (right - left));
    m.m[13] = (GLfloat)(-(top + bottom) / (top - bottom));
    m.m[14] = (GLfloat)(-(far_val + near_val) / (far_val - near_val));
    m.m[15] = 1.0f;

    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &m);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val)
{
    mat4_t m;
    memset(&m, 0, sizeof(mat4_t));

    m.m[0]  = (GLfloat)(2.0 * near_val / (right - left));
    m.m[2]  = (GLfloat)((right + left) / (right - left));
    m.m[5]  = (GLfloat)(2.0 * near_val / (top - bottom));
    m.m[6]  = (GLfloat)((top + bottom) / (top - bottom));
    m.m[10] = (GLfloat)(-(far_val + near_val) / (far_val - near_val));
    m.m[11] = (GLfloat)(-2.0 * far_val * near_val / (far_val - near_val));
    m.m[14] = -1.0f;

    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &m);
}

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar)
{
    GLdouble fh = tan(fovy * 3.14159265358979323846 / 360.0);
    GLdouble w = fh * aspect;
    glFrustum(-w, w, -fh, fh, zNear, zFar);
}

void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ)
{
    GLdouble fx = centerX - eyeX;
    GLdouble fy = centerY - eyeY;
    GLdouble fz = centerZ - eyeZ;
    GLdouble flen = sqrt(fx * fx + fy * fy + fz * fz);
    fx /= flen;
    fy /= flen;
    fz /= flen;

    GLdouble sx = fy * upZ - fz * upY;
    GLdouble sy = fz * upX - fx * upZ;
    GLdouble sz = fx * upY - fy * upX;
    GLdouble slen = sqrt(sx * sx + sy * sy + sz * sz);
    sx /= slen;
    sy /= slen;
    sz /= slen;

    GLdouble ux = sy * fz - sz * fy;
    GLdouble uy = sz * fx - sx * fz;
    GLdouble uz = sx * fy - sy * fx;

    mat4_t m;
    memset(&m, 0, sizeof(mat4_t));
    m.m[0]  = (GLfloat)sx;
    m.m[1]  = (GLfloat)sy;
    m.m[2]  = (GLfloat)sz;
    m.m[4]  = (GLfloat)ux;
    m.m[5]  = (GLfloat)uy;
    m.m[6]  = (GLfloat)uz;
    m.m[8]  = (GLfloat)-fx;
    m.m[9]  = (GLfloat)-fy;
    m.m[10] = (GLfloat)-fz;
    m.m[15] = 1.0f;

    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &m);

    mat4_t t;
    memset(&t, 0, sizeof(mat4_t));
    t.m[0]  = 1.0f;
    t.m[5]  = 1.0f;
    t.m[10] = 1.0f;
    t.m[15] = 1.0f;
    t.m[12] = (GLfloat)-eyeX;
    t.m[13] = (GLfloat)-eyeY;
    t.m[14] = (GLfloat)-eyeZ;
    mat4_mul(g_gl_state.matrix_mode_ptr, g_gl_state.matrix_mode_ptr, &t);
}
