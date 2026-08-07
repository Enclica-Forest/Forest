#ifndef GL_API_BUFFER_H
#define GL_API_BUFFER_H

#include "buffer.h"

#define GL_READ_ONLY  0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_READ_WRITE 0x88BA

void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data);
void *glMapBuffer(GLenum target, GLenum access);
GLboolean glUnmapBuffer(GLenum target);

GLubyte *gl_buffer_get_data(GLuint name);
GLsizeiptr gl_buffer_get_size(GLuint name);

#endif
