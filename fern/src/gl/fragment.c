#include "fragment.h"
#include "state.h"
#include "texture.h"
#include <string.h>
#include "../include/libc/math.h"

fragment_shader_fn g_gl_fragment_shader = gl_default_fragment_shader;

static gl_texture_t *get_bound_texture_unit(unsigned int unit)
{
    if (unit >= 8) unit = 0;
    unsigned int name = g_gl_state.bound_textures[unit];
    if (name == 0) return 0;
    return gl_texture_get(name);
}

static gl_texture_t *get_bound_texture(void)
{
    unsigned int idx = g_gl_state.active_texture - GL_TEXTURE0;
    if (idx >= 8) idx = 0;
    return get_bound_texture_unit(idx);
}

static void texture_sample2d(gl_texture_t *tex, float u, float v,
                             float *out_r, float *out_g, float *out_b, float *out_a)
{
    if (!tex || !tex->data || tex->width == 0 || tex->height == 0) {
        *out_r = 1.0f; *out_g = 1.0f; *out_b = 1.0f; *out_a = 1.0f;
        return;
    }

    if (tex->wrap_s == GL_REPEAT) {
        u = u - (float)(int)u;
        if (u < 0.0f) u += 1.0f;
    } else {
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
    }
    if (tex->wrap_t == GL_REPEAT) {
        v = v - (float)(int)v;
        if (v < 0.0f) v += 1.0f;
    } else {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
    }

    int px = (int)(u * (tex->width - 1));
    int py = (int)(v * (tex->height - 1));
    if (px < 0) px = 0; if (px >= tex->width)  px = tex->width  - 1;
    if (py < 0) py = 0; if (py >= tex->height) py = tex->height - 1;

    int bpp = 4;
    if (tex->internal_format == GL_RGB || tex->internal_format == GL_RGB8)
        bpp = 3;
    else if (tex->internal_format == GL_LUMINANCE || tex->internal_format == GL_LUMINANCE8)
        bpp = 1;

    int idx = (py * tex->width + px) * bpp;
    if (bpp == 4) {
        *out_r = tex->data[idx + 0] / 255.0f;
        *out_g = tex->data[idx + 1] / 255.0f;
        *out_b = tex->data[idx + 2] / 255.0f;
        *out_a = tex->data[idx + 3] / 255.0f;
    } else if (bpp == 3) {
        *out_r = tex->data[idx + 0] / 255.0f;
        *out_g = tex->data[idx + 1] / 255.0f;
        *out_b = tex->data[idx + 2] / 255.0f;
        *out_a = 1.0f;
    } else {
        float lum = tex->data[idx] / 255.0f;
        *out_r = lum; *out_g = lum; *out_b = lum; *out_a = 1.0f;
    }
}

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float dot3_rgb(float r0, float g0, float b0, float r1, float g1, float b1)
{
    return r0 * r1 + g0 * g1 + b0 * b1;
}

