/*
 * Cross-architecture timer implementation for Fern
 *
 * Provides the common software layer (tick counter, callback dispatch,
 * busy-wait sleep) and delegates hardware programming to the active
 * architecture's timer driver.
 */

#include "arch/timer.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Software tick counter (common to all architectures)
 * --------------------------------------------------------------------- */

static volatile uint64_t g_ticks;

/* Frequency passed to timer_init(), used for us conversion fallback */
static uint32_t g_frequency_hz;

/* -----------------------------------------------------------------------
 * Periodic callback list
 * --------------------------------------------------------------------- */

static void (*g_callbacks[TIMER_MAX_CALLBACKS])(void);
static uint32_t g_num_callbacks;

/* -----------------------------------------------------------------------
 * Architecture dispatch – function pointers set by timer_init()
 * --------------------------------------------------------------------- */

static void (*arch_init_fn)(uint32_t hz);
static void (*arch_handler_fn)(void);

/* Architecture-specific timer implementations */
#if defined(__i386__) || defined(__x86_64__)
static void arch_init_x86(uint32_t hz);
static void arch_handler_x86(void);
#elif defined(__aarch64__)
static void arch_init_aarch64(uint32_t hz);
static void arch_handler_aarch64(void);
#elif defined(__arm__)
static void arch_init_arm32(uint32_t hz);
static void arch_handler_arm32(void);
#elif defined(__riscv) && (__riscv_xlen == 64)
static void arch_init_riscv64(uint32_t hz);
static void arch_handler_riscv64(void);
#endif

/* -----------------------------------------------------------------------
 * timer_init
 * --------------------------------------------------------------------- */

int timer_init(uint32_t frequency_hz)
{
    if (frequency_hz == 0) {
        frequency_hz = TIMER_DEFAULT_FREQUENCY_HZ;
    }

    g_ticks = 0;
    g_frequency_hz = frequency_hz;
    g_num_callbacks = 0;

    /* Select architecture-specific driver */

#if defined(__i386__) || defined(__x86_64__)
    arch_init_fn   = arch_init_x86;
    arch_handler_fn = arch_handler_x86;
#elif defined(__aarch64__)
    arch_init_fn   = arch_init_aarch64;
    arch_handler_fn = arch_handler_aarch64;
#elif defined(__arm__)
    arch_init_fn   = arch_init_arm32;
    arch_handler_fn = arch_handler_arm32;
#elif defined(__riscv) && (__riscv_xlen == 64)
    arch_init_fn   = arch_init_riscv64;
    arch_handler_fn = arch_handler_riscv64;
#else
    #error "Unsupported architecture for timer"
#endif

    /* Programme the hardware timer */
    arch_init_fn(frequency_hz);

    return 0;
}

/* -----------------------------------------------------------------------
 * Common API
 * --------------------------------------------------------------------- */

uint64_t timer_get_ticks(void)
{
    return g_ticks;
}

uint64_t timer_get_frequency(void)
{
    return g_frequency_hz;
}

uint64_t timer_get_us(void)
{
    if (g_frequency_hz == 0) {
        return 0;
    }

    /*
     * Simple conversion: us = ticks × 1 000 000 / frequency.
     * On architectures with a high-resolution hardware counter this could
     * be overridden; the default uses the software tick counter.
     */
    return (g_ticks * 1000000ULL) / g_frequency_hz;
}

void timer_handler(void)
{
    /* Increment the global software tick counter */
    g_ticks++;

    /* Dispatch to every registered periodic callback */
    for (uint32_t i = 0; i < g_num_callbacks; i++) {
        if (g_callbacks[i]) {
            g_callbacks[i]();
        }
    }
}

void timer_sleep(uint32_t ms)
{
    if (g_frequency_hz == 0 || ms == 0) {
        return;
    }

    uint64_t target = g_ticks + ((uint64_t)ms * g_frequency_hz) / 1000;
    while (g_ticks < target) {
#if defined(__i386__) || defined(__x86_64__)
        __asm__ volatile("hlt");
#elif defined(__arm__)
        __asm__ volatile("wfi");
#elif defined(__aarch64__)
        __asm__ volatile("wfi");
#elif defined(__riscv) && (__riscv_xlen == 64)
        __asm__ volatile("wfi");
#endif
    }
}

