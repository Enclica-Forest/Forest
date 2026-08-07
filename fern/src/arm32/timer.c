/*
 * timer.c - ARM Generic Timer (virtual counter) driver for Fern ARM32
 *
 * Target: ARMv7-A with the Generic Timer extension (Cortex-A15, QEMU virt).
 *
 * The ARM Generic Timer has four timer views:
 *   - Physical   (CNTP_*)  : compares against CNTPCT (physical counter)
 *   - Virtual    (CNTV_*)  : compares against CNTVCT = CNTPCT - CNTVOFF
 *   - Hypervisor (CNTHP_*) : available only from Hyp mode
 *   - Secure     (CNTPS_*) : Secure EL1
 *
 * We use the Virtual timer because it is accessible from Non-Secure SVC
 * mode and is architecturally available on all ARMv7-A CPUs that
 * implement the Generic Timer extension.
 *
 * Interrupt routing
 * -----------------
 * On QEMU -machine virt the virtual timer is PPI 11, which maps to GIC
 * interrupt ID 27 (PPIs occupy GIC IDs 16–31; 16 + 11 = 27).
 *
 * PPIs are banked per CPU and are enabled/disabled via the same
 * GICD_ISENABLER / GICD_ICENABLER registers as SPIs.
 *
 * Periodic timer design (drift-free)
 * -----------------------------------
 *  arm32_timer_init(hz):
 *    1. Read CNTFRQ to determine the counter rate.
 *    2. Store g_cycles_per_tick = CNTFRQ / hz.
 *    3. Set CNTV_CVAL = CNTVCT + g_cycles_per_tick.
 *    4. Enable timer (CNTV_CTL.ENABLE=1, IMASK=0).
 *    5. Register arm32_timer_irq_handler for IRQ 27 and enable in GIC.
 *
 *  arm32_timer_irq_handler(irq):
 *    1. Advance g_next_cval += g_cycles_per_tick (no re-read of CNTVCT).
 *    2. Write g_next_cval to CNTV_CVAL.
 *    3. Increment g_tick_count.
 *    4. Re-write CNTV_CTL.ENABLE to clear CNTV_CTL.ISTATUS.
 */

#include "timer.h"
#include "irq.h"
#include "gic.h"

/* -----------------------------------------------------------------------
 * Module-private state
 * --------------------------------------------------------------------- */

/* Number of counter cycles between consecutive interrupts */
static uint32_t g_cycles_per_tick;

/* Counter frequency read from CNTFRQ at init time (Hz) */
static uint32_t g_cntfrq;

/* Monotonically increasing software tick count (updated in IRQ context) */
static volatile uint64_t g_tick_count;

/* Last CNTV_CVAL programmed; advanced by exactly g_cycles_per_tick on
 * each interrupt to prevent accumulated latency drift */
static uint64_t g_next_cval;

/* -----------------------------------------------------------------------
 * Internal barriers (private to this file to avoid pulling in cache.h)
 * --------------------------------------------------------------------- */
static inline void timer_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline void timer_dsb(void)
{
    __asm__ volatile("dsb" ::: "memory");
}

/* -----------------------------------------------------------------------
 * arm32_timer_init()
 * --------------------------------------------------------------------- */
void arm32_timer_init(uint32_t hz)
{
    if (hz == 0u)
        return;

    /* 1. Discover counter frequency */
    g_cntfrq = arm_get_cntfrq();
    if (g_cntfrq == 0u) {
        /*
         * CNTFRQ is zero – the bootloader has not programmed it (common
         * when booting a raw ELF directly in QEMU).  Fall back to the
         * QEMU virt hardware default of 62.5 MHz.
         */
        g_cntfrq = 62500000u;
    }

    /* 2. Calculate tick period (cycles per interrupt) */
    g_cycles_per_tick = g_cntfrq / hz;
    if (g_cycles_per_tick == 0u)
        return;   /* requested rate exceeds counter frequency */

    /* 3. Disable timer and mask interrupt while programming */
    arm_set_cntv_ctl(CNTV_CTL_IMASK);
    timer_isb();

    /* 4. Programme first compare value */
    g_next_cval = arm_get_cntvct() + (uint64_t)g_cycles_per_tick;
    arm_set_cntv_cval(g_next_cval);
    timer_dsb();
    timer_isb();

    /* 5. Enable the timer (ENABLE=1, IMASK=0 – interrupt will fire) */
    arm_set_cntv_ctl(CNTV_CTL_ENABLE);
    timer_isb();

    /* 6. Register the IRQ handler and enable in the GIC */
    irq_register_handler(ARM_VIRTUAL_TIMER_IRQ, arm32_timer_irq_handler);
    gic_set_priority(ARM_VIRTUAL_TIMER_IRQ, 0xA0u); /* mid-level priority */
    gic_set_target(ARM_VIRTUAL_TIMER_IRQ, 0x01u);   /* CPU 0              */
    gic_enable_irq(ARM_VIRTUAL_TIMER_IRQ);
}

