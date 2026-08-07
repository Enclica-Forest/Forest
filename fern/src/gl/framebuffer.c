#include "framebuffer.h"
#include "../include/types.h"
#include "../include/debuglog.h"
#include "../include/string.h"
#include <stdbool.h>

extern void* gl_malloc(size_t size);
extern void  gl_free(void* ptr);

#define GL_FBO_POOL_SIZE 64
#define GL_RBO_POOL_SIZE 64

typedef struct {
    GLuint name;
    GLenum internalformat;
    GLsizei width;
    GLsizei height;
    GLuint refcount;
    void* data;
} gl_rbo_t;

static gl_fbo_t g_fbo_pool[GL_FBO_POOL_SIZE];
static GLuint g_fbo_next_name = 1;
static gl_fbo_t g_default_fbo;
static gl_fbo_t* g_current_fbo = NULL;

static gl_rbo_t g_rbo_pool[GL_RBO_POOL_SIZE];
static GLuint g_rbo_next_name = 1;

void gl_framebuffer_init(void) {
    memset(g_fbo_pool, 0, sizeof(g_fbo_pool));
    memset(g_rbo_pool, 0, sizeof(g_rbo_pool));

    g_fbo_next_name = 1;
    g_rbo_next_name = 1;

    memset(&g_default_fbo, 0, sizeof(g_default_fbo));
    g_default_fbo.name = 0;

    g_current_fbo = &g_default_fbo;

    debuglog(DEBUG_INFO, "[GL_FBO] init pool=%d rbo_pool=%d\n",
             GL_FBO_POOL_SIZE, GL_RBO_POOL_SIZE);
}

static gl_fbo_t* fbo_find(GLuint name) {
    if (name == 0) return &g_default_fbo;
    for (int i = 0; i < GL_FBO_POOL_SIZE; i++) {
        if (g_fbo_pool[i].name == name) return &g_fbo_pool[i];
    }
    return NULL;
}

static gl_fbo_t* fbo_alloc(void) {
    for (int i = 0; i < GL_FBO_POOL_SIZE; i++) {
        if (g_fbo_pool[i].name == 0) {
            memset(&g_fbo_pool[i], 0, sizeof(gl_fbo_t));
            g_fbo_pool[i].name = g_fbo_next_name++;
            return &g_fbo_pool[i];
        }
    }
    return NULL;
}

static gl_rbo_t* rbo_find(GLuint name) {
    for (int i = 0; i < GL_RBO_POOL_SIZE; i++) {
        if (g_rbo_pool[i].name == name) return &g_rbo_pool[i];
    }
    return NULL;
}

static gl_rbo_t* rbo_alloc(void) {
    for (int i = 0; i < GL_RBO_POOL_SIZE; i++) {
        if (g_rbo_pool[i].name == 0) {
            memset(&g_rbo_pool[i], 0, sizeof(gl_rbo_t));
            g_rbo_pool[i].name = g_rbo_next_name++;
            g_rbo_pool[i].refcount = 1;
            return &g_rbo_pool[i];
        }
    }
    return NULL;
}

static int bytes_per_pixel(GLenum internalformat) {
    switch (internalformat) {
    case GL_RGBA8:            return 4;
    case GL_DEPTH_COMPONENT24: return 4;
    default:                  return 4;
    }
}

GLuint gl_renderbuffer_create(void) {
    gl_rbo_t* rbo = rbo_alloc();
    if (!rbo) {
        debuglog(DEBUG_ERROR, "[GL_RBO] pool exhausted\n");
        return 0;
    }
    return rbo->name;
}

void gl_renderbuffer_delete(GLuint name) {
    gl_rbo_t* rbo = rbo_find(name);
    if (!rbo || rbo->name == 0) return;

    if (rbo->data) {
        gl_free(rbo->data);
        rbo->data = NULL;
    }
    rbo->name = 0;
    rbo->width = 0;
    rbo->height = 0;
    rbo->internalformat = 0;
}

