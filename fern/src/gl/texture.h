#ifndef GL_TEXTURE_H
#define GL_TEXTURE_H

#include "state.h"

#define GL_MAX_TEXTURES     256
#define GL_MAX_TEXTURE_SIZE 4096
#define GL_MAX_MIP_LEVELS   11

#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703

#define GL_CLAMP_TO_BORDER 0x812D

#define GL_LUMINANCE       0x1909
#define GL_ALPHA           0x1906

#define GL_RGBA8           0x8058
#define GL_RGB8            0x8051
#define GL_LUMINANCE8      0x8040

#define GL_TEXTURE_MAX_LEVEL  0x280D

typedef struct {
    GLuint name;
    GLint width, height;
    GLint internal_format;
    GLenum format;
    GLenum type;
    GLubyte *data;
    GLenum min_filter;
    GLenum mag_filter;
    GLenum wrap_s;
    GLenum wrap_t;
    GLboolean used;
    int mip_levels;
    int allocated;
    int mip_width[GL_MAX_MIP_LEVELS];
    int mip_height[GL_MAX_MIP_LEVELS];
} gl_texture_t;

void gl_texture_init(void);
GLuint gl_texture_create(void);
void gl_texture_delete(GLuint name);
void gl_texture_bind(GLenum target, GLuint name);
void gl_texture_image2d(GLenum target, GLint level, GLint internal_format,
                        GLsizei width, GLsizei height, GLint border,
                        GLenum format, GLenum type, const void *data);
void gl_texture_sub_image2d(GLenum target, GLint level, GLint xoffset,
                            GLint yoffset, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, const void *data);
void gl_texture_parameteri(GLenum target, GLenum pname, GLint param);
void gl_texture_activate(GLenum unit);
gl_texture_t *gl_texture_get(GLuint name);
GLuint gl_texture_get_bound(void);
void gl_texture_generate_mipmaps(GLuint name);

/* Returns bytes per pixel for the texture's internal storage (always 4 after
   conversion).  Used by the sampler to compute mip-level offsets. */
int gl_texture_bpp(gl_texture_t *tex);

#endif /* GL_TEXTURE_H */