/* -----------------------------------------------------------------------
 * arm32_timer_irq_handler()
 *
 * Called from irq_dispatch() (via irq_table[27]) every time the virtual
 * timer fires.  IRQs are already disabled in CPSR when this runs.
 * --------------------------------------------------------------------- */
void arm32_timer_irq_handler(uint32_t irq)
{
    (void)irq;

    /*
     * Advance the compare value by exactly one tick period.
     *
     * We add g_cycles_per_tick to the *previous* CVAL rather than reading
     * the current CNTVCT.  This prevents the interrupt period from drifting
     * by the handler latency on each call.
     *
     * The timer interrupt is level-sensitive: the GIC IRQ line stays
     * asserted as long as CNTVCT >= CNTV_CVAL.  Writing a new CVAL that
     * is in the future deasserts the line immediately.
     */
    g_next_cval += (uint64_t)g_cycles_per_tick;
    arm_set_cntv_cval(g_next_cval);
    timer_dsb();
    timer_isb();

    /* Increment the software tick counter */
    g_tick_count++;

    /*
     * Re-write CNTV_CTL with ENABLE only (IMASK=0).  Some implementations
     * require a CTL write to latch the new CVAL and clear CNTV_CTL.ISTATUS.
     * Writing CTL is a no-op on implementations that auto-clear ISTATUS.
     */
    arm_set_cntv_ctl(CNTV_CTL_ENABLE);
    timer_isb();
}

/* -----------------------------------------------------------------------
 * arm32_timer_set_hz()
 *
 * Change the periodic interrupt rate at runtime.  Takes effect on the
 * next programmed deadline.
 * --------------------------------------------------------------------- */
void arm32_timer_set_hz(uint32_t hz)
{
    if (hz == 0u || g_cntfrq == 0u)
        return;

    g_cycles_per_tick = g_cntfrq / hz;
    if (g_cycles_per_tick == 0u)
        return;

    /* Re-arm from now */
    g_next_cval = arm_get_cntvct() + (uint64_t)g_cycles_per_tick;
    arm_set_cntv_cval(g_next_cval);
    timer_dsb();
    timer_isb();
}

/* -----------------------------------------------------------------------
 * arm32_timer_get_ticks()
 * --------------------------------------------------------------------- */
uint64_t arm32_timer_get_ticks(void)
{
    return g_tick_count;
}

/* -----------------------------------------------------------------------
 * arm32_timer_get_ns()
 *
 * Convert the hardware CNTVCT counter to nanoseconds.
 *
 * ns = CNTVCT * 1_000_000_000 / CNTFRQ
 *
 * To avoid 64-bit overflow (CNTVCT can be up to 2^64-1) we scale in two
 * steps: first compute microseconds (÷ 1000), then multiply by 1000.
 * This is safe for counters running up to ~585 years at 62.5 MHz.
 * --------------------------------------------------------------------- */
uint64_t arm32_timer_get_ns(void)
{
    if (g_cntfrq == 0u)
        return 0u;

    uint64_t cnt = arm_get_cntvct();
    /* cnt * 1e9 / CNTFRQ: compute as (cnt / CNTFRQ) * 1e9 + remainder */
    uint64_t sec  = cnt / g_cntfrq;
    uint64_t rem  = cnt % g_cntfrq;
    /* rem * 1e9 / CNTFRQ gives sub-second nanoseconds */
    uint64_t frac = (rem * 1000000000ull) / g_cntfrq;
    return sec * 1000000000ull + frac;
}
