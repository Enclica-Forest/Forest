#ifndef ARM32_GIC_H
#define ARM32_GIC_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * GIC-400 (GICv2) base addresses for QEMU -machine virt
 * --------------------------------------------------------------------- */
#define GIC_DIST_BASE   0x08000000UL   /* GICD – Distributor              */
#define GIC_CPU_BASE    0x08010000UL   /* GICC – CPU Interface            */

/* -------------------------------------------------------------------------
 * Distributor register offsets
 * --------------------------------------------------------------------- */
#define GICD_CTLR           0x000   /* Distributor Control Register       */
#define GICD_TYPER          0x004   /* Interrupt Controller Type Register  */
#define GICD_IIDR           0x008   /* Distributor Implementer ID Register */
#define GICD_IGROUPR(n)     (0x080 + (n) * 4)  /* Interrupt Group Registers        */
#define GICD_ISENABLER(n)   (0x100 + (n) * 4)  /* Interrupt Set-Enable Registers   */
#define GICD_ICENABLER(n)   (0x180 + (n) * 4)  /* Interrupt Clear-Enable Registers */
#define GICD_ISPENDR(n)     (0x200 + (n) * 4)  /* Interrupt Set-Pending Registers  */
#define GICD_ICPENDR(n)     (0x280 + (n) * 4)  /* Interrupt Clear-Pending Registers*/
#define GICD_ISACTIVER(n)   (0x300 + (n) * 4)  /* Interrupt Set-Active Registers   */
#define GICD_ICACTIVER(n)   (0x380 + (n) * 4)  /* Interrupt Clear-Active Registers */
#define GICD_IPRIORITYR(n)  (0x400 + (n) * 4)  /* Interrupt Priority Registers     */
#define GICD_ITARGETSR(n)   (0x800 + (n) * 4)  /* Interrupt Processor Targets      */
#define GICD_ICFGR(n)       (0xC00 + (n) * 4)  /* Interrupt Configuration Registers*/
#define GICD_SGIR           0xF00   /* Software Generated Interrupt Register */

/* -------------------------------------------------------------------------
 * CPU Interface register offsets
 * --------------------------------------------------------------------- */
#define GICC_CTLR       0x000   /* CPU Interface Control Register          */
#define GICC_PMR        0x004   /* Interrupt Priority Mask Register        */
#define GICC_BPR        0x008   /* Binary Point Register                   */
#define GICC_IAR        0x00C   /* Interrupt Acknowledge Register          */
#define GICC_EOIR       0x010   /* End of Interrupt Register               */
#define GICC_RPR        0x014   /* Running Priority Register               */
#define GICC_HPPIR      0x018   /* Highest Priority Pending Interrupt Reg  */
#define GICC_ABPR       0x01C   /* Aliased Binary Point Register           */
#define GICC_AIAR       0x020   /* Aliased Interrupt Acknowledge Register  */
#define GICC_AEOIR      0x024   /* Aliased End of Interrupt Register       */
#define GICC_AHPPIR     0x028   /* Aliased Highest Priority Pending IRQ    */
#define GICC_IIDR       0x0FC   /* CPU Interface Identification Register   */
#define GICC_DIR        0x1000  /* Deactivate Interrupt Register           */

/* -------------------------------------------------------------------------
 * GICD_CTLR bits
 * --------------------------------------------------------------------- */
#define GICD_CTLR_ENABLE        (1u << 0)   /* Enable Group 0 interrupts  */
#define GICD_CTLR_ENABLE_GRP1   (1u << 1)   /* Enable Group 1 interrupts  */

/* -------------------------------------------------------------------------
 * GICC_CTLR bits
 * --------------------------------------------------------------------- */
