#ifndef SOUND_PCSPEAKER_H
#define SOUND_PCSPEAKER_H

#include "types.h"
#include <stdbool.h>

/* Default config: the TTY bell is enabled unless explicitly disabled. */
#ifndef ENABLE_TTY_BELL
#define ENABLE_TTY_BELL 1
#endif

/*
 * PC speaker fallback API.
 *
 * Produces simple beep tones via the PIT channel 2 + speaker gate. Used as a
 * fallback when no real audio hardware is detected, and by the TTY bell so
 * audible feedback works even on sound-card-less systems.
 *
 * When ENABLE_AUDIO is not defined, the real implementation is not built;
 * feature_stubs.c provides no-op equivalents so callers link cleanly.
 */

/* Play a pure tone at freq Hz for ms milliseconds (blocking). freq==0 or
 * ms==0 is a no-op. */
void snd_pcspeaker_tone(uint32_t freq, uint32_t ms);

/* Convenience: the standard terminal bell tone (800 Hz, 120 ms). */
void snd_pcspeaker_beep(void);

/* True if the PC speaker driver has been initialized and is usable. */
bool snd_pcspeaker_available(void);

#endif /* SOUND_PCSPEAKER_H */
