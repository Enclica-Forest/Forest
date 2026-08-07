/*
 * Fern - AArch64 Generic Timer driver
 *
 * The ARMv8 Generic Timer provides four views of a single physical counter:
 *   Physical  (CNTP_*)  – compares against CNTPCT_EL0 (physical counter)
 *   Virtual   (CNTV_*)  – compares against CNTVCT_EL0 = CNTPCT - CNTVOFF_EL2
 *   Hypervisor (CNTHP_*)– EL2 only
 *   Secure    (CNTPS_*) – Secure EL1 only
 *
 * This driver uses the virtual timer (CNTV_*) so that CNTVOFF_EL2 set by a
 * hypervisor is respected.  QEMU virt sets CNTVOFF_EL2 = 0, so physical and
 * virtual counts are identical in practice.
 *
 * System registers used:
 *   CNTFRQ_EL0    – counter frequency in Hz, written by firmware (read-only)
 *   CNTVCT_EL0    – current virtual counter value (read-only, free-running)
 *   CNTV_CTL_EL0  – virtual timer control register
 *                     bit 0  ENABLE  – 1 = timer is counting
 *                     bit 1  IMASK   – 1 = IRQ output masked
 *                     bit 2  ISTATUS – 1 = compare condition met (read-only)
 *   CNTV_CVAL_EL0 – virtual timer compare value (absolute, 64-bit)
 *                   IRQ fires when CNTVCT_EL0 >= CNTV_CVAL_EL0 and !IMASK
 *   CNTV_TVAL_EL0 – countdown convenience register (write-only in practice)
 *                   writing N sets CNTV_CVAL = CNTVCT + N
 *
 * Interrupt routing (QEMU -machine virt):
 *   INTID 27  PPI #11  virtual timer  (this driver)
 *   INTID 30  PPI #14  physical timer
 *
 * PPIs are per-CPU and enabled via the GICv3 Redistributor (GICR_ISENABLER0).
 */

#ifndef AARCH64_TIMER_H
#define AARCH64_TIMER_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* INTID constants                                                      */
/* ------------------------------------------------------------------ */

/** Virtual timer PPI (GIC INTID 27, PPI #11) */
#define TIMER_VTIMER_INTID  27U
/** Physical timer PPI (GIC INTID 30, PPI #14) */
#define TIMER_PTIMER_INTID  30U

/* ------------------------------------------------------------------ */
/* CNTV_CTL_EL0 bit definitions                                        */
/* ------------------------------------------------------------------ */

#define CNTV_CTL_ENABLE     (UINT64_C(1) << 0)  /* Timer counting         */
#define CNTV_CTL_IMASK      (UINT64_C(1) << 1)  /* Mask IRQ output        */
#define CNTV_CTL_ISTATUS    (UINT64_C(1) << 2)  /* Condition met (RO)     */

/* ------------------------------------------------------------------ */
/* System register accessors – all inline to avoid call overhead        */
/* ------------------------------------------------------------------ */

/**
 * read_cntfrq_el0 - Return the counter frequency (Hz) set by firmware.
 * Typical values: 62 500 000 Hz (QEMU default), 1 000 000 Hz, etc.
 */
static inline uint64_t read_cntfrq_el0(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/**
 * read_cntpct_el0 - Return the physical counter (CNTPCT_EL0).
 * This is the raw free-running counter.  For elapsed-time queries use
 * this rather than CNTVCT when CNTVOFF is unknown.
 */
static inline uint64_t read_cntpct_el0(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/**
 * read_cntvct_el0 - Return the virtual counter (CNTVCT_EL0).
 * Equal to CNTPCT - CNTVOFF_EL2.  On QEMU virt CNTVOFF = 0.
 */
static inline uint64_t read_cntvct_el0(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/**
 * write_cntv_cval_el0 - Set the virtual timer compare value (absolute).
 * The timer fires when CNTVCT_EL0 >= this value and IMASK == 0.
 */
static inline void write_cntv_cval_el0(uint64_t v)
{
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(v));
}

/**
 * read_cntv_cval_el0 - Read the current virtual timer compare value.
 * Used in the IRQ handler to advance the compare value by one interval.
 */
static inline uint64_t read_cntv_cval_el0(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(v));
    return v;
}

/**
 * write_cntv_ctl_el0 - Write the virtual timer control register.
 * Use CNTV_CTL_ENABLE to enable (and unmask) the timer.
 */
static inline void write_cntv_ctl_el0(uint64_t v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(v));
}

/**
 * read_cntv_ctl_el0 - Read the virtual timer control register.
 */
static inline uint64_t read_cntv_ctl_el0(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(v));
    return v;
}

/**
 * write_cntv_tval_el0 - Write the virtual timer countdown value.
 * This is a write-only convenience: the hardware sets
 *   CNTV_CVAL = CNTVCT + (int32_t)tval
 * Use write_cntv_cval_el0() directly for drift-free periodic operation.
 */
static inline void write_cntv_tval_el0(uint32_t v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"((uint64_t)v));
}

/* ------------------------------------------------------------------ */
/* CNTP — Physical Timer system registers                               */
/* ------------------------------------------------------------------ */

#define CNTP_CTL_ENABLE     (UINT64_C(1) << 0)
#define CNTP_CTL_IMASK      (UINT64_C(1) << 1)
#define CNTP_CTL_ISTATUS    (UINT64_C(1) << 2)