#define GICC_CTLR_ENABLE        (1u << 0)   /* Enable CPU interface       */
#define GICC_CTLR_ENABLE_GRP1   (1u << 1)   /* Enable Group 1 signalling  */
#define GICC_CTLR_ACKCTL        (1u << 2)   /* Group 1 ack control        */
#define GICC_CTLR_FIQEN         (1u << 3)   /* Signal Group 0 as FIQ      */

/* IAR[9:0] = interrupt ID; 1023 indicates a spurious interrupt */
#define GIC_SPURIOUS_IRQ        1023u
#define GIC_IAR_ID_MASK         0x3FFu      /* bits [9:0] of IAR/EOIR     */
#define GIC_IAR_CPU_MASK        (0x7u << 10)/* bits [12:10]: CPU source   */

/* Maximum interrupts supported (GICv2 hardware max) */
#define GIC_MAX_IRQS            1020u

/* SGI target list filter values for GICD_SGIR */
#define GIC_SGI_TARGET_LIST     0x0u    /* send to CPUs in TargetList     */
#define GIC_SGI_TARGET_OTHER    0x1u    /* send to all CPUs except self   */
#define GIC_SGI_TARGET_SELF     0x2u    /* send to self only              */

/* -------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/**
 * gic_init - Initialise the GIC-400 distributor and local CPU interface.
 *
 * Must be called once during kernel startup before enabling interrupts in
 * CPSR.  On SMP systems every CPU must call gic_init_cpu() for its own
 * CPU interface after the boot CPU has called gic_init().
 */
void gic_init(void);

/**
 * gic_init_cpu - Initialise the CPU interface for the calling CPU.
 *
 * Called by secondary CPUs on SMP systems.  The boot CPU's CPU interface
 * is already initialised by gic_init().
 */
void gic_init_cpu(void);

/**
 * gic_enable_irq - Enable forwarding of interrupt @irq to the CPU.
 * @irq: interrupt number (0 – GIC_MAX_IRQS-1)
 */
void gic_enable_irq(uint32_t irq);

/**
 * gic_disable_irq - Disable forwarding of interrupt @irq.
 * @irq: interrupt number (0 – GIC_MAX_IRQS-1)
 */
void gic_disable_irq(uint32_t irq);

/**
 * gic_set_priority - Set the priority of interrupt @irq.
 * @irq:      interrupt number
 * @priority: 8-bit priority value (0 = highest, 0xFF = lowest)
 */
void gic_set_priority(uint32_t irq, uint8_t priority);

/**
 * gic_set_target - Set which CPUs receive interrupt @irq.
 * @irq:      interrupt number
 * @cpu_mask: bitmask of target CPUs (bit 0 = CPU0, bit 1 = CPU1, …)
 */
void gic_set_target(uint32_t irq, uint8_t cpu_mask);

/**
 * gic_acknowledge - Acknowledge the highest-priority pending interrupt.
 *
 * Returns the full IAR value (interrupt ID in bits [9:0], source CPU in
 * [12:10]).  Pass the returned value verbatim to gic_end_of_interrupt().
 */
uint32_t gic_acknowledge(void);

/**
 * gic_end_of_interrupt - Signal End-of-Interrupt to the CPU interface.
 * @iar: value previously returned by gic_acknowledge()
 */
void gic_end_of_interrupt(uint32_t iar);

/**
 * gic_is_spurious - Test whether an IAR value represents a spurious IRQ.
 * @iar: value returned by gic_acknowledge()
 *
 * Returns true when the interrupt ID field is GIC_SPURIOUS_IRQ (1023).
 */
bool gic_is_spurious(uint32_t iar);

/**
 * gic_send_sgi - Send a Software Generated Interrupt.
 * @sgi_id:         SGI number (0 – 15)
 * @cpu_target_list: bitmask of target CPUs (used when filter == GIC_SGI_TARGET_LIST)
 *
 * Sends the SGI to the CPUs specified in @cpu_target_list.
 */
void gic_send_sgi(uint8_t sgi_id, uint8_t cpu_target_list);

#endif /* ARM32_GIC_H */
