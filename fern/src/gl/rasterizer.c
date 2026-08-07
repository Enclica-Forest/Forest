#include "rasterizer.h"
#include "state.h"
#include "fragment.h"
#include "simd.h"
#include "stats.h"
#include "framebuffer.h"
#include <string.h>
#include "../include/libc/math.h"

gl_framebuffer_t *g_gl_framebuffer;

static inline gl_framebuffer_t* get_framebuffer(void) {
    gl_framebuffer_t* fb = gl_resolve_framebuffer();
    if (fb) return fb;
    return g_gl_framebuffer;
}

/* ============================================================
 * Fixed-point: 12.4 format for screen-space coordinates.
 * 12 integer bits (up to 4096 px), 4 fractional bits (sub-pixel).
 * ============================================================ */
#define FP_SHIFT 4
#define FP_SCALE (1 << FP_SHIFT)

/* ============================================================
 * Hot-path helper functions -- marked always_inline so the
 * compiler eliminates call/return overhead in the inner loop.
 * ============================================================ */

static inline __attribute__((always_inline))
int min3i(int a, int b, int c)
{
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static inline __attribute__((always_inline))
int max3i(int a, int b, int c)
{
    int m = a > b ? a : b;
    return m > c ? m : c;
}

static inline __attribute__((always_inline))
float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* 2D orientation test (signed area of parallelogram).
 * Result > 0 => CCW, < 0 => CW, == 0 => degenerate. */
static inline __attribute__((always_inline))
int orient2d(int ax, int ay, int bx, int by, int cx, int cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* Branchless depth function -- no switch, no branch for the
 * common cases (LESS, LEQUAL, ALWAYS). Ternary chain is
 * predicted well by the branch predictor and compiles to
 * cmov on x86. */
static inline __attribute__((always_inline))
int depth_func_pass(unsigned int func, float src, float dst)
{
    return (func == 0x0207) ? 1 :                            /* GL_ALWAYS   */
           (func == 0x0201) ? (src <  dst) :                 /* GL_LESS     */
           (func == 0x0203) ? (src <= dst) :                 /* GL_LEQUAL   */
           (func == 0x0204) ? (src >  dst) :                 /* GL_GREATER  */
           (func == 0x0206) ? (src >= dst) :                 /* GL_GEQUAL   */
           (func == 0x0202) ? (src == dst) :                 /* GL_EQUAL    */
           (func == 0x0205) ? (src != dst) :                 /* GL_NOTEQUAL */
           0;                                                 /* GL_NEVER    */
}

static inline __attribute__((always_inline))
int stencil_func_pass(unsigned int func, unsigned char src, unsigned char ref, unsigned char mask)
{
    return (func == 0x0207) ? 1 :
           (func == 0x0201) ? ((src & mask) <  (ref & mask)) :
           (func == 0x0203) ? ((src & mask) <= (ref & mask)) :
           (func == 0x0204) ? ((src & mask) >  (ref & mask)) :
           (func == 0x0206) ? ((src & mask) >= (ref & mask)) :
           (func == 0x0202) ? ((src & mask) == (ref & mask)) :
           (func == 0x0205) ? ((src & mask) != (ref & mask)) :
           0;
}

static inline __attribute__((always_inline))
unsigned char apply_stencil_op(unsigned int op, unsigned char current, unsigned char ref)
{
    return (op == GL_STENCIL_KEEP)    ? current :
           (op == GL_STENCIL_ZERO)    ? 0 :
           (op == GL_STENCIL_REPLACE) ? ref :
           (op == GL_STENCIL_INCR)    ? (unsigned char)(current + 1) :
           (op == GL_STENCIL_DECR)    ? (unsigned char)(current - 1) :
           (op == GL_STENCIL_INVERT)  ? (unsigned char)(~current) :
           (op == GL_STENCIL_INCR_WRAP) ? ((current == 255) ? 0 : (unsigned char)(current + 1)) :
           (op == GL_STENCIL_DECR_WRAP) ? ((current == 0)   ? 255 : (unsigned char)(current - 1)) :
           current;
}

static inline __attribute__((always_inline))
float get_blend_factor(unsigned int factor, float src, float dst)
{
    return (factor == 0x0000) ? 0.0f :
           (factor == 0x0001) ? 1.0f :
           (factor == 0x0300) ? src :
           (factor == 0x0301) ? (1.0f - src) :
           (factor == 0x0302) ? src :
           (factor == 0x0303) ? (1.0f - src) :
           (factor == 0x0304) ? dst :
           (factor == 0x0305) ? (1.0f - dst) :
           (factor == 0x0306) ? dst :
           (factor == 0x0307) ? (1.0f - dst) :
           1.0f;
}

/* ============================================================
 * Perspective-correct attribute interpolation.
 * ============================================================ */

static inline __attribute__((always_inline))
void interpolate_fragment(gl_fragment_t *frag, gl_vertex_t *v0, gl_vertex_t *v1, gl_vertex_t *v2,
                          float b0, float b1, float b2)
{
    float inv_w = b0 / v0->clip_pos.w + b1 / v1->clip_pos.w + b2 / v2->clip_pos.w;
    if (inv_w == 0.0f) inv_w = 0.0001f;
    float nw0 = (b0 / v0->clip_pos.w) / inv_w;
    float nw1 = (b1 / v1->clip_pos.w) / inv_w;
    float nw2 = (b2 / v2->clip_pos.w) / inv_w;

    frag->r = nw0 * v0->color.x + nw1 * v1->color.x + nw2 * v2->color.x;
    frag->g = nw0 * v0->color.y + nw1 * v1->color.y + nw2 * v2->color.y;
    frag->b = nw0 * v0->color.z + nw1 * v1->color.z + nw2 * v2->color.z;
    frag->a = nw0 * v0->color.w + nw1 * v1->color.w + nw2 * v2->color.w;

    frag->u = nw0 * v0->texcoord.x + nw1 * v1->texcoord.x + nw2 * v2->texcoord.x;
    frag->v = nw0 * v0->texcoord.y + nw1 * v1->texcoord.y + nw2 * v2->texcoord.y;

    frag->nx = nw0 * v0->normal.x + nw1 * v1->normal.x + nw2 * v2->normal.x;
    frag->ny = nw0 * v0->normal.y + nw1 * v1->normal.y + nw2 * v2->normal.y;
    frag->nz = nw0 * v0->normal.z + nw1 * v1->normal.z + nw2 * v2->normal.z;

    frag->depth = b0 * v0->eye_z + b1 * v1->eye_z + b2 * v2->eye_z;
}

/* ============================================================
 * Pixel output: blend, logic-op, write to color buffer.
 * ============================================================ */

static inline __attribute__((always_inline))
void write_pixel(gl_framebuffer_t *fb, int x, int y, float r, float g, float b, float a)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height)
        return;

    int idx = y * fb->stride + x;
    unsigned int dst_raw = fb->color_buffer[idx];

    if (g_gl_state.blend) {
        float dr = ((dst_raw >> 0)  & 0xFF) / 255.0f;
        float dg = ((dst_raw >> 8)  & 0xFF) / 255.0f;
        float db = ((dst_raw >> 16) & 0xFF) / 255.0f;
        float da = ((dst_raw >> 24) & 0xFF) / 255.0f;

        float sf_r = get_blend_factor(g_gl_state.blend_src, r, dr);
        float sf_g = get_blend_factor(g_gl_state.blend_src, g, dg);
        float sf_b = get_blend_factor(g_gl_state.blend_src, b, db);
        float sf_a = get_blend_factor(g_gl_state.blend_src, a, da);
        float df_r = get_blend_factor(g_gl_state.blend_dst, r, dr);
        float df_g = get_blend_factor(g_gl_state.blend_dst, g, dg);
        float df_b = get_blend_factor(g_gl_state.blend_dst, b, db);
        float df_a = get_blend_factor(g_gl_state.blend_dst, a, da);

        r = r * sf_r + dr * df_r;
        g = g * sf_g + dg * df_g;
        b = b * sf_b + db * df_b;
        a = a * sf_a + da * df_a;
    }

    if (g_gl_state.color_logic_op_enabled) {
        unsigned int src_raw = (unsigned int)(
            ((int)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) |
            ((int)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f) << 16) |
            ((int)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f) << 8)  |
            ((int)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f)));
        unsigned int res;
        switch (g_gl_state.logic_op) {
        case 0x1500: res = 0;                                   break;
        case 0x150F: res = 0xFFFFFFFF;                          break;
        case 0x1503: res = src_raw;                              break;
        case 0x150C: res = ~src_raw;                             break;
        case 0x1505: res = dst_raw;                              break;
        case 0x150A: res = ~dst_raw;                             break;
        case 0x1501: res = src_raw & dst_raw;                    break;
        case 0x150E: res = ~(src_raw & dst_raw);                 break;
        case 0x1507: res = src_raw | dst_raw;                    break;
        case 0x1508: res = ~(src_raw | dst_raw);                 break;
        case 0x1506: res = src_raw ^ dst_raw;                    break;
        case 0x1509: res = ~(src_raw ^ dst_raw);                 break;
        case 0x1502: res = src_raw & ~dst_raw;                   break;
        case 0x1504: res = ~src_raw & dst_raw;                   break;
        case 0x150B: res = src_raw | ~dst_raw;                   break;
        case 0x150D: res = ~src_raw | dst_raw;                   break;
        default:     res = src_raw;                              break;
        }
        fb->color_buffer[idx] = res;
        return;
    }

    int ri = (int)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    int gi = (int)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    int bi = (int)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    int ai = (int)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);

    fb->color_buffer[idx] = (unsigned int)((ai << 24) | (bi << 16) | (gi << 8) | ri);
}

