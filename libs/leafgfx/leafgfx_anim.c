/**
 * LeafGFX Animation System Implementation
 *
 * Modern animation system with spring physics and easing functions.
 */

#include "leafgfx_anim.h"
#include <stdlib.h>

// =============================================================================
// Fixed-Point Helpers
// =============================================================================

#define FP_ONE      GFX_FP_ONE
#define FP_HALF     GFX_FP_HALF
#define FP_SHIFT    GFX_FP_BITS

// Sine lookup table (quarter period, 0-90 degrees, 256 entries)
// Values are in 16.16 fixed point
static const int32_t sine_table[257] = {
    0, 402, 804, 1206, 1608, 2010, 2412, 2814,
    3216, 3617, 4019, 4420, 4821, 5222, 5623, 6023,
    6424, 6824, 7224, 7623, 8022, 8421, 8820, 9218,
    9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391,
    12785, 13180, 13573, 13966, 14359, 14751, 15143, 15534,
    15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699,
    22078, 22457, 22834, 23210, 23586, 23961, 24335, 24708,
    25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538,
    30893, 31248, 31600, 31952, 32303, 32652, 33000, 33347,
    33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716,
    39040, 39362, 39683, 40002, 40320, 40636, 40951, 41264,
    41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056,
    46341, 46624, 46906, 47186, 47464, 47741, 48015, 48288,
    48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398,
    52639, 52878, 53114, 53349, 53581, 53812, 54040, 54267,
    54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607,
    57798, 57986, 58172, 58356, 58538, 58718, 58896, 59071,
    59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568,
    61705, 61839, 61971, 62101, 62228, 62353, 62476, 62596,
    62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197,
    64277, 64355, 64430, 64503, 64573, 64641, 64707, 64770,
    64830, 64889, 64945, 64998, 65049, 65098, 65144, 65188,
    65229, 65268, 65305, 65339, 65371, 65400, 65427, 65452,
    65474, 65494, 65512, 65527, 65539, 65550, 65558, 65563,
    65536  // GFX_FP_ONE
};

// Fast sine approximation using lookup table
static int32_t fast_sin(int32_t angle_fp) {
    // angle_fp is in [0, GFX_FP_ONE] representing [0, 2*PI]
    int32_t normalized = angle_fp & (FP_ONE - 1);  // Wrap to [0, FP_ONE)

    int32_t quadrant = (normalized * 4) >> FP_SHIFT;
    int32_t index = (normalized * 1024) >> FP_SHIFT;
    index &= 1023;

    int32_t table_idx;
    bool negate = false;

    switch (quadrant & 3) {
        case 0:  // 0-90
            table_idx = index >> 2;
            break;
        case 1:  // 90-180
            table_idx = 256 - (index >> 2);
            break;
        case 2:  // 180-270
            table_idx = index >> 2;
            negate = true;
            break;
        case 3:  // 270-360
        default:
            table_idx = 256 - (index >> 2);
            negate = true;
            break;
    }

    if (table_idx > 256) table_idx = 256;
    int32_t result = sine_table[table_idx];

    return negate ? -result : result;
}

// Fast cosine
static int32_t fast_cos(int32_t angle_fp) {
    return fast_sin(angle_fp + FP_ONE / 4);  // cos(x) = sin(x + PI/2)
}

// =============================================================================
// Spring Physics Implementation
// =============================================================================

void gfx_spring_init(gfx_spring_t* spring, int32_t initial, int32_t target) {
    if (!spring) return;

    spring->position = initial << FP_SHIFT;
    spring->target = target << FP_SHIFT;
    spring->velocity = 0;

    // Default: iOS/macOS-like spring
    spring->stiffness = 170 * FP_ONE;
    spring->damping = 26 * FP_ONE;
    spring->mass = FP_ONE;

    spring->rest_threshold = FP_ONE / 100;  // Small threshold
    spring->at_rest = false;
}

