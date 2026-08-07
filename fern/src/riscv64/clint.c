/*
 * Fern - RISC-V 64 CLINT timer driver
 * Target: QEMU -machine virt
 *
 * Initialisation sequence:
 *   1. Read current mtime.
 *   2. Write mtimecmp = mtime + interval (clears pending interrupt).
 *   3. Enable Supervisor Timer Interrupt bit in sie (STIE).
 *   4. Enable global interrupts in sstatus if not already enabled.
 */

#include "clint.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* MMIO access helpers                                                  */
/* ------------------------------------------------------------------ */

static inline void mmio_write64(uint64_t addr, uint64_t val)
{
    *((volatile uint64_t *)(uintptr_t)addr) = val;
}

static inline uint64_t mmio_read64(uint64_t addr)
{
    return *((volatile uint64_t *)(uintptr_t)addr);
}

/* ------------------------------------------------------------------ */
/* Supervisor CSR helpers                                               */
/* ------------------------------------------------------------------ */

static inline uint64_t csrr_sie(void)
{
    uint64_t val;
    __asm__ volatile("csrr %0, sie" : "=r"(val));
    return val;
}

static inline void csrw_sie(uint64_t val)
{
    __asm__ volatile("csrw sie, %0" :: "r"(val));
}

static inline uint64_t csrr_sstatus(void)
{
    uint64_t val;
    __asm__ volatile("csrr %0, sstatus" : "=r"(val));
    return val;
}

static inline void csrw_sstatus(uint64_t val)
{
    __asm__ volatile("csrw sstatus, %0" :: "r"(val));
}

/* sstatus.SIE bit */
#define SSTATUS_SIE (1UL << 1)
/* sie.STIE bit — Supervisor Timer Interrupt Enable */
#define SIE_STIE    (1UL << 5)

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

uint64_t clint_get_time(void)
{
    return mmio_read64(CLINT_MTIME);
}

void clint_set_timer(int hart, uint64_t next_val)
{
    /* Writing mtimecmp clears the pending timer interrupt for this hart. */
    mmio_write64(CLINT_MTIMECMP(hart), next_val);
}

void clint_init(uint64_t interval)
{
    /* Set mtimecmp = mtime + interval for hart 0. */
    uint64_t now = clint_get_time();
    clint_set_timer(0, now + interval);

    /* Enable Supervisor Timer Interrupt (sie.STIE). */
    csrw_sie(csrr_sie() | SIE_STIE);

    /* Enable global interrupts in sstatus if not already enabled. */
    csrw_sstatus(csrr_sstatus() | SSTATUS_SIE);
}

void clint_init_timer_irq(void)
{
    clint_init(CLINT_DEFAULT_INTERVAL);
}
