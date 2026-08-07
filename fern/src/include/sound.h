
#ifndef SOUND_H
#define SOUND_H

#include "types.h"
#include "ioctl.h"
#include <stdbool.h>

typedef enum {
    SOUND_DEVICE_NONE = 0,
    SOUND_DEVICE_PC_SPEAKER,
    SOUND_DEVICE_SOUND_BLASTER16,
    SOUND_DEVICE_SOUND_BLASTER_PRO,
    SOUND_DEVICE_AC97,
    SOUND_DEVICE_HDA,
    SOUND_DEVICE_ENSONIQ_AUDIOPCI,
    SOUND_DEVICE_OPL3,
    SOUND_DEVICE_USB_AUDIO,
    SOUND_DEVICE_UNIVERSAL
} SoundDeviceType;

/* Legacy values (PCM_U8/PCM_S16/PCM_F32) keep their original numeric
 * values so existing driver capability tables continue to work. The
 * extended names alias to the legacy ones where they overlap. */
typedef enum {
    PCM_U8    = 1,
    PCM_S16   = 2,
    PCM_F32   = 3,
    PCM_S8     = 4,
    PCM_S16LE  = PCM_S16,
    PCM_S16BE  = 5,
    PCM_S24LE  = 6,
    PCM_S32LE  = 7,
    PCM_F32LE  = PCM_F32
} PcmFormat;

typedef struct {
    PcmFormat format;
    uint16_t channels;
    uint32_t sample_rate;
} PcmDesc;

typedef struct {
    uint32 sample_rate;
    uint16 channels;
    uint16 bits_per_sample;
    bool   signed_samples;
} SoundFormat;

typedef struct {
    PcmFormat supported_formats[4];
    uint32 max_channels;
    uint32 native_sample_rates[8];
    bool stereo_supported;
    bool little_endian;
    uint32 max_buffer_size;
} DeviceCapabilities;

typedef uint32 sound_stream_id;

typedef enum {
    SOUND_PRIORITY_LOW = 0,
    SOUND_PRIORITY_NORMAL = 10,
    SOUND_PRIORITY_HIGH = 20
} SoundPriority;

typedef enum {
    SOUND_STREAM_IDLE = 0,
    SOUND_STREAM_PLAYING,
    SOUND_STREAM_PAUSED,
    SOUND_STREAM_DONE
} SoundStreamState;

typedef void (*sound_callback_t)(sound_stream_id id, void* user_data);

typedef struct SoundDriver {
    const char* name;
    SoundDeviceType type;
    bool (*detect)(struct SoundDriver* driver);
    bool (*init)(struct SoundDriver* driver);
    bool (*play_pcm)(struct SoundDriver* driver,
                      const uint8* data,
                      uint32 length,
                      const SoundFormat* format);
    bool (*get_capabilities)(struct SoundDriver* driver, DeviceCapabilities* caps);
    void (*set_volume)(struct SoundDriver* driver, uint8 volume);
    void (*beep)(struct SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms);
    void (*shutdown)(struct SoundDriver* driver);
    void* state;
    uint8 volume;
} SoundDriver;

bool sound_system_init(void);
void sound_shutdown(void);
const SoundDriver* sound_active_driver(void);
bool sound_play_wav(const char* path);
void sound_beep(uint32 frequency_hz, uint32 duration_ms);
void sound_set_volume(uint8 volume);

sound_stream_id sound_stream_create(PcmDesc desc, SoundPriority priority);
bool sound_stream_write(sound_stream_id id, const void* data, uint32 bytes);
bool sound_stream_play(sound_stream_id id);
bool sound_stream_pause(sound_stream_id id);
void sound_stream_stop(sound_stream_id id);
void sound_stream_destroy(sound_stream_id id);
void sound_stream_set_volume(sound_stream_id id, uint8 volume);
void sound_stream_set_mute(sound_stream_id id, bool muted);
void sound_stream_set_callback(sound_stream_id id, sound_callback_t cb, void* user_data);
SoundStreamState sound_stream_get_state(sound_stream_id id);
void sound_set_master_volume(uint8 volume);
uint8 sound_get_master_volume(void);

SoundDriver* sound_pc_speaker_driver(void);
SoundDriver* sound_sb16_driver(void);
SoundDriver* sound_sbpro_driver(void);
SoundDriver* sound_ac97_driver(void);
SoundDriver* sound_hda_driver(void);
SoundDriver* sound_ensoniq_driver(void);
SoundDriver* sound_opl3_driver(void);
SoundDriver* sound_usb_sound_driver(void);
SoundDriver* sound_universal_driver(void);

