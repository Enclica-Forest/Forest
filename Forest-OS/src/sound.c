#include "include/sound.h"
#include "include/vfs.h"
#include "include/screen.h"
#include "include/libc/string.h"
#include "include/timer.h"
#include "include/audio_wav.h"
#include "include/memory.h"
#include "include/thread.h"

extern void* kmalloc(size_t size);

extern uint32_t convert_to_sb16_pcm(const void* src, uint32_t src_frames, const PcmDesc* src_desc,
                                   uint8_t* dst, uint32_t dst_max_frames, PcmFormat target_format,
                                   uint16_t target_channels, uint32_t target_rate);
extern uint32_t convert_to_ac97_pcm(const void* src, uint32_t src_frames, const PcmDesc* src_desc,
                                    int16_t* dst, uint32_t dst_max_frames, uint32_t* out_sample_rate);
extern uint32_t convert_to_hda_pcm(const void* src, uint32_t src_frames, const PcmDesc* src_desc,
                                  int16_t* dst, uint32_t dst_max_frames);
extern uint32_t convert_to_ensoniq_pcm(const void* src, uint32_t src_frames, const PcmDesc* src_desc,
                                      int16_t* dst, uint32_t dst_max_frames);

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define MAX_SOUND_STREAMS 8
#define RING_BUFFER_SIZE 32768
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define MIX_BUFFER_FRAMES 512
#define PROD_CHUNK_RAW 4096
#define PROD_CHUNK_CANONICAL (PROD_CHUNK_RAW * 2)
#define PROD_CHUNK_OUTPUT (PROD_CHUNK_RAW * 2)
#define MAX_WORK_QUEUE 32

typedef struct {
    uint8 data[RING_BUFFER_SIZE];
    volatile uint32 head;
    volatile uint32 tail;
} SoundRingBuffer;

typedef struct {
    sound_stream_id id;
    volatile SoundStreamState state;
    SoundPriority priority;
    uint8 volume;
    bool muted;
    bool has_source;
    bool source_eof;
    vfs_node_t* source_file;
    uint32 source_data_offset;
    uint32 source_data_remaining;
    wav_info_t source_wav_info;
    PcmDesc source_canonical_desc;
    SoundRingBuffer ring;
    sound_callback_t callback;
    void* callback_data;
} SoundStream;

typedef enum {
    WORK_PLAY_WAV = 0,
    WORK_STOP_STREAM,
    WORK_SET_MASTER_VOLUME,
    WORK_DESTROY_STREAM
} SoundWorkType;

typedef struct {
    SoundWorkType type;
    SoundPriority priority;
    bool pending;
    union {
        struct {
            char path[256];
            sound_callback_t callback;
            void* callback_data;
        } play_wav;
        struct {
            sound_stream_id id;
        } stop;
        struct {
            uint8 volume;
        } vol;
        struct {
            sound_stream_id id;
        } destroy;
    };
} SoundWorkItem;

typedef SoundDriver* (*sound_driver_factory_t)(void);

static SoundDriver* g_active_driver = 0;
static SoundStream g_streams[MAX_SOUND_STREAMS];
static sound_stream_id g_next_stream_id = 1;
static uint8 g_master_volume = 255;
static PcmDesc g_output_format;

static struct thread* g_mixer_thread = NULL;
static volatile bool g_mixer_running = false;
static struct semaphore g_mixer_sem;

static SoundWorkItem g_work_queue[MAX_WORK_QUEUE];
static volatile uint32 g_work_count = 0;

static int16_t* g_mix_buffer = NULL;
static int16_t* g_stream_temp = NULL;
static uint8* g_prod_raw = NULL;
static uint8* g_prod_canonical = NULL;
static uint8* g_prod_output = NULL;

static uint32 g_callback_count = 0;
static struct {
    sound_callback_t func;
    void* user_data;
    sound_stream_id id;
} g_pending_callbacks[MAX_SOUND_STREAMS];

static bool sound_play_wav_sync(const char* path);

static void ring_init(SoundRingBuffer* ring) {
    ring->head = 0;
    ring->tail = 0;
}

static uint32 ring_available(const SoundRingBuffer* ring) {
    return ring->head - ring->tail;
}

static uint32 ring_free(const SoundRingBuffer* ring) {
    return RING_BUFFER_SIZE - ring_available(ring);
}