void gl_renderbuffer_storage(GLenum internalformat, GLsizei width, GLsizei height) {
    if (!g_current_fbo) return;

    GLuint rbo_name = 0;
    if (g_current_fbo->color.renderbuffer)
        rbo_name = g_current_fbo->color.renderbuffer;
    else if (g_current_fbo->depth.renderbuffer)
        rbo_name = g_current_fbo->depth.renderbuffer;

    if (!rbo_name) {
        debuglog(DEBUG_WARN, "[GL_RBO] storage called with no renderbuffer attached\n");
        return;
    }

    gl_rbo_t* rbo = rbo_find(rbo_name);
    if (!rbo) return;

    if (rbo->data) {
        gl_free(rbo->data);
        rbo->data = NULL;
    }

    rbo->internalformat = internalformat;
    rbo->width = width;
    rbo->height = height;

    int bpp = bytes_per_pixel(internalformat);
    rbo->data = gl_malloc((size_t)width * (size_t)height * (size_t)bpp);
    if (!rbo->data) {
        debuglog(DEBUG_ERROR, "[GL_RBO] failed to allocate %dx%d fmt=0x%x\n",
                 width, height, internalformat);
        return;
    }
    memset(rbo->data, 0, (size_t)width * (size_t)height * (size_t)bpp);

    if (g_current_fbo->width == 0 || g_current_fbo->height == 0) {
        g_current_fbo->width = (GLuint)width;
        g_current_fbo->height = (GLuint)height;
    }

    debuglog(DEBUG_DETAIL, "[GL_RBO] storage %ux%u fmt=0x%x rbo=%u\n",
             width, height, internalformat, rbo_name);
}

GLuint gl_framebuffer_create(void) {
    gl_fbo_t* fbo = fbo_alloc();
    if (!fbo) {
        debuglog(DEBUG_ERROR, "[GL_FBO] pool exhausted\n");
        return 0;
    }
    return fbo->name;
}

void gl_framebuffer_delete(GLuint name) {
    gl_fbo_t* fbo = fbo_find(name);
    if (!fbo || fbo->name == 0) return;

    if (fbo->color.renderbuffer) {
        gl_rbo_t* rbo = rbo_find(fbo->color.renderbuffer);
        if (rbo && rbo->data) {
            gl_free(rbo->data);
            rbo->data = NULL;
        }
        if (rbo) rbo->name = 0;
        fbo->color.renderbuffer = 0;
    }
    fbo->color.texture = 0;

    if (fbo->depth.renderbuffer) {
        gl_rbo_t* rbo = rbo_find(fbo->depth.renderbuffer);
        if (rbo && rbo->data) {
            gl_free(rbo->data);
            rbo->data = NULL;
        }
        if (rbo) rbo->name = 0;
        fbo->depth.renderbuffer = 0;
    }
    fbo->depth.texture = 0;

    if (fbo->color_buffer) { gl_free(fbo->color_buffer); fbo->color_buffer = NULL; }
    if (fbo->depth_buffer) { gl_free(fbo->depth_buffer); fbo->depth_buffer = NULL; }
    if (fbo->stencil_buffer) { gl_free(fbo->stencil_buffer); fbo->stencil_buffer = NULL; }

    fbo->name = 0;
    fbo->width = 0;
    fbo->height = 0;
    fbo->stride = 0;

    if (g_current_fbo == fbo) {
        g_current_fbo = &g_default_fbo;
    }
}

void gl_framebuffer_bind(GLuint name) {
    gl_fbo_t* fbo = fbo_find(name);
    if (!fbo) {
        debuglog(DEBUG_WARN, "[GL_FBO] bind unknown name=%u\n", name);
        return;
    }
    g_current_fbo = fbo;
}

void gl_framebuffer_texture2d(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    (void)target;
    (void)textarget;
    (void)level;

    if (!g_current_fbo) return;

    if (attachment == GL_COLOR_ATTACHMENT0) {
        g_current_fbo->color.texture = texture;
        if (texture) {
            g_current_fbo->color.renderbuffer = 0;
        }
    } else if (attachment == GL_DEPTH_ATTACHMENT) {
        g_current_fbo->depth.texture = texture;
        if (texture) {
            g_current_fbo->depth.renderbuffer = 0;
        }
    }
}