static inline __attribute__((always_inline))
int scissor_test(int x, int y)
{
    if (!g_gl_state.scissor_test)
        return 1;
    int sx = g_gl_state.scissor_box[0];
    int sy = g_gl_state.scissor_box[1];
    int sw = g_gl_state.scissor_box[2];
    int sh = g_gl_state.scissor_box[3];
    return (x >= sx && x < sx + sw && y >= sy && y < sy + sh);
}

/* ============================================================
 * OPTIMIZED TRIANGLE RASTERIZER
 *
 * Optimizations applied:
 *   1. Fixed-point viewport transform (12.4) -- avoids float
 *      division in screen-to-clip mapping.
 *   2. Incremental scanline edge functions -- precompute step
 *      values per triangle, add them per pixel/row.
 *   3. 4x4 block rendering -- process pixels in cache-line-
 *      aligned blocks for better L1 utilization.
 *   4. Early depth test -- compute depth before fragment
 *      shader; skip shader on depth-fail.
 *   5. Branchless depth test -- ternary chain compiles to cmov.
 *   6. Inline hot functions -- always_inline eliminates
 *      call/return overhead in the inner loop.
 * ============================================================ */

void gl_rasterize_triangle(gl_triangle_t *tri)
{
    gl_framebuffer_t *fb = get_framebuffer();
    if (!fb || !fb->color_buffer) return;

    gl_stats_record_triangle();

    gl_vertex_t *v0 = &tri->v[0];
    gl_vertex_t *v1 = &tri->v[1];
    gl_vertex_t *v2 = &tri->v[2];

    /* ---- Fixed-point viewport transform (12.4) ---- */
    int fp_half_w = (fb->width  * FP_SCALE) / 2;
    int fp_half_h = (fb->height * FP_SCALE) / 2;
    int cx = fp_half_w + FP_SCALE / 2;
    int cy = fp_half_h + FP_SCALE / 2;

    int sx0 = (int)(v0->clip_pos.x * fp_half_w) + cx;
    int sy0 = (int)(v0->clip_pos.y * fp_half_h) + cy;
    int sx1 = (int)(v1->clip_pos.x * fp_half_w) + cx;
    int sy1 = (int)(v1->clip_pos.y * fp_half_h) + cy;
    int sx2 = (int)(v2->clip_pos.x * fp_half_w) + cx;
    int sy2 = (int)(v2->clip_pos.y * fp_half_h) + cy;

    /* ---- Edge function area (scaled by FP_SCALE^2) ---- */
    int area = orient2d(sx0, sy0, sx1, sy1, sx2, sy2);

    /* ---- Face culling ---- */
    if (g_gl_state.cull_face) {
        int ccw = (g_gl_state.front_face == 0x0901);
        if (g_gl_state.cull_face_mode == 0x0405) {
            if (ccw && area < 0) return;
            if (!ccw && area > 0) return;
        } else if (g_gl_state.cull_face_mode == 0x0404) {
            if (ccw && area > 0) return;
            if (!ccw && area < 0) return;
        } else {
            return;
        }
    }

    /* ---- Bounding box in pixel coords ---- */
    int min_x = min3i(sx0, sx1, sx2) >> FP_SHIFT;
    int min_y = min3i(sy0, sy1, sy2) >> FP_SHIFT;
    int max_x = max3i(sx0, sx1, sx2) >> FP_SHIFT;
    int max_y = max3i(sy0, sy1, sy2) >> FP_SHIFT;

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width)  max_x = fb->width  - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;

    /* ---- Precompute barycentric scale ---- */
    float abs_area = (area < 0) ? (float)(-area) : (float)area;
    float inv_area = (abs_area == 0.0f) ? 0.0f : 1.0f / abs_area;
    if (area < 0) inv_area = -inv_area;

    /* ---- Incremental edge-function step values ----
     * Moving right by 1 pixel: edge += step_x
     * Moving down by 1 pixel:  edge += step_y
     * Derived from orient2d linearity. */
    int w0_step_x = (sy1 - sy2) * FP_SCALE;
    int w1_step_x = (sy2 - sy0) * FP_SCALE;
    int w2_step_x = (sy0 - sy1) * FP_SCALE;
    int w0_step_y = (sx2 - sx1) * FP_SCALE;
    int w1_step_y = (sx0 - sx2) * FP_SCALE;
    int w2_step_y = (sx1 - sx0) * FP_SCALE;

    /* ---- Cache state for early-out decisions ---- */
    int early_z      = g_gl_state.depth_test;
    int has_stencil  = g_gl_state.stencil_test;
    int has_polygon_offset = (g_gl_state.polygon_offset_factor != 0.0f ||
                              g_gl_state.polygon_offset_units  != 0.0f);
    float poly_bias = 0.0f;
    if (has_polygon_offset) {
        poly_bias = g_gl_state.polygon_offset_factor * 0.00001f +
                    g_gl_state.polygon_offset_units  * 0.0000001f;
    }

    /* ============================================================
     * 4x4 BLOCK RASTERIZATION
     *
     * The bounding box is aligned to 4-pixel boundaries and
     * processed in 4x4 blocks.  Each block's depth/color data
     * fits in 1-2 cache lines, improving L1 hit rate vs. a
     * pure scanline walk.
     *
     * Within each block we use incremental edge-function
     * updates: precompute the value at the block origin, then
     * add step_x per column and step_y per row.
     * ============================================================ */
    int blk_min_x = min_x & ~3;
    int blk_min_y = min_y & ~3;
    int blk_max_x = (max_x + 3) & ~3;
    int blk_max_y = (max_y + 3) & ~3;

    for (int by = blk_min_y; by <= blk_max_y; by += 4) {
        for (int bx = blk_min_x; bx <= blk_max_x; bx += 4) {
            gl_stats_record_block(0);

            /* Edge functions at block top-left corner (fixed-point) */
            int fp_bx = bx << FP_SHIFT;
            int fp_by = by << FP_SHIFT;
            int w0_row = orient2d(sx1, sy1, sx2, sy2, fp_bx, fp_by);
            int w1_row = orient2d(sx2, sy2, sx0, sy0, fp_bx, fp_by);
            int w2_row = orient2d(sx0, sy0, sx1, sy1, fp_bx, fp_by);

            /* ---- Block-level quick reject ----
             * If all edge functions are negative at both the
             * top-left and bottom-right corners, the block is
             * fully outside the triangle. Skip it. */
            int w0_br = w0_row + 3 * w0_step_x + 3 * w0_step_y;
            int w1_br = w1_row + 3 * w1_step_x + 3 * w1_step_y;
            int w2_br = w2_row + 3 * w2_step_x + 3 * w2_step_y;

            if ((w0_row < 0 && w0_br < 0) ||
                (w1_row < 0 && w1_br < 0) ||
                (w2_row < 0 && w2_br < 0)) {
                gl_stats_record_block(1);
                continue;
            }

            /* ---- Process 4 rows of the block ---- */
            for (int dy = 0; dy < 4; dy++) {
                int py = by + dy;
                if (py > max_y) break;

                int w0 = w0_row;
                int w1 = w1_row;
                int w2 = w2_row;

                for (int dx = 0; dx < 4; dx++) {
                    int px = bx + dx;
                    if (px > max_x) break;

                    /* Incremental edge update (skip first pixel) */
                    if (dx > 0) {
                        w0 += w0_step_x;
                        w1 += w1_step_x;
                        w2 += w2_step_x;
                    }

                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;

                    if (!scissor_test(px, py))
                        continue;

                    float b0 = (float)w0 * inv_area;
                    float b1 = (float)w1 * inv_area;
                    float b2 = (float)w2 * inv_area;

                    /* ---- Compute depth (cheap) ---- */
                    float depth = b0 * v0->eye_z + b1 * v1->eye_z + b2 * v2->eye_z;

                    if (has_polygon_offset)
                        depth += poly_bias;

                    /* ---- EARLY DEPTH TEST ----
                     * Test depth BEFORE the fragment shader.
                     * When depth_write is on and func is the
                     * common LESS/LEQUAL, we can safely write
                     * depth here and skip the shader entirely
                     * on depth-fail. This is the "early z"
                     * optimization that GPUs use. */
                    if (early_z) {
                        int didx = py * fb->stride + px;
                        if (!depth_func_pass(g_gl_state.depth_func, depth, fb->depth_buffer[didx])) {
                            gl_stats_record_depth(0, 1);
                            continue;
                        }
                        fb->depth_buffer[didx] = depth;
                    }

                    /* ---- Stencil test ---- */
                    int stencil_pass = 1;
                    if (has_stencil) {
                        int didx = py * fb->stride + px;
                        unsigned char sval = fb->stencil_buffer[didx];
                        stencil_pass = stencil_func_pass(g_gl_state.stencil_func, sval,
                                                         (unsigned char)g_gl_state.stencil_ref,
                                                         (unsigned char)g_gl_state.stencil_val_mask);
                    }

                    /* ---- Fragment shader (only if we got here) ---- */
                    gl_fragment_t frag;
                    frag.x = (float)px;
                    frag.y = (float)py;
                    frag.discard = 0;

                    interpolate_fragment(&frag, v0, v1, v2, b0, b1, b2);

                    if (g_gl_fragment_shader)
                        g_gl_fragment_shader(&frag);

                    if (frag.discard)
                        continue;

                    gl_stats_record_pixels(1);
                    write_pixel(fb, px, py, frag.r, frag.g, frag.b, frag.a);

                    /* ---- Stencil update ---- */
                    if (has_stencil) {
                        int didx = py * fb->stride + px;
                        unsigned char sval = fb->stencil_buffer[didx];
                        unsigned char ref  = (unsigned char)g_gl_state.stencil_ref;
                        unsigned char wmask = (unsigned char)g_gl_state.stencil_write_mask;
                        unsigned char new_val;
                        if (stencil_pass) {
                            new_val = apply_stencil_op(g_gl_state.stencil_dppass, sval, ref);
                        } else {
                            new_val = apply_stencil_op(g_gl_state.stencil_dpfail, sval, ref);
                        }
                        fb->stencil_buffer[didx] = (sval & ~wmask) | (new_val & wmask);
                    }
                }

                /* ---- Advance to next scanline ---- */
                w0_row += w0_step_y;
                w1_row += w1_step_y;
                w2_row += w2_step_y;
            }
        }
    }
}

