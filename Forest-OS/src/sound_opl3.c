#include "include/sound.h"
#include "include/io_ports.h"
#include "include/screen.h"
#include "include/libc/string.h"
#include "include/debuglog.h"

#define OPL3_PORT_BASE 0x388

typedef struct {
    bool initialized;
    uint8 volume;
    uint16_t port_base;
} opl3_state_t;

static opl3_state_t g_opl3_state = {0};

static bool opl3_detect(SoundDriver* driver) {
    if (!driver) {
        return false;
    }
    driver->state = &g_opl3_state;
    memset(&g_opl3_state, 0, sizeof(g_opl3_state));

    // Try to detect OPL3 at common port addresses
    uint16_t ports[] = {0x388, 0x398};
    for (int i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        uint16_t port = ports[i];
        
        // Check OPL3 presence
        outportb(port, 0x00);
        uint8_t test1 = inportb(port);
        outportb(port, 0xFF);
        uint8_t test2 = inportb(port);
        
        if (test1 != test2) {
            debuglog(DEBUG_INFO, "Yamaha OPL3: Found at port 0x%X\n", port);
            g_opl3_state.port_base = port;
            return true;
        }
    }

    debuglog(DEBUG_INFO, "Yamaha OPL3: Not detected\n");
    return false;
}

static bool opl3_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    opl3_state_t* state = (opl3_state_t*)driver->state;
    if (!state->port_base) {
        debuglog(DEBUG_ERROR, "Yamaha OPL3: No port base detected\n");
        return false;
    }

    state->initialized = true;
    state->volume = 255;
    debuglog(DEBUG_INFO, "Yamaha OPL3: Initialized\n");
    return true;
}

static bool opl3_play_pcm(SoundDriver* driver, const uint8_t* data, uint32_t length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format) {
        return false;
    }
    opl3_state_t* state = (opl3_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    // TODO: Implement OPL3 PCM playback
    debuglog(DEBUG_WARN, "Yamaha OPL3: PCM playback not implemented yet\n");
    return false;
}

static bool opl3_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps) {
    if (!driver || !driver->state || !caps) {
        return false;
    }
    opl3_state_t* state = (opl3_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    memset(caps, 0, sizeof(DeviceCapabilities));
    caps->supported_formats[0] = PCM_U8;
    caps->supported_formats[1] = PCM_S16;
    caps->max_channels = 2;
    caps->stereo_supported = true;
    caps->little_endian = true;
    caps->native_sample_rates[0] = 22050;
    caps->native_sample_rates[1] = 11025;
    caps->max_buffer_size = 2048;

    return true;
}

static void opl3_set_volume(SoundDriver* driver, uint8_t volume) {
    if (!driver || !driver->state) {
        return;
    }
    opl3_state_t* state = (opl3_state_t*)driver->state;
    state->volume = volume;
    debuglog(DEBUG_INFO, "Yamaha OPL3: Volume set to %u\n", volume);
}

static void opl3_beep(SoundDriver* driver, uint32_t frequency_hz, uint32_t duration_ms) {
    // Fallback to PC speaker for beep
    SoundDriver* pc_driver = sound_pc_speaker_driver();
    if (pc_driver && pc_driver->beep) {
        pc_driver->beep(pc_driver, frequency_hz, duration_ms);
    } else {
        debuglog(DEBUG_WARN, "Yamaha OPL3: No beep functionality available\n");
    }
}

static void opl3_shutdown(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return;
    }
    opl3_state_t* state = (opl3_state_t*)driver->state;
    state->initialized = false;
    debuglog(DEBUG_INFO, "Yamaha OPL3: Shutdown\n");
}

static SoundDriver g_opl3_driver = {
    .name = "Yamaha OPL3",
    .type = SOUND_DEVICE_UNIVERSAL,
    .detect = opl3_detect,
    .init = opl3_init,
    .play_pcm = opl3_play_pcm,
    .get_capabilities = opl3_get_capabilities,
    .set_volume = opl3_set_volume,
    .beep = opl3_beep,
    .shutdown = opl3_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_opl3_driver(void) {
    return &g_opl3_driver;
}