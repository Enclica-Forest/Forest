/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/audio.h - PC speaker (8254 PIT) beeper + event tones.
 * =============================================================================
 * Freestanding (no libc). The only universally-present pre-OS sound source is
 * the legacy PC speaker driven by PIT channel 2 + port 0x61 - no firmware audio
 * protocol is guaranteed to exist, and HD Audio / AC'97 codec bring-up is far
 * too firmware-dependent to rely on in a bootloader. So the beeper is the
 * portable backend; WAV sample playback is approximated on it (see audio.c) and
 * a real HDA backend is left as a documented TODO.
 *
 * All tones are gated by a runtime config (audio_configure) so forebo.cfg can
 * turn sound off entirely or retune each UI event. x86-only; every entry point
 * is a safe no-op on non-x86 or when disabled.
 * ========================================================================== */
#ifndef FOREB_UEFI_AUDIO_H
#define FOREB_UEFI_AUDIO_H

#include "../efi.h"

/* Logical UI events that map to a tone. */
enum audio_event {
    AUDIO_EV_NAV = 0,    /* menu Up/Down move          */
    AUDIO_EV_SELECT,     /* Enter / activate           */
    AUDIO_EV_OPEN,       /* window / dialog opened      */
    AUDIO_EV_ERROR,      /* rejected action / failure   */
    AUDIO_EV_BACK,       /* Esc / go up a level         */
    AUDIO_EV__COUNT
};

/* Per-event tone (freq in Hz, duration in ms). freq 0 => silent event. */
struct audio_tone { UINT16 freq; UINT16 ms; };

/* Runtime configuration pushed in from the parsed forebo.cfg. */
struct audio_cfg {
    int  enabled;                              /* master on/off (pcspeaker)    */
    int  volume;                               /* 0..100 (PWM gate duty)       */
    struct audio_tone tone[AUDIO_EV__COUNT];   /* per-event freq/duration      */
};

/* Bind BootServices (for Stall) and reset to sane default tones. */
void audio_init(EFI_BOOT_SERVICES *bs);

/* Replace the active config (NULL keeps current). Fields <=0 keep the default
 * for that slot so a partial config still works. */
void audio_configure(const struct audio_cfg *cfg);

/* 1 if sound is available + enabled on this platform. */
int  audio_enabled(void);

/* Low-level: square-wave beep at freq_hz for ms milliseconds, then silence.
 * freq_hz 0 or ms 0 is a no-op. Blocks for the duration (Stall). */
void audio_beep(UINT32 freq_hz, UINT32 ms);
void audio_silence(void);

/* Fire the tone bound to a UI event (honors enabled + volume). */
void audio_event(int ev);

/* Load + "play" a PCM WAV file from the ESP by mapping its coarse amplitude
 * envelope onto the beeper (a recognizable chirp, not hi-fi). Returns 1 if the
 * file parsed. A real HDA backend would replace the body; the interface stays.
 * `root` is an opened ESP EFI_FILE_PROTOCOL*, `path` a CHAR16* ESP path. */
int  audio_play_wav(void *root, const CHAR16 *path);

/* Advance any in-progress WAV playback by at most one blip and return. Call
 * once per UI-loop iteration; a no-op when nothing is playing. Lets a long
 * sample "play" without blocking the event loop (cursor/input stay live). */
void audio_poll(void);

#endif /* FOREB_UEFI_AUDIO_H */
