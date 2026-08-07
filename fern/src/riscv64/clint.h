/*
 * Fern - RISC-V 64 CLINT (Core-Local Interruptor) timer driver
 * Target: QEMU -machine virt
 *
 * CLINT base addresses for QEMU -machine virt:
 *   CLINT_BASE : 0x2000000
 *   mtime      : CLINT_BASE + 0xBFF8  (64-bit MMIO timer counter)
 *   mtimecmp   : CLINT_BASE + 0x4000 + hart*8  (per-hart compare)
 *
 * Reference: RISC-V Privileged Specification.
 */
#ifndef RISCV64_CLINT_H
#define RISCV64_CLINT_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* CLINT memory map                                                     */
/* ------------------------------------------------------------------ */
#define CLINT_BASE              0x2000000UL

/* mtime — machine-mode timer counter (64-bit, MMIO) */
#define CLINT_MTIME             (CLINT_BASE + 0xBFF8UL)

/* mtimecmp — per-hart timer compare (64-bit, MMIO) */
#define CLINT_MTIMECMP(hart)    (CLINT_BASE + 0x4000UL + (uint64_t)(hart) * 8UL)

/* Default timer interval: 10 ms at 10 MHz mtime → 100,000 ticks */
#define CLINT_TICKS_PER_SECOND  10000000UL
#define CLINT_DEFAULT_INTERVAL  (CLINT_TICKS_PER_SECOND / 100UL)  /* 100 Hz */

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * clint_init - Set mtimecmp = mtime + interval and enable sie.STIE.
 * @interval: Number of mtime ticks between timer interrupts.
 */
void clint_init(uint64_t interval);

/**
 * clint_get_time - Read the current mtime value.
 * Returns the 64-bit mtime counter value.
 */
uint64_t clint_get_time(void);

/**
 * clint_set_timer - Program mtimecmp for a specific hart.
 * @hart:     Hart ID.
 * @next_val: The mtime value at which the interrupt fires.
 *
 * Reading mtimecmp before writing a new value clears any pending
 * timer interrupt.
 */
void clint_set_timer(int hart, uint64_t next_val);

/**
 * clint_init_timer_irq - Initialise timer with default 10 ms (100 Hz) interval.
 */
void clint_init_timer_irq(void);

#endif /* RISCV64_CLINT_H */
