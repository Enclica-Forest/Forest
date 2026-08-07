/*
 * Fern - RISC-V 64 PLIC driver
 * Target: QEMU -machine virt
 *
 * Initialisation sequence:
 *   1. Set interrupt threshold to 0 (accept all) for hart 0 context 0.
 *   2. Enable Supervisor External Interrupt bit in sie.
 *
 * IRQ lifecycle:
 *   plic_enable_irq  — set enable bit in the PLIC enable word
 *   plic_claim_irq   — read claim register (marks IRQ active)
 *   plic_complete_irq — write IRQ to complete register (deactivate)
 */

#include "plic.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* MMIO access helpers                                                  */
/* ------------------------------------------------------------------ */

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
    *((volatile uint32_t *)(uintptr_t)addr) = val;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
    return *((volatile uint32_t *)(uintptr_t)addr);
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
/* sie.SEIE bit — Supervisor External Interrupt Enable */
#define SIE_SEIE    (1UL << 9)

/* ------------------------------------------------------------------ */
/* plic_init                                                            */
/* ------------------------------------------------------------------ */

void plic_init(void)
{
    /* Set threshold to 0 on hart 0, context 0 (supervisor).
     * Threshold 0 means all interrupts with priority > 0 are delivered. */
    plic_set_threshold(0, 0, 0);

    /* Enable Supervisor External Interrupt (sie.SEIE). */
    csrw_sie(csrr_sie() | SIE_SEIE);

    /* Enable global interrupts in sstatus if not already enabled. */
    csrw_sstatus(csrr_sstatus() | SSTATUS_SIE);
}

/* ------------------------------------------------------------------ */
/* plic_enable_irq / plic_disable_irq                                   */
/* ------------------------------------------------------------------ */

void plic_enable_irq(uint32_t irq)
{
    if (irq == 0 || irq > 1023)
        return;

    uint64_t addr   = PLIC_ENABLE(PLIC_BASE, 0, 0) + ((uint64_t)irq / 32UL) * 4UL;
    uint32_t bit    = 1UL << (irq % 32U);

    mmio_write32(addr, mmio_read32(addr) | bit);
}

void plic_disable_irq(uint32_t irq)
{
    if (irq == 0 || irq > 1023)
        return;

    uint64_t addr   = PLIC_ENABLE(PLIC_BASE, 0, 0) + ((uint64_t)irq / 32UL) * 4UL;
    uint32_t bit    = 1UL << (irq % 32U);

    mmio_write32(addr, mmio_read32(addr) & ~bit);
}

/* ------------------------------------------------------------------ */
/* plic_claim_irq / plic_complete_irq                                   */
/* ------------------------------------------------------------------ */

uint32_t plic_claim_irq(int hart, int ctx)
{
    return mmio_read32(PLIC_CLAIM(PLIC_BASE, hart, ctx));
}

void plic_complete_irq(int hart, int ctx, uint32_t irq)
{
    mmio_write32(PLIC_COMPLETE(PLIC_BASE, hart, ctx, irq), irq);
}

/* ------------------------------------------------------------------ */
/* plic_set_priority / plic_set_threshold                               */
/* ------------------------------------------------------------------ */

void plic_set_priority(uint32_t irq, uint32_t priority)
{
    if (irq == 0 || irq > 1023)
        return;

    /* Priority 0 = never interrupt; values 1-7 are valid priority levels. */
    mmio_write32(PLIC_PRIORITY(PLIC_BASE, irq), priority & 0x7U);
}

void plic_set_threshold(int hart, int ctx, uint32_t threshold)
{
    mmio_write32(PLIC_THRESHOLD(PLIC_BASE, hart, ctx), threshold & 0x7U);
}
