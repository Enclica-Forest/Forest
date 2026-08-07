/*
 * Fern - AArch64 Sound Drivers
 * aarch64/sound_aarch64.c
 *
 * Provides sound drivers for AArch64 platforms:
 *
 *   1. VirtIO Sound (QEMU virt with virtio-snd device)
 *   2. BCM PWM beep (Raspberry Pi 3/4 – BCM2837/2711 PWM peripheral)
 *   3. Silent stub for platforms without audio hardware
 *
 * When QEMU is invoked with -device virtio-snd, this driver detects the
 * virtio-sound MMIO device via FDT, initializes it, and provides full
 * PCM playback through the virtio-snd driver.
 *
 * On Raspberry Pi platforms, the PWM peripheral generates square waves
 * for beep output.  Full PCM playback is not yet supported on PWM.
 */

#include "include/sound.h"
#include "arch/sound.h"
#include "arch/platform.h"
#include "include/timer.h"
#include "include/libc/string.h"
#include "include/memory.h"
#include "../virtio_snd.h"

/* =========================================================================
 * BCM PWM Peripheral Registers (Raspberry Pi 3/4)
 *
 * On BCM2837 (RPi3) the PWM block sits at peripheral_base + 0x20C000.
 * On BCM2711 (RPi4) it is at peripheral_base + 0x20C000 as well.
 * The PWM clock is derived from the GPU clock (typically 250 MHz on RPi).
 * ========================================================================= */

#if PLATFORM_RASPI3 || PLATFORM_RASPI4

#define PWM_BASE        PLATFORM_PWM_BASE
#define PWM_CTRL        (*(volatile uint32_t *)(PWM_BASE + 0x00))
#define PWM_STATUS      (*(volatile uint32_t *)(PWM_BASE + 0x04))
#define PWM_RANGE1      (*(volatile uint32_t *)(PWM_BASE + 0x08))
#define PWM_FIFO        (*(volatile uint32_t *)(PWM_BASE + 0x10))

/* BCM2835/2837/2711 PWM clock manager registers (peripheral_base + 0x101000) */
#define CM_PWM_BASE     (PLATFORM_PERIPH_BASE + 0x101000)
#define CM_PWM_CNTL     (*(volatile uint32_t *)(CM_PWM_BASE + 0x00))
#define CM_PWM_DIVI     (*(volatile uint32_t *)(CM_PWM_BASE + 0x04))

/* PWM control bits */
#define PWM_CTL_PWEN1   (1U << 0)
#define PWM_CTL_MODE1   (1U << 1)   /* 1 = serializer (serialiser) mode */
#define PWM_CTL_CLRF1   (1U << 6)   /* Clear FIFO */
#define PWM_CTL_USEF1   (1U << 5)   /* Use FIFO for channel 1 */
#define PWM_CTL_RPTL1   (1U << 2)   /* Repeat last data when FIFO empty */

/* Clock manager bits */
#define CM_PWM_PASSWD   0x5A000000U
#define CM_PWM_ENAB     (1U << 4)
#define CM_PWM_SRC_GND  (0U << 0)   /* Clock source = ground (disable) */
#define CM_PWM_SRC_PLLD (6U << 0)   /* Clock source = PLLD (500 MHz) */

#define PWM_BASE_CLOCK_HZ  500000000U  /* PLLD output */
#define PWM_DEFAULT_DIV    10          /* Gives 50 MHz PWM clock */
#define PWM_PWM_CLOCK_HZ   (PWM_BASE_CLOCK_HZ / PWM_DEFAULT_DIV)

static bool g_pwm_initialized = false;

/**
 * pwm_clock_set - Configure the PWM clock source and divider.
 * @divider: integer clock divider (1..4095). PWM clock = PLLD / divider.
 */
static void pwm_clock_set(uint32_t divider)
{
    /* Disable clock first */
    CM_PWM_CNTL = CM_PWM_PASSWD | CM_PWM_SRC_GND;
    /* Wait for clock to stabilize */
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");
    /* Enable with PLLD source and integer divider */
    CM_PWM_CNTL = CM_PWM_PASSWD | CM_PWM_ENAB | CM_PWM_SRC_PLLD
                | ((divider & 0xFFF) << 12);
}

/**
 * pwm_init - Initialise the BCM PWM peripheral for audio output.
 *
 * Configures:
 *   - Clock to ~50 MHz (PLLD / 10)
 *   - Channel 1 in serialiser mode, using FIFO, 8-bit data width
 *   - RANGE1 = 256 (8-bit range, matches data width)
 */
static void pwm_init(void)
{
    if (g_pwm_initialized) return;

    /* Set PWM clock */
    pwm_clock_set(PWM_DEFAULT_DIV);

    /* Disable PWM while configuring */
    PWM_CTRL = 0;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");

    /* Clear FIFO */
    PWM_CTRL = PWM_CTL_CLRF1;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");

    /* Set range: 256 = 8-bit unsigned resolution */
    PWM_RANGE1 = 256;

    /* Enable: serialiser mode, use FIFO, channel 1 enabled */
    PWM_CTRL = PWM_CTL_PWEN1 | PWM_CTL_MODE1 | PWM_CTL_USEF1;

    g_pwm_initialized = true;
}