int timer_register_callback(void (*callback)(void))
{
    if (!callback) {
        return -1;
    }

    /* Check for duplicate */
    for (uint32_t i = 0; i < g_num_callbacks; i++) {
        if (g_callbacks[i] == callback) {
            return 0; /* already registered */
        }
    }

    if (g_num_callbacks >= TIMER_MAX_CALLBACKS) {
        return -1;
    }

    g_callbacks[g_num_callbacks++] = callback;
    return 0;
}

void timer_unregister_callback(void (*callback)(void))
{
    if (!callback) {
        return;
    }

    for (uint32_t i = 0; i < g_num_callbacks; i++) {
        if (g_callbacks[i] == callback) {
            /* Shift remaining callbacks down */
            for (uint32_t j = i; j < g_num_callbacks - 1; j++) {
                g_callbacks[j] = g_callbacks[j + 1];
            }
            g_callbacks[--g_num_callbacks] = 0;
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * Architecture-specific wrappers
 * --------------------------------------------------------------------- */

#if defined(__i386__) || defined(__x86_64__)

#include "pit.h"
#include "../include/interrupt.h"

extern void timer_interrupt_handler(void);

static void arch_init_x86(uint32_t hz)
{
    (void)hz;
    /*
     * x86 has two timer sources:
     *  1. PIT  – always available, used during early boot and for
     *            calibration of the APIC timer.
     *  2. APIC – per-CPU, higher precision, preferred once calibrated.
     *
     * pit_init_advanced() sets up the PIT at its default 1 kHz rate
     * and registers the IRQ0 handler which calls timer_interrupt_handler().
     * apic_timer_init() calibrates against the PIT and takes over as
     * the primary timing source.
     */
    pit_init_advanced();
    apic_timer_init();
}

static void arch_handler_x86(void)
{
    /* The PIT/APIC ISR already calls timer_interrupt_handler() which
     * in turn calls our timer_handler().  This wrapper exists so the
     * dispatch table is uniform. */
    timer_handler();
}

#endif /* x86 */

/* --------------------------------------------------------------------- */

#if defined(__arm__)

#include "../arm32/timer.h"

static void arch_init_arm32(uint32_t hz)
{
    arm32_timer_init(hz);
}

static void arch_handler_arm32(void)
{
    /*
     * The ARM32 timer IRQ handler calls timer_handler() directly
     * via arm32_timer_irq_handler().  This wrapper satisfies the
     * function pointer table.
     */
    timer_handler();
}

#endif /* ARM32 */

/* --------------------------------------------------------------------- */

#if defined(__aarch64__)

#include "../aarch64/timer.h"

static void arch_init_aarch64(uint32_t hz)
{
    aarch64_timer_init(hz);
}

static void arch_handler_aarch64(void)
{
    /*
     * aarch64_timer_irq_handler() calls timer_tick() (weak alias).
     * The platform layer should define timer_tick() to call our
     * timer_handler().  This wrapper provides the dispatch target.
     */
    timer_handler();
}

#endif /* AArch64 */

/* --------------------------------------------------------------------- */

#if defined(__riscv) && (__riscv_xlen == 64)

#include "../riscv64/clint.h"

/* CLINT tick rate – mtime runs at 10 MHz on QEMU virt */
#define RISCV_MTIME_FREQ    10000000ULL

static void arch_init_riscv64(uint32_t hz)
{
    uint64_t interval = RISCV_MTIME_FREQ / hz;
    if (interval == 0) {
        interval = 1;
    }
    clint_init(interval);
}

static void arch_handler_riscv64(void)
{
    /*
     * The RISC-V timer ISR (SBI call or direct mtimecmp programming)
     * should call our timer_handler() on each tick.
     */
    timer_handler();
}

#endif /* RISC-V 64 */