void gfx_spring_init_custom(gfx_spring_t* spring, int32_t initial, int32_t target,
                            int32_t stiffness, int32_t damping, int32_t mass) {
    if (!spring) return;

    spring->position = initial << FP_SHIFT;
    spring->target = target << FP_SHIFT;
    spring->velocity = 0;

    spring->stiffness = stiffness;
    spring->damping = damping;
    spring->mass = mass > 0 ? mass : FP_ONE;

    spring->rest_threshold = FP_ONE / 100;
    spring->at_rest = false;
}

void gfx_spring_preset_gentle(gfx_spring_t* spring) {
    if (!spring) return;
    spring->stiffness = 120 * FP_ONE;
    spring->damping = 14 * FP_ONE;
    spring->mass = FP_ONE;
}

void gfx_spring_preset_snappy(gfx_spring_t* spring) {
    if (!spring) return;
    spring->stiffness = 400 * FP_ONE;
    spring->damping = 30 * FP_ONE;
    spring->mass = FP_ONE;
}

void gfx_spring_preset_bouncy(gfx_spring_t* spring) {
    if (!spring) return;
    spring->stiffness = 200 * FP_ONE;
    spring->damping = 10 * FP_ONE;
    spring->mass = FP_ONE;
}

void gfx_spring_preset_stiff(gfx_spring_t* spring) {
    if (!spring) return;
    spring->stiffness = 300 * FP_ONE;
    spring->damping = 50 * FP_ONE;
    spring->mass = FP_ONE;
}

void gfx_spring_update(gfx_spring_t* spring, uint32_t dt_ms) {
    if (!spring || spring->at_rest) return;

    // Convert dt to seconds in fixed point (scaled down for stability)
    // dt_fp = dt_ms / 1000 * FP_ONE = dt_ms * 65.536
    int32_t dt_fp = (dt_ms * FP_ONE) / 1000;
    if (dt_fp <= 0) dt_fp = FP_ONE / 60;  // Default to 16ms

    // Spring force: F = -k * (x - target)
    int32_t displacement = spring->position - spring->target;
    int64_t spring_force = -((int64_t)spring->stiffness * displacement) >> FP_SHIFT;

    // Damping force: F = -c * v
    int64_t damping_force = -((int64_t)spring->damping * spring->velocity) >> FP_SHIFT;

    // Total acceleration: a = (F_spring + F_damping) / mass
    int64_t total_force = spring_force + damping_force;
    int32_t acceleration = (int32_t)((total_force << FP_SHIFT) / spring->mass);

    // Update velocity: v += a * dt
    spring->velocity += (int32_t)(((int64_t)acceleration * dt_fp) >> FP_SHIFT);

    // Update position: x += v * dt
    spring->position += (int32_t)(((int64_t)spring->velocity * dt_fp) >> FP_SHIFT);

    // Check if at rest
    int32_t dist = displacement < 0 ? -displacement : displacement;
    int32_t vel = spring->velocity < 0 ? -spring->velocity : spring->velocity;

    if (dist < spring->rest_threshold && vel < spring->rest_threshold) {
        spring->position = spring->target;
        spring->velocity = 0;
        spring->at_rest = true;
    }
}

void gfx_spring_set_target(gfx_spring_t* spring, int32_t target) {
    if (!spring) return;
    spring->target = target << FP_SHIFT;
    spring->at_rest = false;
}

int32_t gfx_spring_get_position(gfx_spring_t* spring) {
    if (!spring) return 0;
    return spring->position >> FP_SHIFT;
}

bool gfx_spring_is_at_rest(gfx_spring_t* spring) {
    return spring ? spring->at_rest : true;
}

void gfx_spring_reset(gfx_spring_t* spring, int32_t position) {
    if (!spring) return;
    spring->position = position << FP_SHIFT;
    spring->target = spring->position;
    spring->velocity = 0;
    spring->at_rest = true;
}

