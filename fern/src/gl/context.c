#include <string.h>
#include "context.h"
#include "os.h"
#include "../include/debuglog.h"

gl_context_t g_context_pool[GL_CONTEXT_POOL_SIZE];
static gl_context_t *g_current_context = NULL;
static int g_next_context_id = 1;

extern gl_state_t g_gl_state;
extern gl_framebuffer_t *g_gl_framebuffer;

void gl_context_init(void) {
    memset(g_context_pool, 0, sizeof(g_context_pool));
    for (int i = 0; i < GL_CONTEXT_POOL_SIZE; i++) {
        g_context_pool[i].id = -1;
    }
    g_current_context = NULL;
    g_next_context_id = 1;
    debuglog(DEBUG_INFO, "[GL_CTX] init pool=%d\n", GL_CONTEXT_POOL_SIZE);
}

gl_context_t* gl_create_context(const gl_context_attribs_t *attribs) {
    gl_context_t *ctx = NULL;

    for (int i = 0; i < GL_CONTEXT_POOL_SIZE; i++) {
        if (g_context_pool[i].id == -1) {
            ctx = &g_context_pool[i];
            break;
        }
    }

    if (!ctx) {
        debuglog(DEBUG_ERROR, "[GL_CTX] pool exhausted\n");
        return NULL;
    }

    memset(&ctx->state, 0, sizeof(gl_state_t));
    memset(&ctx->framebuffer, 0, sizeof(gl_framebuffer_t));

    if (attribs) {
        ctx->attribs = *attribs;
        ctx->width = attribs->width > 0 ? attribs->width : 320;
        ctx->height = attribs->height > 0 ? attribs->height : 200;
    } else {
        memset(&ctx->attribs, 0, sizeof(gl_context_attribs_t));
        ctx->attribs.version = GL_CONTEXT_VERSION_1_1;
        ctx->attribs.profile = GL_CONTEXT_PROFILE_COMPATIBILITY;
        ctx->attribs.red_bits = 8;
        ctx->attribs.green_bits = 8;
        ctx->attribs.blue_bits = 8;
        ctx->attribs.alpha_bits = 8;
        ctx->attribs.depth_bits = 24;
        ctx->attribs.stencil_bits = 8;
        ctx->attribs.double_buffer = 1;
        ctx->width = 320;
        ctx->height = 200;
    }

    ctx->framebuffer.width = ctx->width;
    ctx->framebuffer.height = ctx->height;
    ctx->framebuffer.stride = ctx->width;

    int color_size = ctx->width * ctx->height;
    ctx->framebuffer.color_buffer = (unsigned int *)gl_malloc(color_size * sizeof(unsigned int));
    if (!ctx->framebuffer.color_buffer) {
        debuglog(DEBUG_ERROR, "[GL_CTX] alloc color buffer failed\n");
        ctx->id = -1;
        return NULL;
    }
    memset(ctx->framebuffer.color_buffer, 0, color_size * sizeof(unsigned int));

    if (ctx->attribs.depth_bits > 0) {
        ctx->framebuffer.depth_buffer = (float *)gl_malloc(color_size * sizeof(float));
        if (ctx->framebuffer.depth_buffer) {
            for (int i = 0; i < color_size; i++)
                ctx->framebuffer.depth_buffer[i] = 1.0f;
        }
    }

    if (ctx->attribs.stencil_bits > 0) {
        ctx->framebuffer.stencil_buffer = (unsigned char *)gl_malloc(color_size * sizeof(unsigned char));
        if (ctx->framebuffer.stencil_buffer)
            memset(ctx->framebuffer.stencil_buffer, 0, color_size);
    }

    ctx->id = g_next_context_id++;
    ctx->current = GL_FALSE;

    debuglog(DEBUG_INFO, "[GL_CTX] created id=%d size=%dx%d\n",
             ctx->id, ctx->width, ctx->height);
    return ctx;
}

