/**
 * LeafGFX Animation System
 *
 * Modern animation system with spring physics, easing functions,
 * and animation sequencing for fluid UI animations.
 */

#ifndef LEAFGFX_ANIM_H
#define LEAFGFX_ANIM_H

#include "leafgfx.h"

// =============================================================================
// Animation Types
// =============================================================================

typedef enum {
    GFX_ANIM_STATE_IDLE,
    GFX_ANIM_STATE_RUNNING,
    GFX_ANIM_STATE_PAUSED,
    GFX_ANIM_STATE_COMPLETED
} gfx_anim_state_t;

typedef enum {
    GFX_ANIM_DIR_FORWARD,
    GFX_ANIM_DIR_BACKWARD,
    GFX_ANIM_DIR_ALTERNATE
} gfx_anim_direction_t;

// =============================================================================
// Spring Physics
// =============================================================================

/**
 * Spring animation state
 *
 * Uses a damped harmonic oscillator model for natural-feeling animations.
 * Default values give iOS/macOS-like spring feel.
 */
typedef struct {
    int32_t position;      // Current position (16.16 fixed point)
    int32_t velocity;      // Current velocity (16.16 fixed point)
    int32_t target;        // Target position (16.16 fixed point)

    int32_t stiffness;     // Spring constant (default: 170 * 65536)
    int32_t damping;       // Damping ratio (default: 26 * 65536)
    int32_t mass;          // Mass (default: 1 * 65536)

    int32_t rest_threshold; // Velocity threshold to consider at rest
    bool    at_rest;        // True when spring has settled
} gfx_spring_t;

/**
 * Initialize a spring with default parameters (stiffness=170, damping=26, mass=1)
 */
void gfx_spring_init(gfx_spring_t* spring, int32_t initial, int32_t target);

/**
 * Initialize a spring with custom parameters
 * All values are 16.16 fixed point
 */
void gfx_spring_init_custom(gfx_spring_t* spring, int32_t initial, int32_t target,
                            int32_t stiffness, int32_t damping, int32_t mass);

/**
 * Preset spring configurations
 */
void gfx_spring_preset_gentle(gfx_spring_t* spring);   // Slow, smooth
void gfx_spring_preset_snappy(gfx_spring_t* spring);   // Fast, responsive
void gfx_spring_preset_bouncy(gfx_spring_t* spring);   // Noticeable bounce
void gfx_spring_preset_stiff(gfx_spring_t* spring);    // Almost no bounce

/**
 * Update spring state
 * @param spring  Spring to update
 * @param dt_ms   Delta time in milliseconds
 */
void gfx_spring_update(gfx_spring_t* spring, uint32_t dt_ms);

/**
 * Set a new target for the spring
 */
void gfx_spring_set_target(gfx_spring_t* spring, int32_t target);

/**
 * Get current position as integer
 */
int32_t gfx_spring_get_position(gfx_spring_t* spring);

/**
 * Check if spring is at rest (settled)
 */
bool gfx_spring_is_at_rest(gfx_spring_t* spring);

/**
 * Reset spring to a position without animation
 */
void gfx_spring_reset(gfx_spring_t* spring, int32_t position);

// =============================================================================
// Tween Animation
// =============================================================================

/**
 * Tween animation state
 */
typedef struct {
    int32_t start_value;   // Starting value (16.16 fixed point)
    int32_t end_value;     // Ending value (16.16 fixed point)
    uint32_t duration_ms;  // Total duration
    uint32_t elapsed_ms;   // Time elapsed
    int32_t (*easing)(int32_t t);  // Easing function

    gfx_anim_state_t state;
    gfx_anim_direction_t direction;
    int32_t iterations;    // -1 for infinite
    int32_t current_iter;
} gfx_tween_t;

/**
 * Initialize a tween animation
 */
void gfx_tween_init(gfx_tween_t* tween, int32_t start, int32_t end,
                    uint32_t duration_ms, int32_t (*easing)(int32_t));

/**
 * Update tween and return current value
 */
int32_t gfx_tween_update(gfx_tween_t* tween, uint32_t dt_ms);

/**
 * Get current tween value without updating
 */
int32_t gfx_tween_get_value(gfx_tween_t* tween);

/**
 * Control functions
 */
void gfx_tween_start(gfx_tween_t* tween);
void gfx_tween_pause(gfx_tween_t* tween);
void gfx_tween_resume(gfx_tween_t* tween);
void gfx_tween_reset(gfx_tween_t* tween);
void gfx_tween_reverse(gfx_tween_t* tween);