/* ============================================================
 * LINE RASTERIZER (Bresenham)
 * ============================================================ */

void gl_rasterize_line(gl_vertex_t *v0, gl_vertex_t *v1)
{
    gl_framebuffer_t *fb = get_framebuffer();
    if (!fb || !fb->color_buffer) return;

    int x0 = (int)(v0->clip_pos.x * 0.5f * fb->width  + 0.5f * fb->width  + 0.5f);
    int y0 = (int)(v0->clip_pos.y * 0.5f * fb->height + 0.5f * fb->height + 0.5f);
    int x1 = (int)(v1->clip_pos.x * 0.5f * fb->width  + 0.5f * fb->width  + 0.5f);
    int y1 = (int)(v1->clip_pos.y * 0.5f * fb->height + 0.5f * fb->height + 0.5f);

    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = dx > 0 ? dx : -dx;
    if (dy > steps) steps = dy;
    if (steps == 0) steps = 1;

    float inv = 1.0f / (float)steps;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i * inv;
        int px = x0 + (int)(dx * t + 0.5f);
        int py = y0 + (int)(dy * t + 0.5f);

        if (!scissor_test(px, py))
            continue;
        if (px < 0 || px >= fb->width || py < 0 || py >= fb->height)
            continue;

        float w0 = v0->clip_pos.w;
        float w1 = v1->clip_pos.w;
        float inv_w = (1.0f - t) / w0 + t / w1;
        if (inv_w == 0.0f) inv_w = 0.0001f;
        float nw0 = ((1.0f - t) / w0) / inv_w;
        float nw1 = (t / w1) / inv_w;

        gl_fragment_t frag;
        frag.x = (float)px;
        frag.y = (float)py;
        frag.discard = 0;

        frag.r = nw0 * v0->color.x + nw1 * v1->color.x;
        frag.g = nw0 * v0->color.y + nw1 * v1->color.y;
        frag.b = nw0 * v0->color.z + nw1 * v1->color.z;
        frag.a = nw0 * v0->color.w + nw1 * v1->color.w;

        frag.u = nw0 * v0->texcoord.x + nw1 * v1->texcoord.x;
        frag.v = nw0 * v0->texcoord.y + nw1 * v1->texcoord.y;

        frag.nx = nw0 * v0->normal.x + nw1 * v1->normal.x;
        frag.ny = nw0 * v0->normal.y + nw1 * v1->normal.y;
        frag.nz = nw0 * v0->normal.z + nw1 * v1->normal.z;

        frag.depth = (1.0f - t) * v0->eye_z + t * v1->eye_z;

        if (g_gl_state.depth_test) {
            int didx = py * fb->stride + px;
            if (!depth_func_pass(g_gl_state.depth_func, frag.depth, fb->depth_buffer[didx]))
                continue;
            fb->depth_buffer[didx] = frag.depth;
        }

        int stencil_pass = 1;
        if (g_gl_state.stencil_test) {
            int didx = py * fb->stride + px;
            unsigned char sval = fb->stencil_buffer[didx];
            stencil_pass = stencil_func_pass(g_gl_state.stencil_func, sval,
                                             (unsigned char)g_gl_state.stencil_ref,
                                             (unsigned char)g_gl_state.stencil_val_mask);
        }

        if (g_gl_fragment_shader)
            g_gl_fragment_shader(&frag);

        if (frag.discard)
            continue;

        write_pixel(fb, px, py, frag.r, frag.g, frag.b, frag.a);

        if (g_gl_state.stencil_test) {
            int didx = py * fb->stride + px;
            unsigned char sval = fb->stencil_buffer[didx];
            unsigned char ref = (unsigned char)g_gl_state.stencil_ref;
            unsigned char wmask = (unsigned char)g_gl_state.stencil_write_mask;
            unsigned char new_val;
            if (stencil_pass) {
                new_val = apply_stencil_op(g_gl_state.stencil_dppass, sval, ref);
            } else {
                new_val = apply_stencil_op(g_gl_state.stencil_dpfail, sval, ref);
            }
            fb->stencil_buffer[didx] = (sval & ~wmask) | (new_val & wmask);
        }
    }
}

