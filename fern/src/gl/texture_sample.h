#ifndef GL_TEXTURE_SAMPLE_H
#define GL_TEXTURE_SAMPLE_H

#include "texture.h"
#include "../include/math.h"

static inline int gl_mip_level_offset(gl_texture_t *tex, int level) {
    int off = 0;
    for (int i = 0; i < level; i++)
        off += tex->mip_width[i] * tex->mip_height[i] * 4;
    return off;
}

static inline void gl_texel_fetch_level(gl_texture_t *tex, int level,
                                        int x, int y, float *rgba) {
    if (!tex || !tex->data || !rgba) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }

    int lw = tex->mip_width[level];
    int lh = tex->mip_height[level];
    if (lw <= 0 || lh <= 0) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= lw) x = lw - 1;
    if (y >= lh) y = lh - 1;

    int off = gl_mip_level_offset(tex, level) + (y * lw + x) * 4;
    rgba[0] = tex->data[off + 0] / 255.0f;
    rgba[1] = tex->data[off + 1] / 255.0f;
    rgba[2] = tex->data[off + 2] / 255.0f;
    rgba[3] = tex->data[off + 3] / 255.0f;
}

static inline void gl_texel_fetch(gl_texture_t *tex, int x, int y,
                                   float *rgba) {
    gl_texel_fetch_level(tex, 0, x, y, rgba);
}

static inline int gl_wrap_coord(int coord, int size, GLenum wrap) {
    if (wrap == GL_REPEAT) {
        coord = coord % size;
        if (coord < 0) coord += size;
    } else {
        if (coord < 0) coord = 0;
        if (coord >= size) coord = size - 1;
    }
    return coord;
}

static inline int gl_is_mipmap_filter(GLenum f) {
    return f == GL_NEAREST_MIPMAP_NEAREST ||
           f == GL_LINEAR_MIPMAP_NEAREST ||
           f == GL_NEAREST_MIPMAP_LINEAR ||
           f == GL_LINEAR_MIPMAP_LINEAR;
}

static inline int gl_needs_bilinear(GLenum f) {
    return f == GL_LINEAR ||
           f == GL_LINEAR_MIPMAP_NEAREST ||
           f == GL_LINEAR_MIPMAP_LINEAR;
}

static inline int gl_needs_two_levels(GLenum f) {
    return f == GL_NEAREST_MIPMAP_LINEAR ||
           f == GL_LINEAR_MIPMAP_LINEAR;
}

static inline float gl_lod_select(gl_texture_t *tex, float u, float v,
                                  float du_dx, float dv_dx,
                                  float du_dy, float dv_dy) {
    if (tex->mip_levels <= 1) return 0.0f;

    float dudx = du_dx * tex->width;
    float dvdx = dv_dx * tex->width;
    float dudy = du_dy * tex->height;
    float dvdy = dv_dy * tex->height;

    float rho_sq = dudx * dudx + dvdx * dvdx + dudy * dudy + dvdy * dvdy;
    if (rho_sq < 1e-10f) rho_sq = 1e-10f;

    float lod = 0.5f * logf(rho_sq) / logf(2.0f);
    if (lod < 0.0f) lod = 0.0f;
    if (lod > (float)(tex->mip_levels - 1))
        lod = (float)(tex->mip_levels - 1);

    return lod;
}