static void ring_write(SoundRingBuffer* ring, const uint8* data, uint32 len) {
    uint32 free_space = ring_free(ring);
    if (len > free_space) len = free_space;
    if (len == 0) return;

    uint32 pos = ring->head & RING_BUFFER_MASK;
    uint32 first = RING_BUFFER_SIZE - pos;
    if (first > len) first = len;

    memcpy(ring->data + pos, data, first);
    if (len > first) {
        memcpy(ring->data, data + first, len - first);
    }
    ring->head += len;
}

static uint32 ring_read(SoundRingBuffer* ring, uint8* dst, uint32 len) {
    uint32 avail = ring_available(ring);
    if (len > avail) len = avail;
    if (len == 0) return 0;

    uint32 pos = ring->tail & RING_BUFFER_MASK;
    uint32 first = RING_BUFFER_SIZE - pos;
    if (first > len) first = len;

    memcpy(dst, ring->data + pos, first);
    if (len > first) {
        memcpy(dst + first, ring->data, len - first);
    }
    ring->tail += len;
    return len;
}

static inline int16_t clamp_s16(int32 sample) {
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return (int16_t)sample;
}

static inline uint8_t s16_to_u8(int16_t sample) {
    return (uint8_t)((sample >> 8) + 128);
}

static uint32 sound_convert_to_output(
    const int16_t* src, uint32 src_frames, uint32 src_rate, uint32 src_channels,
    uint8* dst, uint32 dst_max_frames,
    uint32 dst_rate, uint32 dst_channels, PcmFormat dst_format)
{
    if (src_rate == dst_rate && src_channels == dst_channels &&
        dst_format == PCM_S16 && dst_max_frames >= src_frames) {
        memcpy(dst, src, src_frames * sizeof(int16_t) * src_channels);
        return src_frames;
    }

    uint32 out_frames = 0;
    if (dst_rate > 0 && src_rate > 0) {
        out_frames = (src_frames * dst_rate + src_rate - 1) / src_rate;
    }
    if (out_frames > dst_max_frames) out_frames = dst_max_frames;

    uint32 bytes_per_sample = (dst_format == PCM_S16) ? 2 : 1;
    uint32 frame_bytes = bytes_per_sample * dst_channels;

    for (uint32 i = 0; i < out_frames; i++) {
        uint32 src_i = (src_rate > 0) ? (i * src_rate / dst_rate) : i;
        if (src_i >= src_frames) src_i = src_frames - 1;

        int32 sample_l = 0, sample_r = 0;
        if (src_channels == 2) {
            sample_l = src[src_i * 2];
            sample_r = src[src_i * 2 + 1];
        } else {
            sample_l = sample_r = src[src_i];
        }

        if (dst_channels == 2) {
            if (dst_format == PCM_S16) {
                int16_t* d = (int16_t*)(dst + i * frame_bytes);
                d[0] = clamp_s16(sample_l);
                d[1] = clamp_s16(sample_r);
            } else {
                uint8_t* d = dst + i * frame_bytes;
                d[0] = s16_to_u8(clamp_s16(sample_l));
                d[1] = s16_to_u8(clamp_s16(sample_r));
            }
        } else {
            int32 mono = (sample_l + sample_r) / 2;
            if (dst_format == PCM_S16) {
                ((int16_t*)dst)[i] = clamp_s16(mono);
            } else {
                dst[i] = s16_to_u8(clamp_s16(mono));
            }
        }
    }
    return out_frames;
}

static void sound_determine_output_format(void) {
    DeviceCapabilities caps;
    memset(&caps, 0, sizeof(caps));

    if (g_active_driver && g_active_driver->get_capabilities &&
        g_active_driver->get_capabilities(g_active_driver, &caps)) {
        g_output_format.format = caps.supported_formats[0];
        if (g_output_format.format == 0) g_output_format.format = PCM_S16;
        g_output_format.channels = caps.stereo_supported ? 2 : 1;
        if (g_output_format.channels > caps.max_channels)
            g_output_format.channels = caps.max_channels;
        g_output_format.sample_rate = caps.native_sample_rates[0];
        if (g_output_format.sample_rate == 0) g_output_format.sample_rate = 44100;
    } else {
        g_output_format.format = PCM_S16;
        g_output_format.channels = 2;
        g_output_format.sample_rate = 44100;
    }
}

static SoundStream* sound_find_stream(sound_stream_id id) {
    for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
        if (g_streams[i].id == id && g_streams[i].state != SOUND_STREAM_IDLE) {
            return &g_streams[i];
        }
    }
    return 0;
}

