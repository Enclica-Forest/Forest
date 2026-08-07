#ifndef GL_API_VERTEX_ARRAYS_H
#define GL_API_VERTEX_ARRAYS_H

#include "state.h"

void gl_vertex_pointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void gl_color_pointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void gl_normal_pointer(GLenum type, GLsizei stride, const GLvoid *pointer);
void gl_tex_coord_pointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void gl_enable_client_state(GLenum array);
void gl_disable_client_state(GLenum array);
void gl_draw_arrays(GLenum mode, GLint first, GLsizei count);
void gl_draw_elements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

#endif
