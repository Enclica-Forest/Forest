#include "include/sound_mixer.h"
#include "include/memory.h"
#include "include/libc/string.h"

/*
 * Kernel software mixer.
 *
 * Renders multiple input streams (each with its own format/rate/channels and
 * per-stream volume) into a single interleaved int16_t output at the mixer's
 * configured output format. Uses linear resampling when input and output
 * rates differ.
 *
 * Fully guarded by ENABLE_AUDIO: when audio is disabled at build time the
 * real implementation is elided and the stub section provides no-op/NULL
 * bodies so the linker is satisfied and /dev/snd degrades to -ENOSYS.
 */

#ifdef ENABLE_AUDIO

#define SND_MIXER_RING_SIZE  16384
#define SND_MIXER_RING_MASK  (SND_MIXER_RING_SIZE - 1)
#define SND_MIXER_MAX_STREAMS 4
#define RATE_SCALE           65536u   /* 16.16 fixed point */

struct snd_mixer_stream {
    snd_mixer_t*       mixer;
    snd_pcm_format_t   in_format;
    volatile bool      muted;
    uint8_t            volume;        /* 0..255 */
    uint8_t            ring[SND_MIXER_RING_SIZE];
    volatile uint32_t  head;
    volatile uint32_t  tail;
    uint32_t           resample_pos;  /* 16.16 fractional input-frame pos */
};

struct snd_mixer {
    snd_pcm_format_t      out_format;
    uint8_t               master_volume;
    snd_mixer_stream_t*   streams[SND_MIXER_MAX_STREAMS];
    uint32_t              stream_count;
};

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

/* ---- small helpers ---- */

static uint8_t bytes_per_sample_fmt(PcmFormat f) {
    switch (f) {
        case PCM_S8:
        case PCM_U8:    return 1;
        case PCM_S16LE:
        case PCM_S16BE: return 2;
        case PCM_S24LE: return 3;
        case PCM_S32LE:
        case PCM_F32LE: return 4;
        default:        return 0;
    }
}

static uint32_t frame_bytes(const snd_pcm_format_t* f) {
    return (uint32_t)bytes_per_sample_fmt(f->format) * f->channels;
}

static uint32_t ring_avail(const snd_mixer_stream_t* s) {
    return s->head - s->tail;
}

static uint32_t ring_free(const snd_mixer_stream_t* s) {
    return SND_MIXER_RING_SIZE - ring_avail(s);
}

static void ring_push(snd_mixer_stream_t* s, const uint8_t* data, uint32_t len) {
    uint32_t free_space = ring_free(s);
    if (len > free_space) len = free_space;
    uint32_t pos = s->head & SND_MIXER_RING_MASK;
    uint32_t first = SND_MIXER_RING_SIZE - pos;
    if (first > len) first = len;
    memcpy(s->ring + pos, data, first);
    if (len > first) memcpy(s->ring, data + first, len - first);
    s->head += len;
}

/* Peek up to `n` bytes at logical byte offset `off` from the tail. */
static void ring_peek(const snd_mixer_stream_t* s, uint32_t off, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pos = (s->tail + off + i) & SND_MIXER_RING_MASK;
        dst[i] = s->ring[pos];
    }
}

static void ring_drop(snd_mixer_stream_t* s, uint32_t len) {
    uint32_t avail = ring_avail(s);
    if (len > avail) len = avail;
    s->tail += len;
}

/* Decode one sample (channel `ch`) at input-frame `fidx` into a normalized
 * int32_t roughly in [-32768, 32767]. */
static int32_t decode_at(const snd_mixer_stream_t* s, uint32_t fidx, uint8_t ch) {
    uint8_t bps = bytes_per_sample_fmt(s->in_format.format);
    uint32_t fb = (uint32_t)bps * s->in_format.channels;
    uint32_t off = fidx * fb + (uint32_t)ch * bps;
    uint8_t buf[4] = {0, 0, 0, 0};
    ring_peek(s, off, buf, bps);
    switch (s->in_format.format) {
        case PCM_S8:    return ((int32_t)((int8_t)buf[0])) << 8;
        case PCM_U8:    return ((int32_t)buf[0] - 128) << 8;
        case PCM_S16LE: return (int32_t)((int16_t)(buf[0] | (buf[1] << 8)));
        case PCM_S16BE: return (int32_t)((int16_t)((buf[0] << 8) | buf[1]));
        case PCM_S24LE: {
            uint32_t u = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16);
            if (u & 0x800000u) u |= 0xFF000000u;
            return (int32_t)u >> 8;
        }
        case PCM_S32LE: {
            uint32_t u = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            return (int32_t)u >> 16;
        }
        case PCM_F32LE: {
            uint32_t u = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            float f;
            memcpy(&f, &u, sizeof(f));
            int64_t v = (int64_t)(f * 32767.0f);
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            return (int32_t)v;
        }
        default: return 0;
    }
}