static void sound_produce_for_stream(SoundStream* stream) {
    if (!stream->has_source || stream->source_eof) return;
    if (!stream->source_file) return;

    uint32 free_space = ring_free(&stream->ring);
    if (free_space < PROD_CHUNK_OUTPUT) return;

    uint32 chunk = MIN(stream->source_data_remaining, PROD_CHUNK_RAW);
    if (chunk == 0) {
        stream->source_eof = true;
        return;
    }

    uint32 bytes_read = vfs_read(stream->source_file, stream->source_data_offset, chunk, g_prod_raw);
    if (bytes_read == 0) {
        stream->source_eof = true;
        return;
    }

    uint32 canonical_size = 0;
    wav_error_t err = wav_decode_to_canonical(&stream->source_wav_info, g_prod_raw, bytes_read,
                                              g_prod_canonical, PROD_CHUNK_CANONICAL,
                                              &canonical_size, &stream->source_canonical_desc);
    if (err != WAV_OK || canonical_size == 0) {
        stream->source_data_offset += bytes_read;
        stream->source_data_remaining -= bytes_read;
        return;
    }

    uint32 src_frames = canonical_size / (sizeof(int16_t) * stream->source_canonical_desc.channels);
    uint32 out_frames = sound_convert_to_output(
        (const int16_t*)g_prod_canonical, src_frames,
        stream->source_canonical_desc.sample_rate, stream->source_canonical_desc.channels,
        g_prod_output, PROD_CHUNK_OUTPUT,
        g_output_format.sample_rate, g_output_format.channels, g_output_format.format);

    uint32 bytes_per_sample = (g_output_format.format == PCM_S16) ? 2 : 1;
    uint32 write_bytes = out_frames * bytes_per_sample * g_output_format.channels;
    ring_write(&stream->ring, g_prod_output, write_bytes);

    stream->source_data_offset += bytes_read;
    stream->source_data_remaining -= bytes_read;
    if (stream->source_data_remaining == 0) {
        stream->source_eof = true;
    }
}

static uint32 sound_mix_streams(void) {
    uint32 bytes_per_sample = (g_output_format.format == PCM_S16) ? 2 : 1;
    uint32 frame_bytes = bytes_per_sample * g_output_format.channels;

    memset(g_mix_buffer, 0, MIX_BUFFER_FRAMES * frame_bytes);

    uint32 frames_mixed = 0;

    for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
        SoundStream* s = &g_streams[i];
        if (s->state != SOUND_STREAM_PLAYING) continue;
        if (s->muted || s->volume == 0) continue;

        uint32 bytes_to_read = MIX_BUFFER_FRAMES * frame_bytes;
        uint32 bytes_read = ring_read(&s->ring, (uint8*)g_stream_temp, bytes_to_read);
        uint32 frames_read = bytes_read / frame_bytes;
        if (frames_read > frames_mixed) frames_mixed = frames_read;

        int32 vol = s->volume;
        if (g_output_format.format == PCM_S16) {
            for (uint32 f = 0; f < frames_read; f++) {
                if (g_output_format.channels == 2) {
                    int32 left = ((int32)g_stream_temp[f * 2] * vol) / 255;
                    int32 right = ((int32)g_stream_temp[f * 2 + 1] * vol) / 255;
                    g_mix_buffer[f * 2] = clamp_s16((int32)g_mix_buffer[f * 2] + left);
                    g_mix_buffer[f * 2 + 1] = clamp_s16((int32)g_mix_buffer[f * 2 + 1] + right);
                } else {
                    int32 sample = ((int32)g_stream_temp[f] * vol) / 255;
                    g_mix_buffer[f] = clamp_s16((int32)g_mix_buffer[f] + sample);
                }
            }
        } else {
            for (uint32 f = 0; f < frames_read; f++) {
                if (g_output_format.channels == 2) {
                    uint8* src = (uint8*)g_stream_temp + f * 2;
                    uint8* dst = (uint8*)g_mix_buffer + f * 2;
                    int32 left = (((int32)src[0] - 128) << 8) * vol / 255;
                    int32 right = (((int32)src[1] - 128) << 8) * vol / 255;
                    int32 cur_l = ((int32)dst[0] - 128) << 8;
                    int32 cur_r = ((int32)dst[1] - 128) << 8;
                    cur_l = clamp_s16(cur_l + left);
                    cur_r = clamp_s16(cur_r + right);
                    dst[0] = s16_to_u8(cur_l);
                    dst[1] = s16_to_u8(cur_r);
                } else {
                    int32 src_s = ((int32)((uint8*)g_stream_temp)[f] - 128) << 8;
                    int32 dst_s = ((int32)((uint8*)g_mix_buffer)[f] - 128) << 8;
                    int32 mixed = clamp_s16(dst_s + (src_s * vol) / 255);
                    ((uint8*)g_mix_buffer)[f] = s16_to_u8(mixed);
                }
            }
        }
    }

    if (frames_mixed > 0 && g_master_volume < 255) {
        int32 mv = g_master_volume;
        for (uint32 f = 0; f < frames_mixed; f++) {
            if (g_output_format.channels == 2) {
                g_mix_buffer[f * 2] = (int16_t)(((int32)g_mix_buffer[f * 2] * mv) / 255);
                g_mix_buffer[f * 2 + 1] = (int16_t)(((int32)g_mix_buffer[f * 2 + 1] * mv) / 255);
            } else {
                g_mix_buffer[f] = (int16_t)(((int32)g_mix_buffer[f] * mv) / 255);
            }
        }
    }

    return frames_mixed;
}

