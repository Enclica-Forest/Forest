#ifndef GL_API_STATE_H
#define GL_API_STATE_H

#include "state.h"

typedef unsigned int GLbitfield;

#define GL_FILL  0x1B02
#define GL_LINE  0x1B01
#define GL_POINT 0x1B00

void gl_enable(GLenum cap);
void gl_disable(GLenum cap);
void gl_blend_func(GLenum sfactor, GLenum dfactor);
void gl_depth_func(GLenum func);
void gl_depth_mask(GLboolean flag);
void gl_cull_face(GLenum mode);
void gl_front_face(GLenum mode);
void gl_scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void gl_stencil_func(GLenum func, GLint ref, GLuint mask);
void gl_stencil_op(GLenum sfail, GLenum dpfail, GLenum dppass);
void gl_stencil_mask(GLuint mask);
void gl_polygon_mode(GLenum face, GLenum mode);
void gl_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void gl_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gl_clear_depth(GLdouble depth);
void gl_clear(GLbitfield mask);
void gl_viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void gl_polygon_offset(GLfloat factor, GLfloat units);
void gl_read_pixels(GLint x, GLint y, GLsizei width, GLsizei height,
                    GLenum format, GLenum type, GLvoid *data);
void gl_flush(void);
void gl_finish(void);
void gl_fogf(GLenum pname, GLfloat param);
void gl_fogfv(GLenum pname, const GLfloat *params);
void gl_alpha_func(GLenum func, GLclampf ref);
void gl_logic_op(GLenum opcode);

#endif
