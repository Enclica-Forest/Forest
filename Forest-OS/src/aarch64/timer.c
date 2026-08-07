/*
 * Fern - AArch64 Generic Timer implementation
 *
 * Uses the virtual timer (CNTV_*_EL0) so that CNTVOFF_EL2 offsets applied
 * by a hypervisor are honoured.  On bare QEMU virt the offset is 0, making
 * virtual and physical counters identical.
 *
 * Periodic operation
 * ------------------
 * The timer generates an IRQ when CNTVCT_EL0 >= CNTV_CVAL_EL0 and the
 * interrupt is not masked (CNTV_CTL_EL0.IMASK = 0).  The IRQ line remains
 * asserted for as long as the condition holds, so the handler must advance
 * CNTV_CVAL past the current counter before returning.
 *
 * Drift prevention
 * ----------------
 * The handler advances CNTV_CVAL by exactly g_interval from the *previous*
 * compare value, not from the current counter.  Accumulated handler latency
 * therefore does not cause long-term frequency error.
 *
 * GIC integration
 * ---------------
 * INTID 27 is a PPI (per-CPU private interrupt).  It is enabled via the
 * GICv3 Redistributor GICR_ISENABLER0.  gicv3_enable_irq(27) handles the
 * correct register selection automatically (see gic.c).
 *
 * timer_tick() hook
 * -----------------
 * If the Fern platform layer defines timer_tick() it will be called
 * from aarch64_timer_irq_handler() on every tick.  The function is declared
 * with a weak alias so that the driver links even when no callback exists.
 */

#include "timer.h"
#include "uart.h"
#include "gic.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Fallback CNTFRQ when firmware leaves it at zero                      */
/* ------------------------------------------------------------------ */

/** QEMU virt default counter frequency: 62.5 MHz */
#define TIMER_FALLBACK_FREQ  62500000UL

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

/** Counter frequency (Hz) read from CNTFRQ_EL0 at init time. */
static uint64_t g_freq = 0;

/** Counter ticks per timer IRQ period. */
static uint64_t g_interval = 0;

/** CNTV_CVAL value set at the last IRQ, advanced each period. */
static uint64_t g_next_cval = 0;

/** Software tick counter, incremented once per IRQ. */
static volatile uint64_t g_tick_count = 0;

/* ------------------------------------------------------------------ */
/* Weak timer_tick() hook                                               */
/* ------------------------------------------------------------------ */

/*
 * timer_tick is declared weak so that:
 *   - The driver links without it (it becomes a no-op).
 *   - The platform layer can override it with a real function.
 */
void __attribute__((weak)) timer_tick(void)
{
    /* default: nothing */
}

/* ------------------------------------------------------------------ */
/* aarch64_timer_init                                                   */
/* ------------------------------------------------------------------ */

void aarch64_timer_init(uint32_t hz)
{
    /* Validate hz before computing the interval. */
    if (hz == 0U) {
        uart_puts("[timer] ERROR: hz=0 in aarch64_timer_init, using hz=100\n");
        hz = 100U;
    }

    /* 1. Read the counter frequency programmed by firmware. */
    g_freq = read_cntfrq_el0();
    if (g_freq == 0UL) {
        uart_puts("[timer] WARNING: CNTFRQ_EL0 = 0, assuming 62.5 MHz\n");
        g_freq = TIMER_FALLBACK_FREQ;
    }

    uart_printf("[timer] Counter frequency: %lu Hz\n", g_freq);

    /* 2. Calculate counter ticks per IRQ interval. */
    g_interval = g_freq / (uint64_t)hz;
    if (g_interval == 0UL) {
        uart_printf("[timer] ERROR: hz=%u exceeds CNTFRQ=%lu, clamped to 1\n",
                    hz, g_freq);
        g_interval = 1UL;
    }

    uart_printf("[timer] Tick rate: %u Hz, interval: %lu ticks\n",
                hz, g_interval);

    /* 3. Disable the timer while programming the compare value. */
    write_cntv_ctl_el0(0UL);
    __asm__ volatile("isb");

    /* 4. Program the first compare value = current counter + one interval. */
    g_next_cval = read_cntvct_el0() + g_interval;
    write_cntv_cval_el0(g_next_cval);

    /* 5. Enable the timer with interrupt unmasked (ENABLE=1, IMASK=0). */
    write_cntv_ctl_el0(CNTV_CTL_ENABLE);
    __asm__ volatile("isb");

    /* 6. Enable the virtual timer PPI in the GICv3 Redistributor.
     *    gicv3_set_priority sets a mid-level priority (below the PMR of 0xFF).
     *    gicv3_enable_irq routes the PPI enable to GICR_ISENABLER0 for
     *    INTID < 32 (see gic.c). */
    gicv3_set_priority(TIMER_VTIMER_INTID, 0xA0U);
    gicv3_enable_irq(TIMER_VTIMER_INTID);

    uart_puts("[timer] Virtual timer (INTID 27) enabled\n");
}