static void sound_fire_callbacks(void) {
    uint32 count = g_callback_count;
    g_callback_count = 0;

    for (uint32 i = 0; i < count; i++) {
        if (g_pending_callbacks[i].func) {
            g_pending_callbacks[i].func(g_pending_callbacks[i].id,
                                        g_pending_callbacks[i].user_data);
        }
    }
}

static void sound_enqueue_callback(sound_stream_id id, sound_callback_t func, void* user_data) {
    if (g_callback_count >= MAX_SOUND_STREAMS) return;
    g_pending_callbacks[g_callback_count].func = func;
    g_pending_callbacks[g_callback_count].user_data = user_data;
    g_pending_callbacks[g_callback_count].id = id;
    g_callback_count++;
}

static void sound_process_work_queue(void) {
    while (g_work_count > 0) {
        int best = -1;
        SoundPriority best_pri = SOUND_PRIORITY_LOW - 1;

        for (uint32 i = 0; i < MAX_WORK_QUEUE; i++) {
            if (g_work_queue[i].pending && g_work_queue[i].priority > best_pri) {
                best_pri = g_work_queue[i].priority;
                best = i;
            }
        }
        if (best < 0) break;

        SoundWorkItem item = g_work_queue[best];
        g_work_queue[best].pending = false;
        g_work_count--;

        switch (item.type) {
            case WORK_PLAY_WAV:
                sound_play_wav_sync(item.play_wav.path);
                if (item.play_wav.callback) {
                    sound_enqueue_callback(0, item.play_wav.callback, item.play_wav.callback_data);
                }
                break;
            case WORK_STOP_STREAM: {
                SoundStream* s = sound_find_stream(item.stop.id);
                if (s) {
                    s->state = SOUND_STREAM_DONE;
                    if (s->callback) {
                        sound_enqueue_callback(s->id, s->callback, s->callback_data);
                    }
                }
                break;
            }
            case WORK_SET_MASTER_VOLUME:
                g_master_volume = item.vol.volume;
                if (g_active_driver && g_active_driver->set_volume) {
                    g_active_driver->set_volume(g_active_driver, item.vol.volume);
                }
                break;
            case WORK_DESTROY_STREAM: {
                SoundStream* s = sound_find_stream(item.destroy.id);
                if (s) {
                    if (s->source_file) {
                        vfs_close(s->source_file);
                        s->source_file = 0;
                    }
                    s->state = SOUND_STREAM_IDLE;
                    s->id = 0;
                }
                break;
            }
        }
    }
}

