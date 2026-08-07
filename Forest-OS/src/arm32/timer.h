/*
 * timer.h - ARM Generic Timer (virtual counter) interface for Fern ARM32
 *
 * Target: ARMv7-A with the Generic Timer extension (mandatory on Cortex-A15
 * and later; present in all QEMU -machine virt implementations).
 *
 * The ARM Generic Timer registers are accessed via CP15 coprocessor
 * instructions on ARMv7 (MRC/MCR for 32-bit, MRRC/MCRR for 64-bit).
 *
 * Virtual timer CP15 register encoding
 * -------------------------------------
 *  CNTFRQ     MRC p15, 0, Rt, c14, c0, 0  -- Counter frequency (Hz), RO
 *  CNTVCT     MRRC p15, 1, Rt, Rt2, c14   -- Virtual counter, 64-bit, RO
 *  CNTV_CTL   MRC/MCR p15, 0, Rt, c14, c3, 1  -- Virtual timer control
 *  CNTV_CVAL  MRRC/MCRR p15, 3, Rt, Rt2, c14  -- Compare value, 64-bit
 *  CNTV_TVAL  MRC/MCR p15, 0, Rt, c14, c3, 0  -- TimerValue (convenience)
 *
 * CNTV_CTL bits:
 *   [0] ENABLE  - 1 = timer enabled
 *   [1] IMASK   - 1 = interrupt masked (signal suppressed to GIC)
 *   [2] ISTATUS - 1 = condition met (CVAL <= CNTVCT), read-only
 *
 * Interrupt routing (QEMU -machine virt):
 *   Virtual timer PPI 11 -> GIC ID 27  (PPIs: GIC IDs 16-31; 16+11=27)
 *
 * Usage
 * -----
 *  arm32_timer_init(hz)       - configure periodic interrupt at @hz Hz
 *  arm32_timer_get_ticks()    - software tick count since boot
 *  arm32_timer_get_ns()       - nanoseconds since boot (hardware counter)
 *  arm32_timer_set_hz(hz)     - change the interrupt frequency at runtime
 *  arm32_timer_irq_handler()  - call from irq.c for IRQ 27
 */

#ifndef ARM32_TIMER_H
#define ARM32_TIMER_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * GIC interrupt number for the ARM virtual timer PPI (QEMU -machine virt)
 * ----------------------------------------------------------------------- */
#define ARM_VIRTUAL_TIMER_IRQ   27u     /* PPI 11 = GIC ID 27 */

/* -------------------------------------------------------------------------
 * CNTV_CTL bit definitions
 * ----------------------------------------------------------------------- */
#define CNTV_CTL_ENABLE     (1u << 0)   /* Enable the virtual timer        */
#define CNTV_CTL_IMASK      (1u << 1)   /* Mask interrupt output to GIC    */
#define CNTV_CTL_ISTATUS    (1u << 2)   /* Condition met (read-only)       */

/* -------------------------------------------------------------------------
 * CP15 register access helpers
 *
 * All inlines use the correct ARMv7 coprocessor encoding.  The memory
 * clobber on write instructions prevents the compiler from reordering
 * accesses across the barrier.  Callers should issue arm_isb() / arm_dsb()
 * where the ARM architecture mandates it (see timer.c).
 * ----------------------------------------------------------------------- */

/**
 * arm_get_cntfrq() - Read the counter frequency register (CNTFRQ).
 *
 * Returns the number of counter cycles per second.  On QEMU -machine virt
 * this is typically 62,500,000 (62.5 MHz).  The bootloader or firmware is
 * responsible for programming this register; the kernel reads it here.
 *
 * CP15 encoding: MRC p15, 0, Rt, c14, c0, 0
 */
static inline uint32_t arm_get_cntfrq(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(val));
    return val;
}

/**
 * arm_get_cntvct() - Read the 64-bit virtual counter (CNTVCT).
 *
 * On ARMv7 a 64-bit coprocessor register is read with MRRC (Move Register
 * from Coprocessor, 64-bit form):
 *
 *   MRRC p15, 1, Rt, Rt2, c14
 *     Rt  = CNTVCT[31:0]   (low word)
 *     Rt2 = CNTVCT[63:32]  (high word)
 *
 * The counter increments at CNTFRQ Hz and is free-running.
 */