static inline void gl_sample_bilinear_level(gl_texture_t *tex, int level,
                                            float u, float v, float *rgba) {
    int lw = tex->mip_width[level];
    int lh = tex->mip_height[level];

    float fu = u * lw - 0.5f;
    float fv = v * lh - 0.5f;

    int x0 = (int)fu;
    int y0 = (int)fv;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float frac_x = fu - (float)x0;
    float frac_y = fv - (float)y0;
    if (frac_x < 0.0f) frac_x += 1.0f;
    if (frac_y < 0.0f) frac_y += 1.0f;

    x0 = gl_wrap_coord(x0, lw, tex->wrap_s);
    y0 = gl_wrap_coord(y0, lh, tex->wrap_t);
    x1 = gl_wrap_coord(x1, lw, tex->wrap_s);
    y1 = gl_wrap_coord(y1, lh, tex->wrap_t);

    float c00[4], c10[4], c01[4], c11[4];
    gl_texel_fetch_level(tex, level, x0, y0, c00);
    gl_texel_fetch_level(tex, level, x1, y0, c10);
    gl_texel_fetch_level(tex, level, x0, y1, c01);
    gl_texel_fetch_level(tex, level, x1, y1, c11);

    float inv_fx = 1.0f - frac_x;
    float inv_fy = 1.0f - frac_y;

    for (int i = 0; i < 4; i++)
        rgba[i] = c00[i] * inv_fx * inv_fy
                + c10[i] * frac_x * inv_fy
                + c01[i] * inv_fx * frac_y
                + c11[i] * frac_x * frac_y;
}

static inline void gl_sample_nearest_level(gl_texture_t *tex, int level,
                                           float u, float v, float *rgba) {
    int lw = tex->mip_width[level];
    int lh = tex->mip_height[level];

    int x = (int)(u * lw);
    int y = (int)(v * lh);
    x = gl_wrap_coord(x, lw, tex->wrap_s);
    y = gl_wrap_coord(y, lh, tex->wrap_t);
    gl_texel_fetch_level(tex, level, x, y, rgba);
}

static inline void gl_texture_sample_lod(gl_texture_t *tex, float u, float v,
                                         float du_dx, float dv_dx,
                                         float du_dy, float dv_dy,
                                         float *rgba) {
    if (!tex || !tex->data || !rgba) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
        return;
    }

    GLenum filter = gl_is_mipmap_filter(tex->min_filter)
                        ? tex->min_filter : tex->mag_filter;

    if (tex->mip_levels <= 1 || !gl_is_mipmap_filter(filter)) {
        if (gl_needs_bilinear(filter)) {
            gl_sample_bilinear_level(tex, 0, u, v, rgba);
        } else {
            gl_sample_nearest_level(tex, 0, u, v, rgba);
        }
        return;
    }

    float lod = gl_lod_select(tex, u, v, du_dx, dv_dx, du_dy, dv_dy);
    int level0 = (int)lod;
    int level1 = level0 + 1;
    float frac = lod - (float)level0;

    if (level0 >= tex->mip_levels) level0 = tex->mip_levels - 1;
    if (level1 >= tex->mip_levels) level1 = tex->mip_levels - 1;

    if (gl_needs_two_levels(filter)) {
        float c0[4], c1[4];
        if (gl_needs_bilinear(filter)) {
            gl_sample_bilinear_level(tex, level0, u, v, c0);
            gl_sample_bilinear_level(tex, level1, u, v, c1);
        } else {
            gl_sample_nearest_level(tex, level0, u, v, c0);
            gl_sample_nearest_level(tex, level1, u, v, c1);
        }
        float inv_frac = 1.0f - frac;
        for (int i = 0; i < 4; i++)
            rgba[i] = c0[i] * inv_frac + c1[i] * frac;
    } else {
        int selected = (frac < 0.5f) ? level0 : level1;
        if (selected >= tex->mip_levels) selected = tex->mip_levels - 1;

        if (gl_needs_bilinear(filter)) {
            gl_sample_bilinear_level(tex, selected, u, v, rgba);
        } else {
            gl_sample_nearest_level(tex, selected, u, v, rgba);
        }
    }
}

static inline void gl_texture_sample(gl_texture_t *tex, float u, float v,
                                     float *rgba) {
    gl_texture_sample_lod(tex, u, v, 0.0f, 0.0f, 0.0f, 0.0f, rgba);
}

#endif /* GL_TEXTURE_SAMPLE_H */