static void* sound_mixer_thread(void* arg) {
    (void)arg;
    print("[SOUND] Mixer thread started\n");

    while (g_mixer_running) {
        sound_process_work_queue();

        for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
            if (g_streams[i].state == SOUND_STREAM_PLAYING && g_streams[i].has_source) {
                sound_produce_for_stream(&g_streams[i]);
            }
        }

        for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
            SoundStream* s = &g_streams[i];
            if (s->state == SOUND_STREAM_PLAYING && s->has_source &&
                s->source_eof && ring_available(&s->ring) == 0) {
                s->state = SOUND_STREAM_DONE;
                if (s->callback) {
                    sound_enqueue_callback(s->id, s->callback, s->callback_data);
                }
            }
        }

        uint32 frames_mixed = sound_mix_streams();

        if (frames_mixed > 0 && g_active_driver && g_active_driver->play_pcm) {
            SoundFormat fmt;
            fmt.sample_rate = g_output_format.sample_rate;
            fmt.channels = g_output_format.channels;
            fmt.bits_per_sample = (g_output_format.format == PCM_S16) ? 16 : 8;
            fmt.signed_samples = (g_output_format.format == PCM_S16);

            uint32 bytes_per_sample = fmt.bits_per_sample / 8;
            uint32 total_bytes = frames_mixed * bytes_per_sample * fmt.channels;

            g_active_driver->play_pcm(g_active_driver, (const uint8*)g_mix_buffer, total_bytes, &fmt);
        }

        sound_fire_callbacks();

        bool any_active = false;
        for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
            if (g_streams[i].state == SOUND_STREAM_PLAYING) {
                any_active = true;
                break;
            }
        }

        if (any_active && frames_mixed > 0 && g_output_format.sample_rate > 0) {
            uint32 sleep_ms = (frames_mixed * 1000) / g_output_format.sample_rate;
            if (sleep_ms == 0) sleep_ms = 1;
            timer_sleep_ms(sleep_ms);
        } else {
            semaphore_down_timeout(&g_mixer_sem, 50);
        }
    }

    print("[SOUND] Mixer thread exiting\n");
    return NULL;
}

static void sound_mixer_init(void) {
    if (g_mixer_thread) return;

    memset(g_streams, 0, sizeof(g_streams));
    memset(g_work_queue, 0, sizeof(g_work_queue));

    g_mix_buffer = kmalloc(MIX_BUFFER_FRAMES * 2 * sizeof(int16_t));
    g_stream_temp = kmalloc(MIX_BUFFER_FRAMES * 2 * sizeof(int16_t));
    g_prod_raw = kmalloc(PROD_CHUNK_RAW);
    g_prod_canonical = kmalloc(PROD_CHUNK_CANONICAL);
    g_prod_output = kmalloc(PROD_CHUNK_OUTPUT);

    if (!g_mix_buffer || !g_stream_temp || !g_prod_raw || !g_prod_canonical || !g_prod_output) {
        print("[SOUND] Failed to allocate mixer buffers\n");
        return;
    }

    semaphore_init(&g_mixer_sem, 0);
    g_mixer_running = true;
    g_mixer_thread = thread_create("sound_mixer", sound_mixer_thread, NULL);

    if (g_mixer_thread) {
        thread_start(g_mixer_thread);
        print("[SOUND] Mixer thread initialized\n");
    } else {
        print("[SOUND] Failed to create mixer thread\n");
        g_mixer_running = false;
    }
}

static void sound_mixer_shutdown(void) {
    if (!g_mixer_thread) return;

    g_mixer_running = false;
    semaphore_up(&g_mixer_sem);

    void* retval;
    thread_join(g_mixer_thread, &retval);
    thread_destroy(g_mixer_thread);
    g_mixer_thread = NULL;

    for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
        if (g_streams[i].source_file) {
            vfs_close(g_streams[i].source_file);
            g_streams[i].source_file = 0;
        }
    }

    if (g_mix_buffer) { kfree(g_mix_buffer); g_mix_buffer = 0; }
    if (g_stream_temp) { kfree(g_stream_temp); g_stream_temp = 0; }
    if (g_prod_raw) { kfree(g_prod_raw); g_prod_raw = 0; }
    if (g_prod_canonical) { kfree(g_prod_canonical); g_prod_canonical = 0; }
    if (g_prod_output) { kfree(g_prod_output); g_prod_output = 0; }

    print("[SOUND] Mixer thread shut down\n");
}

static SoundDriver* fallback_pc_driver(void) {
    SoundDriver* driver = sound_pc_speaker_driver();
    if (!driver) return 0;
    if (driver->detect) driver->detect(driver);
    if (driver->init) driver->init(driver);
    return driver;
}