/**
 * Set looping behavior
 */
void gfx_tween_set_loop(gfx_tween_t* tween, int32_t iterations, gfx_anim_direction_t dir);

/**
 * Check if tween is complete
 */
bool gfx_tween_is_complete(gfx_tween_t* tween);

// =============================================================================
// Modern Easing Functions
// =============================================================================

// All easing functions take and return 16.16 fixed point values in [0, GFX_FP_ONE]

// Linear
int32_t gfx_ease_linear(int32_t t);

// Quadratic
int32_t gfx_ease_in_quad(int32_t t);
int32_t gfx_ease_out_quad(int32_t t);
int32_t gfx_ease_in_out_quad(int32_t t);

// Cubic
int32_t gfx_ease_in_cubic(int32_t t);
int32_t gfx_ease_out_cubic(int32_t t);
int32_t gfx_ease_in_out_cubic(int32_t t);

// Quartic
int32_t gfx_ease_in_quart(int32_t t);
int32_t gfx_ease_out_quart(int32_t t);
int32_t gfx_ease_in_out_quart(int32_t t);

// Quintic
int32_t gfx_ease_in_quint(int32_t t);
int32_t gfx_ease_out_quint(int32_t t);
int32_t gfx_ease_in_out_quint(int32_t t);

// Exponential (GNOME-style fast deceleration)
int32_t gfx_ease_in_expo(int32_t t);
int32_t gfx_ease_out_expo(int32_t t);
int32_t gfx_ease_in_out_expo(int32_t t);

// Circular
int32_t gfx_ease_in_circ(int32_t t);
int32_t gfx_ease_out_circ(int32_t t);
int32_t gfx_ease_in_out_circ(int32_t t);

// Back (overshoot, macOS-style)
int32_t gfx_ease_in_back(int32_t t);
int32_t gfx_ease_out_back(int32_t t);
int32_t gfx_ease_in_out_back(int32_t t);

// Elastic (spring-like bounce with overshoot)
int32_t gfx_ease_in_elastic(int32_t t);
int32_t gfx_ease_out_elastic(int32_t t);
int32_t gfx_ease_in_out_elastic(int32_t t);

// Bounce (ball dropping)
int32_t gfx_ease_in_bounce(int32_t t);
int32_t gfx_ease_out_bounce(int32_t t);
int32_t gfx_ease_in_out_bounce(int32_t t);

// Sine (gentle, natural)
int32_t gfx_ease_in_sine(int32_t t);
int32_t gfx_ease_out_sine(int32_t t);
int32_t gfx_ease_in_out_sine(int32_t t);

/**
 * Custom cubic bezier easing
 * @param t   Input time [0, GFX_FP_ONE]
 * @param p1  Control point 1 Y (0-65536)
 * @param p2  Control point 2 Y (0-65536)
 * Returns interpolated value
 */
int32_t gfx_ease_bezier(int32_t t, int32_t p1, int32_t p2);

// =============================================================================
// Color Animation
// =============================================================================

/**
 * Interpolate between two colors
 * @param c1  Start color (ARGB)
 * @param c2  End color (ARGB)
 * @param t   Interpolation factor [0, 255]
 */
uint32_t gfx_color_lerp(uint32_t c1, uint32_t c2, uint8_t t);

/**
 * Interpolate in HSL color space (smoother for hue transitions)
 * @param c1  Start color (ARGB)
 * @param c2  End color (ARGB)
 * @param t   Interpolation factor [0, 255]
 */
uint32_t gfx_color_lerp_hsl(uint32_t c1, uint32_t c2, uint8_t t);

// =============================================================================
// Animation Utilities
// =============================================================================

/**
 * Smoothstep function for smooth transitions
 * @param edge0  Lower edge
 * @param edge1  Upper edge
 * @param x      Value to smooth
 * Returns smooth interpolation in [0, GFX_FP_ONE]
 */
int32_t gfx_smoothstep(int32_t edge0, int32_t edge1, int32_t x);

/**
 * Smootherstep (Ken Perlin's improved smoothstep)
 */
int32_t gfx_smootherstep(int32_t edge0, int32_t edge1, int32_t x);

/**
 * Map a value from one range to another
 */
int32_t gfx_map(int32_t value, int32_t in_min, int32_t in_max,
                int32_t out_min, int32_t out_max);

/**
 * Clamp a value to a range
 */
int32_t gfx_clamp_fp(int32_t value, int32_t min, int32_t max);

#endif // LEAFGFX_ANIM_H
