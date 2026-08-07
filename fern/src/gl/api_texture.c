#include "api_texture.h"
#include "texture.h"
#include "displaylist.h"

void gl_gen_textures(GLsizei n, GLuint *textures)
{
    for (GLsizei i = 0; i < n; i++)
        textures[i] = gl_texture_create();
}

void gl_delete_textures(GLsizei n, const GLuint *textures)
{
    for (GLsizei i = 0; i < n; i++)
        gl_texture_delete(textures[i]);
}

void gl_bind_texture(GLenum target, GLuint texture)
{
    (void)target;
    if (gl_dl_is_recording()) {
        gl_dl_record_bind_texture(texture);
        return;
    }
    gl_texture_bind(GL_TEXTURE_2D, texture);
}

void gl_active_texture(GLenum texture)
{
    gl_texture_activate(texture);
}

void gl_tex_image_2d(GLenum target, GLint level, GLint internalformat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)target;
    gl_texture_image2d(GL_TEXTURE_2D, level, internalformat,
                       width, height, border, format, type, pixels);
}

void gl_tex_sub_image_2d(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLsizei width, GLsizei height,
                         GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)target;
    gl_texture_sub_image2d(GL_TEXTURE_2D, level, xoffset, yoffset,
                           width, height, format, type, pixels);
}

void gl_tex_parameteri(GLenum target, GLenum pname, GLint param)
{
    (void)target;
    gl_texture_parameteri(GL_TEXTURE_2D, pname, param);
}

void gl_generate_mipmap(GLenum target)
{
    (void)target;
    GLuint tex = gl_texture_get_bound();
    if (tex)
        gl_texture_generate_mipmaps(tex);
}

void gl_multi_tex_coord2f(GLenum target, GLfloat s, GLfloat t)
{
    unsigned int idx = target - GL_TEXTURE0;
    if (idx >= 8) idx = 0;
    g_gl_state.multi_texcoord[idx][0] = s;
    g_gl_state.multi_texcoord[idx][1] = t;
}

void gl_tex_envi(GLenum target, GLenum pname, GLint param)
{
    if (target != GL_TEXTURE_ENV) return;
    unsigned int idx = g_gl_state.active_texture - GL_TEXTURE0;
    if (idx >= 8) idx = 0;
    if (pname == GL_TEXTURE_ENV_MODE) {
        g_gl_state.tex_env_mode[idx] = (GLenum)param;
    }
}

void gl_tex_envfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (target != GL_TEXTURE_ENV) return;
    unsigned int idx = g_gl_state.active_texture - GL_TEXTURE0;
    if (idx >= 8) idx = 0;
    if (pname == GL_TEXTURE_ENV_COLOR) {
        g_gl_state.tex_env_color[idx][0] = params[0];
        g_gl_state.tex_env_color[idx][1] = params[1];
        g_gl_state.tex_env_color[idx][2] = params[2];
        g_gl_state.tex_env_color[idx][3] = params[3];
    }
}
