/*
 * Sound PCM character device (/dev/snd)
 *
 * Userspace audio entry point. Applications open /dev/snd, configure the
 * input format/rate/channels via ioctls, then write() PCM bytes which are
 * buffered in a software mixer stream and drained to the active hardware
 * driver by a background thread. Blocking write + double-buffered output +
 * a DRAIN ioctl give the sound player low-latency, back-pressured playback.
 *
 * When no sound driver is available (ENABLE_AUDIO disabled at build time, or
 * no hardware detected at runtime) the device still exists but ioctls return
 * -ENOSYS and write() returns -ENODEV, so callers get a clean failure instead
 * of a missing-node error.
 */

#include "include/device_fs.h"
#include "include/sound.h"
#include "include/sound_mixer.h"
#include "include/sound_pcspeaker.h"
#include "include/memory.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/types.h"
#include "include/thread.h"
#include "include/timer.h"
#include "include/spinlock.h"

#define SND_PCM_MAJOR       116
#define SND_PCM_MINOR       0

#define SND_PCM_OUT_FRAMES  512
#define SND_PCM_OUT_CH_MAX  2
#define SND_PCM_SLEEP_MS    2

extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);

typedef struct {
    const SoundDriver*  driver;
    DeviceCapabilities  caps;
    bool                caps_valid;

    snd_mixer_t*        mixer;
    snd_mixer_stream_t* stream;
    snd_pcm_format_t    in_format;
    snd_pcm_format_t    out_format;
    bool                format_valid;
    bool                playback_active;
    bool                registered;

    uint64_t            position_frames;
    snd_volume_t        volume;
    snd_level_t         last_level;

    int16_t*            out_buf;
    struct thread*      drain_thread;
    volatile bool       drain_running;
} pcm_device_state_t;

static pcm_device_state_t g_pcm;
static device_operations_t g_snd_ops;
static spinlock_t g_pcm_lock;

/* ---- forward declarations ---- */
static int     snd_pcm_open(struct device_node* dev, uint32_t flags);
static int     snd_pcm_close(struct device_node* dev);
static ssize_t snd_pcm_read(struct device_node* dev, void* buf, size_t count, uint64_t off);
static ssize_t snd_pcm_write(struct device_node* dev, const void* buf, size_t count, uint64_t off);
static int     snd_pcm_ioctl(struct device_node* dev, uint32_t request, void* arg);
static int     snd_pcm_mmap(struct device_node* dev, void* addr, size_t len, uint32_t prot, uint64_t off);
static void*   snd_pcm_drain_thread(void* arg);

/* ---- helpers ---- */

static void pcm_resolve_driver(void) {
    if (g_pcm.driver) return;
    g_pcm.driver = sound_active_driver();
    if (g_pcm.driver && g_pcm.driver->get_capabilities) {
        if (g_pcm.driver->get_capabilities((SoundDriver*)g_pcm.driver, &g_pcm.caps)) {
            g_pcm.caps_valid = true;
        }
    }
}

static void pcm_determine_output_format(void) {
    snd_pcm_format_t f;
    f.format = PCM_S16LE;
    f.channels = 2;
    f.rate = 44100;

    if (g_pcm.caps_valid) {
        if (g_pcm.caps.supported_formats[0] != 0) {
            f.format = g_pcm.caps.supported_formats[0];
        }
        if (f.format == 0) f.format = PCM_S16LE;
        f.channels = g_pcm.caps.stereo_supported ? 2 : 1;
        if (f.channels > g_pcm.caps.max_channels && g_pcm.caps.max_channels > 0) {
            f.channels = (uint16_t)g_pcm.caps.max_channels;
        }
        if (g_pcm.caps.native_sample_rates[0] != 0) {
            f.rate = g_pcm.caps.native_sample_rates[0];
        }
    }
    g_pcm.out_format = f;
}