static inline uint64_t arm_get_cntvct(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/**
 * arm_get_cntv_ctl() - Read the virtual timer control register (CNTV_CTL).
 *
 * CP15 encoding: MRC p15, 0, Rt, c14, c3, 1
 */
static inline uint32_t arm_get_cntv_ctl(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c14, c3, 1" : "=r"(val));
    return val;
}

/**
 * arm_set_cntv_ctl() - Write the virtual timer control register (CNTV_CTL).
 *
 * CP15 encoding: MCR p15, 0, Rt, c14, c3, 1
 */
static inline void arm_set_cntv_ctl(uint32_t val)
{
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 1" :: "r"(val) : "memory");
}

/**
 * arm_set_cntv_cval() - Write the 64-bit virtual compare value (CNTV_CVAL).
 *
 * The timer asserts its interrupt when CNTVCT >= CNTV_CVAL and
 * CNTV_CTL.ENABLE=1 and CNTV_CTL.IMASK=0.
 *
 * On ARMv7 a 64-bit coprocessor register is written with MCRR:
 *   MCRR p15, 3, Rt, Rt2, c14
 *     Rt  = CNTV_CVAL[31:0]
 *     Rt2 = CNTV_CVAL[63:32]
 */
static inline void arm_set_cntv_cval(uint64_t cval)
{
    uint32_t lo = (uint32_t)(cval & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(cval >> 32);
    __asm__ volatile("mcrr p15, 3, %0, %1, c14" :: "r"(lo), "r"(hi) : "memory");
}

/**
 * arm_set_cntv_tval() - Write the virtual TimerValue register (CNTV_TVAL).
 *
 * Hardware sets CNTV_CVAL = CNTVCT + (int32_t)tval on each write.
 * Useful for simple one-shot or periodic programming without reading CNTVCT.
 *
 * CP15 encoding: MCR p15, 0, Rt, c14, c3, 0
 */
static inline void arm_set_cntv_tval(uint32_t tval)
{
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 0" :: "r"(tval) : "memory");
}

/* -------------------------------------------------------------------------
 * Public driver API
 * ----------------------------------------------------------------------- */

/**
 * arm32_timer_init() - Initialise the ARM Generic Timer for periodic IRQs.
 *
 * @hz: Desired interrupt frequency in Hz (e.g. 100 for a 10 ms tick).
 *
 * Steps performed:
 *  1. Reads CNTFRQ; falls back to 62.5 MHz if zero (QEMU default).
 *  2. Disables the virtual timer and masks its interrupt.
 *  3. Sets CNTV_CVAL = CNTVCT + (CNTFRQ / hz) for the first deadline.
 *  4. Enables the timer (CNTV_CTL.ENABLE=1, IMASK=0).
 *  5. Registers arm32_timer_irq_handler() for ARM_VIRTUAL_TIMER_IRQ.
 *  6. Enables and prioritises the IRQ in the GIC.
 */
void arm32_timer_init(uint32_t hz);

/**
 * arm32_timer_get_ticks() - Return the software tick count since boot.
 *
 * Incremented once per timer interrupt.  Monotonically increasing.
 * Wraps after ~5.8e12 years at 100 Hz.
 */
uint64_t arm32_timer_get_ticks(void);

/**
 * arm32_timer_get_ns() - Return nanoseconds elapsed since boot.
 *
 * Derived from the hardware CNTVCT counter and the CNTFRQ frequency.
 * Resolution is 1/CNTFRQ seconds (~16 ns at 62.5 MHz).
 */
uint64_t arm32_timer_get_ns(void);

/**
 * arm32_timer_set_hz() - Change the periodic interrupt rate at runtime.
 *
 * @hz: New frequency in Hz.  Takes effect on the next timer deadline.
 *
 * Safe to call from non-IRQ context; the next CVAL is programmed from
 * CNTVCT at the time of the call.
 */
void arm32_timer_set_hz(uint32_t hz);

/**
 * arm32_timer_irq_handler() - ISR for the virtual timer (IRQ 27).
 *
 * Registered via irq_register_handler() during arm32_timer_init().
 * Called from irq_dispatch() in irq.c with IRQs masked in CPSR.
 *
 * Actions:
 *  1. Advances CNTV_CVAL by one interval (drift-free: adds to previous CVAL).
 *  2. Increments the software tick counter.
 *  3. Re-asserts CNTV_CTL.ENABLE to clear CNTV_CTL.ISTATUS.
 */
void arm32_timer_irq_handler(uint32_t irq);

#endif /* ARM32_TIMER_H */
