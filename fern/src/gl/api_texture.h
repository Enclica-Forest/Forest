#ifndef GL_API_TEXTURE_H
#define GL_API_TEXTURE_H

#include "state.h"

void gl_gen_textures(GLsizei n, GLuint *textures);
void gl_delete_textures(GLsizei n, const GLuint *textures);
void gl_bind_texture(GLenum target, GLuint texture);
void gl_active_texture(GLenum texture);
void gl_tex_image_2d(GLenum target, GLint level, GLint internalformat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *pixels);
void gl_tex_sub_image_2d(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLsizei width, GLsizei height,
                         GLenum format, GLenum type, const GLvoid *pixels);
void gl_tex_parameteri(GLenum target, GLenum pname, GLint param);
void gl_generate_mipmap(GLenum target);
void gl_multi_tex_coord2f(GLenum target, GLfloat s, GLfloat t);
void gl_tex_envi(GLenum target, GLenum pname, GLint param);
void gl_tex_envfv(GLenum target, GLenum pname, const GLfloat *params);

#endif
