#include "include/sound.h"
#include "include/screen.h"
#include "include/libc/string.h"
#include "include/debuglog.h"

typedef struct {
    bool initialized;
    uint8 volume;
} universal_sound_state_t;

static universal_sound_state_t g_universal_state = {0};

static bool universal_detect(SoundDriver* driver) {
    if (!driver) {
        return false;
    }
    driver->state = &g_universal_state;
    memset(&g_universal_state, 0, sizeof(g_universal_state));
    debuglog(DEBUG_INFO, "Universal Sound Driver: Always detects as available\n");
    return true;
}

static bool universal_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    universal_sound_state_t* state = (universal_sound_state_t*)driver->state;
    state->initialized = true;
    state->volume = 255;
    debuglog(DEBUG_INFO, "Universal Sound Driver: Initialized\n");
    return true;
}

static bool universal_play_pcm(SoundDriver* driver, const uint8_t* data, uint32_t length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format) {
        return false;
    }
    universal_sound_state_t* state = (universal_sound_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    // Fallback to PC speaker if available
    SoundDriver* pc_driver = sound_pc_speaker_driver();
    if (pc_driver && pc_driver->play_pcm) {
        debuglog(DEBUG_INFO, "Universal Sound Driver: Using PC speaker fallback\n");
        return pc_driver->play_pcm(pc_driver, data, length, format);
    }

    debuglog(DEBUG_WARN, "Universal Sound Driver: No PCM playback available\n");
    return false;
}

static bool universal_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps) {
    if (!driver || !driver->state || !caps) {
        return false;
    }
    universal_sound_state_t* state = (universal_sound_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    memset(caps, 0, sizeof(DeviceCapabilities));
    caps->supported_formats[0] = PCM_S16;
    caps->supported_formats[1] = PCM_U8;
    caps->max_channels = 1;
    caps->stereo_supported = false;
    caps->little_endian = true;
    caps->native_sample_rates[0] = 22050;
    caps->native_sample_rates[1] = 11025;
    caps->max_buffer_size = 4096;

    return true;
}

static void universal_set_volume(SoundDriver* driver, uint8_t volume) {
    if (!driver || !driver->state) {
        return;
    }
    universal_sound_state_t* state = (universal_sound_state_t*)driver->state;
    state->volume = volume;
    debuglog(DEBUG_INFO, "Universal Sound Driver: Volume set to %u\n", volume);
}

static void universal_beep(SoundDriver* driver, uint32_t frequency_hz, uint32_t duration_ms) {
    // Fallback to PC speaker for beep
    SoundDriver* pc_driver = sound_pc_speaker_driver();
    if (pc_driver && pc_driver->beep) {
        pc_driver->beep(pc_driver, frequency_hz, duration_ms);
    } else {
        debuglog(DEBUG_WARN, "Universal Sound Driver: No beep functionality available\n");
    }
}

static void universal_shutdown(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return;
    }
    universal_sound_state_t* state = (universal_sound_state_t*)driver->state;
    state->initialized = false;
    debuglog(DEBUG_INFO, "Universal Sound Driver: Shutdown\n");
}

static SoundDriver g_universal_driver = {
    .name = "Universal Sound Driver",
    .type = SOUND_DEVICE_NONE,
    .detect = universal_detect,
    .init = universal_init,
    .play_pcm = universal_play_pcm,
    .get_capabilities = universal_get_capabilities,
    .set_volume = universal_set_volume,
    .beep = universal_beep,
    .shutdown = universal_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_universal_driver(void) {
    return &g_universal_driver;
}