static inline void write_cntp_tval_el1(uint32_t v)
{
    __asm__ volatile("msr cntp_tval_el1, %0" :: "r"((uint64_t)v));
}

static inline void write_cntp_ctl_el1(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el1, %0" :: "r"(v));
}

static inline uint64_t read_cntp_ctl_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntp_ctl_el1" : "=r"(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* Compatibility aliases requested by the Fern AArch64 spec       */
/* These map the aarch64_* prefix used in some call-sites to the       */
/* canonical names above.                                               */
/* ------------------------------------------------------------------ */
static inline uint64_t aarch64_get_cntfrq(void)  { return read_cntfrq_el0(); }
static inline uint64_t aarch64_get_cntvct(void)  { return read_cntvct_el0(); }
static inline void     aarch64_set_cntv_cval(uint64_t v) { write_cntv_cval_el0(v); }
static inline void     aarch64_set_cntv_ctl(uint64_t v)  { write_cntv_ctl_el0(v);  }

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * aarch64_timer_init - Initialise the AArch64 virtual timer.
 *
 * @hz: Desired periodic interrupt rate in Hz (e.g. 100 for 10 ms ticks).
 *
 * Actions:
 *   1. Reads CNTFRQ_EL0 (falls back to 62.5 MHz if zero).
 *   2. Computes interval = CNTFRQ / hz.
 *   3. Programs CNTV_CVAL_EL0 = CNTVCT + interval.
 *   4. Enables the timer (CNTV_CTL_EL0 = ENABLE, IMASK = 0).
 *   5. Enables virtual timer PPI (INTID 27) via GICv3.
 *
 * The GIC (gicv3_init) must have been called before this function.
 */
void aarch64_timer_init(uint32_t hz);

/**
 * aarch64_timer_get_ticks - Return the current raw counter value.
 *
 * Returns CNTPCT_EL0 (physical counter), which is the free-running
 * hardware tick.  Useful for fine-grained timing within a single period.
 */
uint64_t aarch64_timer_get_ticks(void);

/**
 * aarch64_timer_get_ns - Return nanoseconds elapsed since boot.
 *
 * Computed from CNTPCT_EL0 and CNTFRQ_EL0.  Uses integer arithmetic
 * split across seconds and sub-second remainder to avoid 64-bit overflow.
 *
 * @return: Elapsed nanoseconds (approximate; depends on counter linearity).
 */
uint64_t aarch64_timer_get_ns(void);

/**
 * aarch64_timer_get_ms - Return milliseconds elapsed since boot.
 *
 * Convenience wrapper: aarch64_timer_get_ns() / 1 000 000.
 */
uint64_t aarch64_timer_get_ms(void);

/**
 * aarch64_timer_get_freq - Return the counter frequency in Hz.
 *
 * Returns the value read from CNTFRQ_EL0 during aarch64_timer_init(),
 * or zero if the timer has not been initialised yet.
 */
uint32_t aarch64_timer_get_freq(void);

/**
 * aarch64_timer_set_interval - Change the tick rate at runtime.
 *
 * @hz: New tick rate in Hz.
 *
 * Recomputes the interval and re-arms the compare register from the
 * current counter value.  May be called from any context as long as
 * the caller handles any race with the timer IRQ.
 */
void aarch64_timer_set_interval(uint32_t hz);

/**
 * aarch64_timer_init_phys - Initialise the AArch64 physical timer (CNTP).
 *
 * @hz: Desired periodic interrupt rate in Hz.
 *
 * Uses CNTP_TVAL_EL1 / CNTP_CTL_EL1 for the periodic tick.
 * The GIC (gicv3_init) must have been called before this function.
 */
void aarch64_timer_init_phys(uint32_t hz);

/**
 * aarch64_timer_reload - Advance CNTV_CVAL by one interval.
 *
 * Must be called inside the timer IRQ handler to schedule the next tick.
 * Advances from the *previous* compare value (not from the current counter)
 * to prevent long-term drift caused by handler latency.
 */
void aarch64_timer_reload(void);

/**
 * aarch64_timer_irq_handler - Default timer interrupt handler.
 *
 * Called by handle_irq() (exceptions.S) when INTID 27 fires.
 *
 * Actions:
 *   1. Advances CNTV_CVAL by one interval (calls aarch64_timer_reload).
 *   2. Increments the software tick counter.
 *   3. Calls timer_tick() if it is defined (Fern callback hook).
 */
void aarch64_timer_irq_handler(void);

/**
 * aarch64_timer_get_tick_count - Return software tick count since boot.
 *
 * Incremented once per timer IRQ.  Wraps after ~584 years at 1000 Hz.
 */
uint64_t aarch64_timer_get_tick_count(void);

/*
 * Legacy names – kept for compatibility with code that calls timer_*
 * directly (e.g. exceptions.S dispatch table, early kernel_main).
 */
static inline void     timer_init(uint32_t hz)     { aarch64_timer_init(hz); }
static inline uint64_t timer_get_ticks(void)        { return aarch64_timer_get_ticks(); }
static inline uint64_t timer_get_ns(void)           { return aarch64_timer_get_ns(); }
static inline uint64_t timer_get_ms(void)           { return aarch64_timer_get_ms(); }
static inline uint64_t timer_get_tick_count(void)   { return aarch64_timer_get_tick_count(); }
static inline void     timer_reload(void)           { aarch64_timer_reload(); }
static inline void     timer_irq_handler(void)      { aarch64_timer_irq_handler(); }

#endif /* AARCH64_TIMER_H */