/* ============================================================
 * POINT RASTERIZER
 * ============================================================ */

void gl_rasterize_point(gl_vertex_t *v)
{
    gl_framebuffer_t *fb = get_framebuffer();
    if (!fb || !fb->color_buffer) return;

    int px = (int)(v->clip_pos.x * 0.5f * fb->width  + 0.5f * fb->width  + 0.5f);
    int py = (int)(v->clip_pos.y * 0.5f * fb->height + 0.5f * fb->height + 0.5f);

    if (!scissor_test(px, py))
        return;
    if (px < 0 || px >= fb->width || py < 0 || py >= fb->height)
        return;

    gl_fragment_t frag;
    frag.x = (float)px;
    frag.y = (float)py;
    frag.r = v->color.x;
    frag.g = v->color.y;
    frag.b = v->color.z;
    frag.a = v->color.w;
    frag.u = v->texcoord.x;
    frag.v = v->texcoord.y;
    frag.nx = v->normal.x;
    frag.ny = v->normal.y;
    frag.nz = v->normal.z;
    frag.depth = v->eye_z;
    frag.discard = 0;

    if (g_gl_state.depth_test) {
        int didx = py * fb->stride + px;
        if (!depth_func_pass(g_gl_state.depth_func, frag.depth, fb->depth_buffer[didx]))
            return;
        fb->depth_buffer[didx] = frag.depth;
    }

    int stencil_pass = 1;
    if (g_gl_state.stencil_test) {
        int didx = py * fb->stride + px;
        unsigned char sval = fb->stencil_buffer[didx];
        stencil_pass = stencil_func_pass(g_gl_state.stencil_func, sval,
                                         (unsigned char)g_gl_state.stencil_ref,
                                         (unsigned char)g_gl_state.stencil_val_mask);
    }

    if (g_gl_fragment_shader)
        g_gl_fragment_shader(&frag);

    if (frag.discard)
        return;

    write_pixel(fb, px, py, frag.r, frag.g, frag.b, frag.a);

    if (g_gl_state.stencil_test) {
        int didx = py * fb->stride + px;
        unsigned char sval = fb->stencil_buffer[didx];
        unsigned char ref = (unsigned char)g_gl_state.stencil_ref;
        unsigned char wmask = (unsigned char)g_gl_state.stencil_write_mask;
        unsigned char new_val;
        if (stencil_pass) {
            new_val = apply_stencil_op(g_gl_state.stencil_dppass, sval, ref);
        } else {
            new_val = apply_stencil_op(g_gl_state.stencil_dpfail, sval, ref);
        }
        fb->stencil_buffer[didx] = (sval & ~wmask) | (new_val & wmask);
    }
}

