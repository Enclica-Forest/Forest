#ifndef SOUND_MIXER_H
#define SOUND_MIXER_H

#include "sound.h"
#include <stdbool.h>

/*
 * Kernel software mixer API.
 *
 * Mixes one or more input streams (each with its own format/rate/channels
 * and per-stream volume) into a single hardware output stream at a common
 * output format. Linear resampling is used when input and output rates
 * differ. The PCM character device (/dev/snd) uses this to feed userspace
 * audio into the active hardware driver.
 *
 * All functions are safe to call from the mixer/drain thread and from the
 * syscall write() path. When ENABLE_AUDIO is not defined, every entry point
 * degrades to a no-op / NULL return so the linker is satisfied without a
 * real audio backend.
 */

typedef struct snd_mixer        snd_mixer_t;
typedef struct snd_mixer_stream snd_mixer_stream_t;

/* Create a mixer bound to a hardware output format. Returns NULL on failure. */
snd_mixer_t* snd_mixer_create(const snd_pcm_format_t* output_format);

/* Destroy a mixer and all streams attached to it. */
void snd_mixer_destroy(snd_mixer_t* m);

/* Output format the mixer renders to. Never NULL for a valid mixer. */
const snd_pcm_format_t* snd_mixer_output_format(const snd_mixer_t* m);

/* Master volume (0..255). Applied to all mixed output. */
void   snd_mixer_set_master(snd_mixer_t* m, uint8_t volume);
uint8_t snd_mixer_get_master(const snd_mixer_t* m);

/* Create / destroy an input stream. in_format describes the PCM bytes the
 * caller will push via snd_mixer_stream_write(). */
snd_mixer_stream_t* snd_mixer_stream_create(snd_mixer_t* m,
                                            const snd_pcm_format_t* in_format);
void snd_mixer_stream_destroy(snd_mixer_t* m, snd_mixer_stream_t* s);

/* Per-stream volume (0..255) and mute. */
void snd_mixer_stream_set_volume(snd_mixer_stream_t* s, uint8_t volume);
void snd_mixer_stream_set_mute(snd_mixer_stream_t* s, bool muted);

/* Non-blocking write into the stream's internal ring buffer. Returns the
 * number of bytes actually accepted (may be < bytes when the ring is full). */
uint32_t snd_mixer_stream_write(snd_mixer_stream_t* s, const void* data, uint32_t bytes);

/* Bytes currently queued in the stream ring. */
uint32_t snd_mixer_stream_queued(const snd_mixer_stream_t* s);

/* Flush the stream ring (drop pending data). */
void snd_mixer_stream_flush(snd_mixer_stream_t* s);

/* Render up to dst_frames mixed frames into dst (interleaved int16_t at the
 * mixer's output format/channels). Returns the number of frames actually
 * written. Zero is returned when no input streams have data. The caller is
 * responsible for handing the rendered buffer to the hardware driver. */
uint32_t snd_mixer_process(snd_mixer_t* m, int16_t* dst, uint32_t dst_frames);

/* Convenience: bytes per output frame for this mixer. */
uint32_t snd_mixer_frame_bytes(const snd_mixer_t* m);

#endif /* SOUND_MIXER_H */