// =============================================================================
// Tween Animation Implementation
// =============================================================================

void gfx_tween_init(gfx_tween_t* tween, int32_t start, int32_t end,
                    uint32_t duration_ms, int32_t (*easing)(int32_t)) {
    if (!tween) return;

    tween->start_value = start << FP_SHIFT;
    tween->end_value = end << FP_SHIFT;
    tween->duration_ms = duration_ms > 0 ? duration_ms : 1;
    tween->elapsed_ms = 0;
    tween->easing = easing ? easing : gfx_ease_linear;

    tween->state = GFX_ANIM_STATE_IDLE;
    tween->direction = GFX_ANIM_DIR_FORWARD;
    tween->iterations = 1;
    tween->current_iter = 0;
}

int32_t gfx_tween_update(gfx_tween_t* tween, uint32_t dt_ms) {
    if (!tween || tween->state != GFX_ANIM_STATE_RUNNING) {
        return gfx_tween_get_value(tween);
    }

    tween->elapsed_ms += dt_ms;

    if (tween->elapsed_ms >= tween->duration_ms) {
        if (tween->iterations < 0 ||
            tween->current_iter < tween->iterations - 1) {
            // Loop
            tween->current_iter++;
            tween->elapsed_ms = tween->elapsed_ms % tween->duration_ms;

            if (tween->direction == GFX_ANIM_DIR_ALTERNATE) {
                int32_t tmp = tween->start_value;
                tween->start_value = tween->end_value;
                tween->end_value = tmp;
            }
        } else {
            tween->elapsed_ms = tween->duration_ms;
            tween->state = GFX_ANIM_STATE_COMPLETED;
        }
    }

    return gfx_tween_get_value(tween);
}

int32_t gfx_tween_get_value(gfx_tween_t* tween) {
    if (!tween) return 0;

    uint32_t progress = (tween->elapsed_ms * FP_ONE) / tween->duration_ms;
    if (progress > FP_ONE) progress = FP_ONE;

    int32_t eased = tween->easing ? tween->easing(progress) : progress;

    int64_t range = tween->end_value - tween->start_value;
    int32_t value = tween->start_value + (int32_t)((range * eased) >> FP_SHIFT);

    return value >> FP_SHIFT;
}

void gfx_tween_start(gfx_tween_t* tween) {
    if (!tween) return;
    tween->state = GFX_ANIM_STATE_RUNNING;
}

void gfx_tween_pause(gfx_tween_t* tween) {
    if (!tween) return;
    if (tween->state == GFX_ANIM_STATE_RUNNING) {
        tween->state = GFX_ANIM_STATE_PAUSED;
    }
}

void gfx_tween_resume(gfx_tween_t* tween) {
    if (!tween) return;
    if (tween->state == GFX_ANIM_STATE_PAUSED) {
        tween->state = GFX_ANIM_STATE_RUNNING;
    }
}

void gfx_tween_reset(gfx_tween_t* tween) {
    if (!tween) return;
    tween->elapsed_ms = 0;
    tween->current_iter = 0;
    tween->state = GFX_ANIM_STATE_IDLE;
}

void gfx_tween_reverse(gfx_tween_t* tween) {
    if (!tween) return;
    int32_t tmp = tween->start_value;
    tween->start_value = tween->end_value;
    tween->end_value = tmp;
    tween->elapsed_ms = tween->duration_ms - tween->elapsed_ms;
}

void gfx_tween_set_loop(gfx_tween_t* tween, int32_t iterations, gfx_anim_direction_t dir) {
    if (!tween) return;
    tween->iterations = iterations;
    tween->direction = dir;
}

bool gfx_tween_is_complete(gfx_tween_t* tween) {
    return tween ? tween->state == GFX_ANIM_STATE_COMPLETED : true;
}

// =============================================================================
// Easing Functions Implementation
// =============================================================================