static bool sound_play_wav_sync(const char* path) {
    if (!path) return false;
    if (!g_active_driver && !sound_system_init()) return false;
    if (!g_active_driver || !g_active_driver->play_pcm || !g_active_driver->get_capabilities) return false;

    print("[SOUND] Playing WAV: ");
    print(path);
    print("\n");

    DeviceCapabilities caps;
    if (!g_active_driver->get_capabilities(g_active_driver, &caps) ||
        caps.supported_formats[0] == 0 || caps.max_buffer_size == 0) {
        print("[SOUND] Device does not support PCM playback\n");
        return false;
    }

    vfs_node_t* file = vfs_open(path, 0);
    if (!file) {
        print("[SOUND] Failed to open file: ");
        print(path);
        print("\n");
        return false;
    }

    uint32 file_size = file->length;
    if (file_size < 44) {
        print("[SOUND] File too small for WAV: ");
        print(path);
        print("\n");
        vfs_close(file);
        return false;
    }

    uint8 header[4096];
    uint32 header_size = MIN(file_size, sizeof(header));
    uint32 bytes_read = vfs_read(file, 0, header_size, header);
    if (bytes_read < 44) {
        print("[SOUND] Failed to read WAV header\n");
        vfs_close(file);
        return false;
    }

    wav_info_t wav_info;
    wav_error_t wav_error = wav_parse_header(header, bytes_read, &wav_info);
    while ((wav_error == WAV_ERROR_NO_DATA_CHUNK || wav_error == WAV_ERROR_NO_FMT_CHUNK) &&
           header_size < file_size && header_size < sizeof(header)) {
        uint32 new_size = MIN(file_size, header_size + 512);
        if (new_size == header_size) break;
        header_size = new_size;
        bytes_read = vfs_read(file, 0, header_size, header);
        wav_error = wav_parse_header(header, bytes_read, &wav_info);
    }
    if (wav_error != WAV_OK) {
        print("[SOUND] WAV parse error: ");
        print(wav_error_string(wav_error));
        print(" - ");
        print(path);
        print("\n");
        vfs_close(file);
        return false;
    }

    if (wav_info.format_code != WAV_FORMAT_PCM &&
        wav_info.format_code != WAV_FORMAT_IEEE_FLOAT &&
        wav_info.format_code != WAV_FORMAT_ALAW &&
        wav_info.format_code != WAV_FORMAT_MULAW) {
        print("[SOUND] Unsupported WAV format: 0x");
        print_hex(wav_info.format_code);
        print("\n");
        vfs_close(file);
        return false;
    }

    if (wav_info.channels < 1 || wav_info.channels > 2) {
        print("[SOUND] Unsupported channel count: ");
        print_dec(wav_info.channels);
        print("\n");
        vfs_close(file);
        return false;
    }

    if (wav_info.sample_rate < 8000 || wav_info.sample_rate > 48000) {
        print("[SOUND] Unsupported sample rate: ");
        print_dec(wav_info.sample_rate);
        print("\n");
        vfs_close(file);
        return false;
    }

    uint32 data_offset = wav_info.data_offset;
    if (wav_info.data_ptr && data_offset == 0) {
        data_offset = (uint32)(wav_info.data_ptr - header);
    }
    uint32 data_size = wav_info.data_size;
    if (data_offset >= file_size) {
        print("[SOUND] WAV data offset outside file\n");
        vfs_close(file);
        return false;
    }
    if (data_offset + data_size > file_size) {
        data_size = file_size - data_offset;
    }

    PcmDesc stream_fmt;
    stream_fmt.format = g_output_format.format;
    stream_fmt.channels = g_output_format.channels;
    stream_fmt.sample_rate = g_output_format.sample_rate;

    sound_stream_id sid = sound_stream_create(stream_fmt, SOUND_PRIORITY_NORMAL);
    if (sid == 0) {
        print("[SOUND] Failed to create stream\n");
        vfs_close(file);
        return false;
    }

    SoundStream* stream = sound_find_stream(sid);
    if (!stream) {
        vfs_close(file);
        return false;
    }

    stream->has_source = true;
    stream->source_eof = false;
    stream->source_file = file;
    stream->source_data_offset = data_offset;
    stream->source_data_remaining = data_size;
    memcpy(&stream->source_wav_info, &wav_info, sizeof(wav_info_t));
    memset(&stream->source_canonical_desc, 0, sizeof(PcmDesc));

    sound_stream_play(sid);
    return true;
}

static void log_driver_failure(const char* driver_name, const char* reason) {
    print("[SOUND] ");
    print(driver_name ? driver_name : "Unknown");
    print(" unavailable: ");
    print(reason ? reason : "no reason given");
    print("\n");
}

