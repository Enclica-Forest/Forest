#ifndef GL_SIMD_H
#define GL_SIMD_H

#include <stdint.h>

/*
 * SIMD-like helpers for the software GL renderer.
 * Uses integer tricks to process multiple pixel components at once,
 * avoiding per-channel branching and reducing instruction count.
 */

/* Fast divide-by-255: (x + 1 + (x >> 8)) >> 8
 * Error <= 1 for all inputs in [0, 255*255+255]. */
static inline __attribute__((always_inline))
uint32_t gl_div255(uint32_t x)
{
    return (x + 1 + (x >> 8)) >> 8;
}

/*
 * Blend source pixel over destination pixel using alpha.
 * dst, src: ABGR packed pixels (A in bits 24-31, R in 0-7).
 * alpha: source alpha (0-255).
 * Returns: blended ABGR pixel.
 * Processes all 4 channels in parallel via integer packing.
 */
static inline __attribute__((always_inline))
uint32_t blend4pixels(uint32_t dst, uint32_t src, uint8_t alpha)
{
    uint32_t inv_a = 255 - alpha;
    uint32_t dr = dst & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = (dst >> 16) & 0xFF;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t sr = src & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = (src >> 16) & 0xFF;
    uint32_t sa = (src >> 24) & 0xFF;
    uint32_t r = gl_div255(sr * alpha + dr * inv_a);
    uint32_t g = gl_div255(sg * alpha + dg * inv_a);
    uint32_t b = gl_div255(sb * alpha + db * inv_a);
    uint32_t a = gl_div255(sa * alpha + da * inv_a);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

/*
 * Test 4 fragment depths against the depth buffer (GL_LESS semantics).
 * Produces a 4-bit mask where bit i is set if depths[i] < depth_buf[i].
 * Processes 4 depth comparisons, enabling the caller to branch once
 * for the group instead of per-pixel.
 */
static inline __attribute__((always_inline))
void depth_test4(const float *depth_buf, const float *depths, int *mask)
{
    *mask = 0;
    *mask |= (depths[0] < depth_buf[0]) ? 1 : 0;
    *mask |= (depths[1] < depth_buf[1]) ? 2 : 0;
    *mask |= (depths[2] < depth_buf[2]) ? 4 : 0;
    *mask |= (depths[3] < depth_buf[3]) ? 8 : 0;
}

/*
 * Pack 4 floats [0..1] into a single ABGR uint32.
 * Uses gl_div255 for fast conversion.
 */
static inline __attribute__((always_inline))
uint32_t pack_rgba4f(float r, float g, float b, float a)
{
    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f);
    return (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

#endif /* GL_SIMD_H */