int32_t gfx_ease_linear(int32_t t) {
    return t;
}

// These easing functions are already defined in leafgfx.c - use extern declarations
extern int32_t gfx_ease_in_quad(int32_t t);
extern int32_t gfx_ease_out_quad(int32_t t);
extern int32_t gfx_ease_in_out_quad(int32_t t);
extern int32_t gfx_ease_in_cubic(int32_t t);
extern int32_t gfx_ease_out_cubic(int32_t t);
extern int32_t gfx_ease_in_out_cubic(int32_t t);
extern int32_t gfx_ease_out_bounce(int32_t t);

int32_t gfx_ease_in_quart(int32_t t) {
    int32_t t2 = GFX_FP_MUL(t, t);
    return GFX_FP_MUL(t2, t2);
}

int32_t gfx_ease_out_quart(int32_t t) {
    int32_t f = t - FP_ONE;
    int32_t f2 = GFX_FP_MUL(f, f);
    return FP_ONE - GFX_FP_MUL(f2, f2);
}

int32_t gfx_ease_in_out_quart(int32_t t) {
    if (t < FP_HALF) {
        int32_t t2 = GFX_FP_MUL(t, t);
        return 8 * GFX_FP_MUL(t2, t2);
    } else {
        int32_t f = t - FP_ONE;
        int32_t f2 = GFX_FP_MUL(f, f);
        return FP_ONE - 8 * GFX_FP_MUL(f2, f2);
    }
}

int32_t gfx_ease_in_quint(int32_t t) {
    int32_t t2 = GFX_FP_MUL(t, t);
    return GFX_FP_MUL(GFX_FP_MUL(t2, t2), t);
}

int32_t gfx_ease_out_quint(int32_t t) {
    int32_t f = t - FP_ONE;
    int32_t f2 = GFX_FP_MUL(f, f);
    return GFX_FP_MUL(GFX_FP_MUL(f2, f2), f) + FP_ONE;
}

int32_t gfx_ease_in_out_quint(int32_t t) {
    if (t < FP_HALF) {
        int32_t t2 = GFX_FP_MUL(t, t);
        return 16 * GFX_FP_MUL(GFX_FP_MUL(t2, t2), t);
    } else {
        int32_t f = t - FP_ONE;
        int32_t f2 = GFX_FP_MUL(f, f);
        return 16 * GFX_FP_MUL(GFX_FP_MUL(f2, f2), f) + FP_ONE;
    }
}

int32_t gfx_ease_in_expo(int32_t t) {
    if (t == 0) return 0;
    // Approximate 2^(10*(t-1)) using shifts
    int32_t shifted = ((t - FP_ONE) * 10) >> FP_SHIFT;
    if (shifted < -10) return 0;
    return FP_ONE >> (-shifted);
}

int32_t gfx_ease_out_expo(int32_t t) {
    if (t >= FP_ONE) return FP_ONE;
    // Approximate 1 - 2^(-10*t)
    int32_t shifted = (t * 10) >> FP_SHIFT;
    if (shifted > 20) return FP_ONE;
    return FP_ONE - (FP_ONE >> shifted);
}

int32_t gfx_ease_in_out_expo(int32_t t) {
    if (t == 0) return 0;
    if (t >= FP_ONE) return FP_ONE;
    if (t < FP_HALF) {
        return gfx_ease_in_expo(t * 2) / 2;
    } else {
        return FP_HALF + gfx_ease_out_expo((t - FP_HALF) * 2) / 2;
    }
}

int32_t gfx_ease_in_circ(int32_t t) {
    // 1 - sqrt(1 - t^2)
    int32_t t2 = GFX_FP_MUL(t, t);
    int32_t inner = FP_ONE - t2;
    if (inner <= 0) return FP_ONE;
    // Approximate sqrt
    int32_t sqrt_val = inner;
    for (int i = 0; i < 5; i++) {
        sqrt_val = (sqrt_val + GFX_FP_DIV(inner, sqrt_val)) / 2;
    }
    return FP_ONE - sqrt_val;
}

