/*
 * Fern - RISC-V 64-bit Sound Drivers
 * riscv64/sound_riscv64.c
 *
 * Provides sound drivers for RISC-V 64-bit platforms:
 *
 *   1. VirtIO Sound (QEMU virt with virtio-snd device)
 *   2. Silent stub (QEMU virt without audio hardware)
 *
 * When QEMU is invoked with -device virtio-snd, this driver detects the
 * virtio-sound MMIO device via FDT, initializes it, and provides full
 * PCM playback/capture through the virtio-snd driver.
 *
 * All arch_sound_* convenience functions are implemented as no-ops so
 * callers do not need to guard with #ifdef ARCH_RISCV64.
 */

#include "include/sound.h"
#include "arch/sound.h"
#include "include/libc/string.h"
#include "../virtio_snd.h"

/* =========================================================================
 * VirtIO Sound integration
 * ========================================================================= */

static bool g_virtio_snd_probed = false;
static bool g_virtio_snd_found = false;

/**
 * probe_virtio_snd - Try to detect and initialise the virtio-snd device.
 *
 * Calls virtio_snd_init() which probes the FDT for a virtio-mmio node
 * with device ID 18.  If found, the device is initialised and ready for
 * PCM playback.
 */
static void probe_virtio_snd(void)
{
    if (g_virtio_snd_probed) return;
    g_virtio_snd_probed = true;

    if (virtio_snd_init() == 0) {
        g_virtio_snd_found = true;
    }
}

/* =========================================================================
 * SoundDriver instance
 * ========================================================================= */

static SoundDriver g_sound_driver;

/* --- detect --- */
static bool sound_arch_detect(SoundDriver* driver)
{
    (void)driver;
    probe_virtio_snd();
    return g_virtio_snd_found;
}

/* --- init --- */
static bool sound_arch_init(SoundDriver* driver)
{
    (void)driver;
    if (!g_virtio_snd_found) return false;
    return true;
}

/* --- play_pcm --- */
static bool sound_arch_play_pcm(SoundDriver* driver,
                                 const uint8* data, uint32 length,
                                 const SoundFormat* format)
{
    (void)driver;
    if (!g_virtio_snd_found || !format) return false;
    return virtio_snd_play(data, length, format->sample_rate,
                           format->channels, format->bits_per_sample) > 0;
}

/* --- get_capabilities --- */
static bool sound_arch_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps)
{
    (void)driver;
    if (!caps) return false;

    if (g_virtio_snd_found) {
        return virtio_snd_driver_get_capabilities(NULL, caps);
    }

    memset(caps, 0, sizeof(DeviceCapabilities));
    return true;
}

/* --- set_volume --- */
static void sound_arch_set_volume(SoundDriver* driver, uint8 volume)
{
    (void)driver;
    if (g_virtio_snd_found) {
        virtio_snd_set_volume(volume);
    }
}

/* --- beep --- */
static void sound_arch_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms)
{
    (void)driver;
    if (g_virtio_snd_found) {
        /* Use the virtio-snd beep capability via the SoundDriver vtable. */
        SoundDriver* vdriver = sound_virtio_driver();
        if (vdriver && vdriver->beep) {
            vdriver->beep(vdriver, frequency_hz, duration_ms);
        }
    }
}

/* --- shutdown --- */
static void sound_arch_shutdown(SoundDriver* driver)
{
    (void)driver;
    if (g_virtio_snd_found) {
        SoundDriver* vdriver = sound_virtio_driver();
        if (vdriver && vdriver->shutdown) {
            vdriver->shutdown(vdriver);
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

struct SoundDriver *arch_sound_init(void)
{
    memset(&g_sound_driver, 0, sizeof(SoundDriver));

    g_sound_driver.name = "riscv64-virtio-snd";
    g_sound_driver.type = SOUND_DEVICE_VIRTIO;
    g_sound_driver.detect = sound_arch_detect;
    g_sound_driver.init = sound_arch_init;
    g_sound_driver.play_pcm = sound_arch_play_pcm;
    g_sound_driver.get_capabilities = sound_arch_get_capabilities;
    g_sound_driver.set_volume = sound_arch_set_volume;
    g_sound_driver.beep = sound_arch_beep;
    g_sound_driver.shutdown = sound_arch_shutdown;

    if (g_sound_driver.detect && !g_sound_driver.detect(&g_sound_driver)) {
        return NULL;
    }
    if (g_sound_driver.init && !g_sound_driver.init(&g_sound_driver)) {
        return NULL;
    }
    return &g_sound_driver;
}

bool arch_sound_play_pcm(const uint8_t *data, uint32_t len,
                         uint32_t rate, uint8_t channels, uint8_t bits)
{
    if (!g_sound_driver.play_pcm) return false;
    SoundFormat fmt;
    fmt.sample_rate = rate;
    fmt.channels = channels;
    fmt.bits_per_sample = bits;
    fmt.signed_samples = (bits == 16);
    return g_sound_driver.play_pcm(&g_sound_driver, data, len, &fmt);
}

void arch_sound_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    if (g_sound_driver.beep)
        g_sound_driver.beep(&g_sound_driver, freq_hz, duration_ms);
}

void arch_sound_set_volume(uint8_t volume)
{
    if (g_sound_driver.set_volume)
        g_sound_driver.set_volume(&g_sound_driver, volume);
}