static bool pcm_ensure_stream(void) {
    if (g_pcm.stream) return true;
    if (!g_pcm.mixer) {
        pcm_resolve_driver();
        pcm_determine_output_format();
        g_pcm.mixer = snd_mixer_create(&g_pcm.out_format);
        if (!g_pcm.mixer) return false;
        snd_mixer_set_master(g_pcm.mixer, g_pcm.volume.master);
    }
    snd_pcm_format_t in = g_pcm.format_valid ? g_pcm.in_format : g_pcm.out_format;
    g_pcm.stream = snd_mixer_stream_create(g_pcm.mixer, &in);
    if (!g_pcm.stream) return false;
    snd_mixer_stream_set_volume(g_pcm.stream, g_pcm.volume.pcm);
    if (g_pcm.muted) snd_mixer_stream_set_mute(g_pcm.stream, true);

    if (!g_pcm.out_buf) {
        g_pcm.out_buf = (int16_t*)kmalloc(SND_PCM_OUT_FRAMES * SND_PCM_OUT_CH_MAX * sizeof(int16_t));
        if (!g_pcm.out_buf) return false;
    }
    if (!g_pcm.drain_thread) {
        g_pcm.drain_running = true;
        g_pcm.drain_thread = thread_create("snd_pcm_drain", snd_pcm_drain_thread, NULL);
        if (!g_pcm.drain_thread) return false;
        thread_start(g_pcm.drain_thread);
    }
    return true;
}

static void pcm_stop_drain(void) {
    if (!g_pcm.drain_thread) return;
    g_pcm.drain_running = false;
    void* rv;
    thread_join(g_pcm.drain_thread, &rv);
    thread_destroy(g_pcm.drain_thread);
    g_pcm.drain_thread = NULL;
}

static void pcm_teardown_stream(void) {
    pcm_stop_drain();
    if (g_pcm.mixer && g_pcm.stream) {
        snd_mixer_stream_destroy(g_pcm.mixer, g_pcm.stream);
    }
    g_pcm.stream = NULL;
    if (g_pcm.mixer) {
        snd_mixer_destroy(g_pcm.mixer);
        g_pcm.mixer = NULL;
    }
    if (g_pcm.out_buf) {
        kfree(g_pcm.out_buf);
        g_pcm.out_buf = NULL;
    }
}

static int pcm_validate_format(const snd_pcm_format_t* f) {
    if (!f || f->channels == 0 || f->channels > 2) return 0;
    switch (f->format) {
        case PCM_S8: case PCM_U8:
        case PCM_S16LE: case PCM_S16BE:
        case PCM_S24LE: case PCM_S32LE:
        case PCM_F32LE:
            break;
        default: return 0;
    }
    if (f->rate < 8000 || f->rate > 192000) return 0;
    return 1;
}

/* ---- drain thread: pull mixed frames and feed the hardware driver ---- */

static void* snd_pcm_drain_thread(void* arg) {
    (void)arg;
    while (g_pcm.drain_running) {
        if (!g_pcm.playback_active || !g_pcm.mixer || !g_pcm.stream || !g_pcm.driver) {
            timer_sleep_ms(SND_PCM_SLEEP_MS);
            continue;
        }
        if (!g_pcm.driver->play_pcm) {
            timer_sleep_ms(SND_PCM_SLEEP_MS);
            continue;
        }

        uint32_t frames = snd_mixer_process(g_pcm.mixer, g_pcm.out_buf, SND_PCM_OUT_FRAMES);
        if (frames == 0) {
            timer_sleep_ms(SND_PCM_SLEEP_MS);
            continue;
        }

        SoundFormat sf;
        memset(&sf, 0, sizeof(sf));
        sf.sample_rate = g_pcm.out_format.rate;
        sf.channels = g_pcm.out_format.channels;
        sf.bits_per_sample = 16;
        sf.signed_samples = true;

        uint32_t bytes = frames * g_pcm.out_format.channels * sizeof(int16_t);
        g_pcm.driver->play_pcm((SoundDriver*)g_pcm.driver,
                               (const uint8_t*)g_pcm.out_buf, bytes, &sf);

        spinlock_acquire(&g_pcm_lock);
        g_pcm.position_frames += frames;
        spinlock_release(&g_pcm_lock);

        snd_vu_meter_compute(g_pcm.out_buf, frames, PCM_S16LE,
                             (uint8_t)g_pcm.out_format.channels, &g_pcm.last_level);
    }
    return NULL;
}

