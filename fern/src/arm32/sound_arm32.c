/*
 * Fern - ARM32 Sound Drivers
 * arm32/sound_arm32.c
 *
 * Provides sound drivers for ARM32 platforms:
 *
 *   1. BCM PWM beep (Raspberry Pi 3 – BCM2837 PWM peripheral)
 *   2. Silent stub for platforms without audio hardware (QEMU versatilepb, etc.)
 *
 * The BCM PWM peripheral is identical across ARM32 and AArch64 Raspberry Pi
 * builds; this file provides the ARM32-specific entry point.  The hardware
 * register layout and beep logic mirror the AArch64 implementation.
 */

#include "include/sound.h"
#include "arch/sound.h"
#include "arch/platform.h"
#include "include/timer.h"
#include "include/libc/string.h"
#include "include/memory.h"

/* =========================================================================
 * BCM PWM Peripheral Registers (Raspberry Pi 3 – BCM2837)
 * ========================================================================= */

#if PLATFORM_RASPI3

#define PWM_BASE        PLATFORM_PWM_BASE
#define PWM_CTRL        (*(volatile uint32_t *)(PWM_BASE + 0x00))
#define PWM_STATUS      (*(volatile uint32_t *)(PWM_BASE + 0x04))
#define PWM_RANGE1      (*(volatile uint32_t *)(PWM_BASE + 0x08))
#define PWM_FIFO        (*(volatile uint32_t *)(PWM_BASE + 0x10))

#define CM_PWM_BASE     (PLATFORM_PERIPH_BASE + 0x101000)
#define CM_PWM_CNTL     (*(volatile uint32_t *)(CM_PWM_BASE + 0x00))
#define CM_PWM_DIVI     (*(volatile uint32_t *)(CM_PWM_BASE + 0x04))

#define PWM_CTL_PWEN1   (1U << 0)
#define PWM_CTL_MODE1   (1U << 1)
#define PWM_CTL_CLRF1   (1U << 6)
#define PWM_CTL_USEF1   (1U << 5)

#define CM_PWM_PASSWD   0x5A000000U
#define CM_PWM_ENAB     (1U << 4)
#define CM_PWM_SRC_GND  (0U << 0)
#define CM_PWM_SRC_PLLD (6U << 0)

#define PWM_BASE_CLOCK_HZ  500000000U
#define PWM_DEFAULT_DIV    10
#define PWM_PWM_CLOCK_HZ   (PWM_BASE_CLOCK_HZ / PWM_DEFAULT_DIV)

static bool g_pwm_initialized = false;

static void pwm_clock_set(uint32_t divider)
{
    CM_PWM_CNTL = CM_PWM_PASSWD | CM_PWM_SRC_GND;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");
    CM_PWM_CNTL = CM_PWM_PASSWD | CM_PWM_ENAB | CM_PWM_SRC_PLLD
                | ((divider & 0xFFF) << 12);
}

static void pwm_init(void)
{
    if (g_pwm_initialized) return;

    pwm_clock_set(PWM_DEFAULT_DIV);

    PWM_CTRL = 0;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");

    PWM_CTRL = PWM_CTL_CLRF1;
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");

    PWM_RANGE1 = 256;
    PWM_CTRL = PWM_CTL_PWEN1 | PWM_CTL_MODE1 | PWM_CTL_USEF1;

    g_pwm_initialized = true;
}

static void pwm_start_tone(uint32_t freq_hz)
{
    if (freq_hz == 0) return;
    if (!g_pwm_initialized) pwm_init();

    uint32_t range = PWM_PWM_CLOCK_HZ / (8 * freq_hz);
    if (range < 2) range = 2;
    if (range > 0xFFFF) range = 0xFFFF;

    uint32_t duty = range / 2;

    PWM_RANGE1 = range;
    PWM_FIFO = duty;
}

static void pwm_stop_tone(void)
{
    PWM_FIFO = 0;
    PWM_CTRL = 0;
    g_pwm_initialized = false;
}

#endif /* PLATFORM_RASPI3 */

/* =========================================================================
 * SoundDriver instance
 * ========================================================================= */

static SoundDriver g_sound_driver;

static bool sound_arch_detect(SoundDriver* driver)
{
    (void)driver;
#if PLATFORM_RASPI3
    return true;
#else
    return false;
#endif
}

static bool sound_arch_init(SoundDriver* driver)
{
    if (!driver) return false;
#if PLATFORM_RASPI3
    pwm_init();
    return true;
#else
    return false;
#endif
}

static bool sound_arch_play_pcm(SoundDriver* driver,
                                 const uint8* data, uint32 length,
                                 const SoundFormat* format)
{
    (void)driver; (void)data; (void)length; (void)format;
    return false;
}

static bool sound_arch_get_capabilities(SoundDriver* driver, DeviceCapabilities* caps)
{
    (void)driver;
    if (!caps) return false;

    memset(caps, 0, sizeof(DeviceCapabilities));
#if PLATFORM_RASPI3
    caps->supported_formats[0] = PCM_U8;
    caps->max_channels = 1;
    caps->native_sample_rates[0] = 22050;
    caps->stereo_supported = false;
    caps->little_endian = true;
    caps->max_buffer_size = 4096;
#endif
    return true;
}

static void sound_arch_set_volume(SoundDriver* driver, uint8 volume)
{
    (void)driver; (void)volume;
}

static void sound_arch_beep(SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms)
{
    (void)driver;
#if PLATFORM_RASPI3
    if (frequency_hz == 0 || duration_ms == 0) return;
    pwm_start_tone(frequency_hz);
    timer_sleep_ms(duration_ms);
    pwm_stop_tone();
#else
    (void)frequency_hz; (void)duration_ms;
#endif
}

static void sound_arch_shutdown(SoundDriver* driver)
{
    (void)driver;
#if PLATFORM_RASPI3
    PWM_CTRL = 0;
    g_pwm_initialized = false;
#endif
}

/* =========================================================================
 * Public API
 * ========================================================================= */

struct SoundDriver *arch_sound_init(void)
{
    memset(&g_sound_driver, 0, sizeof(SoundDriver));

    g_sound_driver.name = "arm32-platform";
    g_sound_driver.type = SOUND_DEVICE_NONE;
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