int32_t gfx_ease_out_circ(int32_t t) {
    // sqrt(1 - (t-1)^2)
    int32_t f = t - FP_ONE;
    int32_t f2 = GFX_FP_MUL(f, f);
    int32_t inner = FP_ONE - f2;
    if (inner <= 0) return FP_ONE;
    int32_t sqrt_val = inner;
    for (int i = 0; i < 5; i++) {
        sqrt_val = (sqrt_val + GFX_FP_DIV(inner, sqrt_val)) / 2;
    }
    return sqrt_val;
}

int32_t gfx_ease_in_out_circ(int32_t t) {
    if (t < FP_HALF) {
        return gfx_ease_in_circ(t * 2) / 2;
    } else {
        return FP_HALF + gfx_ease_out_circ((t - FP_HALF) * 2) / 2;
    }
}

// Back (overshoot)
#define BACK_OVERSHOOT (((int64_t)1.70158 * FP_ONE) >> 0)  // ~111521 in 16.16

int32_t gfx_ease_in_back(int32_t t) {
    int32_t c1 = 111521;  // 1.70158 in 16.16
    int32_t c3 = c1 + FP_ONE;
    int32_t t2 = GFX_FP_MUL(t, t);
    int32_t t3 = GFX_FP_MUL(t2, t);
    return GFX_FP_MUL(c3, t3) - GFX_FP_MUL(c1, t2);
}

int32_t gfx_ease_out_back(int32_t t) {
    int32_t c1 = 111521;
    int32_t c3 = c1 + FP_ONE;
    int32_t f = t - FP_ONE;
    int32_t f2 = GFX_FP_MUL(f, f);
    int32_t f3 = GFX_FP_MUL(f2, f);
    return FP_ONE + GFX_FP_MUL(c3, f3) + GFX_FP_MUL(c1, f2);
}

int32_t gfx_ease_in_out_back(int32_t t) {
    int32_t c1 = 111521;
    int32_t c2 = (int32_t)(c1 * 1.525);

    if (t < FP_HALF) {
        int32_t t2 = GFX_FP_MUL(2 * t, 2 * t);
        return GFX_FP_MUL(t2, GFX_FP_MUL(c2 + FP_ONE, 2 * t) - c2) / 2;
    } else {
        int32_t f = 2 * t - 2 * FP_ONE;
        int32_t f2 = GFX_FP_MUL(f, f);
        return (GFX_FP_MUL(f2, GFX_FP_MUL(c2 + FP_ONE, f) + c2) + 2 * FP_ONE) / 2;
    }
}

int32_t gfx_ease_in_elastic(int32_t t) {
    if (t == 0) return 0;
    if (t >= FP_ONE) return FP_ONE;

    int32_t c4 = FP_ONE * 2 / 3;  // 2*PI/3 phase

    int32_t pow_term = gfx_ease_in_expo(t);
    int32_t angle = ((t - FP_ONE) * 10 - 3 * FP_ONE / 4) * c4 / FP_ONE;
    int32_t sin_term = fast_sin(angle);

    return -GFX_FP_MUL(pow_term, sin_term);
}

int32_t gfx_ease_out_elastic(int32_t t) {
    if (t == 0) return 0;
    if (t >= FP_ONE) return FP_ONE;

    int32_t c4 = FP_ONE * 2 / 3;

    int32_t pow_term = gfx_ease_out_expo(t);
    int32_t angle = (t * 10 - 3 * FP_ONE / 4) * c4 / FP_ONE;
    int32_t sin_term = fast_sin(angle);

    return FP_ONE - GFX_FP_MUL(FP_ONE - pow_term, sin_term);
}