/* ---- device operations ---- */

static int snd_pcm_open(struct device_node* dev, uint32_t flags) {
    (void)dev;
    (void)flags;
    pcm_resolve_driver();
    if (!g_pcm.format_valid) {
        g_pcm.in_format.format = PCM_S16LE;
        g_pcm.in_format.channels = 2;
        g_pcm.in_format.rate = 44100;
    }
    if (g_pcm.volume.master == 0 && g_pcm.volume.pcm == 0) {
        g_pcm.volume.master = 255;
        g_pcm.volume.pcm = 255;
    }
    debuglog(DEBUG_INFO, "SND_PCM: opened (driver=%s)\n",
             g_pcm.driver ? (g_pcm.driver->name ? g_pcm.driver->name : "?") : "none");
    return DEVICE_SUCCESS;
}

static int snd_pcm_close(struct device_node* dev) {
    (void)dev;
    /* Drain pending data on close so nothing is cut off abruptly. */
    if (g_pcm.playback_active && g_pcm.stream && g_pcm.mixer) {
        uint32_t idle = 0;
        while (snd_mixer_stream_queued(g_pcm.stream) > 0 && idle < 5000) {
            timer_sleep_ms(SND_PCM_SLEEP_MS);
            idle++;
        }
    }
    g_pcm.playback_active = false;
    pcm_teardown_stream();
    return DEVICE_SUCCESS;
}

static ssize_t snd_pcm_read(struct device_node* dev, void* buf, size_t count, uint64_t off) {
    (void)dev; (void)buf; (void)count; (void)off;
    return -DEVICE_ERROR_NOT_SUPPORTED;
}

static ssize_t snd_pcm_write(struct device_node* dev, const void* buf, size_t count, uint64_t off) {
    (void)dev; (void)off;
    if (!buf || count == 0) return 0;
    if (!g_pcm.driver) return -DEVICE_ERROR_NOT_FOUND;

    if (!pcm_ensure_stream()) return -DEVICE_ERROR_NO_MEMORY;

    /* Blocking write: push into the mixer stream ring, yielding while full. */
    size_t written = 0;
    const uint8_t* p = (const uint8_t*)buf;
    while (written < count) {
        uint32_t n = snd_mixer_stream_write(g_pcm.stream, p + written,
                                            (uint32_t)(count - written));
        if (n == 0) {
            /* Ring full: unless playback is active, drop to avoid an
             * infinite stall (e.g. user never called START). */
            if (!g_pcm.playback_active) {
                if (written == 0) return -DEVICE_ERROR_NOT_SUPPORTED;
                break;
            }
            timer_sleep_ms(SND_PCM_SLEEP_MS);
            continue;
        }
        written += n;
    }
    return (ssize_t)written;
}

static int snd_pcm_mmap(struct device_node* dev, void* addr, size_t len, uint32_t prot, uint64_t off) {
    (void)dev; (void)addr; (void)len; (void)prot; (void)off;
    return -DEVICE_ERROR_NOT_SUPPORTED;
}

static void fill_device_info(snd_device_info_t* info) {
    memset(info, 0, sizeof(*info));
    if (g_pcm.driver) {
        info->formats = (1u << PCM_S16LE);
        info->min_rate = 8000;
        info->max_rate = 48000;
        info->max_channels = 2;
        info->capabilities = SND_CAP_PLAYBACK | SND_CAP_VOLUME | SND_CAP_MIXING;
        if (g_pcm.driver->beep) info->capabilities |= SND_CAP_BEEP;
        if (g_pcm.caps_valid) {
            if (g_pcm.caps.max_channels) info->max_channels = (uint8_t)g_pcm.caps.max_channels;
            if (g_pcm.caps.native_sample_rates[0]) info->min_rate = g_pcm.caps.native_sample_rates[0];
            if (g_pcm.caps.native_sample_rates[1]) info->max_rate = g_pcm.caps.native_sample_rates[1];
        }
        if (g_pcm.driver->name) {
            for (int i = 0; g_pcm.driver->name[i] && i < 31; i++) info->name[i] = g_pcm.driver->name[i];
        }
    } else {
        info->formats = 0;
        info->capabilities = 0;
        info->name[0] = 0;
    }
}