void gl_framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    (void)target;
    (void)renderbuffertarget;

    if (!g_current_fbo) return;

    if (attachment == GL_COLOR_ATTACHMENT0) {
        g_current_fbo->color.renderbuffer = renderbuffer;
        if (renderbuffer) {
            g_current_fbo->color.texture = 0;
        }
    } else if (attachment == GL_DEPTH_ATTACHMENT) {
        g_current_fbo->depth.renderbuffer = renderbuffer;
        if (renderbuffer) {
            g_current_fbo->depth.texture = 0;
        }
    }
}

GLenum gl_framebuffer_check_status(void) {
    if (!g_current_fbo) return GL_FRAMEBUFFER_UNDEFINED;

    if (g_current_fbo->name == 0) return GL_FRAMEBUFFER_COMPLETE;

    bool has_color = (g_current_fbo->color.texture != 0) ||
                     (g_current_fbo->color.renderbuffer != 0);
    if (!has_color) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;

    GLuint w = g_current_fbo->width;
    GLuint h = g_current_fbo->height;

    if (g_current_fbo->color.renderbuffer) {
        gl_rbo_t* rbo = rbo_find(g_current_fbo->color.renderbuffer);
        if (!rbo) return GL_FRAMEBUFFER_UNSUPPORTED;
        if (rbo->width == 0 || rbo->height == 0) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (w == 0) w = rbo->width;
        if (h == 0) h = rbo->height;
    }

    if (g_current_fbo->depth.renderbuffer) {
        gl_rbo_t* rbo = rbo_find(g_current_fbo->depth.renderbuffer);
        if (!rbo) return GL_FRAMEBUFFER_UNSUPPORTED;
        if (rbo->width == 0 || rbo->height == 0) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (w == 0) w = rbo->width;
        if (h == 0) h = rbo->height;
    }

    if (w == 0 || h == 0) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;

    g_current_fbo->width = w;
    g_current_fbo->height = h;

    return GL_FRAMEBUFFER_COMPLETE;
}

gl_fbo_t* gl_framebuffer_get_current(void) {
    return g_current_fbo;
}

static void fbo_ensure_buffers(gl_fbo_t* fbo) {
    if (!fbo || fbo->name != 0) return;
    if (fbo->color_buffer) return;

    GLuint w = fbo->width;
    GLuint h = fbo->height;
    if (w == 0 || h == 0) return;

    fbo->stride = (int)w;

    size_t color_sz = (size_t)fbo->stride * h * sizeof(unsigned int);
    size_t depth_sz = (size_t)fbo->stride * h * sizeof(float);
    size_t stencil_sz = (size_t)fbo->stride * h * sizeof(unsigned char);

    fbo->color_buffer = (unsigned int*)gl_malloc(color_sz);
    fbo->depth_buffer = (float*)gl_malloc(depth_sz);
    fbo->stencil_buffer = (unsigned char*)gl_malloc(stencil_sz);

    if (!fbo->color_buffer || !fbo->depth_buffer || !fbo->stencil_buffer) {
        debuglog(DEBUG_ERROR, "[GL_FBO] failed to allocate raster buffers %ux%u\n", w, h);
        if (fbo->color_buffer) { gl_free(fbo->color_buffer); fbo->color_buffer = NULL; }
        if (fbo->depth_buffer) { gl_free(fbo->depth_buffer); fbo->depth_buffer = NULL; }
        if (fbo->stencil_buffer) { gl_free(fbo->stencil_buffer); fbo->stencil_buffer = NULL; }
        return;
    }

    memset(fbo->color_buffer, 0, color_sz);
    for (size_t i = 0; i < (size_t)fbo->stride * h; i++)
        fbo->depth_buffer[i] = 1.0f;
    memset(fbo->stencil_buffer, 0, stencil_sz);

    debuglog(DEBUG_DETAIL, "[GL_FBO] allocated raster buffers %ux%u stride=%d\n", w, h, fbo->stride);
}

gl_framebuffer_t* gl_resolve_framebuffer(void) {
    gl_fbo_t* fbo = g_current_fbo;
    if (!fbo || fbo->name != 0) return NULL;

    fbo_ensure_buffers(fbo);
    if (!fbo->color_buffer) return NULL;

    static gl_framebuffer_t resolved;
    resolved.color_buffer  = fbo->color_buffer;
    resolved.depth_buffer  = fbo->depth_buffer;
    resolved.stencil_buffer = fbo->stencil_buffer;
    resolved.width  = (int)fbo->width;
    resolved.height = (int)fbo->height;
    resolved.stride = fbo->stride;
    return &resolved;
}