bool sound_system_init(void) {
    if (g_active_driver) return true;

    /* Register /dev/snd early so the device node exists even when no
     * hardware driver is ultimately detected; ops resolve the driver
     * lazily and return -ENOSYS when none is available. */
    sound_pcm_init();

    sound_driver_factory_t factories[] = {
        sound_hda_driver,
        sound_ac97_driver,
        sound_ensoniq_driver,
        sound_sb16_driver,
        sound_sbpro_driver,
        sound_opl3_driver,
        sound_usb_sound_driver,
        sound_pc_speaker_driver,
        sound_universal_driver
    };

    const uint32 factory_count = sizeof(factories) / sizeof(factories[0]);

    for (uint32 i = 0; i < factory_count; i++) {
        SoundDriver* driver = 0;
        if (factories[i]) {
            print("[SOUND] Trying driver factory #");
            print_dec(i);
            print("...\n");
            driver = factories[i]();
            print("[SOUND] Factory #");
            print_dec(i);
            print(" returned: ");
            print_hex((uint32)driver);
            print("\n");
        }

        if (!driver) {
            print("[SOUND] No driver from factory #");
            print_dec(i);
            print("\n");
            continue;
        }

        bool detected = true;
        if (driver->detect) {
            print("[SOUND] Running detection for driver: ");
            print(driver->name ? driver->name : "unknown");
            print("\n");
            detected = driver->detect(driver);
            print("[SOUND] Detection result for ");
            print(driver->name ? driver->name : "unknown");
            print(": ");
            print(detected ? "SUCCESS" : "NOT DETECTED");
            print("\n");
        }
        if (!detected) continue;

        if (!driver->init || !driver->init(driver)) {
            log_driver_failure(driver->name, "init failed");
            continue;
        }

        g_active_driver = driver;
        print("[SOUND] Active driver: ");
        print(driver->name);
        print("\n");

        sound_determine_output_format();
        sound_mixer_init();
        return true;
    }

    print("[SOUND] No usable sound devices detected.\n");
    return false;
}

void sound_shutdown(void) {
    sound_pcm_cleanup();
    sound_mixer_shutdown();

    if (g_active_driver && g_active_driver->shutdown) {
        g_active_driver->shutdown(g_active_driver);
    }
    g_active_driver = 0;
}

const SoundDriver* sound_active_driver(void) {
    return g_active_driver;
}

bool sound_play_wav(const char* path) {
    if (!path) return false;

    if (!g_active_driver && !sound_system_init()) return false;
    if (!g_mixer_thread) sound_mixer_init();

    for (uint32 i = 0; i < MAX_WORK_QUEUE; i++) {
        if (!g_work_queue[i].pending) {
            memset(&g_work_queue[i], 0, sizeof(SoundWorkItem));
            g_work_queue[i].type = WORK_PLAY_WAV;
            g_work_queue[i].priority = SOUND_PRIORITY_NORMAL;
            g_work_queue[i].pending = true;
            strncpy(g_work_queue[i].play_wav.path, path, 255);
            g_work_queue[i].play_wav.path[255] = 0;
            g_work_count++;
            semaphore_up(&g_mixer_sem);
            print("[SOUND] Queued WAV playback: ");
            print(path);
            print("\n");
            return true;
        }
    }

    print("[SOUND] Work queue full, dropping: ");
    print(path);
    print("\n");
    return false;
}

void sound_beep(uint32 frequency_hz, uint32 duration_ms) {
    if (!g_active_driver || !g_active_driver->beep) {
        SoundDriver* fallback = fallback_pc_driver();
        if (fallback && fallback->beep) {
            fallback->beep(fallback, frequency_hz, duration_ms);
        }
        return;
    }
    g_active_driver->beep(g_active_driver, frequency_hz, duration_ms);
}

void sound_set_volume(uint8 volume) {
    for (uint32 i = 0; i < MAX_WORK_QUEUE; i++) {
        if (!g_work_queue[i].pending) {
            memset(&g_work_queue[i], 0, sizeof(SoundWorkItem));
            g_work_queue[i].type = WORK_SET_MASTER_VOLUME;
            g_work_queue[i].priority = SOUND_PRIORITY_HIGH;
            g_work_queue[i].pending = true;
            g_work_queue[i].vol.volume = volume;
            g_work_count++;
            semaphore_up(&g_mixer_sem);
            return;
        }
    }
}

