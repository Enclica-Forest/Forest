/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_toys.h - "Toys & Audio" tool category (KEY = toys).
 * =============================================================================
 * Fun, self-contained GUI toys (paint, sound, screensavers). Each tool is a
 * template-B wm.c window: its open() calls wm_open() and returns immediately;
 * the bootx64.c menu loop drives input + compositing.
 *
 * Sound is produced on the legacy PC-speaker (PIT channel 2 + port 0x61), which
 * needs BootServices->Stall for tone timing, so this category exposes an init
 * that captures the system table (clock.c idiom). Pure draw toys ignore it.
 *
 * Freestanding (no libc), pre-ExitBootServices, fixed pools, integer-only math.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_TOYS_H
#define FOREB_UEFI_TOOLS_TOYS_H

#include "tools.h"   /* struct forebo_tool + efi/wm/ui/input */

/* Capture gST/gBS/gRT for the audio toys (Stall). NULL-safe. */
void cat_toys_init(EFI_SYSTEM_TABLE *st);

/* ---- individual tool open() callbacks (template B) ---- */
void tool_toys_paint_open(void);      /* Pixel Paint: mouse draw grid + palette */
void tool_toys_piano_open(void);      /* Piano: clickable keys -> speaker tones  */
void tool_toys_tone_open(void);       /* Tone Generator: freq slider -> speaker  */
void tool_toys_metronome_open(void);  /* Metronome: BPM -> speaker ticks         */
void tool_toys_mixer_open(void);      /* Colour Mixer: R/G/B sliders + swatch    */
void tool_toys_aquarium_open(void);   /* ASCII Aquarium: animated fish           */
void tool_toys_starfield_open(void);  /* Starfield screensaver                   */
void tool_toys_drums_open(void);      /* Drum pads: keys/clicks -> tones         */
void tool_toys_dvd_open(void);        /* Bouncing DVD-logo                       */

/* ---- category exports ---- */
extern const struct forebo_tool cat_toys_tools[];
extern const int                cat_toys_count;

#endif /* FOREB_UEFI_TOOLS_TOYS_H */
