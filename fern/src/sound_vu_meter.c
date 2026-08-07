#include "include/sound.h"
#include "include/libc/string.h"

/*
 * VU meter helper: compute per-channel RMS and peak levels from an
 * interleaved PCM buffer. Output values are scaled to 0..32767 so callers
 * (the sound player TUI, or SND_IOCTL_GET_LEVEL) can render bars directly.
 *
 * This module is pure arithmetic with no dependency on the active sound
 * driver, so it links cleanly whether or not ENABLE_AUDIO is defined.
 */

static int32_t s24le_to_s32(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (u & 0x800000u) u |= 0xFF000000u;
    return (int32_t)u;
}

static int32_t s32le_to_s32(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

static float f32le_to_f(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* Decode one sample at byte offset into a normalized int32_t roughly in
 * [-32768, 32767] for integer formats (24/32 bit right-shifted), or
 * [-32768, 32767] for float (-1.0..1.0 scaled). */
static int32_t decode_sample(const uint8_t* base, uint32_t idx, PcmFormat fmt) {
    const uint8_t* p = base + idx;
    switch (fmt) {
        case PCM_S8:    return ((int32_t)((int8_t)p[0])) << 8;
        case PCM_U8:    return ((int32_t)p[0] - 128) << 8;
        case PCM_S16LE: return (int32_t)((int16_t)(p[0] | (p[1] << 8)));
        case PCM_S16BE: return (int32_t)((int16_t)((p[0] << 8) | p[1]));
        case PCM_S24LE: return s24le_to_s32(p) >> 8;
        case PCM_S32LE: return s32le_to_s32(p) >> 16;
        case PCM_F32LE: {
            float f = f32le_to_f(p);
            int64_t s = (int64_t)(f * 32767.0f);
            if (s > 32767)  s = 32767;
            if (s < -32768) s = -32768;
            return (int32_t)s;
        }
        default:        return 0;
    }
}

static uint8_t bytes_per_sample(PcmFormat fmt) {
    switch (fmt) {
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

void snd_vu_meter_compute(const void* pcm, uint32_t frames,
                          PcmFormat fmt, uint8_t channels, snd_level_t* out) {
    if (!out) return;
    out->rms_l = out->rms_r = out->peak_l = out->peak_r = 0;
    if (!pcm || frames == 0 || channels == 0) return;

    uint8_t bps = bytes_per_sample(fmt);
    if (bps == 0) return;

    const uint8_t* base = (const uint8_t*)pcm;
    uint32_t frame_bytes = bps * channels;

    uint64_t sum_l = 0, sum_r = 0;
    int32_t  peak_l = 0, peak_r = 0;
    uint32_t counted = 0;

    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        if (channels == 1) {
            l = r = decode_sample(base, i * frame_bytes, fmt);
        } else {
            l = decode_sample(base, i * frame_bytes, fmt);
            r = decode_sample(base, i * frame_bytes + bps, fmt);
        }
        int32_t al = l < 0 ? -l : l;
        int32_t ar = r < 0 ? -r : r;
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;
        sum_l += (uint64_t)al;
        sum_r += (uint64_t)ar;
        counted++;
    }

    if (counted == 0) return;
    /* RMS approximation using mean of absolute values (cheap, monotonic). */
    uint32_t rms_l = (uint32_t)(sum_l / counted);
    uint32_t rms_r = (uint32_t)(sum_r / counted);

    out->rms_l  = (uint16_t)(rms_l  > 32767 ? 32767 : rms_l);
    out->rms_r  = (uint16_t)(rms_r  > 32767 ? 32767 : rms_r);
    out->peak_l = (uint16_t)(peak_l > 32767 ? 32767 : peak_l);
    out->peak_r = (uint16_t)(peak_r > 32767 ? 32767 : peak_r);
}