void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter) {
    (void)filter;

    gl_fbo_t* src_fbo = g_current_fbo;
    gl_fbo_t* dst_fbo = &g_default_fbo;

    if (!src_fbo || !src_fbo->color_buffer) return;
    if (!dst_fbo->color_buffer) return;

    int src_w = srcX1 - srcX0;
    int src_h = srcY1 - srcY0;
    int dst_w = dstX1 - dstX0;
    int dst_h = dstY1 - dstY0;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    int flip_x = (dstX0 > dstX1) ? -1 : 1;
    int flip_y = (dstY0 > dstY1) ? -1 : 1;
    if (src_w < 0) { src_w = -src_w; flip_x = -flip_x; }
    if (src_h < 0) { src_h = -src_h; flip_y = -flip_y; }
    if (dst_w < 0) dst_w = -dst_w;
    if (dst_h < 0) dst_h = -dst_h;

    if (srcX0 < 0) srcX0 = 0;
    if (srcY0 < 0) srcY0 = 0;

    if (mask & GL_COLOR_BUFFER_BIT) {
        for (int y = 0; y < dst_h; y++) {
            int sy = srcY0 + (int)((long)y * src_h / dst_h);
            int dy = dstY0 + y * flip_y;
            if (sy < 0 || sy >= (int)src_fbo->height) continue;
            if (dy < 0 || dy >= (int)dst_fbo->height) continue;

            unsigned int* src_row = src_fbo->color_buffer + sy * src_fbo->stride;
            unsigned int* dst_row = dst_fbo->color_buffer + dy * dst_fbo->stride;

            for (int x = 0; x < dst_w; x++) {
                int sx = srcX0 + (int)((long)x * src_w / dst_w);
                int dx = dstX0 + x * flip_x;
                if (sx < 0 || sx >= (int)src_fbo->width) continue;
                if (dx < 0 || dx >= (int)dst_fbo->width) continue;
                dst_row[dx] = src_row[sx];
            }
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        for (int y = 0; y < dst_h; y++) {
            int sy = srcY0 + (int)((long)y * src_h / dst_h);
            int dy = dstY0 + y * flip_y;
            if (sy < 0 || sy >= (int)src_fbo->height) continue;
            if (dy < 0 || dy >= (int)dst_fbo->height) continue;

            float* src_row = src_fbo->depth_buffer + sy * src_fbo->stride;
            float* dst_row = dst_fbo->depth_buffer + dy * dst_fbo->stride;

            for (int x = 0; x < dst_w; x++) {
                int sx = srcX0 + (int)((long)x * src_w / dst_w);
                int dx = dstX0 + x * flip_x;
                if (sx < 0 || sx >= (int)src_fbo->width) continue;
                if (dx < 0 || dx >= (int)dst_fbo->width) continue;
                dst_row[dx] = src_row[sx];
            }
        }
    }

    if (mask & GL_STENCIL_BUFFER_BIT) {
        for (int y = 0; y < dst_h; y++) {
            int sy = srcY0 + (int)((long)y * src_h / dst_h);
            int dy = dstY0 + y * flip_y;
            if (sy < 0 || sy >= (int)src_fbo->height) continue;
            if (dy < 0 || dy >= (int)dst_fbo->height) continue;

            unsigned char* src_row = src_fbo->stencil_buffer + sy * src_fbo->stride;
            unsigned char* dst_row = dst_fbo->stencil_buffer + dy * dst_fbo->stride;

            for (int x = 0; x < dst_w; x++) {
                int sx = srcX0 + (int)((long)x * src_w / dst_w);
                int dx = dstX0 + x * flip_x;
                if (sx < 0 || sx >= (int)src_fbo->width) continue;
                if (dx < 0 || dx >= (int)dst_fbo->width) continue;
                dst_row[dx] = src_row[sx];
            }
        }
    }
}