sound_stream_id sound_stream_create(PcmDesc desc, SoundPriority priority) {
    if (!g_mixer_thread) {
        if (!g_active_driver && !sound_system_init()) return 0;
        sound_mixer_init();
    }

    for (uint32 i = 0; i < MAX_SOUND_STREAMS; i++) {
        if (g_streams[i].state == SOUND_STREAM_IDLE) {
            memset(&g_streams[i], 0, sizeof(SoundStream));
            g_streams[i].id = g_next_stream_id++;
            if (g_next_stream_id == 0) g_next_stream_id = 1;
            g_streams[i].state = SOUND_STREAM_IDLE;
            g_streams[i].priority = priority;
            g_streams[i].volume = 255;
            g_streams[i].muted = false;
            g_streams[i].source_canonical_desc = desc;
            ring_init(&g_streams[i].ring);
            g_streams[i].callback = 0;
            g_streams[i].callback_data = 0;
            g_streams[i].has_source = false;
            g_streams[i].source_eof = false;
            g_streams[i].source_file = 0;

            print("[SOUND] Created stream #");
            print_dec(g_streams[i].id);
            print("\n");
            return g_streams[i].id;
        }
    }

    print("[SOUND] No free streams\n");
    return 0;
}

bool sound_stream_write(sound_stream_id id, const void* data, uint32 bytes) {
    SoundStream* s = sound_find_stream(id);
    if (!s || !data || bytes == 0) return false;
    ring_write(&s->ring, (const uint8*)data, bytes);
    semaphore_up(&g_mixer_sem);
    return true;
}

bool sound_stream_play(sound_stream_id id) {
    SoundStream* s = sound_find_stream(id);
    if (!s) return false;
    s->state = SOUND_STREAM_PLAYING;
    semaphore_up(&g_mixer_sem);
    print("[SOUND] Stream #");
    print_dec(id);
    print(" playing\n");
    return true;
}

bool sound_stream_pause(sound_stream_id id) {
    SoundStream* s = sound_find_stream(id);
    if (!s) return false;
    if (s->state == SOUND_STREAM_PLAYING) {
        s->state = SOUND_STREAM_PAUSED;
    }
    return true;
}

void sound_stream_stop(sound_stream_id id) {
    for (uint32 i = 0; i < MAX_WORK_QUEUE; i++) {
        if (!g_work_queue[i].pending) {
            memset(&g_work_queue[i], 0, sizeof(SoundWorkItem));
            g_work_queue[i].type = WORK_STOP_STREAM;
            g_work_queue[i].priority = SOUND_PRIORITY_HIGH;
            g_work_queue[i].pending = true;
            g_work_queue[i].stop.id = id;
            g_work_count++;
            semaphore_up(&g_mixer_sem);
            return;
        }
    }
}

void sound_stream_destroy(sound_stream_id id) {
    for (uint32 i = 0; i < MAX_WORK_QUEUE; i++) {
        if (!g_work_queue[i].pending) {
            memset(&g_work_queue[i], 0, sizeof(SoundWorkItem));
            g_work_queue[i].type = WORK_DESTROY_STREAM;
            g_work_queue[i].priority = SOUND_PRIORITY_HIGH;
            g_work_queue[i].pending = true;
            g_work_queue[i].destroy.id = id;
            g_work_count++;
            semaphore_up(&g_mixer_sem);
            return;
        }
    }
}

void sound_stream_set_volume(sound_stream_id id, uint8 volume) {
    SoundStream* s = sound_find_stream(id);
    if (s) s->volume = volume;
}

void sound_stream_set_mute(sound_stream_id id, bool muted) {
    SoundStream* s = sound_find_stream(id);
    if (s) s->muted = muted;
}

void sound_stream_set_callback(sound_stream_id id, sound_callback_t cb, void* user_data) {
    SoundStream* s = sound_find_stream(id);
    if (s) {
        s->callback = cb;
        s->callback_data = user_data;
    }
}

SoundStreamState sound_stream_get_state(sound_stream_id id) {
    SoundStream* s = sound_find_stream(id);
    if (!s) return SOUND_STREAM_IDLE;
    return s->state;
}

void sound_set_master_volume(uint8 volume) {
    g_master_volume = volume;
    if (g_active_driver && g_active_driver->set_volume) {
        g_active_driver->set_volume(g_active_driver, volume);
    }
}

uint8 sound_get_master_volume(void) {
    return g_master_volume;
}