static void combine_textures(GLenum mode,
                              float sr, float sg, float sb, float sa,
                              float pr, float pg, float pb, float pa,
                              float cr, float cg, float cb, float ca,
                              float *out_r, float *out_g, float *out_b, float *out_a)
{
    switch (mode) {
    case GL_TEX_REPLACE:
        *out_r = sr; *out_g = sg; *out_b = sb; *out_a = sa;
        break;
    case GL_MODULATE:
        *out_r = sr * pr;
        *out_g = sg * pg;
        *out_b = sb * pb;
        *out_a = sa * pa;
        break;
    case GL_ADD:
        *out_r = clampf(sr + pr, 0.0f, 1.0f);
        *out_g = clampf(sg + pg, 0.0f, 1.0f);
        *out_b = clampf(sb + pb, 0.0f, 1.0f);
        *out_a = clampf(sa + pa, 0.0f, 1.0f);
        break;
    case GL_ADD_SIGNED:
        *out_r = clampf(sr + pr - 0.5f, 0.0f, 1.0f);
        *out_g = clampf(sg + pg - 0.5f, 0.0f, 1.0f);
        *out_b = clampf(sb + pb - 0.5f, 0.0f, 1.0f);
        *out_a = clampf(sa + pa - 0.5f, 0.0f, 1.0f);
        break;
    case GL_INTERPOLATE:
        *out_r = sr * cr + pr * (1.0f - cr);
        *out_g = sg * cr + pg * (1.0f - cr);
        *out_b = sb * cr + pb * (1.0f - cr);
        *out_a = sa * ca + pa * (1.0f - ca);
        break;
    case GL_SUBTRACT:
        *out_r = clampf(sr - pr, 0.0f, 1.0f);
        *out_g = clampf(sg - pg, 0.0f, 1.0f);
        *out_b = clampf(sb - pb, 0.0f, 1.0f);
        *out_a = clampf(sa - pa, 0.0f, 1.0f);
        break;
    case GL_DOT3_RGB:
    case GL_DOT3_RGBA: {
        float d = dot3_rgb(sr, sg, sb, pr, pg, pb);
        *out_r = clampf(d, 0.0f, 1.0f);
        *out_g = clampf(d, 0.0f, 1.0f);
        *out_b = clampf(d, 0.0f, 1.0f);
        *out_a = (mode == GL_DOT3_RGBA) ? clampf(d, 0.0f, 1.0f) : 1.0f;
        break;
    }
    default:
        *out_r = sr; *out_g = sg; *out_b = sb; *out_a = sa;
        break;
    }
}

