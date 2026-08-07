#include "texture.h"
#include "texture_sample.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/memory.h"

static gl_texture_t texture_pool[GL_MAX_TEXTURES];
static GLuint active_texture_unit = 0;
static GLuint bound_textures[GL_MAX_TEXTURES];
static GLuint next_name = 1;

void gl_texture_init(void) {
    memset(texture_pool, 0, sizeof(texture_pool));
    memset(bound_textures, 0, sizeof(bound_textures));
    next_name = 1;
    debuglog(DEBUG_INFO, "GL texture: pool %d slots\n", GL_MAX_TEXTURES);
}

GLuint gl_texture_create(void) {
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (!texture_pool[i].used) {
            memset(&texture_pool[i], 0, sizeof(gl_texture_t));
            texture_pool[i].name = next_name++;
            texture_pool[i].used = GL_TRUE;
            texture_pool[i].min_filter = GL_NEAREST;
            texture_pool[i].mag_filter = GL_NEAREST;
            texture_pool[i].wrap_s = GL_REPEAT;
            texture_pool[i].wrap_t = GL_REPEAT;
            texture_pool[i].mip_levels = 1;
            return texture_pool[i].name;
        }
    }
    debuglog(DEBUG_WARN, "GL texture: pool exhausted\n");
    return 0;
}

void gl_texture_delete(GLuint name) {
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (texture_pool[i].used && texture_pool[i].name == name) {
            if (texture_pool[i].data) {
                kfree(texture_pool[i].data);
            }
            texture_pool[i].used = GL_FALSE;
            texture_pool[i].data = NULL;
            for (int j = 0; j < GL_MAX_TEXTURES; j++) {
                if (bound_textures[j] == name)
                    bound_textures[j] = 0;
            }
            return;
        }
    }
}

void gl_texture_bind(GLenum target, GLuint name) {
    (void)target;
    if (active_texture_unit < GL_MAX_TEXTURES)
        bound_textures[active_texture_unit] = name;
}

static int bytes_per_pixel(GLenum format, GLint internal_format) {
    switch (format) {
        case GL_RGBA:    return 4;
        case GL_RGB:     return 3;
        case GL_LUMINANCE: return 1;
        case GL_ALPHA:   return 1;
        default: break;
    }
    switch (internal_format) {
        case GL_RGBA8:     return 4;
        case GL_RGB8:      return 3;
        case GL_LUMINANCE8: return 1;
        default: break;
    }
    return 4;
}

static gl_texture_t *get_bound(void) {
    if (active_texture_unit >= GL_MAX_TEXTURES) return NULL;
    GLuint name = bound_textures[active_texture_unit];
    if (name == 0) return NULL;
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (texture_pool[i].used && texture_pool[i].name == name)
            return &texture_pool[i];
    }
    return NULL;
}

static void convert_rgb_to_rgba(const uint8_t *src, GLubyte *dst, int count) {
    for (int i = 0; i < count; i++) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
}

static void convert_luminance_to_rgba(const uint8_t *src, GLubyte *dst,
                                      int count) {
    for (int i = 0; i < count; i++) {
        dst[i * 4 + 0] = src[i];
        dst[i * 4 + 1] = src[i];
        dst[i * 4 + 2] = src[i];
        dst[i * 4 + 3] = 255;
    }
}

int gl_texture_bpp(gl_texture_t *tex) {
    (void)tex;
    return 4;
}

void gl_texture_image2d(GLenum target, GLint level, GLint internal_format,
                        GLsizei width, GLsizei height, GLint border,
                        GLenum format, GLenum type, const void *data) {
    (void)target;
    (void)border;
    (void)type;

    gl_texture_t *tex = get_bound();
    if (!tex) return;
    if (width <= 0 || height <= 0) return;
    if (width > GL_MAX_TEXTURE_SIZE || height > GL_MAX_TEXTURE_SIZE) return;
    if (level < 0 || level >= GL_MAX_MIP_LEVELS) return;

    if (level == 0) {
        if (tex->data) {
            kfree(tex->data);
            tex->data = NULL;
        }
        tex->width = width;
        tex->height = height;
        tex->internal_format = internal_format;
        tex->format = format;
        tex->type = type;
    }

    tex->mip_width[level] = width;
    tex->mip_height[level] = height;

    int size = width * height * 4;

    if (level == 0) {
        tex->data = (GLubyte *)kmalloc(size);
        if (!tex->data) return;
        tex->allocated = size;
    }

    if (!data) {
        if (level == 0) memset(tex->data, 0, size);
        return;
    }

    if (level == 0) {
        int bpp_src = bytes_per_pixel(format, internal_format);

        if (bpp_src == 4) {
            memcpy(tex->data, data, size);
        } else if (bpp_src == 3) {
            convert_rgb_to_rgba((const uint8_t *)data, tex->data, width * height);
        } else if (bpp_src == 1) {
            convert_luminance_to_rgba((const uint8_t *)data, tex->data,
                                      width * height);
        } else {
            memset(tex->data, 0, size);
        }
    }
}

