/*
 * Fern - RISC-V 64 PLIC (Platform-Level Interrupt Controller) driver
 * Target: QEMU -machine virt
 *
 * PLIC base addresses for QEMU -machine virt:
 *   PLIC_BASE : 0x0C000000   size 0x4000000 (64MB)
 *
 * Reference: "SiFive Interrupt Cookbook" and RISC-V Privileged Spec.
 */
#ifndef RISCV64_PLIC_H
#define RISCV64_PLIC_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* PLIC memory map                                                     */
/* ------------------------------------------------------------------ */
#define PLIC_BASE               0x0C000000UL
#define PLIC_SIZE               0x4000000UL

/* Register address helpers */
#define PLIC_PRIORITY(base, irq) \
    ((base) + (uint64_t)(irq) * 4UL)
#define PLIC_ENABLE(base, hart, ctx) \
    ((base) + 0x2000UL + (uint64_t)(hart) * 0x80UL + (uint64_t)(ctx) * 4UL)
#define PLIC_THRESHOLD(base, hart, ctx) \
    ((base) + 0x200000UL + (uint64_t)(hart) * 0x1000UL + (uint64_t)(ctx) * 4UL)
#define PLIC_CLAIM(base, hart, ctx) \
    ((base) + 0x200004UL + (uint64_t)(hart) * 0x1000UL + (uint64_t)(ctx) * 4UL)
#define PLIC_COMPLETE(base, hart, ctx, irq) \
    ((base) + 0x200004UL + (uint64_t)(hart) * 0x1000UL + (uint64_t)(ctx) * 4UL)

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * plic_init - Enable Supervisor external interrupts and set threshold to 0.
 */
void plic_init(void);

/**
 * plic_enable_irq - Enable a specific IRQ on hart 0, context 0.
 * @irq: IRQ number (1-1023, source 0 is reserved).
 */
void plic_enable_irq(uint32_t irq);

/**
 * plic_disable_irq - Disable a specific IRQ on hart 0, context 0.
 * @irq: IRQ number.
 */
void plic_disable_irq(uint32_t irq);

/**
 * plic_claim_irq - Claim (acknowledge) the highest-priority pending IRQ.
 * @hart: Hart ID.
 * @ctx:  Context (0 = supervisor).
 *
 * Returns the IRQ number, or 0 if none pending.
 */
uint32_t plic_claim_irq(int hart, int ctx);

/**
 * plic_complete_irq - Signal completion of an IRQ handler.
 * @hart: Hart ID.
 * @ctx:  Context.
 * @irq:  IRQ number returned by plic_claim_irq().
 */
void plic_complete_irq(int hart, int ctx, uint32_t irq);

/**
 * plic_set_priority - Set priority for a given IRQ.
 * @irq:      IRQ number.
 * @priority: Priority value (0 = never, 1 = lowest … 7 = highest).
 */
void plic_set_priority(uint32_t irq, uint32_t priority);

/**
 * plic_set_threshold - Set interrupt threshold for a hart/context.
 * @hart:      Hart ID.
 * @ctx:       Context.
 * @threshold: Threshold value (0 = all enabled, 7 = all disabled).
 */
void plic_set_threshold(int hart, int ctx, uint32_t threshold);

#endif /* RISCV64_PLIC_H */