int32_t gfx_ease_in_out_elastic(int32_t t) {
    if (t == 0) return 0;
    if (t >= FP_ONE) return FP_ONE;
    if (t < FP_HALF) {
        return gfx_ease_in_elastic(t * 2) / 2;
    } else {
        return FP_HALF + gfx_ease_out_elastic((t - FP_HALF) * 2) / 2;
    }
}

int32_t gfx_ease_in_bounce(int32_t t) {
    return FP_ONE - gfx_ease_out_bounce(FP_ONE - t);
}

// gfx_ease_out_bounce is defined in leafgfx.c - use the extern declaration above

int32_t gfx_ease_in_out_bounce(int32_t t) {
    if (t < FP_HALF) {
        return gfx_ease_in_bounce(t * 2) / 2;
    } else {
        return FP_HALF + gfx_ease_out_bounce((t - FP_HALF) * 2) / 2;
    }
}

/* gfx_ease_in_sine / gfx_ease_out_sine / gfx_ease_in_out_sine are implemented
 * in leafgfx.c (Taylor-series approximations with clamping).  They are not
 * redefined here to avoid multiple-definition link errors. */

int32_t gfx_ease_bezier(int32_t t, int32_t p1, int32_t p2) {
    // Cubic bezier with control points (0,0), (0.5,p1), (0.5,p2), (1,1)
    // Simplified: just blend between p1 and p2 based on t
    int32_t t2 = GFX_FP_MUL(t, t);
    int32_t t3 = GFX_FP_MUL(t2, t);
    int32_t mt = FP_ONE - t;
    int32_t mt2 = GFX_FP_MUL(mt, mt);
    int32_t mt3 = GFX_FP_MUL(mt2, mt);

    // B(t) = (1-t)^3 * P0 + 3(1-t)^2*t * P1 + 3(1-t)*t^2 * P2 + t^3 * P3
    // P0 = 0, P3 = 1
    int32_t b1 = 3 * GFX_FP_MUL(GFX_FP_MUL(mt2, t), p1);
    int32_t b2 = 3 * GFX_FP_MUL(GFX_FP_MUL(mt, t2), p2);

    return b1 + b2 + t3;
}

// =============================================================================
// Color Animation
// =============================================================================