/* ------------------------------------------------------------------ */
/* aarch64_timer_reload                                                 */
/* ------------------------------------------------------------------ */

void aarch64_timer_reload(void)
{
    /*
     * Advance the compare value by exactly one interval from the previous
     * compare value.  This keeps the average interrupt rate accurate even
     * when the handler runs slightly late.
     */
    g_next_cval += g_interval;
    write_cntv_cval_el0(g_next_cval);

    /*
     * Re-enable the timer in case a previous write to CNTV_CTL masked it.
     * Some implementations also clear CNTV_CTL_EL0.ISTATUS on a write to
     * CNTV_CVAL, but we write CTL anyway to be safe.
     */
    write_cntv_ctl_el0(CNTV_CTL_ENABLE);
    __asm__ volatile("isb");
}

/* ------------------------------------------------------------------ */
/* aarch64_timer_irq_handler                                            */
/* ------------------------------------------------------------------ */

void aarch64_timer_irq_handler(void)
{
    /*
     * 1. Advance the compare register so the IRQ line de-asserts.
     *    Must happen before EOI to avoid an immediate re-entry.
     */
    aarch64_timer_reload();

    /* 2. Increment the software tick counter. */
    g_tick_count++;

    /* 3. Notify the Fern platform layer (weak hook; no-op if absent). */
    timer_tick();
}

/* ------------------------------------------------------------------ */
/* aarch64_timer_set_interval                                           */
/* ------------------------------------------------------------------ */

void aarch64_timer_set_interval(uint32_t hz)
{
    if (hz == 0U || g_freq == 0UL)
        return;

    g_interval = g_freq / (uint64_t)hz;
    if (g_interval == 0UL)
        g_interval = 1UL;

    /* Re-arm from the current counter value. */
    g_next_cval = read_cntvct_el0() + g_interval;
    write_cntv_cval_el0(g_next_cval);
    write_cntv_ctl_el0(CNTV_CTL_ENABLE);
    __asm__ volatile("isb");
}

/* ------------------------------------------------------------------ */
/* Read-only accessors                                                  */
/* ------------------------------------------------------------------ */

uint64_t aarch64_timer_get_ticks(void)
{
    /*
     * Return the physical counter (CNTPCT_EL0) as the raw hardware tick.
     * This is monotonically increasing and usable for fine-grained stamps
     * between two IRQ events.
     */
    return read_cntpct_el0();
}

uint64_t aarch64_timer_get_ns(void)
{
    if (g_freq == 0UL)
        return 0UL;

    uint64_t ticks = read_cntpct_el0();

    /*
     * Convert ticks to nanoseconds without overflow.
     *
     * Naive formula:  ns = ticks * 1_000_000_000 / freq
     * For a 62.5 MHz counter the product overflows after ~148 seconds.
     *
     * Safe split:
     *   sec     = ticks / freq            (integer seconds)
     *   rem     = ticks % freq            (sub-second ticks)
     *   ns_frac = rem * 1_000_000_000 / freq
     *
     * rem < freq < 2^32 (typical), so rem * 1e9 fits in 63 bits for
     * frequencies >= 2 (which is always true).
     */
    uint64_t sec     = ticks / g_freq;
    uint64_t rem     = ticks % g_freq;
    uint64_t ns_frac = (rem * 1000000000UL) / g_freq;

    return sec * 1000000000UL + ns_frac;
}

uint64_t aarch64_timer_get_ms(void)
{
    return aarch64_timer_get_ns() / 1000000UL;
}

uint32_t aarch64_timer_get_freq(void)
{
    return (uint32_t)g_freq;
}

uint64_t aarch64_timer_get_tick_count(void)
{
    return g_tick_count;
}