void gl_destroy_context(gl_context_t *ctx) {
    if (!ctx) return;

    if (ctx->current) {
        gl_make_current(NULL);
    }

    if (ctx->framebuffer.color_buffer) {
        gl_free(ctx->framebuffer.color_buffer);
        ctx->framebuffer.color_buffer = NULL;
    }
    if (ctx->framebuffer.depth_buffer) {
        gl_free(ctx->framebuffer.depth_buffer);
        ctx->framebuffer.depth_buffer = NULL;
    }
    if (ctx->framebuffer.stencil_buffer) {
        gl_free(ctx->framebuffer.stencil_buffer);
        ctx->framebuffer.stencil_buffer = NULL;
    }

    debuglog(DEBUG_INFO, "[GL_CTX] destroyed id=%d\n", ctx->id);
    ctx->id = -1;
    ctx->current = GL_FALSE;
}

void gl_make_current(gl_context_t *ctx) {
    if (g_current_context) {
        g_current_context->current = GL_FALSE;
        memcpy(&g_current_context->state, &g_gl_state, sizeof(gl_state_t));
    }

    g_current_context = ctx;

    if (ctx) {
        ctx->current = GL_TRUE;
        memcpy(&g_gl_state, &ctx->state, sizeof(gl_state_t));
        g_gl_framebuffer = &ctx->framebuffer;
    } else {
        g_gl_framebuffer = NULL;
    }
}

gl_context_t* gl_get_current_context(void) {
    return g_current_context;
}

void gl_context_set_surface_size(gl_context_t *ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) return;
    if (width == ctx->width && height == ctx->height) return;

    int new_size = width * height;

    unsigned int *new_color = (unsigned int *)gl_malloc(new_size * sizeof(unsigned int));
    if (!new_color) return;
    memset(new_color, 0, new_size * sizeof(unsigned int));

    if (ctx->framebuffer.color_buffer) {
        int copy_w = width < ctx->width ? width : ctx->width;
        int copy_h = height < ctx->height ? height : ctx->height;
        for (int y = 0; y < copy_h; y++) {
            memcpy(new_color + y * width,
                   ctx->framebuffer.color_buffer + y * ctx->width,
                   copy_w * sizeof(unsigned int));
        }
        gl_free(ctx->framebuffer.color_buffer);
    }
    ctx->framebuffer.color_buffer = new_color;

    if (ctx->attribs.depth_bits > 0) {
        float *new_depth = (float *)gl_malloc(new_size * sizeof(float));
        if (new_depth) {
            for (int i = 0; i < new_size; i++)
                new_depth[i] = 1.0f;
            if (ctx->framebuffer.depth_buffer) {
                int copy_w = width < ctx->width ? width : ctx->width;
                int copy_h = height < ctx->height ? height : ctx->height;
                for (int y = 0; y < copy_h; y++) {
                    memcpy(new_depth + y * width,
                           ctx->framebuffer.depth_buffer + y * ctx->width,
                           copy_w * sizeof(float));
                }
                gl_free(ctx->framebuffer.depth_buffer);
            }
            ctx->framebuffer.depth_buffer = new_depth;
        }
    }

    if (ctx->attribs.stencil_bits > 0) {
        unsigned char *new_stencil = (unsigned char *)gl_malloc(new_size);
        if (new_stencil) {
            memset(new_stencil, 0, new_size);
            if (ctx->framebuffer.stencil_buffer) {
                int copy_w = width < ctx->width ? width : ctx->width;
                int copy_h = height < ctx->height ? height : ctx->height;
                for (int y = 0; y < copy_h; y++) {
                    memcpy(new_stencil + y * width,
                           ctx->framebuffer.stencil_buffer + y * ctx->width,
                           copy_w);
                }
                gl_free(ctx->framebuffer.stencil_buffer);
            }
            ctx->framebuffer.stencil_buffer = new_stencil;
        }
    }

    ctx->width = width;
    ctx->height = height;
    ctx->framebuffer.width = width;
    ctx->framebuffer.height = height;
    ctx->framebuffer.stride = width;
}

int gl_context_get_id(gl_context_t *ctx) {
    return ctx ? ctx->id : -1;
}