uint32_t gfx_color_lerp(uint32_t c1, uint32_t c2, uint8_t t) {
    if (t == 0) return c1;
    if (t == 255) return c2;

    uint8_t a1 = (c1 >> 24) & 0xFF, a2 = (c2 >> 24) & 0xFF;
    uint8_t r1 = (c1 >> 16) & 0xFF, r2 = (c2 >> 16) & 0xFF;
    uint8_t g1 = (c1 >> 8) & 0xFF, g2 = (c2 >> 8) & 0xFF;
    uint8_t b1 = c1 & 0xFF, b2 = c2 & 0xFF;

    uint8_t a = a1 + (((int32_t)a2 - a1) * t) / 255;
    uint8_t r = r1 + (((int32_t)r2 - r1) * t) / 255;
    uint8_t g = g1 + (((int32_t)g2 - g1) * t) / 255;
    uint8_t b = b1 + (((int32_t)b2 - b1) * t) / 255;

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// RGB to HSL conversion helpers
static void rgb_to_hsl(uint8_t r, uint8_t g, uint8_t b, int32_t* h, int32_t* s, int32_t* l) {
    int32_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int32_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int32_t sum = max + min;

    *l = sum / 2;

    if (max == min) {
        *h = 0;
        *s = 0;
        return;
    }

    int32_t diff = max - min;
    *s = (*l > 127) ? (diff * 255) / (510 - sum) : (diff * 255) / sum;

    if (max == r) {
        *h = ((g - b) * 60) / diff;
        if (*h < 0) *h += 360;
    } else if (max == g) {
        *h = 120 + ((b - r) * 60) / diff;
    } else {
        *h = 240 + ((r - g) * 60) / diff;
    }
}

static int32_t hue_to_rgb_helper(int32_t p, int32_t q, int32_t t) {
    if (t < 0) t += 360;
    if (t >= 360) t -= 360;
    if (t < 60) return p + (q - p) * t / 60;
    if (t < 180) return q;
    if (t < 240) return p + (q - p) * (240 - t) / 60;
    return p;
}

static uint32_t hsl_to_rgb(int32_t h, int32_t s, int32_t l, uint8_t a) {
    if (s == 0) {
        return ((uint32_t)a << 24) | ((uint32_t)l << 16) | ((uint32_t)l << 8) | l;
    }

    int32_t q = (l < 128) ? l * (255 + s) / 255 : l + s - (l * s / 255);
    int32_t p = 2 * l - q;

    uint8_t r = (uint8_t)hue_to_rgb_helper(p, q, h + 120);
    uint8_t g = (uint8_t)hue_to_rgb_helper(p, q, h);
    uint8_t b = (uint8_t)hue_to_rgb_helper(p, q, h - 120);

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t gfx_color_lerp_hsl(uint32_t c1, uint32_t c2, uint8_t t) {
    if (t == 0) return c1;
    if (t == 255) return c2;

    uint8_t a1 = (c1 >> 24) & 0xFF, a2 = (c2 >> 24) & 0xFF;
    uint8_t r1 = (c1 >> 16) & 0xFF, r2 = (c2 >> 16) & 0xFF;
    uint8_t g1 = (c1 >> 8) & 0xFF, g2 = (c2 >> 8) & 0xFF;
    uint8_t b1 = c1 & 0xFF, b2 = c2 & 0xFF;

    int32_t h1, s1, l1, h2, s2, l2;
    rgb_to_hsl(r1, g1, b1, &h1, &s1, &l1);
    rgb_to_hsl(r2, g2, b2, &h2, &s2, &l2);

    // Interpolate in HSL space
    int32_t dh = h2 - h1;
    if (dh > 180) dh -= 360;
    if (dh < -180) dh += 360;

    int32_t h = h1 + (dh * t) / 255;
    if (h < 0) h += 360;
    if (h >= 360) h -= 360;

    int32_t s = s1 + ((s2 - s1) * t) / 255;
    int32_t l = l1 + ((l2 - l1) * t) / 255;
    uint8_t a = a1 + (((int32_t)a2 - a1) * t) / 255;

    return hsl_to_rgb(h, s, l, a);
}

// =============================================================================
// Animation Utilities
// =============================================================================

int32_t gfx_smoothstep(int32_t edge0, int32_t edge1, int32_t x) {
    if (x <= edge0) return 0;
    if (x >= edge1) return FP_ONE;

    int32_t t = GFX_FP_DIV(x - edge0, edge1 - edge0);
    // t * t * (3 - 2 * t)
    return GFX_FP_MUL(GFX_FP_MUL(t, t), 3 * FP_ONE - 2 * t);
}

int32_t gfx_smootherstep(int32_t edge0, int32_t edge1, int32_t x) {
    if (x <= edge0) return 0;
    if (x >= edge1) return FP_ONE;

    int32_t t = GFX_FP_DIV(x - edge0, edge1 - edge0);
    // t * t * t * (t * (t * 6 - 15) + 10)
    int32_t t3 = GFX_FP_MUL(GFX_FP_MUL(t, t), t);
    int32_t inner = GFX_FP_MUL(t, 6 * t - 15 * FP_ONE) + 10 * FP_ONE;
    return GFX_FP_MUL(t3, inner);
}

int32_t gfx_map(int32_t value, int32_t in_min, int32_t in_max,
                int32_t out_min, int32_t out_max) {
    if (in_max == in_min) return out_min;
    return out_min + ((int64_t)(value - in_min) * (out_max - out_min)) / (in_max - in_min);
}

int32_t gfx_clamp_fp(int32_t value, int32_t min, int32_t max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}