static int snd_pcm_ioctl(struct device_node* dev, uint32_t request, void* arg) {
    (void)dev;

    /* Legacy raw ioctl numbers -> map to the new ones. */
    switch (request) {
        case SOUND_IOCTL_GET_POSITION: request = SND_IOCTL_GET_POSITION; break;
        case SOUND_IOCTL_GET_FORMAT:   request = SND_IOCTL_GET_FORMAT;   break;
        case SOUND_IOCTL_SET_FORMAT:   request = SND_IOCTL_SET_FORMAT;   break;
        case SOUND_IOCTL_GET_CAPS:     request = SND_IOCTL_GET_CAPS;     break;
        case SOUND_IOCTL_START:        request = SND_IOCTL_START;        break;
        case SOUND_IOCTL_STOP:         request = SND_IOCTL_STOP;         break;
        default: break;
    }

    switch (request) {
        case SND_IOCTL_OPEN:
            pcm_resolve_driver();
            return DEVICE_SUCCESS;

        case SND_IOCTL_GET_INFO:
        case SND_IOCTL_GET_CAPS: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            fill_device_info((snd_device_info_t*)arg);
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_GET_FORMAT: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            if (!g_pcm.format_valid) return -DEVICE_ERROR_INVALID_PARAM;
            *(snd_pcm_format_t*)arg = g_pcm.in_format;
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_SET_FORMAT: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            const snd_pcm_format_t* f = (const snd_pcm_format_t*)arg;
            if (!pcm_validate_format(f)) return -DEVICE_ERROR_INVALID_PARAM;
            g_pcm.in_format = *f;
            g_pcm.format_valid = true;
            /* Recreate the stream so the mixer resamples from the new format. */
            if (g_pcm.stream && g_pcm.mixer) {
                snd_mixer_stream_destroy(g_pcm.mixer, g_pcm.stream);
                g_pcm.stream = snd_mixer_stream_create(g_pcm.mixer, &g_pcm.in_format);
                if (g_pcm.stream) {
                    snd_mixer_stream_set_volume(g_pcm.stream, g_pcm.volume.pcm);
                    if (g_pcm.muted) snd_mixer_stream_set_mute(g_pcm.stream, true);
                }
            }
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_SET_RATE: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            uint32_t rate = *(uint32_t*)arg;
            if (rate < 8000 || rate > 192000) return -DEVICE_ERROR_INVALID_PARAM;
            g_pcm.in_format.rate = rate;
            g_pcm.format_valid = true;
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_SET_CHANNELS: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            uint8_t ch = *(uint8_t*)arg;
            if (ch == 0 || ch > 2) return -DEVICE_ERROR_INVALID_PARAM;
            g_pcm.in_format.channels = ch;
            g_pcm.format_valid = true;
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_SET_VOLUME: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            snd_volume_t v = *(snd_volume_t*)arg;
            g_pcm.volume = v;
            g_pcm.muted = (v.muted != 0);
            if (g_pcm.mixer) snd_mixer_set_master(g_pcm.mixer, v.master);
            if (g_pcm.stream) {
                snd_mixer_stream_set_volume(g_pcm.stream, v.pcm);
                snd_mixer_stream_set_mute(g_pcm.stream, g_pcm.muted);
            }
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_GET_VOLUME: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            *(snd_volume_t*)arg = g_pcm.volume;
            ((snd_volume_t*)arg)->muted = g_pcm.muted ? 1 : 0;
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_GET_POSITION: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            spinlock_acquire(&g_pcm_lock);
            *(uint64_t*)arg = g_pcm.position_frames;
            spinlock_release(&g_pcm_lock);
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_START: {
            if (!g_pcm.driver) return -DEVICE_ERROR_NOT_SUPPORTED;
            if (!pcm_ensure_stream()) return -DEVICE_ERROR_NO_MEMORY;
            g_pcm.playback_active = true;
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_STOP: {
            g_pcm.playback_active = false;
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_DRAIN: {
            if (!g_pcm.stream) return DEVICE_SUCCESS;
            uint32_t idle = 0;
            while (snd_mixer_stream_queued(g_pcm.stream) > 0 &&
                   g_pcm.playback_active && idle < 5000) {
                timer_sleep_ms(SND_PCM_SLEEP_MS);
                idle++;
            }
            return DEVICE_SUCCESS;
        }

        case SND_IOCTL_WRITE_PCM: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            const snd_buffer_t* b = (const snd_buffer_t*)arg;
            if (!b->data || b->size == 0) return -DEVICE_ERROR_INVALID_PARAM;
            ssize_t r = snd_pcm_write(NULL, b->data, b->size, 0);
            return (r < 0) ? (int)r : DEVICE_SUCCESS;
        }

        case SND_IOCTL_GET_LEVEL: {
            if (!arg) return -DEVICE_ERROR_INVALID_PARAM;
            *(snd_level_t*)arg = g_pcm.last_level;
            return DEVICE_SUCCESS;
        }

        default:
            return -DEVICE_ERROR_NOT_SUPPORTED;
    }
}

/* ---- init / cleanup ---- */

int sound_pcm_init(void) {
    if (g_pcm.registered) return DEVICE_SUCCESS;

    memset(&g_pcm, 0, sizeof(g_pcm));
    g_pcm.in_format.format = PCM_S16LE;
    g_pcm.in_format.channels = 2;
    g_pcm.in_format.rate = 44100;
    g_pcm.volume.master = 255;
    g_pcm.volume.pcm = 255;
    spinlock_init(&g_pcm_lock, "snd_pcm");

    pcm_resolve_driver();

    g_snd_ops.open  = snd_pcm_open;
    g_snd_ops.close = snd_pcm_close;
    g_snd_ops.read  = snd_pcm_read;
    g_snd_ops.write = snd_pcm_write;
    g_snd_ops.ioctl = snd_pcm_ioctl;
    g_snd_ops.mmap  = snd_pcm_mmap;
    g_snd_ops.poll  = NULL;
    g_snd_ops.flush = NULL;
    g_snd_ops.suspend = NULL;
    g_snd_ops.resume  = NULL;
    g_snd_ops.get_info = NULL;
    g_snd_ops.set_config = NULL;

    device_params_t params;
    memset(&params, 0, sizeof(params));
    params.name = "snd";
    params.major = SND_PCM_MAJOR;
    params.minor = SND_PCM_MINOR;
    params.type = DT_CHR;
    params.mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    params.uid = 0;
    params.gid = 0;
    params.ops = &g_snd_ops;
    params.private_data = &g_pcm;

    int result = device_register(&params);
    if (result != DEVICE_SUCCESS) {
        debuglog(DEBUG_ERROR, "SND_PCM: device_register failed: %d\n", result);
        return result;
    }

    g_pcm.registered = true;
    debuglog(DEBUG_INFO, "SND_PCM: /dev/snd registered (driver=%s)\n",
             g_pcm.driver ? (g_pcm.driver->name ? g_pcm.driver->name : "?") : "none");
    return DEVICE_SUCCESS;
}

void sound_pcm_cleanup(void) {
    if (!g_pcm.registered) return;
    pcm_teardown_stream();
    device_unregister(device_make_device_id(SND_PCM_MAJOR, SND_PCM_MINOR));
    g_pcm.registered = false;
    g_pcm.driver = NULL;
    debuglog(DEBUG_INFO, "SND_PCM: /dev/snd unregistered\n");
}
