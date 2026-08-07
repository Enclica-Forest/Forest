/*
 * Cross-architecture timer interface for Fern
 *
 * Provides a unified API that dispatches to the appropriate hardware timer
 * driver on each supported architecture:
 *   x86:     PIT + APIC timer (pit.c, apic_timer.c)
 *   ARM32:   ARM Generic Timer (arm32/timer.c)
 *   AArch64: CNTP_TVAL_EL1 virtual timer (aarch64/timer.c)
 *   RISC-V:  CLINT mtimecmp (riscv64/clint.c)
 *
 * All architectures share:
 *   - A software tick counter incremented on each hardware interrupt.
 *   - A list of periodic callbacks invoked every tick.
 *   - A busy-wait sleep primitive based on the tick counter.
 */

#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of periodic callbacks that can be registered */
#define TIMER_MAX_CALLBACKS 16

/* Default timer frequency used when no explicit frequency is given */
#define TIMER_DEFAULT_FREQUENCY_HZ 1000

/* -----------------------------------------------------------------------
 * Core API – common to every architecture
 * --------------------------------------------------------------------- */

/**
 * timer_init – Initialise the architecture-specific hardware timer.
 * @frequency_hz: Desired tick rate in Hz (e.g. 100 for a 10 ms period).
 *
 * Returns 0 on success, negative errno on failure.
 *
 * After this call the software tick counter starts incrementing and any
 * callbacks previously registered via timer_register_callback() will be
 * invoked on every tick.
 */
int timer_init(uint32_t frequency_hz);

/**
 * timer_get_ticks – Return the software tick count since boot.
 *
 * Incremented once per timer interrupt.  Wraps after ~5.8×10^12 years at
 * 1000 Hz so overflow is not a practical concern.
 */
uint64_t timer_get_ticks(void);

/**
 * timer_get_frequency – Return the timer frequency in Hz.
 */
uint64_t timer_get_frequency(void);

/**
 * timer_get_us – Return microseconds elapsed since boot.
 *
 * Derived from the hardware counter where available (ARM CNTPCT, RISC-V
 * mtime), falling back to tick_count × (1000000 / frequency).
 */
uint64_t timer_get_us(void);

/**
 * timer_handler – Called by each architecture's IRQ handler on every tick.
 *
 * This function:
 *   1. Increments the global software tick counter.
 *   2. Invokes every registered periodic callback.
 *
 * Architecture-specific drivers must call this from their timer ISR.
 */
void timer_handler(void);

/**
 * timer_sleep – Busy-wait for @ms milliseconds.
 *
 * Simple spin-loop comparing the tick counter.  Suitable for short delays
 * during early boot; for longer waits the scheduler sleep should be used.
 */
void timer_sleep(uint32_t ms);

/**
 * timer_register_callback – Register a function to be called every tick.
 * @callback: Function pointer (no arguments, no return value).
 *
 * Up to TIMER_MAX_CALLBACKS callbacks may be registered.  Returns 0 on
 * success, -1 if the list is full.
 */
int timer_register_callback(void (*callback)(void));

/**
 * timer_unregister_callback – Remove a previously registered callback.
 * @callback: The same pointer passed to timer_register_callback().
 *
 * No-op if the callback was not previously registered.
 */
void timer_unregister_callback(void (*callback)(void));

/* -----------------------------------------------------------------------
 * Architecture-specific helpers
 *
 * Each architecture provides these internally.  The unified layer calls
 * them through function pointers set during timer_init(); the prototypes
 * here exist so that individual arch drivers can be tested in isolation.
 * --------------------------------------------------------------------- */

/* x86 */
void x86_pit_init(uint32_t frequency_hz);
void x86_pit_handler(void);

/* ARM32 */
void arm32_timer_init(uint32_t hz);
void arm32_timer_irq_handler(uint32_t irq);

/* AArch64 */
void aarch64_timer_init(uint32_t hz);
void aarch64_timer_irq_handler(void);

/* RISC-V 64 */
void clint_init(uint64_t interval);
uint64_t clint_get_time(void);

#endif /* ARCH_TIMER_H */
