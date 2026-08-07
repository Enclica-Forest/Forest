#include "buffer.h"
#include "../include/debuglog.h"
#include <stddef.h>

void glGenBuffers(GLsizei n, GLuint *buffers)
{
    if (!buffers || n <= 0) return;
    for (GLsizei i = 0; i < n; i++) {
        buffers[i] = gl_buffer_create();
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    if (!buffers || n <= 0) return;
    for (GLsizei i = 0; i < n; i++) {
        gl_buffer_delete(buffers[i]);
    }
}

void glBindBuffer(GLenum target, GLuint buffer)
{
    gl_buffer_bind(target, buffer);
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    gl_buffer_data(target, size, data, usage);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    gl_buffer_sub_data(target, offset, size, data);
}

void *glMapBuffer(GLenum target, GLenum access)
{
    (void)access;
    gl_buffer_t *buf = gl_buffer_get(
        target == GL_ARRAY_BUFFER ? g_gl_state.bound_array_buffer : g_gl_state.bound_element_buffer
    );
    if (!buf || !buf->data) return NULL;
    buf->mapped = GL_TRUE;
    return buf->data;
}

GLboolean glUnmapBuffer(GLenum target)
{
    gl_buffer_t *buf = gl_buffer_get(
        target == GL_ARRAY_BUFFER ? g_gl_state.bound_array_buffer : g_gl_state.bound_element_buffer
    );
    if (!buf) return GL_FALSE;
    buf->mapped = GL_FALSE;
    return GL_TRUE;
}

GLubyte *gl_buffer_get_data(GLuint name)
{
    gl_buffer_t *buf = gl_buffer_get(name);
    return buf ? buf->data : NULL;
}

GLsizeiptr gl_buffer_get_size(GLuint name)
{
    gl_buffer_t *buf = gl_buffer_get(name);
    return buf ? buf->size : 0;
}
