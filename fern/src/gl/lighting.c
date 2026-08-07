#include "lighting.h"
#include "state.h"
#include "../include/libc/math.h"

static float clampf01(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static float powf_approx(float base, float exp)
{
    if (exp == 0.0f) return 1.0f;
    if (exp == 1.0f) return base;
    if (base <= 0.0f) return 0.0f;

    float result = 1.0f;
    int e = (int)exp;
    float f = base;
    while (e > 0) {
        if (e & 1) result *= f;
        f *= f;
        e >>= 1;
    }
    float frac = exp - (float)(int)exp;
    if (frac > 0.001f) {
        result *= 1.0f + frac * (base - 1.0f);
    }
    return result;
}

void gl_update_normal_matrix(void)
{
    mat4_t mv = g_gl_state.modelview_matrix;
    mat3_t nm;

    nm.m[0] = mv.m[0]; nm.m[1] = mv.m[1]; nm.m[2] = mv.m[2];
    nm.m[3] = mv.m[4]; nm.m[4] = mv.m[5]; nm.m[5] = mv.m[6];
    nm.m[6] = mv.m[8]; nm.m[7] = mv.m[9]; nm.m[8] = mv.m[10];

    float det = nm.m[0] * (nm.m[4] * nm.m[8] - nm.m[5] * nm.m[7])
              - nm.m[1] * (nm.m[3] * nm.m[8] - nm.m[5] * nm.m[6])
              + nm.m[2] * (nm.m[3] * nm.m[7] - nm.m[4] * nm.m[6]);

    if (det != 0.0f) {
        float inv_det = 1.0f / det;
        mat3_t adj;
        adj.m[0] = (nm.m[4] * nm.m[8] - nm.m[5] * nm.m[7]) * inv_det;
        adj.m[1] = (nm.m[2] * nm.m[7] - nm.m[1] * nm.m[8]) * inv_det;
        adj.m[2] = (nm.m[1] * nm.m[5] - nm.m[2] * nm.m[4]) * inv_det;
        adj.m[3] = (nm.m[5] * nm.m[6] - nm.m[3] * nm.m[8]) * inv_det;
        adj.m[4] = (nm.m[0] * nm.m[8] - nm.m[2] * nm.m[6]) * inv_det;
        adj.m[5] = (nm.m[2] * nm.m[3] - nm.m[0] * nm.m[5]) * inv_det;
        adj.m[6] = (nm.m[3] * nm.m[7] - nm.m[4] * nm.m[6]) * inv_det;
        adj.m[7] = (nm.m[1] * nm.m[6] - nm.m[0] * nm.m[7]) * inv_det;
        adj.m[8] = (nm.m[0] * nm.m[4] - nm.m[1] * nm.m[3]) * inv_det;
        g_gl_state.normal_matrix = adj;
    } else {
        g_gl_state.normal_matrix = (mat3_t){{1,0,0,0,1,0,0,0,1}};
    }
    g_gl_state.normal_matrix_dirty = 0;
}

void gl_compute_lighting(float nx, float ny, float nz,
                         float *eye_x, float *eye_y, float *eye_z,
                         float *r, float *g, float *b)
{
    if (g_gl_state.normal_matrix_dirty)
        gl_update_normal_matrix();

    mat3_t nm = g_gl_state.normal_matrix;
    float tnx = nm.m[0] * nx + nm.m[3] * ny + nm.m[6] * nz;
    float tny = nm.m[1] * nx + nm.m[4] * ny + nm.m[7] * nz;
    float tnz = nm.m[2] * nx + nm.m[5] * ny + nm.m[8] * nz;
    float len = tnx * tnx + tny * tny + tnz * tnz;
    if (len > 0.0001f) {
        float inv = 1.0f / sqrtf(len);
        tnx *= inv;
        tny *= inv;
        tnz *= inv;
    }

    float cr = g_gl_state.global_ambient[0] * g_gl_state.material_ambient[0];
    float cg = g_gl_state.global_ambient[1] * g_gl_state.material_ambient[1];
    float cb = g_gl_state.global_ambient[2] * g_gl_state.material_ambient[2];

    float ex = 0.0f, ey = 0.0f, ez = 1.0f;
    if (eye_x) ex = *eye_x;
    if (eye_y) ey = *eye_y;
    if (eye_z) ez = *eye_z;
    float elen = ex * ex + ey * ey + ez * ez;
    if (elen > 0.0001f) {
        float inv = 1.0f / sqrtf(elen);
        ex *= inv;
        ey *= inv;
        ez *= inv;
    }

    for (int i = 0; i < 8; i++) {
        if (!g_gl_state.light_enabled[i]) continue;

        float lx = g_gl_state.light_position[i][0];
        float ly = g_gl_state.light_position[i][1];
        float lz = g_gl_state.light_position[i][2];
        float lw = g_gl_state.light_position[i][3];

        float dlx, dly, dlz;
        if (lw == 0.0f) {
            dlx = lx;
            dly = ly;
            dlz = lz;
        } else {
            dlx = lx - (eye_x ? *eye_x : 0.0f);
            dly = ly - (eye_y ? *eye_y : 0.0f);
            dlz = lz - (eye_z ? *eye_z : 0.0f);
        }
        float dlen = dlx * dlx + dly * dly + dlz * dlz;
        if (dlen > 0.0001f) {
            float inv = 1.0f / sqrtf(dlen);
            dlx *= inv;
            dly *= inv;
            dlz *= inv;
        }

        float ndotl = tnx * dlx + tny * dly + tnz * dlz;
        if (ndotl < 0.0f) ndotl = 0.0f;

        float ar = g_gl_state.light_ambient[i][0] * g_gl_state.material_ambient[0];
        float ag = g_gl_state.light_ambient[i][1] * g_gl_state.material_ambient[1];
        float ab = g_gl_state.light_ambient[i][2] * g_gl_state.material_ambient[2];

        float dr = g_gl_state.light_diffuse[i][0] * g_gl_state.material_diffuse[0] * ndotl;
        float dg = g_gl_state.light_diffuse[i][1] * g_gl_state.material_diffuse[1] * ndotl;
        float db = g_gl_state.light_diffuse[i][2] * g_gl_state.material_diffuse[2] * ndotl;

        float rx = 2.0f * ndotl * tnx - dlx;
        float ry = 2.0f * ndotl * tny - dly;
        float rz = 2.0f * ndotl * tnz - dlz;

        float rdotv = rx * ex + ry * ey + rz * ez;
        if (rdotv < 0.0f) rdotv = 0.0f;

        float shin = g_gl_state.material_shininess;
        float spec = powf_approx(rdotv, shin);

        float sr = g_gl_state.light_specular[i][0] * g_gl_state.material_specular[0] * spec;
        float sg = g_gl_state.light_specular[i][1] * g_gl_state.material_specular[1] * spec;
        float sb = g_gl_state.light_specular[i][2] * g_gl_state.material_specular[2] * spec;

        cr += ar + dr + sr;
        cg += ag + dg + sg;
        cb += ab + db + sb;
    }

    if (r) *r = clampf01(cr);
    if (g) *g = clampf01(cg);
    if (b) *b = clampf01(cb);
}