void gl_texture_sub_image2d(GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, const void *data) {
    (void)target;
    (void)level;
    (void)type;

    gl_texture_t *tex = get_bound();
    if (!tex || !tex->data || !data) return;
    if (width <= 0 || height <= 0) return;

    int bpp_src = bytes_per_pixel(format, tex->internal_format);

    for (int row = 0; row < height; row++) {
        int dst_y = yoffset + row;
        if (dst_y < 0 || dst_y >= tex->height) continue;

        for (int col = 0; col < width; col++) {
            int dst_x = xoffset + col;
            if (dst_x < 0 || dst_x >= tex->width) continue;

            int src_idx = (row * width + col) * bpp_src;
            int dst_idx = (dst_y * tex->width + dst_x) * 4;

            switch (bpp_src) {
                case 4:
                    tex->data[dst_idx + 0] = ((const uint8_t *)data)[src_idx + 0];
                    tex->data[dst_idx + 1] = ((const uint8_t *)data)[src_idx + 1];
                    tex->data[dst_idx + 2] = ((const uint8_t *)data)[src_idx + 2];
                    tex->data[dst_idx + 3] = ((const uint8_t *)data)[src_idx + 3];
                    break;
                case 3:
                    tex->data[dst_idx + 0] = ((const uint8_t *)data)[src_idx + 0];
                    tex->data[dst_idx + 1] = ((const uint8_t *)data)[src_idx + 1];
                    tex->data[dst_idx + 2] = ((const uint8_t *)data)[src_idx + 2];
                    tex->data[dst_idx + 3] = 255;
                    break;
                case 1: {
                    uint8_t v = ((const uint8_t *)data)[src_idx];
                    tex->data[dst_idx + 0] = v;
                    tex->data[dst_idx + 1] = v;
                    tex->data[dst_idx + 2] = v;
                    tex->data[dst_idx + 3] = 255;
                    break;
                }
            }
        }
    }
}

void gl_texture_parameteri(GLenum target, GLenum pname, GLint param) {
    (void)target;

    gl_texture_t *tex = get_bound();
    if (!tex) return;

    switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
            tex->min_filter = (GLenum)param;
            break;
        case GL_TEXTURE_MAG_FILTER:
            tex->mag_filter = (GLenum)param;
            break;
        case GL_TEXTURE_WRAP_S:
            tex->wrap_s = (GLenum)param;
            break;
        case GL_TEXTURE_WRAP_T:
            tex->wrap_t = (GLenum)param;
            break;
        default:
            break;
    }
}

gl_texture_t *gl_texture_get(GLuint name) {
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (texture_pool[i].used && texture_pool[i].name == name)
            return &texture_pool[i];
    }
    return NULL;
}

GLuint gl_texture_get_bound(void) {
    if (active_texture_unit < GL_MAX_TEXTURES)
        return bound_textures[active_texture_unit];
    return 0;
}

void gl_texture_generate_mipmaps(GLuint name) {
    gl_texture_t *tex = gl_texture_get(name);
    if (!tex || !tex->data) return;

    int w = tex->width;
    int h = tex->height;
    if (w <= 1 && h <= 1) return;

    int max_level = 0;
    {
        int tw = w, th = h;
        while (tw > 1 || th > 1) {
            if (tw > 1) tw >>= 1;
            if (th > 1) th >>= 1;
            max_level++;
        }
    }
    if (max_level > 10) max_level = 10;

    tex->mip_width[0] = w;
    tex->mip_height[0] = h;

    int total = 0;
    {
        int tw = w, th = h;
        for (int lvl = 0; lvl <= max_level; lvl++) {
            tex->mip_width[lvl] = tw;
            tex->mip_height[lvl] = th;
            total += tw * th * 4;
            if (tw > 1) tw >>= 1;
            if (th > 1) th >>= 1;
        }
    }

    GLubyte *mip_data = (GLubyte *)kmalloc(total);
    if (!mip_data) return;

    memcpy(mip_data, tex->data, w * h * 4);

    int src_w = w;
    int src_h = h;
    int src_off = 0;
    int dst_off = src_w * src_h * 4;

    for (int lvl = 1; lvl <= max_level; lvl++) {
        int dst_w = src_w > 1 ? src_w >> 1 : 1;
        int dst_h = src_h > 1 ? src_h >> 1 : 1;

        for (int y = 0; y < dst_h; y++) {
            for (int x = 0; x < dst_w; x++) {
                int sx = x * 2;
                int sy = y * 2;

                int r = 0, g = 0, b = 0, a = 0;
                int cnt = 0;

                for (int dy = 0; dy < 2 && sy + dy < src_h; dy++) {
                    for (int dx = 0; dx < 2 && sx + dx < src_w; dx++) {
                        int idx = src_off + ((sy + dy) * src_w + (sx + dx)) * 4;
                        r += mip_data[idx + 0];
                        g += mip_data[idx + 1];
                        b += mip_data[idx + 2];
                        a += mip_data[idx + 3];
                        cnt++;
                    }
                }

                int didx = dst_off + (y * dst_w + x) * 4;
                mip_data[didx + 0] = (GLubyte)(r / cnt);
                mip_data[didx + 1] = (GLubyte)(g / cnt);
                mip_data[didx + 2] = (GLubyte)(b / cnt);
                mip_data[didx + 3] = (GLubyte)(a / cnt);
            }
        }

        src_off += src_w * src_h * 4;
        dst_off += dst_w * dst_h * 4;
        src_w = dst_w;
        src_h = dst_h;
    }

    kfree(tex->data);
    tex->data = mip_data;
    tex->allocated = total;
    tex->mip_levels = max_level + 1;
}

void gl_texture_activate(GLenum unit) {
    if (unit < GL_MAX_TEXTURES)
        active_texture_unit = unit;
}
