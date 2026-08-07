/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_gfx.h - "Graphics Demos" tool category (KEY = gfx).
 * =============================================================================
 * A set of self-contained, pure compute/draw animated visual demos rendered as
 * wm.c windows (template B). Every demo uses INTEGER / fixed-point math only
 * (no libc, no heap, no floating point - trig comes from an internal sine LUT)
 * and animates by advancing state each draw_cb frame.
 *
 * This category needs NO firmware services, hence NO cat_gfx_init().
 * Freestanding, pre-ExitBootServices, fixed buffers.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_GFX_H
#define FOREB_UEFI_TOOLS_GFX_H

#include "tools.h"   /* struct forebo_tool */

/* Each opens one wm window and returns immediately (template B). */
void tool_gfx_mandelbrot_open(void);  /* integer fixed-point Mandelbrot, pan/zoom */
void tool_gfx_plasma_open(void);      /* animated sine-LUT plasma field           */
void tool_gfx_starfield_open(void);   /* 3D-ish flying starfield                   */
void tool_gfx_matrix_open(void);      /* falling "Matrix" character rain           */
void tool_gfx_fireworks_open(void);   /* particle fireworks with gravity           */
void tool_gfx_gradient_open(void);    /* interactive 4-corner colour gradient      */
void tool_gfx_lissajous_open(void);   /* animated Lissajous curves (sine LUT)      */
void tool_gfx_balls_open(void);       /* bouncing balls physics toy                */
void tool_gfx_sierpinski_open(void);  /* Sierpinski chaos-game fractal             */

/* Category registry exports (defined in tools_gfx.c). */
extern const struct forebo_tool cat_gfx_tools[];
extern const int                cat_gfx_count;

#endif /* FOREB_UEFI_TOOLS_GFX_H */
