#ifndef GFX_CONFIG_H
#define GFX_CONFIG_H

/*
 * gfx_config.h - Centralised graphics / framebuffer feature gates.
 *
 * Include this header from any source file that touches the framebuffer or
 * graphics stack. It pulls in build_options.h (which honours -D overrides
 * from the build system) and exposes a stable set of HAS_* macros so that
 * code can write:
 *
 *     #if HAS_FRAMEBUFFER
 *         ...framebuffer code...
 *     #else
 *         ...text-mode fallback / stubs...
 *     #endif
 *
 * This avoids fragile include-order-dependent #ifdef ENABLE_FRAMEBUFFER
 * checks scattered across translation units.
 */

#include "build_options.h"

/* Master switch. FB_FORCE_TEXT_MODE overrides everything: even if a
 * framebuffer is enabled at build time, the kernel runs in text mode. */
#if defined(ENABLE_FRAMEBUFFER) && (ENABLE_FRAMEBUFFER) && !(FB_FORCE_TEXT_MODE)
#  define HAS_FRAMEBUFFER 1
#else
#  define HAS_FRAMEBUFFER 0
#endif

#define HAS_DOUBLE_BUFFERING  BO_BOOL(ENABLE_DOUBLE_BUFFERING)
#define HAS_FB_PANNING        BO_BOOL(ENABLE_FB_PANNING)
#define HAS_FB_DIRTY_RECTS    BO_BOOL(ENABLE_FB_DIRTY_RECTS)
#define HAS_FB_VSYNC_WAIT     BO_BOOL(ENABLE_FB_VSYNC_WAIT)

#if defined(ENABLE_GRAPHICS) && (ENABLE_GRAPHICS) && HAS_FRAMEBUFFER
#  define HAS_GRAPHICS 1
#else
#  define HAS_GRAPHICS 0
#endif

#if defined(ENABLE_GPU_ACCEL) && (ENABLE_GPU_ACCEL) && HAS_GRAPHICS
#  define HAS_GPU_ACCEL 1
#else
#  define HAS_GPU_ACCEL 0
#endif

#if defined(ENABLE_COMPOSITOR) && (ENABLE_COMPOSITOR) && HAS_GRAPHICS
#  define HAS_COMPOSITOR 1
#else
#  define HAS_COMPOSITOR 0
#endif

#if defined(ENABLE_PANIC_UI_GFX) && (ENABLE_PANIC_UI_GFX) && HAS_GRAPHICS
#  define HAS_PANIC_UI_GFX 1
#else
#  define HAS_PANIC_UI_GFX 0
#endif

#if defined(ENABLE_SPLASH) && (ENABLE_SPLASH) && HAS_GRAPHICS
#  define HAS_SPLASH 1
#else
#  define HAS_SPLASH 0
#endif

#endif /* GFX_CONFIG_H */