/* =========================================================================
 * Public userspace audio API (/dev/snd ioctls, structs, helpers).
 * Usable by both kernel and userspace (freestanding) builds.
 * ========================================================================= */

/* PCM format descriptor used by SET_FORMAT / GET_FORMAT ioctls. */
typedef struct {
    PcmFormat format;
    uint16_t  channels;
    uint32_t  rate;
} snd_pcm_format_t;

/* Device info returned by SND_IOCTL_GET_INFO / GET_CAPS. */
typedef struct {
    uint32_t formats;       /* bitmask: (1u << PCM_*) */
    uint32_t min_rate;
    uint32_t max_rate;
    uint8_t  max_channels;
    uint8_t  reserved[3];
    uint32_t capabilities;  /* SND_CAP_* */
    char     name[32];
} snd_device_info_t;

/* Buffer descriptor for SND_IOCTL_WRITE_PCM. */
typedef struct {
    const void* data;       /* pointer to PCM bytes */
    uint32_t   size;        /* byte count */
    uint8_t    format;      /* PcmFormat */
    uint8_t    channels;
    uint16_t   reserved;
    uint32_t   rate;
} snd_buffer_t;

/* Volume levels (master, pcm, per-channel). 0..255 each. */
typedef struct {
    uint8_t master;
    uint8_t pcm;
    uint8_t left;
    uint8_t right;
    uint8_t muted;          /* non-zero => muted */
} snd_volume_t;

/* VU meter reading (RMS + peak, 0..32767). */
typedef struct {
    uint16_t rms_l;
    uint16_t rms_r;
    uint16_t peak_l;
    uint16_t peak_r;
} snd_level_t;

/* Device capability flags. */
#define SND_CAP_PLAYBACK   0x01u
#define SND_CAP_CAPTURE    0x02u
#define SND_CAP_DUPLEX     0x04u
#define SND_CAP_VOLUME     0x08u
#define SND_CAP_MIXING     0x10u
#define SND_CAP_BEEP       0x20u

/* Audio device ioctl numbers. Magic = 'S' (0x53), Linux-compatible encoding. */
#define SND_IOCTL_OPEN         _IO('S', 0x00)
#define SND_IOCTL_SET_FORMAT   _IOW('S', 0x01, snd_pcm_format_t)
#define SND_IOCTL_SET_RATE     _IOW('S', 0x02, uint32_t)
#define SND_IOCTL_SET_CHANNELS _IOW('S', 0x03, uint8_t)
#define SND_IOCTL_SET_VOLUME   _IOW('S', 0x04, snd_volume_t)
#define SND_IOCTL_GET_VOLUME   _IOR('S', 0x05, snd_volume_t)
#define SND_IOCTL_GET_POSITION _IOR('S', 0x06, uint64_t)
#define SND_IOCTL_START        _IO('S', 0x07)
#define SND_IOCTL_STOP         _IO('S', 0x08)
#define SND_IOCTL_DRAIN        _IO('S', 0x09)
#define SND_IOCTL_WRITE_PCM    _IOW('S', 0x0A, snd_buffer_t)
#define SND_IOCTL_GET_LEVEL    _IOR('S', 0x0B, snd_level_t)
#define SND_IOCTL_GET_INFO     _IOR('S', 0x0C, snd_device_info_t)
#define SND_IOCTL_GET_CAPS     _IOR('S', 0x0D, snd_device_info_t)
#define SND_IOCTL_GET_FORMAT   _IOR('S', 0x0E, snd_pcm_format_t)

/* Legacy raw ioctl numbers (no in-tree callers; kept for compatibility). */
#define SOUND_IOCTL_GET_POSITION  0x1000
#define SOUND_IOCTL_GET_FORMAT    0x1001
#define SOUND_IOCTL_SET_FORMAT    0x1002
#define SOUND_IOCTL_GET_CAPS      0x1003
#define SOUND_IOCTL_START         0x1004
#define SOUND_IOCTL_STOP          0x1005

/* VU meter helper: compute RMS/peak from an interleaved PCM buffer. */
void snd_vu_meter_compute(const void* pcm, uint32_t frames,
                          PcmFormat fmt, uint8_t channels, snd_level_t* out);

/* PCM character device (/dev/snd) lifecycle, called by the sound core. */
int  sound_pcm_init(void);
void sound_pcm_cleanup(void);

#endif
