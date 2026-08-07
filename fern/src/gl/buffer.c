#include "buffer.h"
#include "../include/debuglog.h"
#include "../include/string.h"
#include "../include/memory.h"

static gl_buffer_t g_buffer_pool[GL_MAX_BUFFERS];
static GLuint g_buffer_next_name = 1;
static gl_attrib_pointer_t g_attribs[GL_MAX_VERTEX_ATTRIBS];

void gl_buffer_init(void)
{
    memset(g_buffer_pool, 0, sizeof(g_buffer_pool));
    memset(g_attribs, 0, sizeof(g_attribs));
    g_buffer_next_name = 1;
    g_gl_state.bound_array_buffer = 0;
    g_gl_state.bound_element_buffer = 0;
}

static gl_buffer_t *buffer_find(GLuint name)
{
    for (int i = 0; i < GL_MAX_BUFFERS; i++) {
        if (g_buffer_pool[i].used && g_buffer_pool[i].name == name)
            return &g_buffer_pool[i];
    }
    return NULL;
}

static gl_buffer_t *buffer_alloc(void)
{
    for (int i = 0; i < GL_MAX_BUFFERS; i++) {
        if (!g_buffer_pool[i].used) {
            g_buffer_pool[i].used = GL_TRUE;
            g_buffer_pool[i].name = g_buffer_next_name++;
            g_buffer_pool[i].data = NULL;
            g_buffer_pool[i].size = 0;
            g_buffer_pool[i].target = 0;
            g_buffer_pool[i].usage = 0;
            g_buffer_pool[i].mapped = GL_FALSE;
            return &g_buffer_pool[i];
        }
    }
    return NULL;
}

static gl_buffer_t *buffer_for_target(GLenum target)
{
    GLuint name = 0;
    if (target == GL_ARRAY_BUFFER)
        name = g_gl_state.bound_array_buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        name = g_gl_state.bound_element_buffer;
    if (name == 0) return NULL;
    return buffer_find(name);
}

gl_buffer_t* gl_buffer_get(GLuint name)
{
    return buffer_find(name);
}

GLuint gl_buffer_create(void)
{
    gl_buffer_t *buf = buffer_alloc();
    if (!buf) {
        debuglog(DEBUG_ERROR, "[GL_BUF] pool exhausted\n");
        return 0;
    }
    return buf->name;
}

void gl_buffer_delete(GLuint name)
{
    gl_buffer_t *buf = buffer_find(name);
    if (!buf) return;
    if (buf->data) {
        kfree(buf->data);
        buf->data = NULL;
    }
    if (g_gl_state.bound_array_buffer == name)
        g_gl_state.bound_array_buffer = 0;
    if (g_gl_state.bound_element_buffer == name)
        g_gl_state.bound_element_buffer = 0;
    buf->used = GL_FALSE;
    buf->name = 0;
}

void gl_buffer_bind(GLenum target, GLuint name)
{
    if (target == GL_ARRAY_BUFFER) {
        g_gl_state.bound_array_buffer = name;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        g_gl_state.bound_element_buffer = name;
    }
}

void gl_buffer_data(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    gl_buffer_t *buf = buffer_for_target(target);
    if (!buf) {
        debuglog(DEBUG_WARN, "[GL_BUF] gl_buffer_data: no buffer bound\n");
        return;
    }

    if (buf->data) {
        kfree(buf->data);
        buf->data = NULL;
    }

    buf->size = size;
    buf->usage = usage;
    buf->target = target;

    if (size > 0) {
        buf->data = (GLubyte *)kmalloc((size_t)size);
        if (!buf->data) {
            debuglog(DEBUG_ERROR, "[GL_BUF] kmalloc failed\n");
            buf->size = 0;
            return;
        }
        if (data)
            memcpy(buf->data, data, (size_t)size);
        else
            memset(buf->data, 0, (size_t)size);
    }
}

void gl_buffer_sub_data(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    gl_buffer_t *buf = buffer_for_target(target);
    if (!buf || !buf->data || !data) return;
    if (offset < 0 || offset + size > buf->size) {
        debuglog(DEBUG_WARN, "[GL_BUF] sub data out of bounds\n");
        return;
    }
    memcpy(buf->data + offset, data, (size_t)size);
}

/* ---- Vertex Attrib Pointer State ---- */

void gl_vertex_attrib_pointer(GLuint index, GLint size, GLenum type,
                              GLboolean normalized, GLsizei stride,
                              const GLvoid *pointer)
{
    if (index >= GL_MAX_VERTEX_ATTRIBS) return;
    gl_attrib_pointer_t *a = &g_attribs[index];
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->pointer = pointer;
    a->normalized = normalized;
    a->buffer = g_gl_state.bound_array_buffer;
}

void gl_enable_vertex_attrib_array(GLuint index)
{
    if (index >= GL_MAX_VERTEX_ATTRIBS) return;
    g_attribs[index].enabled = GL_TRUE;
}

void gl_disable_vertex_attrib_array(GLuint index)
{
    if (index >= GL_MAX_VERTEX_ATTRIBS) return;
    g_attribs[index].enabled = GL_FALSE;
}

gl_attrib_pointer_t *gl_attrib_get(GLuint index)
{
    if (index >= GL_MAX_VERTEX_ATTRIBS) return NULL;
    return &g_attribs[index];
}
