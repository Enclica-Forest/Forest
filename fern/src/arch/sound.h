/*
 * Fern - Cross-Architecture Sound Interface
 * arch/sound.h
 *
 * Provides a thin, architecture-neutral API for the sound subsystem.  The
 * actual implementations live in per-arch source files:
 *
 *   x86_32 / x86_64  – PC speaker, HDA, AC97, SB16, etc. (existing drivers)
 *   AArch64           – PL011 PWM beep + silent stub (src/aarch64/sound_aarch64.c)
 *   ARM32             – PL011 PWM beep + silent stub (src/arm32/sound_arm32.c)
 *   RISC-V 64         – virtio-snd stub             (src/riscv64/sound_riscv64.c)
 *
 * Platforms without audio hardware get a silent stub that returns success
 * so the kernel can continue booting without errors.
 */

#ifndef FOREST_ARCH_SOUND_H
#define FOREST_ARCH_SOUND_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration – full type lives in include/sound.h */
struct SoundDriver;

/**
 * arch_sound_init - Initialise the sound subsystem for the current platform.
 *
 * Called once from sound_system_init().  The implementation should probe for
 * hardware, create a SoundDriver, and return it.  Returns NULL when no audio
 * hardware is found (the silent stub always returns NULL).
 */
struct SoundDriver *arch_sound_init(void);

/**
 * arch_sound_play_pcm - Play PCM audio data (platform convenience wrapper).
 *
 * @data:   pointer to PCM sample data
 * @len:    byte length of the buffer
 * @rate:   sample rate in Hz
 * @channels: number of audio channels (1 or 2)
 * @bits:   bits per sample (8 or 16)
 *
 * Returns true on success, false on error or if no driver is available.
 */
bool arch_sound_play_pcm(const uint8_t *data, uint32_t len,
                         uint32_t rate, uint8_t channels, uint8_t bits);

/**
 * arch_sound_beep - Generate a tone at the given frequency.
 *
 * @freq_hz:      tone frequency in Hertz
 * @duration_ms:  duration in milliseconds
 */
void arch_sound_beep(uint32_t freq_hz, uint32_t duration_ms);

/**
 * arch_sound_set_volume - Set the master volume for the platform driver.
 *
 * @volume: 0..255 (0 = mute, 255 = maximum)
 */
void arch_sound_set_volume(uint8_t volume);

#endif /* FOREST_ARCH_SOUND_H */
