/*
 * Fern - AArch64 GICv3 interrupt controller driver
 * Target: QEMU -machine virt (GICv3 by default since QEMU 2.7)
 *
 * Initialisation sequence (GICv3 spec IHI0069):
 *   1. Enable ICC system register interface: ICC_SRE_EL1 = 0x7 (SRE|DFB|DIB).
 *   2. Set priority mask: ICC_PMR_EL1 = 0xFF (accept all priorities).
 *   3. Enable Group-1 interrupts: ICC_IGRPEN1_EL1 = 0x1.
 *   4. Configure GICD: GICD_CTLR = 0x37
 *        (EnableGrp0 | EnableGrp1NS | EnableGrp1S | ARE_S | ARE_NS)
 *   5. Wake redistributor: clear GICR_WAKER.ProcessorSleep,
 *        poll until GICR_WAKER.ChildrenAsleep = 0.
 *   6. Enable all SGIs and PPIs: GICR_ISENABLER0 = 0xFFFFFFFF.
 *   7. Set SGI/PPI priorities to 0xA0: GICR_IPRIORITYR[0..7].
 *
 * GICD_CTLR value 0x37 breakdown (with affinity routing, GICv3 2-Security):
 *   bit 0  EnableGrp0   = 1
 *   bit 1  EnableGrp1NS = 1
 *   bit 2  EnableGrp1S  = 1
 *   bit 3  (reserved)   = 0
 *   bit 4  ARE_S        = 1
 *   bit 5  ARE_NS       = 1
 *   => 0b00110111 = 0x37
 */

#include "gic.h"
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

static inline void mmio_write64(uint64_t addr, uint64_t val)
{
    *((volatile uint64_t *)(uintptr_t)addr) = val;
}

/* ------------------------------------------------------------------ */
/* Per-CPU Redistributor base address                                   */
/* ------------------------------------------------------------------ */

static inline uint64_t gicr_cpu_base(int cpu)
{
    return GICR_BASE + (uint64_t)cpu * GICR_STRIDE;
}

/* ------------------------------------------------------------------ */
/* gicd_init — GIC Distributor initialisation                          */
/* ------------------------------------------------------------------ */