static int32_t clamp_s16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return v;
}

/* ---- public API ---- */

snd_mixer_t* snd_mixer_create(const snd_pcm_format_t* output_format) {
    if (!output_format || output_format->channels == 0 ||
        output_format->rate == 0 ||
        bytes_per_sample_fmt(output_format->format) == 0) {
        return NULL;
    }
    snd_mixer_t* m = (snd_mixer_t*)kmalloc(sizeof(snd_mixer_t));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->out_format = *output_format;
    m->master_volume = 255;
    return m;
}

void snd_mixer_destroy(snd_mixer_t* m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->stream_count; i++) {
        if (m->streams[i]) kfree(m->streams[i]);
        m->streams[i] = NULL;
    }
    kfree(m);
}

const snd_pcm_format_t* snd_mixer_output_format(const snd_mixer_t* m) {
    return m ? &m->out_format : NULL;
}

void snd_mixer_set_master(snd_mixer_t* m, uint8_t volume) {
    if (m) m->master_volume = volume;
}

uint8_t snd_mixer_get_master(const snd_mixer_t* m) {
    return m ? m->master_volume : 0;
}

uint32_t snd_mixer_frame_bytes(const snd_mixer_t* m) {
    return m ? frame_bytes(&m->out_format) : 0;
}

snd_mixer_stream_t* snd_mixer_stream_create(snd_mixer_t* m,
                                            const snd_pcm_format_t* in_format) {
    if (!m || !in_format || m->stream_count >= SND_MIXER_MAX_STREAMS) return NULL;
    if (bytes_per_sample_fmt(in_format->format) == 0) return NULL;
    if (in_format->rate == 0 || in_format->channels == 0) return NULL;
    snd_mixer_stream_t* s = (snd_mixer_stream_t*)kmalloc(sizeof(snd_mixer_stream_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->mixer = m;
    s->in_format = *in_format;
    s->volume = 255;
    s->muted = false;
    m->streams[m->stream_count++] = s;
    return s;
}

void snd_mixer_stream_destroy(snd_mixer_t* m, snd_mixer_stream_t* s) {
    if (!m || !s) return;
    for (uint32_t i = 0; i < m->stream_count; i++) {
        if (m->streams[i] == s) {
            m->streams[i] = m->streams[--m->stream_count];
            break;
        }
    }
    kfree(s);
}

void snd_mixer_stream_set_volume(snd_mixer_stream_t* s, uint8_t volume) {
    if (s) s->volume = volume;
}

void snd_mixer_stream_set_mute(snd_mixer_stream_t* s, bool muted) {
    if (s) s->muted = muted;
}

uint32_t snd_mixer_stream_write(snd_mixer_stream_t* s, const void* data, uint32_t bytes) {
    if (!s || !data || bytes == 0) return 0;
    uint32_t room = ring_free(s);
    uint32_t n = bytes < room ? bytes : room;
    ring_push(s, (const uint8_t*)data, n);
    return n;
}

uint32_t snd_mixer_stream_queued(const snd_mixer_stream_t* s) {
    return s ? ring_avail(s) : 0;
}

void snd_mixer_stream_flush(snd_mixer_stream_t* s) {
    if (!s) return;
    s->head = s->tail;
    s->resample_pos = 0;
}

uint32_t snd_mixer_process(snd_mixer_t* m, int16_t* dst, uint32_t dst_frames) {
    if (!m || !dst || dst_frames == 0 || m->out_format.rate == 0) return 0;
    uint32_t out_ch = m->out_format.channels;
    if (out_ch == 0) return 0;

    uint32_t rendered = 0;
    for (uint32_t o = 0; o < dst_frames; o++) {
        int32_t mix_l = 0, mix_r = 0;
        bool any = false;

        for (uint32_t si = 0; si < m->stream_count; si++) {
            snd_mixer_stream_t* s = m->streams[si];
            if (!s || s->muted) continue;
            uint32_t fb = frame_bytes(&s->in_format);
            if (fb == 0) continue;
            uint32_t avail_frames = ring_avail(s) / fb;
            if (avail_frames == 0) continue;

            uint32_t step = (s->in_format.rate * RATE_SCALE) / m->out_format.rate;
            uint32_t pos = s->resample_pos;          /* 16.16 */
            uint32_t base = pos >> 16;
            uint32_t frac = pos & 0xFFFF;
            if (base >= avail_frames) continue;       /* starved */

            uint32_t next = base + 1;
            uint8_t in_ch = s->in_format.channels;
            int32_t l0, l1, r0, r1;
            if (in_ch == 1) {
                l0 = r0 = decode_at(s, base, 0);
                l1 = r1 = (next < avail_frames) ? decode_at(s, next, 0) : l0;
            } else {
                l0 = decode_at(s, base, 0);
                r0 = decode_at(s, base, 1);
                l1 = (next < avail_frames) ? decode_at(s, next, 0) : l0;
                r1 = (next < avail_frames) ? decode_at(s, next, 1) : r0;
            }
            int32_t l = l0 + (int32_t)(((int32_t)(l1 - l0) * (int32_t)frac) >> 16);
            int32_t r = r0 + (int32_t)(((int32_t)(r1 - r0) * (int32_t)frac) >> 16);

            int32_t vol = (int32_t)s->volume;          /* 0..255 */
            l = (l * vol) >> 8;
            r = (r * vol) >> 8;

            mix_l += l;
            mix_r += r;
            any = true;

            s->resample_pos += step;
        }

        if (!any) {
            /* No stream had data: render silence for the rest and stop. */
            break;
        }

        int32_t master = (int32_t)m->master_volume;
        mix_l = (mix_l * master) >> 8;
        mix_r = (mix_r * master) >> 8;

        if (out_ch == 1) {
            dst[o] = (int16_t)clamp_s16((mix_l + mix_r) / 2);
        } else {
            dst[o * 2]     = (int16_t)clamp_s16(mix_l);
            dst[o * 2 + 1] = (int16_t)clamp_s16(mix_r);
        }
        rendered++;
    }

    /* Consume the input frames we actually read past. */
    for (uint32_t si = 0; si < m->stream_count; si++) {
        snd_mixer_stream_t* s = m->streams[si];
        if (!s) continue;
        uint32_t fb = frame_bytes(&s->in_format);
        if (fb == 0) continue;
        uint32_t consumed = s->resample_pos >> 16;
        uint32_t avail = ring_avail(s) / fb;
        if (consumed > avail) consumed = avail;
        ring_drop(s, consumed * fb);
        s->resample_pos -= (consumed << 16);
    }

    return rendered;
}

#else /* !ENABLE_AUDIO — stubs so /dev/snd links without an audio backend. */

snd_mixer_t* snd_mixer_create(const snd_pcm_format_t* output_format) {
    (void)output_format; return NULL;
}
void snd_mixer_destroy(snd_mixer_t* m) { (void)m; }
const snd_pcm_format_t* snd_mixer_output_format(const snd_mixer_t* m) { (void)m; return NULL; }
void   snd_mixer_set_master(snd_mixer_t* m, uint8_t volume) { (void)m; (void)volume; }
uint8_t snd_mixer_get_master(const snd_mixer_t* m) { (void)m; return 0; }
uint32_t snd_mixer_frame_bytes(const snd_mixer_t* m) { (void)m; return 0; }
snd_mixer_stream_t* snd_mixer_stream_create(snd_mixer_t* m, const snd_pcm_format_t* in_format) {
    (void)m; (void)in_format; return NULL;
}
void snd_mixer_stream_destroy(snd_mixer_t* m, snd_mixer_stream_t* s) { (void)m; (void)s; }
void snd_mixer_stream_set_volume(snd_mixer_stream_t* s, uint8_t volume) { (void)s; (void)volume; }
void snd_mixer_stream_set_mute(snd_mixer_stream_t* s, bool muted) { (void)s; (void)muted; }
uint32_t snd_mixer_stream_write(snd_mixer_stream_t* s, const void* data, uint32_t bytes) {
    (void)s; (void)data; (void)bytes; return 0;
}
uint32_t snd_mixer_stream_queued(const snd_mixer_stream_t* s) { (void)s; return 0; }
void snd_mixer_stream_flush(snd_mixer_stream_t* s) { (void)s; }
uint32_t snd_mixer_process(snd_mixer_t* m, int16_t* dst, uint32_t dst_frames) {
    (void)m; (void)dst; (void)dst_frames; return 0;
}

#endif /* ENABLE_AUDIO */
