/*
 * Fern - AArch64 GICv3 interrupt controller driver (QEMU -machine virt)
 *
 * GICv3 base addresses for QEMU -machine virt:
 *   GICD (Distributor)      : 0x08000000  size 0x10000
 *   GICR (Redistributor)    : 0x080A0000  per-CPU stride 0x20000 (128KB)
 *   CPU interface           : system registers (ICC_*), not MMIO
 *
 * Each Redistributor has two 64KB frames:
 *   - RD_base   (GICR_CTLR, GICR_WAKER, GICR_TYPER …)
 *   - SGI_base  (RD_base + 0x10000: GICR_IGROUPR0, GICR_ISENABLER0 …)
 *
 * Reference: ARM IHI0069 "ARM Generic Interrupt Controller Architecture
 *            Specification GIC architecture version 3 and version 4"
 */
#ifndef AARCH64_GIC_H
#define AARCH64_GIC_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* GICv3 Distributor registers (MMIO, offsets from GICD_BASE)         */
/* ------------------------------------------------------------------ */
#define GICD_BASE           0x08000000UL

#define GICD_CTLR           0x0000
#define GICD_TYPER          0x0004
#define GICD_IIDR           0x0008
#define GICD_STATUSR        0x0010
#define GICD_SETSPI_NSR     0x0040
#define GICD_CLRSPI_NSR     0x0048
#define GICD_IGROUPR(n)     (0x0080 + (n)*4)
#define GICD_ISENABLER(n)   (0x0100 + (n)*4)
#define GICD_ICENABLER(n)   (0x0180 + (n)*4)
#define GICD_ISPENDR(n)     (0x0200 + (n)*4)
#define GICD_ICPENDR(n)     (0x0280 + (n)*4)
#define GICD_IPRIORITYR(n)  (0x0400 + (n)*4)
#define GICD_ITARGETSR(n)   (0x0800 + (n)*4)
#define GICD_ICFGR(n)       (0x0C00 + (n)*4)
#define GICD_IROUTER(n)     (0x6000 + (n)*8)

/* GICD_CTLR bits (GICv3 with affinity routing enabled) */
#define GICD_CTLR_EnableGrp0    (1U << 0)   /* Enable Group 0 interrupts  */
#define GICD_CTLR_EnableGrp1NS  (1U << 1)   /* Enable Group 1 NS          */
#define GICD_CTLR_EnableGrp1S   (1U << 2)   /* Enable Group 1 Secure      */
#define GICD_CTLR_ARE_S         (1U << 4)   /* Affinity routing (Secure)  */
#define GICD_CTLR_ARE_NS        (1U << 5)   /* Affinity routing (NS)      */
#define GICD_CTLR_RWP          (1U << 31)   /* Register Write Pending     */

/* ------------------------------------------------------------------ */
/* GICv3 Redistributor registers (per-CPU MMIO)                       */
/* ------------------------------------------------------------------ */
#define GICR_BASE           0x080A0000UL
#define GICR_STRIDE         0x20000UL   /* 128KB per redistributor (RD + SGI frames) */

/* RD frame (at GICR_BASE + cpu * GICR_STRIDE) */
#define GICR_CTLR           0x0000
#define GICR_IIDR           0x0004
#define GICR_TYPER          0x0008
#define GICR_STATUSR        0x0010
#define GICR_WAKER          0x0014

/* SGI frame (at RD_base + 0x10000) */
#define GICR_SGI_OFFSET     0x10000
#define GICR_IGROUPR0       (GICR_SGI_OFFSET + 0x0080)
#define GICR_ISENABLER0     (GICR_SGI_OFFSET + 0x0100)
#define GICR_ICENABLER0     (GICR_SGI_OFFSET + 0x0180)
#define GICR_IPRIORITYR(n)  (GICR_SGI_OFFSET + 0x0400 + (n)*4)

/* GICR_WAKER bits */
#define GICR_WAKER_PS       (1U << 1)   /* ProcessorSleep  */
#define GICR_WAKER_CA       (1U << 2)   /* ChildrenAsleep  */

/* ------------------------------------------------------------------ */
/* Special INTID values                                                 */
/* ------------------------------------------------------------------ */
#define GIC_INTID_SPURIOUS  1023U

/* ------------------------------------------------------------------ */
/* GICv3 CPU Interface — system registers (ICC_*)                      */
/*                                                                      */
/* These require ICC_SRE_EL1.SRE=1 to be enabled before use.          */
/* All reads/writes must be followed by ISB where order matters.       */
/* ------------------------------------------------------------------ */