void gl_default_fragment_shader(gl_fragment_t *frag)
{
    float r = frag->r;
    float g = frag->g;
    float b = frag->b;
    float a = frag->a;

    if (g_gl_state.texture_2d) {
        gl_texture_t *tex0 = get_bound_texture_unit(0);
        if (tex0) {
            float tr, tg, tb, ta;
            texture_sample2d(tex0, frag->u, frag->v, &tr, &tg, &tb, &ta);
            GLenum mode = g_gl_state.tex_env_mode[0];
            if (mode == 0) mode = GL_MODULATE;
            float nr, ng, nb, na;
            combine_textures(mode, tr, tg, tb, ta, r, g, b, a,
                             g_gl_state.tex_env_color[0][0],
                             g_gl_state.tex_env_color[0][1],
                             g_gl_state.tex_env_color[0][2],
                             g_gl_state.tex_env_color[0][3],
                             &nr, &ng, &nb, &na);
            r = nr; g = ng; b = nb; a = na;
        }

        for (int i = 1; i < 8; i++) {
            gl_texture_t *tex = get_bound_texture_unit(i);
            if (!tex) continue;
            float tr, tg, tb, ta;
            texture_sample2d(tex, frag->u, frag->v, &tr, &tg, &tb, &ta);
            GLenum mode = g_gl_state.tex_env_mode[i];
            if (mode == 0) mode = GL_MODULATE;
            float nr, ng, nb, na;
            combine_textures(mode, tr, tg, tb, ta, r, g, b, a,
                             g_gl_state.tex_env_color[i][0],
                             g_gl_state.tex_env_color[i][1],
                             g_gl_state.tex_env_color[i][2],
                             g_gl_state.tex_env_color[i][3],
                             &nr, &ng, &nb, &na);
            r = nr; g = ng; b = nb; a = na;
        }
    }

    if (g_gl_state.lighting) {
        vec3_t n = { frag->nx, frag->ny, frag->nz };
        float len = n.x * n.x + n.y * n.y + n.z * n.z;
        if (len > 0.0001f) {
            float inv = 1.0f / len;
            n.x *= inv; n.y *= inv; n.z *= inv;
        }

        float lr = g_gl_state.global_ambient[0];
        float lg = g_gl_state.global_ambient[1];
        float lb = g_gl_state.global_ambient[2];

        for (int i = 0; i < 8; i++) {
            if (!g_gl_state.light_enabled[i]) continue;

            vec3_t ldir = {
                g_gl_state.light_position[i][0],
                g_gl_state.light_position[i][1],
                g_gl_state.light_position[i][2]
            };
            float llen = ldir.x * ldir.x + ldir.y * ldir.y + ldir.z * ldir.z;
            if (llen > 0.0001f) {
                float inv = 1.0f / llen;
                ldir.x *= inv; ldir.y *= inv; ldir.z *= inv;
            }

            float ndotl = n.x * ldir.x + n.y * ldir.y + n.z * ldir.z;
            if (ndotl < 0.0f) ndotl = 0.0f;

            lr += g_gl_state.light_diffuse[i][0] * ndotl * g_gl_state.material_diffuse[0];
            lg += g_gl_state.light_diffuse[i][1] * ndotl * g_gl_state.material_diffuse[1];
            lb += g_gl_state.light_diffuse[i][2] * ndotl * g_gl_state.material_diffuse[2];

            lr += g_gl_state.light_ambient[i][0] * g_gl_state.material_ambient[0];
            lg += g_gl_state.light_ambient[i][1] * g_gl_state.material_ambient[1];
            lb += g_gl_state.light_ambient[i][2] * g_gl_state.material_ambient[2];
        }

        r *= lr; g *= lg; b *= lb;
    }

    frag->r = clampf(r, 0.0f, 1.0f);
    frag->g = clampf(g, 0.0f, 1.0f);
    frag->b = clampf(b, 0.0f, 1.0f);
    frag->a = clampf(a, 0.0f, 1.0f);

    if (g_gl_state.alpha_test_enabled) {
        int pass = 0;
        float ref = g_gl_state.alpha_ref;
        float a = frag->a;
        switch (g_gl_state.alpha_func) {
        case 0x0200: pass = 0;                                    break; /* GL_NEVER    */
        case 0x0201: pass = a <  ref ? 1 : 0;                    break; /* GL_LESS     */
        case 0x0202: pass = a == ref ? 1 : 0;                    break; /* GL_EQUAL    */
        case 0x0203: pass = a <= ref ? 1 : 0;                    break; /* GL_LEQUAL   */
        case 0x0204: pass = a >  ref ? 1 : 0;                    break; /* GL_GREATER  */
        case 0x0205: pass = a != ref ? 1 : 0;                    break; /* GL_NOTEQUAL */
        case 0x0206: pass = a >= ref ? 1 : 0;                    break; /* GL_GEQUAL   */
        case 0x0207: pass = 1;                                    break; /* GL_ALWAYS   */
        default:     pass = 1; break;
        }
        if (!pass) {
            frag->discard = 1;
            return;
        }
    }

    if (g_gl_state.fog_enabled) {
        float dist = frag->depth;
        if (dist < 0.0f) dist = -dist;
        float factor = 1.0f;

        switch (g_gl_state.fog_mode) {
        case 0x2601: { /* GL_LINEAR */
            float range = g_gl_state.fog_end - g_gl_state.fog_start;
            if (range != 0.0f)
                factor = (g_gl_state.fog_end - dist) / range;
            else
                factor = 0.0f;
            break;
        }
        case 0x0800: { /* GL_EXP */
            factor = expf(-g_gl_state.fog_density * dist);
            break;
        }
        case 0x0801: { /* GL_EXP2 */
            float t = g_gl_state.fog_density * dist;
            factor = expf(-(t * t));
            break;
        }
        default:
            break;
        }

        if (factor < 0.0f) factor = 0.0f;
        if (factor > 1.0f) factor = 1.0f;

        float fog_a = 1.0f - factor;
        frag->r = frag->r * factor + g_gl_state.fog_color[0] * fog_a;
        frag->g = frag->g * factor + g_gl_state.fog_color[1] * fog_a;
        frag->b = frag->b * factor + g_gl_state.fog_color[2] * fog_a;
        frag->a = frag->a * factor + g_gl_state.fog_color[3] * fog_a;
    }
}