void gicd_init(void)
{
    /* Read GICD_TYPER to find the number of supported INTIDs.
     * ITLinesNumber[4:0] encodes (max_INTID / 32) - 1.
     * it_lines = ITLinesNumber + 1 gives the count of 32-interrupt groups. */
    uint32_t typer    = mmio_read32(GICD_BASE + GICD_TYPER);
    uint32_t it_lines = (typer & 0x1FU) + 1U;   /* 1..32 */

    /* Step 1: Disable the distributor while reconfiguring.
     * Write 0 to GICD_CTLR to clear all enable bits. */
    mmio_write32(GICD_BASE + GICD_CTLR, 0);

    /* Wait for Register Write Pending (RWP, bit 31) to clear. */
    while (mmio_read32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /* Step 2: Set all SPIs to Group 1 (non-secure).
     * GICD_IGROUPR(0) covers SGIs/PPIs (INTID 0-31); the redistributor
     * controls those, so start from register 1 (INTID 32+). */
    for (uint32_t i = 1; i < it_lines; i++) {
        mmio_write32(GICD_BASE + GICD_IGROUPR(i), 0xFFFFFFFFU);
    }

    /* Step 3: Disable all SPIs (use ICENABLER, not ISENABLER). */
    for (uint32_t i = 1; i < it_lines; i++) {
        mmio_write32(GICD_BASE + GICD_ICENABLER(i), 0xFFFFFFFFU);
    }

    /* Wait for RWP after bulk disable. */
    while (mmio_read32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /* Step 4: Set all SPI priorities to 0xA0.
     * GICD_IPRIORITYR packs four priorities per 32-bit word.
     * Words 0-7 are banked/reserved for SGIs+PPIs; start from word 8
     * (INTID 32) up to it_lines * 8 (one word per 4 INTIDs). */
    for (uint32_t i = 8; i < it_lines * 8U; i++) {
        mmio_write32(GICD_BASE + GICD_IPRIORITYR(i), 0xA0A0A0A0U);
    }

    /* Step 5: Configure all SPIs as level-triggered (ICFGR bits = 0b00).
     * GICD_ICFGR packs two 2-bit fields per INTID, eight INTIDs per word.
     * Words 0-1 cover SGIs/PPIs; start from word 2 (INTID 32). */
    for (uint32_t i = 2; i < it_lines * 2U; i++) {
        mmio_write32(GICD_BASE + GICD_ICFGR(i), 0x00000000U);
    }

    /* Step 6: Route all SPIs to CPU 0 (affinity 0.0.0.0).
     * GICD_IROUTER(n) is a 64-bit register per SPI (INTID >= 32).
     * Value 0 means Aff3=0, Aff2=0, Aff1=0, Aff0=0, no interrupt routing mode. */
    for (uint32_t i = 32; i < it_lines * 32U; i++) {
        mmio_write64(GICD_BASE + GICD_IROUTER(i), 0UL);
    }

    /* Step 7: Re-enable the distributor with affinity routing.
     * 0x37 = EnableGrp0(bit0) | EnableGrp1NS(bit1) | EnableGrp1S(bit2)
     *       | ARE_S(bit4) | ARE_NS(bit5) */
    mmio_write32(GICD_BASE + GICD_CTLR, 0x37U);

    /* Wait for RWP to clear after enabling. */
    while (mmio_read32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP)
        ;
}

/* ------------------------------------------------------------------ */
/* gicr_init — Redistributor initialisation (per CPU)                  */
/* ------------------------------------------------------------------ */

void gicr_init(int cpu)
{
    uint64_t base = gicr_cpu_base(cpu);

    /* Step 5 (spec): Wake the redistributor.
     * Clear ProcessorSleep (bit 1) in GICR_WAKER, then poll until
     * ChildrenAsleep (bit 2) clears, indicating the redistributor is
     * ready to handle interrupts. */
    uint32_t waker = mmio_read32(base + GICR_WAKER);
    waker &= ~GICR_WAKER_PS;
    mmio_write32(base + GICR_WAKER, waker);

    while (mmio_read32(base + GICR_WAKER) & GICR_WAKER_CA)
        ;

    /* Step 6 (spec): Put all SGIs and PPIs into Group 1 (non-secure).
     * GICR_IGROUPR0 covers INTID 0-31 (bit n = Group for INTID n).
     * Writing all-ones sets every SGI/PPI to Group 1. */
    mmio_write32(base + GICR_IGROUPR0, 0xFFFFFFFFU);

    /* Step 7 (spec): Enable all SGIs and PPIs.
     * GICR_ISENABLER0 is a set-enable register; writing a 1 bit enables
     * the corresponding INTID.  0xFFFFFFFF enables all 32 SGIs/PPIs. */
    mmio_write32(base + GICR_ISENABLER0, 0xFFFFFFFFU);

    /* Step 8 (spec): Set SGI/PPI priorities to 0xA0.
     * GICR_IPRIORITYR packs four 8-bit priorities per 32-bit register.
     * There are 8 registers covering INTID 0-31. */
    for (int i = 0; i < 8; i++) {
        mmio_write32(base + GICR_IPRIORITYR(i), 0xA0A0A0A0U);
    }
}

/* ------------------------------------------------------------------ */
/* gicv3_init — Top-level GICv3 initialisation for CPU 0              */
/* ------------------------------------------------------------------ */

void gicv3_init(void)
{
    /* Step 1: Enable the ICC system register interface.
     * ICC_SRE_EL1[2:0]:
     *   bit 0 SRE = 1  — system register interface enabled
     *   bit 1 DFB = 1  — disable FIQ bypass
     *   bit 2 DIB = 1  — disable IRQ bypass
     * Must be set before any other ICC_* register access. */
    icc_sre_el1_write(icc_sre_el1_read() | 0x7UL);

    /* Step 2: Set priority mask to accept all interrupts.
     * ICC_PMR_EL1 = 0xFF means no interrupt is masked by priority. */
    icc_pmr_el1_write(0xFFUL);

    /* Step 3: Enable Group-1 interrupts at the CPU interface.
     * ICC_IGRPEN1_EL1.Enable = 1. */
    icc_igrpen1_el1_write(0x1UL);

    /* Set binary point: all priority bits are group priority (no subpriority). */
    icc_bpr1_el1_write(0UL);

    /* Step 4: Configure the Distributor. */
    gicd_init();

    /* Steps 5-8: Wake and configure the Redistributor for CPU 0. */
    gicr_init(0);
}

/* ------------------------------------------------------------------ */
/* gicv3_enable_irq / gicv3_disable_irq                                */
/* ------------------------------------------------------------------ */

void gicv3_enable_irq(uint32_t irq)
{
    if (irq < 32U) {
        /* SGI / PPI: use Redistributor ISENABLER0. */
        uint64_t base = gicr_cpu_base(0);
        mmio_write32(base + GICR_ISENABLER0, 1U << irq);
    } else {
        /* SPI: use Distributor ISENABLER(n). */
        mmio_write32(GICD_BASE + GICD_ISENABLER(irq / 32U), 1U << (irq % 32U));
    }
}

void gicv3_disable_irq(uint32_t irq)
{
    if (irq < 32U) {
        /* SGI / PPI: use Redistributor ICENABLER0. */
        uint64_t base = gicr_cpu_base(0);
        mmio_write32(base + GICR_ICENABLER0, 1U << irq);
    } else {
        /* SPI: use Distributor ICENABLER(n). */
        mmio_write32(GICD_BASE + GICD_ICENABLER(irq / 32U), 1U << (irq % 32U));
    }

    /* Wait for RWP to clear (disable may take effect asynchronously). */
    if (irq >= 32U) {
        while (mmio_read32(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP)
            ;
    }
}

/* ------------------------------------------------------------------ */
/* gicv3_set_priority                                                   */
/* ------------------------------------------------------------------ */

void gicv3_set_priority(uint32_t irq, uint8_t priority)
{
    /* Priority registers pack four 8-bit fields per 32-bit word.
     * The byte offset within the word is (irq % 4) * 8. */
    uint32_t reg_idx = irq / 4U;
    uint32_t shift   = (irq % 4U) * 8U;

    if (irq < 32U) {
        /* SGI / PPI: Redistributor IPRIORITYR. */
        uint64_t base = gicr_cpu_base(0);
        uint64_t addr = base + GICR_IPRIORITYR(reg_idx);
        uint32_t val  = mmio_read32(addr);
        val &= ~(0xFFU << shift);
        val |= ((uint32_t)priority << shift);
        mmio_write32(addr, val);
    } else {
        /* SPI: Distributor IPRIORITYR. */
        uint64_t addr = GICD_BASE + GICD_IPRIORITYR(reg_idx);
        uint32_t val  = mmio_read32(addr);
        val &= ~(0xFFU << shift);
        val |= ((uint32_t)priority << shift);
        mmio_write32(addr, val);
    }
}

/* ------------------------------------------------------------------ */
/* gicv3_acknowledge / gicv3_end_of_interrupt                          */
/* ------------------------------------------------------------------ */

uint32_t gicv3_acknowledge(void)
{
    /* Reading ICC_IAR1_EL1 returns the INTID of the highest-priority
     * pending Group-1 interrupt and marks it as active.
     * Returns GIC_INTID_SPURIOUS (1023) if no valid interrupt is pending. */
    return (uint32_t)icc_iar1_el1_read();
}

void gicv3_end_of_interrupt(uint32_t irq)
{
    /* Writing ICC_EOIR1_EL1 signals the end of the interrupt handler
     * for a Group-1 interrupt and removes it from the active state. */
    icc_eoir1_el1_write((uint64_t)irq);
}

/* ------------------------------------------------------------------ */
/* gicv3_send_sgi — Software Generated Interrupt (SGI) via system reg  */
/* ------------------------------------------------------------------ */

void gicv3_send_sgi(uint8_t sgi_id, uint64_t target_cpu_aff)
{
    /*
     * Build ICC_SGI1R_EL1 value to send a Group-1 SGI.
     *
     * ICC_SGI1R_EL1 bit layout:
     *   [63:48]  Aff3        — bits [63:56] of MPIDR (only [63:56] matter)
     *   [47:44]  RS          — range selector: high 4 bits of Aff0 / 16
     *   [40]     IRM         — 0 = unicast/multicast, 1 = broadcast (all but self)
     *   [39:32]  Aff2        — MPIDR.Aff2
     *   [27:24]  INTID[3:0]  — SGI number (0-15)
     *   [23:16]  Aff1        — MPIDR.Aff1
     *   [15:0]   TargetList  — bitmask of Aff0 targets within the RS group
     *
     * target_cpu_aff is treated as a raw MPIDR value.
     * For CPU 0 on QEMU virt: MPIDR = 0x0 → TargetList bit 0 = 1.
     */
    uint64_t aff3  = (target_cpu_aff >> 32) & 0xFFUL;  /* MPIDR[39:32] = Aff3 */
    uint64_t aff2  = (target_cpu_aff >> 16) & 0xFFUL;  /* MPIDR[23:16] = Aff2 */
    uint64_t aff1  = (target_cpu_aff >>  8) & 0xFFUL;  /* MPIDR[15:8]  = Aff1 */
    uint64_t aff0  = (target_cpu_aff >>  0) & 0xFFUL;  /* MPIDR[7:0]   = Aff0 */

    /* RS = Aff0[7:4], TargetList bit = 1 << Aff0[3:0] */
    uint64_t rs         = (aff0 >> 4) & 0xFUL;
    uint64_t targetlist = 1UL << (aff0 & 0xFUL);

    uint64_t sgi1r =
        (aff3                        << 48) |
        (rs                          << 44) |
        /* IRM = 0: targeted delivery */
        (aff2                        << 32) |
        ((uint64_t)(sgi_id & 0xFU)  << 24) |
        (aff1                        << 16) |
        targetlist;

    icc_sgi1r_el1_write(sgi1r);
}