/**
 * pwm_start_tone - Start outputting a square wave at the given frequency.
 * @freq_hz: desired frequency in Hz.
 *
 * In serialiser mode with 8-bit FIFO data:
 *   PWM output frequency = PWM_clock / (data_width * range)
 *   With data_width = 8 (serialiser always shifts 8 bits) and range = 256:
 *     freq_pwm = PWM_clock / (8 * 256) = PWM_clock / 2048
 *
 * For a 440 Hz tone with 50 MHz PWM clock:
 *   range_needed = PWM_clock / (8 * freq_hz)
 *   We can't change RANGE dynamically in the middle of output without
 *   glitches, so we set RANGE to achieve the desired frequency, then
 *   push a 50% duty cycle value into the FIFO.
 *
 * The actual output frequency is:
 *   f_out = PWM_clock / (8 * range)
 *   With range = 256: f_out = 50_000_000 / 2048 = 24414 Hz (too high for
 *   direct tone generation).
 *
 * For lower frequencies, we set RANGE = PWM_clock / (8 * freq_hz):
 *   range = 50_000_000 / (8 * 440) = 14204
 *   FIFO value for 50% duty = range / 2 = 7102
 *
 * NOTE: changing RANGE mid-stream causes a brief glitch.  For a simple beep
 * this is acceptable.
 */
static void pwm_start_tone(uint32_t freq_hz)
{
    if (freq_hz == 0) return;
    if (!g_pwm_initialized) pwm_init();

    /* Calculate range for desired frequency */
    uint32_t range = PWM_PWM_CLOCK_HZ / (8 * freq_hz);
    if (range < 2) range = 2;
    if (range > 0xFFFF) range = 0xFFFF;

    uint32_t duty = range / 2;  /* 50% duty for square wave */

    /* Set range and push duty cycle into FIFO */
    PWM_RANGE1 = range;
    PWM_FIFO = duty;
}

static void pwm_stop_tone(void)
{
    /* Push silence (0) into FIFO to stop tone after current cycle */
    PWM_FIFO = 0;
    /* Disable PWM */
    PWM_CTRL = 0;
    g_pwm_initialized = false;
}

#endif /* PLATFORM_RASPI3 || PLATFORM_RASPI4 */

/* =========================================================================
 * VirtIO Sound integration
 * ========================================================================= */

static bool g_virtio_snd_probed = false;
static bool g_virtio_snd_found = false;

static void probe_virtio_snd_aarch64(void)
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

    /* First, try virtio-snd (QEMU virt platforms). */
    probe_virtio_snd_aarch64();
    if (g_virtio_snd_found)
        return true;

#if PLATFORM_RASPI3 || PLATFORM_RASPI4
    return true;
#else
    return false;
#endif
}

/* --- init --- */
static bool sound_arch_init(SoundDriver* driver)
{
    if (!driver) return false;

    if (g_virtio_snd_found)
        return true;

#if PLATFORM_RASPI3 || PLATFORM_RASPI4
    pwm_init();
    return true;
#else
    return false;
#endif
}

/* --- play_pcm --- */
static bool sound_arch_play_pcm(SoundDriver* driver,
                                 const uint8* data, uint32 length,
                                 const SoundFormat* format)
{
    (void)driver;
    if (!data || !format || length == 0) return false;

    if (g_virtio_snd_found) {
        return virtio_snd_play(data, length, format->sample_rate,
                               format->channels, format->bits_per_sample) > 0;
    }

    return false;
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

#if PLATFORM_RASPI3 || PLATFORM_RASPI4
    caps->supported_formats[0] = PCM_U8;
    caps->max_channels = 1;
    caps->native_sample_rates[0] = 22050;
    caps->stereo_supported = false;
    caps->little_endian = true;
    caps->max_buffer_size = 4096;
#else
    caps->supported_formats[0] = 0;
    caps->max_channels = 0;
    caps->max_buffer_size = 0;
#endif
    return true;
}

/* --- set_volume --- */
static void sound_arch_set_volume(SoundDriver* driver, uint8 volume)
{
    (void)driver; (void)volume;
}

/* --- beep --- */
static void sound_arch_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms)
{
    (void)driver;

    if (g_virtio_snd_found) {
        SoundDriver* vdriver = sound_virtio_driver();
        if (vdriver && vdriver->beep) {
            vdriver->beep(vdriver, frequency_hz, duration_ms);
        }
        return;
    }

#if PLATFORM_RASPI3 || PLATFORM_RASPI4
    if (frequency_hz == 0 || duration_ms == 0) return;
    pwm_start_tone(frequency_hz);
    timer_sleep_ms(duration_ms);
    pwm_stop_tone();
#else
    (void)frequency_hz; (void)duration_ms;
#endif
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
        return;
    }
#if PLATFORM_RASPI3 || PLATFORM_RASPI4
    PWM_CTRL = 0;
    g_pwm_initialized = false;
#endif
}

/* =========================================================================
 * Public API (called from sound.c via arch_sound_init)
 * ========================================================================= */

struct SoundDriver *arch_sound_init(void)
{
    memset(&g_sound_driver, 0, sizeof(SoundDriver));

    g_sound_driver.name = g_virtio_snd_found ? "aarch64-virtio-snd" : "aarch64-platform";
    g_sound_driver.type = g_virtio_snd_found ? SOUND_DEVICE_VIRTIO : SOUND_DEVICE_NONE;
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
    if (g_sound_driver.beep) {
        g_sound_driver.beep(&g_sound_driver, freq_hz, duration_ms);
    }
}

void arch_sound_set_volume(uint8_t volume)
{
    if (g_sound_driver.set_volume) {
        g_sound_driver.set_volume(&g_sound_driver, volume);
    }
}
