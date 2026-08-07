#include "include/sound.h"
#include "include/pci.h"
#include "include/usb.h"
#include "include/screen.h"
#include "include/libc/string.h"
#include "include/debuglog.h"

typedef struct {
    bool initialized;
    uint8 volume;
    usb_device_t* usb_device;
} usb_sound_state_t;

static usb_sound_state_t g_usb_sound_state = {0};

static bool usb_sound_detect(SoundDriver* driver) {
    if (!driver) {
        return false;
    }
    driver->state = &g_usb_sound_state;
    memset(&g_usb_sound_state, 0, sizeof(g_usb_sound_state));
    
    // Check if USB subsystem is available and there are any audio devices
    debuglog(DEBUG_INFO, "USB Sound Driver: USB support not implemented yet\n");
    return false;
}

static bool usb_sound_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    usb_sound_state_t* state = (usb_sound_state_t*)driver->state;
    if (!state->usb_device) {
        debuglog(DEBUG_ERROR, "USB Sound Driver: No USB device available\n");
        return false;
    }

    state->initialized = true;
    state->volume = 255;
    debuglog(DEBUG_INFO, "USB Sound Driver: Initialized\n");
    return true;
}

static bool usb_sound_play_pcm(SoundDriver* driver, const uint8_t* data, uint32_t length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format) {
        return false;
    }
    usb_sound_state_t* state = (usb_sound_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    // TODO: Implement USB audio playback
    debuglog(DEBUG_WARN, "USB Sound Driver: PCM playback not implemented yet\n");
    return false;
}

static bool usb_sound_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps) {
    if (!driver || !driver->state || !caps) {
        return false;
    }
    usb_sound_state_t* state = (usb_sound_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    memset(caps, 0, sizeof(DeviceCapabilities));
    caps->supported_formats[0] = PCM_S16;
    caps->supported_formats[1] = PCM_U8;
    caps->supported_formats[2] = PCM_F32;
    caps->max_channels = 2;
    caps->stereo_supported = true;
    caps->little_endian = true;
    caps->native_sample_rates[0] = 48000;
    caps->native_sample_rates[1] = 44100;
    caps->native_sample_rates[2] = 22050;
    caps->native_sample_rates[3] = 11025;
    caps->max_buffer_size = 8192;

    return true;
}

static void usb_sound_set_volume(SoundDriver* driver, uint8_t volume) {
    if (!driver || !driver->state) {
        return;
    }
    usb_sound_state_t* state = (usb_sound_state_t*)driver->state;
    state->volume = volume;
    debuglog(DEBUG_INFO, "USB Sound Driver: Volume set to %u\n", volume);
}

static void usb_sound_beep(SoundDriver* driver, uint32_t frequency_hz, uint32_t duration_ms) {
    // Fallback to PC speaker for beep
    SoundDriver* pc_driver = sound_pc_speaker_driver();
    if (pc_driver && pc_driver->beep) {
        pc_driver->beep(pc_driver, frequency_hz, duration_ms);
    } else {
        debuglog(DEBUG_WARN, "USB Sound Driver: No beep functionality available\n");
    }
}

static void usb_sound_shutdown(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return;
    }
    usb_sound_state_t* state = (usb_sound_state_t*)driver->state;
    state->initialized = false;
    state->usb_device = NULL;
    debuglog(DEBUG_INFO, "USB Sound Driver: Shutdown\n");
}

static SoundDriver g_usb_sound_driver = {
    .name = "USB Audio",
    .type = SOUND_DEVICE_UNIVERSAL,
    .detect = usb_sound_detect,
    .init = usb_sound_init,
    .play_pcm = usb_sound_play_pcm,
    .get_capabilities = usb_sound_get_capabilities,
    .set_volume = usb_sound_set_volume,
    .beep = usb_sound_beep,
    .shutdown = usb_sound_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_usb_sound_driver(void) {
    return &g_usb_sound_driver;
}