/* ============================================================
 * BUFFER CLEAR
 * ============================================================ */

void gl_clear_buffers(unsigned int mask)
{
    gl_framebuffer_t *fb = get_framebuffer();
    if (!fb) return;

    if (mask & 0x00004000) {
        int ri = (int)(g_gl_state.clear_color[0] * 255.0f + 0.5f);
        int gi = (int)(g_gl_state.clear_color[1] * 255.0f + 0.5f);
        int bi = (int)(g_gl_state.clear_color[2] * 255.0f + 0.5f);
        int ai = (int)(g_gl_state.clear_color[3] * 255.0f + 0.5f);
        unsigned int color = (unsigned int)((ai << 24) | (bi << 16) | (gi << 8) | ri);
        int total = fb->stride * fb->height;
        for (int i = 0; i < total; i++)
            fb->color_buffer[i] = color;
    }

    if (mask & 0x00000100) {
        float depth = (float)g_gl_state.clear_depth;
        int total = fb->stride * fb->height;
        for (int i = 0; i < total; i++)
            fb->depth_buffer[i] = depth;
    }

    if (mask & 0x00000400) {
        unsigned char val = (unsigned char)g_gl_state.clear_stencil;
        int total = fb->stride * fb->height;
        for (int i = 0; i < total; i++)
            fb->stencil_buffer[i] = val;
    }
}
