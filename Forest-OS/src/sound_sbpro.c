#include "include/sound.h"
#include "include/io_ports.h"
#include "include/screen.h"
#include "include/libc/string.h"
#include "include/debuglog.h"

#define SBPRO_PORT_BASE 0x220
#define SBPRO_IRQ 5
#define SBPRO_DMA 1

typedef struct {
    bool initialized;
    uint8 volume;
    uint16_t port_base;
    uint8_t irq;
    uint8_t dma;
} sbpro_state_t;

static sbpro_state_t g_sbpro_state = {0};

static bool sbpro_detect(SoundDriver* driver) {
    if (!driver) {
        return false;
    }
    driver->state = &g_sbpro_state;
    memset(&g_sbpro_state, 0, sizeof(g_sbpro_state));

    // Try to detect Sound Blaster Pro at common port addresses
    uint16_t ports[] = {0x220, 0x240, 0x260, 0x280};
    for (int i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        uint16_t port = ports[i];
        
        // Check if SBPro exists by testing the reset port
        outportb(port + 0x6, 1);
        for (volatile int j = 0; j < 100; j++) { }
        outportb(port + 0x6, 0);
        for (volatile int j = 0; j < 1000; j++) { }
        
        uint8_t status = inportb(port + 0xE);
        if (status == 0xAA) {
            debuglog(DEBUG_INFO, "Sound Blaster Pro: Found at port 0x%X\n", port);
            g_sbpro_state.port_base = port;
            g_sbpro_state.irq = SBPRO_IRQ;
            g_sbpro_state.dma = SBPRO_DMA;
            return true;
        }
    }

    debuglog(DEBUG_INFO, "Sound Blaster Pro: Not detected\n");
    return false;
}

static bool sbpro_init(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return false;
    }
    sbpro_state_t* state = (sbpro_state_t*)driver->state;
    if (!state->port_base) {
        debuglog(DEBUG_ERROR, "Sound Blaster Pro: No port base detected\n");
        return false;
    }

    // Reset the sound card
    outportb(state->port_base + 0x6, 1);
    for (volatile int i = 0; i < 100; i++) { }
    outportb(state->port_base + 0x6, 0);
    for (volatile int i = 0; i < 1000; i++) { }

    state->initialized = true;
    state->volume = 255;
    debuglog(DEBUG_INFO, "Sound Blaster Pro: Initialized\n");
    return true;
}

static bool sbpro_play_pcm(SoundDriver* driver, const uint8_t* data, uint32_t length, const SoundFormat* format) {
    if (!driver || !driver->state || !data || !format) {
        return false;
    }
    sbpro_state_t* state = (sbpro_state_t*)driver->state;
    if (!state->initialized) {
        return false;
    }

    // TODO: Implement SBPro PCM playback
    debuglog(DEBUG_WARN, "Sound Blaster Pro: PCM playback not implemented yet\n");
    return false;
}

static bool sbpro_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps) {
    if (!driver || !driver->state || !caps) {
        return false;
    }
    sbpro_state_t* state = (sbpro_state_t*)driver->state;
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
    caps->native_sample_rates[2] = 5512;
    caps->max_buffer_size = 4096;

    return true;
}

static void sbpro_set_volume(SoundDriver* driver, uint8_t volume) {
    if (!driver || !driver->state) {
        return;
    }
    sbpro_state_t* state = (sbpro_state_t*)driver->state;
    state->volume = volume;
    debuglog(DEBUG_INFO, "Sound Blaster Pro: Volume set to %u\n", volume);
}

static void sbpro_beep(SoundDriver* driver, uint32_t frequency_hz, uint32_t duration_ms) {
    // Fallback to PC speaker for beep
    SoundDriver* pc_driver = sound_pc_speaker_driver();
    if (pc_driver && pc_driver->beep) {
        pc_driver->beep(pc_driver, frequency_hz, duration_ms);
    } else {
        debuglog(DEBUG_WARN, "Sound Blaster Pro: No beep functionality available\n");
    }
}

static void sbpro_shutdown(SoundDriver* driver) {
    if (!driver || !driver->state) {
        return;
    }
    sbpro_state_t* state = (sbpro_state_t*)driver->state;
    state->initialized = false;
    debuglog(DEBUG_INFO, "Sound Blaster Pro: Shutdown\n");
}

static SoundDriver g_sbpro_driver = {
    .name = "Sound Blaster Pro",
    .type = SOUND_DEVICE_SOUND_BLASTER16, // Use SB16 type for compatibility
    .detect = sbpro_detect,
    .init = sbpro_init,
    .play_pcm = sbpro_play_pcm,
    .get_capabilities = sbpro_get_capabilities,
    .set_volume = sbpro_set_volume,
    .beep = sbpro_beep,
    .shutdown = sbpro_shutdown,
    .state = 0,
    .volume = 255
};

SoundDriver* sound_sbpro_driver(void) {
    return &g_sbpro_driver;
}