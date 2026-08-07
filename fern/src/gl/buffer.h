#ifndef GL_BUFFER_H
#define GL_BUFFER_H

#include "state.h"

#define GL_MAX_BUFFERS 256
#define GL_MAX_VERTEX_ATTRIBS 16

typedef struct {
    GLuint name;
    GLenum target;      // GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER
    GLenum usage;       // GL_STATIC_DRAW, GL_DYNAMIC_DRAW
    GLsizeiptr size;    // Size in bytes
    GLubyte *data;
    GLboolean mapped;
    GLboolean used;
} gl_buffer_t;

// Vertex attrib pointer state
typedef struct {
    GLint size;          // 1, 2, 3, or 4
    GLenum type;         // GL_FLOAT, GL_UNSIGNED_BYTE, etc.
    GLboolean normalized;
    GLsizei stride;
    const GLvoid *pointer;
    GLboolean enabled;
    GLuint buffer;       // Bound VBO name (0 = client pointer)
} gl_attrib_pointer_t;

// Buffer functions
void gl_buffer_init(void);
GLuint gl_buffer_create(void);
void gl_buffer_delete(GLuint name);
void gl_buffer_bind(GLenum target, GLuint name);
void gl_buffer_data(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void gl_buffer_sub_data(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data);
gl_buffer_t* gl_buffer_get(GLuint name);

// Vertex attrib pointer functions
void gl_vertex_attrib_pointer(GLuint index, GLint size, GLenum type,
                              GLboolean normalized, GLsizei stride,
                              const GLvoid *pointer);
void gl_enable_vertex_attrib_array(GLuint index);
void gl_disable_vertex_attrib_array(GLuint index);
gl_attrib_pointer_t* gl_attrib_get(GLuint index);

#endif /* GL_BUFFER_H */