/* ICC_SRE_EL1 — System Register Enable (SRE=1, DFB=1, DIB=1) */
static inline uint64_t icc_sre_el1_read(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(val));
    return val;
}
static inline void icc_sre_el1_write(uint64_t val)
{
    __asm__ volatile("msr icc_sre_el1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* ICC_PMR_EL1 — Priority Mask Register (0xFF = accept all) */
static inline uint64_t icc_pmr_el1_read(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(val));
    return val;
}
static inline void icc_pmr_el1_write(uint64_t val)
{
    __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* ICC_IGRPEN1_EL1 — Interrupt Group 1 Enable */
static inline uint64_t icc_igrpen1_el1_read(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, icc_igrpen1_el1" : "=r"(val));
    return val;
}
static inline void icc_igrpen1_el1_write(uint64_t val)
{
    __asm__ volatile("msr icc_igrpen1_el1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* ICC_IAR1_EL1 — Interrupt Acknowledge Register (Group 1) */
static inline uint64_t icc_iar1_el1_read(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(val));
    return val;
}

/* ICC_EOIR1_EL1 — End Of Interrupt Register (Group 1) */
static inline void icc_eoir1_el1_write(uint64_t val)
{
    __asm__ volatile("msr icc_eoir1_el1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* ICC_BPR1_EL1 — Binary Point Register (Group 1) */
static inline void icc_bpr1_el1_write(uint64_t val)
{
    __asm__ volatile("msr icc_bpr1_el1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* ICC_SGI1R_EL1 — SGI Generation Register (Group 1) */
static inline void icc_sgi1r_el1_write(uint64_t val)
{
    __asm__ volatile("msr icc_sgi1r_el1, %0" :: "r"(val));
    __asm__ volatile("isb");
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * gicv3_init - Full GICv3 initialisation for CPU 0.
 *
 * Performs the complete init sequence:
 *   1. Enable ICC system register interface (ICC_SRE_EL1 = 0x7)
 *   2. Set priority mask to accept all (ICC_PMR_EL1 = 0xFF)
 *   3. Enable Group-1 interrupts (ICC_IGRPEN1_EL1 = 0x1)
 *   4. Configure GICD (affinity routing, Group-1 SPIs)
 *   5. Wake GICR for CPU 0 and enable SGIs/PPIs as Group-1
 */
void gicv3_init(void);

/**
 * gicv3_enable_irq - Enable a specific interrupt at the GIC.
 * @irq: INTID to enable (SPI: 32+, PPI: 16-31, SGI: 0-15).
 */
void gicv3_enable_irq(uint32_t irq);

/**
 * gicv3_disable_irq - Disable a specific interrupt at the GIC.
 * @irq: INTID to disable.
 */
void gicv3_disable_irq(uint32_t irq);

/**
 * gicv3_set_priority - Set the priority for a given INTID.
 * @irq:      INTID.
 * @priority: Priority value (0 = highest, 0xFF = lowest).
 */
void gicv3_set_priority(uint32_t irq, uint8_t priority);

/**
 * gicv3_acknowledge - Read ICC_IAR1_EL1 and return the INTID.
 * Must be called at the beginning of an IRQ handler.
 * Returns GIC_INTID_SPURIOUS (1023) for spurious interrupts.
 */
uint32_t gicv3_acknowledge(void);

/**
 * gicv3_end_of_interrupt - Signal End-Of-Interrupt to the GIC.
 * @irq: INTID value obtained from gicv3_acknowledge().
 * Must be called after the IRQ handler completes.
 */
void gicv3_end_of_interrupt(uint32_t irq);

/**
 * gicv3_send_sgi - Send a Software Generated Interrupt to target CPUs.
 * @sgi_id:          SGI INTID (0-15).
 * @target_cpu_aff:  Target CPU affinity value (MPIDR_EL1 format).
 *                   Pass 0 to target CPU 0 (Aff3=0, Aff2=0, Aff1=0, Aff0=0).
 *
 * Uses ICC_SGI1R_EL1 to generate a Group-1 SGI.
 * ICC_SGI1R_EL1 layout:
 *   [63:48] Aff3      (MPIDR.Aff3)
 *   [47:44] RS        (range selector — high bits of Aff0)
 *   [40]    IRM       (1 = broadcast to all, 0 = use TargetList)
 *   [39:32] Aff2      (MPIDR.Aff2)
 *   [27:24] INTID     (SGI number, bits [3:0])
 *   [23:16] Aff1      (MPIDR.Aff1)
 *   [15:0]  TargetList (bitmask of Aff0 values within the RS/Aff1/Aff2/Aff3 group)
 */
void gicv3_send_sgi(uint8_t sgi_id, uint64_t target_cpu_aff);

/* ------------------------------------------------------------------ */
/* Lower-level helpers (also usable directly)                           */
/* ------------------------------------------------------------------ */

/**
 * gicd_init - Initialise the GIC Distributor.
 * Disables then re-enables with affinity routing and Group-1 SPIs.
 */
void gicd_init(void);

/**
 * gicr_init - Initialise the Redistributor for a given CPU.
 * @cpu: CPU index (0-based).
 */
void gicr_init(int cpu);

#endif /* AARCH64_GIC_H */
