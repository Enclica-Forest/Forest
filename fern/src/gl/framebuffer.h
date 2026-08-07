#ifndef GL_FRAMEBUFFER_H
#define GL_FRAMEBUFFER_H

#include "../include/types.h"
#include "state.h"
#include "api_state.h"
#include "rasterizer.h"

#define GL_FRAMEBUFFER_COMPLETE             0x8CD5
#define GL_COLOR_ATTACHMENT0                0x8CE0
#define GL_DEPTH_ATTACHMENT                 0x8D00
#define GL_RENDERBUFFER                     0x8D41
#define GL_FRAMEBUFFER                      0x8D40

#define GL_RGBA8                            0x8058
#define GL_DEPTH_COMPONENT24                0x81A6

#define GL_FRAMEBUFFER_UNDEFINED            0x8219
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER 0x8CDB
#define GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER 0x8CDC
#define GL_FRAMEBUFFER_UNSUPPORTED          0x8CDD

#define GL_MAX_COLOR_ATTACHMENTS            0x8CDF

typedef struct gl_attachment {
    GLuint texture;
    GLuint renderbuffer;
} gl_attachment_t;

typedef struct gl_fbo {
    GLuint name;
    GLuint width;
    GLuint height;
    gl_attachment_t color;
    gl_attachment_t depth;
    unsigned int  *color_buffer;
    float         *depth_buffer;
    unsigned char *stencil_buffer;
    int stride;
    struct gl_fbo* next;
} gl_fbo_t;

void gl_framebuffer_init(void);

GLuint gl_renderbuffer_create(void);
void gl_renderbuffer_delete(GLuint name);
void gl_renderbuffer_storage(GLenum internalformat, GLsizei width, GLsizei height);

GLuint gl_framebuffer_create(void);
void gl_framebuffer_delete(GLuint name);
void gl_framebuffer_bind(GLuint name);
void gl_framebuffer_texture2d(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void gl_framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
GLenum gl_framebuffer_check_status(void);

gl_fbo_t* gl_framebuffer_get_current(void);
gl_framebuffer_t* gl_resolve_framebuffer(void);

void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter);

#endif
