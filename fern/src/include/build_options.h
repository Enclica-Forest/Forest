#ifndef BUILD_OPTIONS_H
#define BUILD_OPTIONS_H

/*
 * build_options.h - Single inclusion point for graphics/fb build options.
 *
 * This header does NOT set policy. It only provides safe defaults via
 * #ifndef guards so that conf.sh / build-config.mk / the Makefile can
 * override each option with -D<NAME>=<value> on the compiler command line.
 *
 * Include gfx_config.h (which includes this file) from source files to get
 * the canonical HAS_* feature macros. Include this file directly only from
 * other option headers.
 *
 * All options are expressed as preprocessor integers (0/1) so they can be
 * used uniformly in #if expressions regardless of whether they were defined
 * via -D or via the defaults below.
 */

/* -------------------------------------------------------------------------
 * Master framebuffer / graphics switch
 *
 * conf.sh / the Makefile pass -DENABLE_FRAMEBUFFER (=1) when this feature
 * is on and OMIT it when off. The default below is 0 so that including this
 * header without the build system having defined the flag yields the safe
 * no-framebuffer / text-console path.
 * -----------------------------------------------------------------------*/
#ifndef ENABLE_FRAMEBUFFER
#  define ENABLE_FRAMEBUFFER 0
#endif

/* -------------------------------------------------------------------------
 * Double buffering (back buffer + dirty-rect flip)
 * -----------------------------------------------------------------------*/
#ifndef ENABLE_DOUBLE_BUFFERING
#  define ENABLE_DOUBLE_BUFFERING 0
#endif

/* Hardware panning flip (page-flip via y-offset / panning registers).
 * Requires a driver that exposes a panning ioctl; otherwise fb_present()
 * falls back to a memcpy of dirty rects. */
#ifndef ENABLE_FB_PANNING
#  define ENABLE_FB_PANNING 0
#endif

/* Track and merge dirty rectangles to minimise flip copies. */
#ifndef ENABLE_FB_DIRTY_RECTS
#  define ENABLE_FB_DIRTY_RECTS 1
#endif

/* Wait for device vsync before flipping. If the device exposes a vsync
 * ioctl it is used; otherwise fb_wait_vsync() is a bounded busy-wait or
 * an immediate return (see FB_VSYNC_BUSY_WAIT_US). */
#ifndef ENABLE_FB_VSYNC_WAIT
#  define ENABLE_FB_VSYNC_WAIT 1
#endif

/* -------------------------------------------------------------------------
 * Higher-level graphics stack
 * -----------------------------------------------------------------------*/
#ifndef ENABLE_GRAPHICS
#  define ENABLE_GRAPHICS 0
#endif

#ifndef ENABLE_GPU_ACCEL
#  define ENABLE_GPU_ACCEL 0
#endif

#ifndef ENABLE_COMPOSITOR
#  define ENABLE_COMPOSITOR 0
#endif

/* Panic UI graphics: when disabled, panic output goes to the TTY. */
#ifndef ENABLE_PANIC_UI_GFX
#  define ENABLE_PANIC_UI_GFX 1
#endif

#ifndef ENABLE_SPLASH
#  define ENABLE_SPLASH 1
#endif

/* -------------------------------------------------------------------------
 * Tunables
 * -----------------------------------------------------------------------*/

/* Maximum dirty rectangles tracked before collapsing to a full-screen flip. */
#ifndef FB_MAX_DIRTY_RECTS
#  define FB_MAX_DIRTY_RECTS 64
#endif

/* Default bits-per-pixel when the device does not report one. */
#ifndef FB_DEFAULT_BPP
#  define FB_DEFAULT_BPP 32
#endif

/* When non-zero, force the kernel into text/TTY mode even if a framebuffer
 * is present. Useful for headless / serial-only bring-up. */
#ifndef FB_FORCE_TEXT_MODE
#  define FB_FORCE_TEXT_MODE 0
#endif

/* Bounded busy-wait (microseconds) used by fb_wait_vsync() when the device
 * has no vsync ioctl. 0 = return immediately. */
#ifndef FB_VSYNC_BUSY_WAIT_US
#  define FB_VSYNC_BUSY_WAIT_US 0
#endif

/* If dirty_count exceeds this fraction of FB_MAX_DIRTY_RECTS, fb_present()
 * switches to a full-buffer copy (fb_present_full semantics). */
#ifndef FB_DIRTY_FULL_THRESHOLD
#  define FB_DIRTY_FULL_THRESHOLD(d) ((d) >= (FB_MAX_DIRTY_RECTS * 3 / 4))
#endif

/* Normalise all options to 0/1 so callers can use #if uniformly. */
#define BO_BOOL(x) ((x) ? 1 : 0)

/* -------------------------------------------------------------------------
 * 64-bit / x86_64 long-mode options
 *
 * These control optional 64-bit-only features.  All default to 0 (off) so
 * a baseline 64-bit build still works on any long-mode-capable CPU.  The
 * build system (conf.sh / Makefile) may override each with -D<NAME>=1.
 * Sources that consume these should #include "build_options.h" (or
 * transitively via a 64-bit header such as paging64.h).
 * -----------------------------------------------------------------------*/

/* 5-level paging (LA57).  Needs recent CPU (Ice Lake+/Zen 2+).  When off,
 * the kernel uses 4-level paging (48-bit virtual, 256 TiB user + kernel). */
#ifndef ENABLE_5LEVEL_PAGING
#  define ENABLE_5LEVEL_PAGING 0
#endif

/* PCID (Process Context IDentifier).  Avoid full TLB flush on CR3 write
 * when CR4.PCIDE is set; each address space gets a 12-bit ASID tag. */
#ifndef ENABLE_PCID
#  define ENABLE_PCID 0
#endif

/* Kernel ASLR.  Randomise the high-half kernel base offset at boot.
 * Currently selects a slot; full relocation requires PIC or a relocation
 * table applied by the loader. */
#ifndef ENABLE_KASLR
#  define ENABLE_KASLR 0
#endif

/* XSAVE/XRSTOR for FPU/SSE/AVX state.  Falls back to FXSAVE/FXRSTOR when
 * the CPU lacks XSAVE support.  See x86_64_ist_handling.c. */
#ifndef ENABLE_XSAVE
#  define ENABLE_XSAVE 0
#endif

/* AVX state save/restore.  Requires ENABLE_XSAVE and CPU AVX support. */
#ifndef ENABLE_AVX
#  define ENABLE_AVX 0
#endif

/* Supervisor Mode Execution Prevention (CR4.SMEP).  Prevents the CPU from
 * executing user-mode pages while in ring 0. */
#ifndef ENABLE_SMEP
#  define ENABLE_SMEP 0
#endif

/* Supervisor Mode Access Prevention (CR4.SMAP).  Prevents the CPU from
 * reading/writing user-mode pages while in ring 0 (unless RFLAGS.AC is
 * set or STAC/CLAC is used). */
#ifndef ENABLE_SMAP
#  define ENABLE_SMAP 0
#endif

/* No-Execute (NX/XD) bit.  Requires EFER.NXE.  Allows PAGE_NX to mark
 * pages as non-executable. */
#ifndef ENABLE_NX
#  define ENABLE_NX 0
#endif

/* Compat-mode int 0x80 syscall path.  When enabled, 32-bit userspace on a
 * 64-bit kernel can invoke syscalls via int 0x80 (vector 0x80) in addition
 * to the SYSCALL instruction.  Disable to force 64-bit userspace only. */
#ifndef ENABLE_COMPAT_INT80
#  define ENABLE_COMPAT_INT80 1
#endif

#endif /* BUILD_OPTIONS_H */
