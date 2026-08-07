/*
 * gic.c - ARM Generic Interrupt Controller v2 (GIC-400) driver for Fern
 *
 * Implements the Distributor (GICD) and CPU Interface (GICC) initialisation
 * and runtime helpers for QEMU -machine virt, which places the GIC-400 at:
 *   GICD: 0x08000000
 *   GICC: 0x08010000
 *
 * All register accesses use volatile 32-bit reads/writes as required for
 * memory-mapped I/O.  No caching assumptions are made.
 *
 * Reference: ARM IHI0048B "ARM Generic Interrupt Controller Architecture
 *            Specification", version 2.0.
 */

#include "gic.h"

/* -------------------------------------------------------------------------
 * Low-level MMIO helpers
 * --------------------------------------------------------------------- */

static inline void mmio_write32(uint32_t base, uint32_t offset, uint32_t value)
{
    *((volatile uint32_t *)(base + offset)) = value;
}

static inline uint32_t mmio_read32(uint32_t base, uint32_t offset)
{
    return *((volatile uint32_t *)(base + offset));
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

/*
 * gicd_num_lines - return the number of interrupt lines supported by the
 * distributor as reported by GICD_TYPER.ITLinesNumber.
 *
 * GICD_TYPER[4:0] = ITLinesNumber; total lines = 32 * (ITLinesNumber + 1),
 * capped at GIC_MAX_IRQS.
 */
static uint32_t gicd_num_lines(void)
{
    uint32_t typer = mmio_read32(GIC_DIST_BASE, GICD_TYPER);
    uint32_t it_lines_number = typer & 0x1Fu;
    uint32_t num_irqs = 32u * (it_lines_number + 1u);
    if (num_irqs > GIC_MAX_IRQS)
        num_irqs = GIC_MAX_IRQS;
    return num_irqs;
}

/* -------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/*
 * gic_init - Initialise the GIC-400 for the boot CPU.
 *
 * Sequence (see GIC Architecture Specification §3.1.2 "Initialization"):
 *  1. Disable the Distributor (GICD_CTLR = 0).
 *  2. Set all SPIs to lowest priority (0xFF per byte).
 *  3. Target all SPIs to CPU0 (cpu_mask = 0x01).
 *  4. Clear any pending state for all interrupts.
 *  5. Re-enable the Distributor.
 *  6. Configure the CPU Interface:
 *     - Priority mask 0xFF  → all priorities forwarded.
 *     - Binary Point 0      → all bits are group priority.
 *     - Enable GICC.
 */
void gic_init(void)
{
    uint32_t num_irqs = gicd_num_lines();
    uint32_t i;

    /* --- Distributor --------------------------------------------------- */

    /* 1. Disable the Distributor while programming. */
    mmio_write32(GIC_DIST_BASE, GICD_CTLR, 0);

    /*
     * 2. Set all interrupt priorities to 0xFF (lowest) in GICD_IPRIORITYR.
     *    Each register holds 4 bytes (one per interrupt).
     *    We write 0xFFFFFFFF to set all four bytes at once.
     *
     *    Interrupts 0–31 are PPIs/SGIs and are banked per CPU; they are
     *    initialised here for CPU0 and re-initialised for secondary CPUs
     *    in gic_init_cpu().
     */
    for (i = 0; i < num_irqs / 4; i++) {
        mmio_write32(GIC_DIST_BASE, GICD_IPRIORITYR(i), 0xFFFFFFFFu);
    }

    /*
     * 3. Target all SPIs (interrupts 32+) to CPU0 (target byte = 0x01).
     *    GICD_ITARGETSR[0..7] (interrupts 0–31) are read-only for PPIs/SGIs,
     *    so we start from register 8 (interrupt 32).
     *
     *    Write 0x01010101 to route all four interrupts in each register to
     *    CPU0.
     */
    for (i = 8; i < num_irqs / 4; i++) {
        mmio_write32(GIC_DIST_BASE, GICD_ITARGETSR(i), 0x01010101u);
    }

    /*
     * 4. Clear all pending and active state.
     *    Writing 1 to GICD_ICPENDR clears the pending bit.
     *    Writing 1 to GICD_ICACTIVER clears the active bit.
     *
     *    Each register covers 32 interrupts; we iterate over ceil(num_irqs/32)
     *    registers.
     */
    for (i = 0; i < num_irqs / 32; i++) {
        mmio_write32(GIC_DIST_BASE, GICD_ICPENDR(i),  0xFFFFFFFFu);
        mmio_write32(GIC_DIST_BASE, GICD_ICACTIVER(i), 0xFFFFFFFFu);
    }

    /*
     * Set all interrupts to level-triggered, active-high (GICD_ICFGR = 0).
     * SGIs (0–15) are always edge-triggered; the hardware ignores writes to
     * those bits, so it is safe to write 0 to the whole array.
     */
    for (i = 0; i < num_irqs / 16; i++) {
        mmio_write32(GIC_DIST_BASE, GICD_ICFGR(i), 0u);
    }

    /* 5. Enable the Distributor – forward Group 0 interrupts. */
    mmio_write32(GIC_DIST_BASE, GICD_CTLR, GICD_CTLR_ENABLE);

    /* --- CPU Interface (boot CPU) -------------------------------------- */
    gic_init_cpu();
}

/*
 * gic_init_cpu - Initialise the CPU interface for the calling CPU.
 *
 * Secondary CPUs call this after the Distributor has been set up by the
 * boot CPU via gic_init().
 */
void gic_init_cpu(void)
{
    /*
     * Disable CPU interface before modifying PMR/BPR so that no spurious
     * interrupt can slip through.
     */
    mmio_write32(GIC_CPU_BASE, GICC_CTLR, 0);

    /* Set priority mask: 0xFF allows all priority levels through. */
    mmio_write32(GIC_CPU_BASE, GICC_PMR, 0xFFu);

    /*
     * Set binary point register to 0: all 8 priority bits participate in
     * preemption grouping (no sub-priorities).
     */
    mmio_write32(GIC_CPU_BASE, GICC_BPR, 0u);

    /* Enable the CPU interface – forward pending Group 0 interrupts. */
    mmio_write32(GIC_CPU_BASE, GICC_CTLR, GICC_CTLR_ENABLE);
}

/*
 * gic_enable_irq - Enable forwarding of @irq through the Distributor.
 *
 * Writes to GICD_ISENABLER; the register is a write-1-to-set bitmap,
 * so only the targeted bit changes.
 */
void gic_enable_irq(uint32_t irq)
{
    if (irq >= GIC_MAX_IRQS)
        return;

    uint32_t reg = irq / 32u;   /* which ISENABLER word */
    uint32_t bit = irq % 32u;   /* which bit within the word */
    mmio_write32(GIC_DIST_BASE, GICD_ISENABLER(reg), 1u << bit);
}

/*
 * gic_disable_irq - Disable forwarding of @irq through the Distributor.
 *
 * Writes to GICD_ICENABLER; the register is a write-1-to-clear bitmap.
 */
void gic_disable_irq(uint32_t irq)
{
    if (irq >= GIC_MAX_IRQS)
        return;

    uint32_t reg = irq / 32u;
    uint32_t bit = irq % 32u;
    mmio_write32(GIC_DIST_BASE, GICD_ICENABLER(reg), 1u << bit);
}

/*
 * gic_set_priority - Set the priority of interrupt @irq.
 *
 * GICD_IPRIORITYR packs four 8-bit priority fields per 32-bit register.
 * We perform a read-modify-write to update only the relevant byte.
 */
void gic_set_priority(uint32_t irq, uint8_t priority)
{
    if (irq >= GIC_MAX_IRQS)
        return;

    uint32_t reg    = irq / 4u;          /* which IPRIORITYR word */
    uint32_t shift  = (irq % 4u) * 8u;  /* byte offset in bits   */

    uint32_t val = mmio_read32(GIC_DIST_BASE, GICD_IPRIORITYR(reg));
    val &= ~(0xFFu << shift);
    val |=  ((uint32_t)priority << shift);
    mmio_write32(GIC_DIST_BASE, GICD_IPRIORITYR(reg), val);
}

/*
 * gic_set_target - Set which CPUs receive interrupt @irq.
 *
 * GICD_ITARGETSR packs four 8-bit CPU target fields per 32-bit register.
 * Reads-modify-writes only the relevant byte.
 */
void gic_set_target(uint32_t irq, uint8_t cpu_mask)
{
    if (irq >= GIC_MAX_IRQS)
        return;

    uint32_t reg   = irq / 4u;
    uint32_t shift = (irq % 4u) * 8u;

    uint32_t val = mmio_read32(GIC_DIST_BASE, GICD_ITARGETSR(reg));
    val &= ~(0xFFu << shift);
    val |=  ((uint32_t)cpu_mask << shift);
    mmio_write32(GIC_DIST_BASE, GICD_ITARGETSR(reg), val);
}

/*
 * gic_acknowledge - Read the Interrupt Acknowledge Register.
 *
 * Reading GICC_IAR simultaneously acknowledges the highest-priority pending
 * interrupt and returns the IAR value.  The interrupt ID is in bits [9:0];
 * the source CPU ID (for SGIs) is in bits [12:10].
 *
 * The caller must pass the returned value unchanged to gic_end_of_interrupt()
 * after handling the interrupt.
 */
uint32_t gic_acknowledge(void)
{
    return mmio_read32(GIC_CPU_BASE, GICC_IAR);
}

/*
 * gic_end_of_interrupt - Write the End-of-Interrupt Register.
 *
 * @iar must be the value returned by the corresponding gic_acknowledge()
 * call.  Writing GICC_EOIR signals to the GIC that software has finished
 * handling the interrupt, allowing the GIC to lower the priority level and
 * allow lower-priority interrupts through.
 */
void gic_end_of_interrupt(uint32_t iar)
{
    mmio_write32(GIC_CPU_BASE, GICC_EOIR, iar);
}

/*
 * gic_is_spurious - Test whether @iar represents a spurious interrupt.
 *
 * Returns true if the interrupt ID field (bits [9:0]) equals 1023.
 * Spurious interrupts must not be EOI'd; the caller should simply return.
 */
bool gic_is_spurious(uint32_t iar)
{
    return (iar & GIC_IAR_ID_MASK) == GIC_SPURIOUS_IRQ;
}

/*
 * gic_send_sgi - Send a Software Generated Interrupt via GICD_SGIR.
 *
 * @sgi_id:         SGI number 0–15
 * @cpu_target_list: bitmask of target CPUs (one bit per CPU)
 *
 * GICD_SGIR layout:
 *   [3:0]   SGIINTID  – SGI ID
 *   [23:16] CPUTargetList – target CPU bitmask
 *   [25:24] TargetListFilter – 0b00 = use CPUTargetList
 */
void gic_send_sgi(uint8_t sgi_id, uint8_t cpu_target_list)
{
    if (sgi_id > 15u)
        return;

    uint32_t sgir = ((uint32_t)GIC_SGI_TARGET_LIST   << 24) |
                    ((uint32_t)cpu_target_list        << 16) |
                    ((uint32_t)sgi_id                 &  0xFu);
    mmio_write32(GIC_DIST_BASE, GICD_SGIR, sgir);
}
