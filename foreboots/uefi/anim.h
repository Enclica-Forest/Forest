/*
 * anim.h - GOP framebuffer animation helpers for the ForeB UEFI loader.
 *
 * A small, freestanding (no libc) compositing/animation layer that draws
 * straight to the GOP linear framebuffer, complementing ui.c (menu/progress)
 * and image.c (BMP/TGA blit). It owns its OWN framebuffer parameters (given to
 * anim_init(), mirroring ui_init()) plus an optional full-screen snapshot of
 * the static background, which powers:
 *
 *   - anim_fade_in()      : ramp the captured background in from black,
 *   - anim_particles_*()  : a subtle falling-leaves / embers layer drawn over
 *                           the background and cleanly erased from the snapshot
 *                           each tick (no smearing),
 *   - anim_spinner()      : an 8-dot rotating spinner glyph,
 *   - anim_progress_to()  : an eased progress-bar update (via ui_progress) with
 *                           an integrated spinner.
 *
 * Timing: anim_fade_in()/anim_progress_to() pace themselves with the
 * BootServices Stall() passed to anim_init() (valid only BEFORE
 * ExitBootServices). All the pure-draw routines (anim_particles_step,
 * anim_spinner, anim_progress_to with use_stall==0) are pure framebuffer MMIO
 * and remain valid AFTER ExitBootServices as well.
 */
#ifndef FOREB_UEFI_ANIM_H
#define FOREB_UEFI_ANIM_H

#include "efi.h"

/*
 * One-time init. Pass the same framebuffer parameters given to ui_init() plus
 * the BootServices table (for AllocatePool of the snapshot + Stall pacing).
 * Allocates a width*height*4 snapshot buffer; if that allocation fails the
 * fade + particle features degrade gracefully to no-ops (the spinner and eased
 * progress still work). Safe to call more than once.
 */
void anim_init(UINT64 fb_base, UINT32 pitch, UINT32 width, UINT32 height,
               UINT32 pixfmt, EFI_BOOT_SERVICES *bs);

/*
 * Capture the current framebuffer contents as the "static background" used by
 * anim_fade_in() and by the particle layer's erase step. Call this right AFTER
 * painting the background (image or forest theme) and BEFORE drawing particles
 * or the menu on top.
 */
void anim_capture(void);

/*
 * Fade the captured background in from black over `frames` steps of `step_ms`
 * each (paced with BootServices Stall). Ends with the framebuffer exactly
 * equal to the captured snapshot. No-op (aside from an optional short wait) if
 * no snapshot is available.
 */
void anim_fade_in(int frames, int step_ms);

/*
 * Fade the CURRENT framebuffer contents (menu/particles included) smoothly to
 * solid black over `frames` steps of `step_ms` each (paced with BootServices
 * Stall). Captures the on-screen pixels first, then ramps them toward
 * 0x000000, presenting each step so the fade is visible; ends on a fully black
 * screen. This overwrites the background snapshot, so it is intended as the
 * last thing before a boot handoff (ExitBootServices / StartImage). Degrades to
 * a short wait plus a single black fill when no snapshot/back buffer exists.
 */
void anim_fade_out(int frames, int step_ms);

/*
 * Quadratic ease-out interpolation from `from` to `to` at `step` of `steps`
 * (step<=0 => from, step>=steps => to). Integer-only; used for the menu
 * highlight slide (see GUI_TOOLS.md).
 */
int anim_lerp(int from, int to, int step, int steps);

/*
 * Seed the particle layer with `count` particles (clamped to an internal max).
 * style: 0 = falling leaves (greens), 1 = drifting embers (amber). Requires a
 * captured snapshot; otherwise the layer stays empty.
 */
void anim_particles_init(int count, int style);

/*
 * Restrict the particle layer to AVOID a screen rectangle (px). Particles are
 * neither seeded into nor advanced across this region, so an overlaid panel
 * (e.g. the boot-menu / a window) stays clear of falling leaves instead of
 * being repainted over every tick. Stored in file-static a_excl_* and honored
 * by both anim_particles_init() (seed) and anim_particles_step() (move). A
 * zero/empty rect (w<=0 || h<=0) disables the exclusion.
 */
void anim_particles_set_exclude(int x, int y, int w, int h);

/*
 * Tint the particle layer to match the active UI theme. `accent` and `title`
 * are 0x00RRGGBB; the layer uses a 4-stop blend between them. Pass (0,0) to
 * restore the built-in leaf/ember palettes. Takes effect on the next
 * anim_particles_init().
 */
void anim_set_tint(UINT32 accent, UINT32 title);

/*
 * Advance the particle layer one tick: erase each particle from the snapshot
 * background, move it, and redraw it alpha-blended over the framebuffer. Call
 * on a steady cadence (e.g. every ~50 ms) and repaint the menu panel over it
 * afterwards.
 */
void anim_particles_step(void);

/*
 * Draw an 8-dot rotating spinner centered at (cx,cy). `phase` advances the
 * bright head by one dot; `scale` magnifies the ring/dot size. Pure MMIO
 * (post-ExitBootServices safe). `color` is 0x00RRGGBB.
 */
void anim_spinner(int cx, int cy, int phase, UINT32 color, int scale);

/* Convenience: draw the spinner just to the side of the theme progress bar. */
void anim_load_spinner(int phase);

/*
 * Reset the eased-progress state so the next anim_progress_to() animates up
 * from 0%. Call once before starting a new load.
 */
void anim_progress_reset(void);

/*
 * Eased progress: animate the bar from its last shown percentage toward
 * cur/total (total==0 => 100%), calling ui_progress() and anim_load_spinner()
 * on each intermediate step. use_stall!=0 paces with BootServices Stall (use
 * before ExitBootServices); use_stall==0 uses a busy delay (post-EBS safe).
 * Passing cur==0 also resets the eased state.
 */
void anim_progress_to(const char *label, UINT64 cur, UINT64 total, int use_stall);

#endif /* FOREB_UEFI_ANIM_H */
