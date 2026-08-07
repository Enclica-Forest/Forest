/*
 * Fern - AArch64 (ARMv8-A, 64-bit ARM) Architecture Header
 * arch_aarch64.h
 *
 * Defines structures, macros, and inline helpers for the AArch64 execution
 * state.  Included automatically by arch.h when ARCH_ARM64 == 1.
 * Do NOT include directly.
 *
 * Covers:
 *   - x0-x30, sp, pc, nzcv, fpsr register definitions
 *   - SPSR_EL1 / ELR_EL1 / ESR_EL1 / FAR_EL1
 *   - System register access via MRS/MSR inlines (TTBR0/TTBR1, SCTLR, etc.)
 *   - AArch64 exception levels (EL0-EL3)
 *   - AArch64 DAIF (interrupt) masking
 *   - 4-level page table types (4 KB granule, 48-bit VA)
 *   - GICv3 ICC system registers (CPU interface accessed via system regs)
 *   - TLB maintenance operations
 *   - Cache maintenance operations
 *   - SVC interface
 */

#ifndef FOREST_ARCH_AARCH64_H
#define FOREST_ARCH_AARCH64_H

#ifndef FOREST_ARCH_H
#error "Include arch.h, not arch_aarch64.h directly."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * 1. Register file constants and aliases
 *
 * AArch64 has 31 general-purpose 64-bit registers x0-x30.
 * w0-w30 are the lower 32-bit views of the same physical registers.
 * x30 is the Link Register (LR).
 * ========================================================================= */

/* Calling-convention aliases */
#define AARCH64_REG_FP   29   /* Frame Pointer (x29) */
#define AARCH64_REG_LR   30   /* Link Register (x30) */

/* =========================================================================
 * 2. PSTATE / NZCV / DAIF
 * ========================================================================= */

/* DAIF bits - exception mask flags (write via "msr daifset/daifclr, #n") */
#define DAIF_D   (1U << 3)   /* Debug exceptions masked */
#define DAIF_A   (1U << 2)   /* SError (Asynchronous) exceptions masked */
#define DAIF_I   (1U << 1)   /* IRQ masked */
#define DAIF_F   (1U << 0)   /* FIQ masked */

/* SPSR_EL1 bit definitions (saved processor state) */
#define SPSR_EL1_N     (1ULL << 31)  /* Negative condition flag */
#define SPSR_EL1_Z     (1ULL << 30)  /* Zero condition flag */
#define SPSR_EL1_C     (1ULL << 29)  /* Carry condition flag */
#define SPSR_EL1_V     (1ULL << 28)  /* Overflow condition flag */
#define SPSR_EL1_SS    (1ULL << 21)  /* Software Step */
#define SPSR_EL1_IL    (1ULL << 20)  /* Illegal Execution State */
#define SPSR_EL1_D     (1ULL <<  9)  /* Debug mask */
#define SPSR_EL1_A     (1ULL <<  8)  /* SError mask */
#define SPSR_EL1_I     (1ULL <<  7)  /* IRQ mask */
#define SPSR_EL1_F     (1ULL <<  6)  /* FIQ mask */
#define SPSR_EL1_M_EL0t (0x0ULL)    /* EL0 using SP_EL0 */
#define SPSR_EL1_M_EL1t (0x4ULL)    /* EL1 using SP_EL0 (unusual) */
#define SPSR_EL1_M_EL1h (0x5ULL)    /* EL1 using SP_EL1 */
#define SPSR_EL1_M_MASK  (0xFULL)

/* =========================================================================
 * 3. System register access macros (MRS / MSR)
 *
 * AArch64 uses the MRS / MSR instructions with encoded register operands.
 * GCC/Clang accept the named forms directly in extended asm.
 * ========================================================================= */

/**
 * AARCH64_READ_SYSREG(reg) - Read a named AArch64 system register.
 *
 * Example:
 *   uint64_t sctlr = AARCH64_READ_SYSREG(sctlr_el1);
 */
#define AARCH64_READ_SYSREG(reg)                        \
    ({                                                   \
        uint64_t _val;                                   \
        __asm__ volatile ("mrs %0, " #reg : "=r"(_val)); \
        _val;                                            \
    })

/**
 * AARCH64_WRITE_SYSREG(reg, val) - Write a named AArch64 system register.
 *
 * Example:
 *   AARCH64_WRITE_SYSREG(ttbr0_el1, pg_table_pa);
 */
#define AARCH64_WRITE_SYSREG(reg, val)                          \
    do {                                                          \
        __asm__ volatile ("msr " #reg ", %0" :: "r"((uint64_t)(val)) \
                          : "memory");                            \
    } while (0)

/* -------------------------------------------------------------------------
 * SCTLR_EL1 - System Control Register (EL1)
 * ------------------------------------------------------------------------- */
static inline uint64_t aarch64_read_sctlr_el1(void)
    { return AARCH64_READ_SYSREG(sctlr_el1); }
static inline void aarch64_write_sctlr_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(sctlr_el1, v); }

/* SCTLR_EL1 bits */
#define SCTLR_EL1_M     (1ULL <<  0)  /* MMU enable */
#define SCTLR_EL1_A     (1ULL <<  1)  /* Alignment check */
#define SCTLR_EL1_C     (1ULL <<  2)  /* Data cache enable */
#define SCTLR_EL1_SA    (1ULL <<  3)  /* SP alignment check (EL1) */
#define SCTLR_EL1_SA0   (1ULL <<  4)  /* SP alignment check (EL0) */
#define SCTLR_EL1_I     (1ULL << 12)  /* Instruction cache enable */
#define SCTLR_EL1_WXN   (1ULL << 19)  /* Write XOR Execute */
#define SCTLR_EL1_E0E   (1ULL << 24)  /* EL0 endianness (0=LE) */
#define SCTLR_EL1_EE    (1ULL << 25)  /* EL1 endianness (0=LE) */
#define SCTLR_EL1_UCI   (1ULL << 26)  /* User cache instruction trap */
#define SCTLR_EL1_nTWI  (1ULL << 16)  /* Don't trap WFI at EL0 */
#define SCTLR_EL1_nTWE  (1ULL << 18)  /* Don't trap WFE at EL0 */
#define SCTLR_EL1_UCT   (1ULL << 15)  /* User cache type reg access */

/* -------------------------------------------------------------------------
 * Translation Table Base Registers
 * ------------------------------------------------------------------------- */
static inline uint64_t aarch64_read_ttbr0_el1(void)
    { return AARCH64_READ_SYSREG(ttbr0_el1); }
static inline void aarch64_write_ttbr0_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(ttbr0_el1, v); }

static inline uint64_t aarch64_read_ttbr1_el1(void)
    { return AARCH64_READ_SYSREG(ttbr1_el1); }
static inline void aarch64_write_ttbr1_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(ttbr1_el1, v); }

/* TCR_EL1 - Translation Control Register */
static inline uint64_t aarch64_read_tcr_el1(void)
    { return AARCH64_READ_SYSREG(tcr_el1); }
static inline void aarch64_write_tcr_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(tcr_el1, v); }

/* TCR_EL1 field encodings */
#define TCR_EL1_T0SZ(n)      ((uint64_t)((n) & 0x3F) <<  0)  /* VA size = 2^(64-T0SZ) */
#define TCR_EL1_IRGN0_WB_WA  (0x1ULL <<  8)
#define TCR_EL1_ORGN0_WB_WA  (0x1ULL << 10)
#define TCR_EL1_SH0_INNER    (0x3ULL << 12)
#define TCR_EL1_TG0_4K       (0x0ULL << 14)   /* 4 KB granule for TTBR0 */
#define TCR_EL1_T1SZ(n)      ((uint64_t)((n) & 0x3F) << 16)
#define TCR_EL1_IRGN1_WB_WA  (0x1ULL << 24)
#define TCR_EL1_ORGN1_WB_WA  (0x1ULL << 26)
#define TCR_EL1_SH1_INNER    (0x3ULL << 28)
#define TCR_EL1_TG1_4K       (0x2ULL << 30)   /* 4 KB granule for TTBR1 */
#define TCR_EL1_IPS_48BIT    (0x5ULL << 32)   /* 48-bit PA */
#define TCR_EL1_AS_16BIT     (1ULL   << 36)   /* ASID size: 16-bit */

/* MAIR_EL1 - Memory Attribute Indirection Register */
static inline uint64_t aarch64_read_mair_el1(void)
    { return AARCH64_READ_SYSREG(mair_el1); }
static inline void aarch64_write_mair_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(mair_el1, v); }

/* Common MAIR attribute index encodings used by Fern */
#define MAIR_ATTR_DEVICE_nGnRnE    0x00U  /* Strongly ordered device */
#define MAIR_ATTR_DEVICE_nGnRE     0x04U  /* Device, no Reorder, Execute */
#define MAIR_ATTR_NORMAL_NC        0x44U  /* Normal, Non-Cacheable */
#define MAIR_ATTR_NORMAL_WB        0xFFU  /* Normal, Write-Back, Read/Write Alloc */
#define MAIR_ATTR_NORMAL_WT        0xBBU  /* Normal, Write-Through */

#define MAIR_EL1_ATTR(idx, val) ((uint64_t)((val) & 0xFF) << ((idx) * 8))

/* MAIR index assignments for Fern */
#define MAIR_IDX_DEVICE         0
#define MAIR_IDX_NORMAL_NC      1
#define MAIR_IDX_NORMAL_WB      2
#define MAIR_IDX_NORMAL_WT      3

/* Misc useful system registers */
static inline uint64_t aarch64_read_vbar_el1(void)
    { return AARCH64_READ_SYSREG(vbar_el1); }
static inline void aarch64_write_vbar_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(vbar_el1, v); }

static inline uint64_t aarch64_read_esr_el1(void)
    { return AARCH64_READ_SYSREG(esr_el1); }
static inline uint64_t aarch64_read_far_el1(void)
    { return AARCH64_READ_SYSREG(far_el1); }
static inline uint64_t aarch64_read_elr_el1(void)
    { return AARCH64_READ_SYSREG(elr_el1); }
static inline void aarch64_write_elr_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(elr_el1, v); }
static inline uint64_t aarch64_read_spsr_el1(void)
    { return AARCH64_READ_SYSREG(spsr_el1); }
static inline void aarch64_write_spsr_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(spsr_el1, v); }

static inline uint64_t aarch64_read_sp_el0(void)
    { return AARCH64_READ_SYSREG(sp_el0); }
static inline void aarch64_write_sp_el0(uint64_t v)
    { AARCH64_WRITE_SYSREG(sp_el0, v); }

static inline uint64_t aarch64_read_tpidr_el0(void)
    { return AARCH64_READ_SYSREG(tpidr_el0); }
static inline void aarch64_write_tpidr_el0(uint64_t v)
    { AARCH64_WRITE_SYSREG(tpidr_el0, v); }

static inline uint64_t aarch64_read_tpidr_el1(void)
    { return AARCH64_READ_SYSREG(tpidr_el1); }
static inline void aarch64_write_tpidr_el1(uint64_t v)
    { AARCH64_WRITE_SYSREG(tpidr_el1, v); }

static inline uint64_t aarch64_read_midr_el1(void)
    { return AARCH64_READ_SYSREG(midr_el1); }
static inline uint64_t aarch64_read_mpidr_el1(void)
    { return AARCH64_READ_SYSREG(mpidr_el1); }

static inline uint64_t aarch64_read_cntpct_el0(void)
    { return AARCH64_READ_SYSREG(cntpct_el0); }
static inline uint64_t aarch64_read_cntfrq_el0(void)
    { return AARCH64_READ_SYSREG(cntfrq_el0); }

/* =========================================================================
 * 4. Exception Levels
 * ========================================================================= */

#define AARCH64_EL0   0  /* Least privileged (user applications) */
#define AARCH64_EL1   1  /* OS kernel */
#define AARCH64_EL2   2  /* Hypervisor */
#define AARCH64_EL3   3  /* Secure Monitor (TrustZone) */

/** Read the current exception level from CurrentEL. */
static inline uint32_t aarch64_current_el(void)
{
    uint64_t el;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    return (uint32_t)((el >> 2) & 0x3);
}

/* =========================================================================
 * 5. DAIF - IRQ / FIQ masking
 * ========================================================================= */

/** Save DAIF and mask all interrupts (I + F). */
static inline uint64_t aarch64_irq_save(void)
{
    uint64_t daif = AARCH64_READ_SYSREG(daif);
    __asm__ volatile ("msr daifset, #3" ::: "memory");  /* set I + F */
    return daif;
}

/** Restore DAIF (re-enables IRQ/FIQ if they were clear). */
static inline void aarch64_irq_restore(uint64_t daif)
{
    AARCH64_WRITE_SYSREG(daif, daif);
}

static inline void aarch64_enable_irq(void)
{
    __asm__ volatile ("msr daifclr, #2" ::: "memory");  /* clear I */
}

static inline void aarch64_disable_irq(void)
{
    __asm__ volatile ("msr daifset, #2" ::: "memory");  /* set I */
}

static inline void aarch64_enable_fiq(void)
{
    __asm__ volatile ("msr daifclr, #1" ::: "memory");
}

static inline void aarch64_disable_fiq(void)
{
    __asm__ volatile ("msr daifset, #1" ::: "memory");
}

/* =========================================================================
 * 6. 4-level page table types (4 KB granule, 48-bit VA)
 *
 * Virtual address decomposition (Sv48 analogue; AArch64 uses:
 *   [47:39] L0 (PGD) index
 *   [38:30] L1 (PUD) index
 *   [29:21] L2 (PMD) index
 *   [20:12] L3 (PTE) index
 *   [11:0]  page offset
 * ========================================================================= */

#define AARCH64_PAGE_SHIFT    12
#define AARCH64_PAGE_SIZE     (1ULL << AARCH64_PAGE_SHIFT)
#define AARCH64_PAGE_MASK     (~(AARCH64_PAGE_SIZE - 1))

#define AARCH64_PGTBL_ENTRIES 512   /* 2^9 per level */

/* Page table entry / block descriptor flags (stage-1, EL1) */
#define AARCH64_PTE_VALID     (1ULL <<  0)  /* Entry is valid */
#define AARCH64_PTE_TABLE     (1ULL <<  1)  /* 1=table (non-leaf), 0=block */
#define AARCH64_PTE_PAGE      (1ULL <<  1)  /* L3 entries: 1=page descriptor */

/* Lower attribute bits */
#define AARCH64_PTE_ATTRINDX(n) ((uint64_t)((n) & 0x7) << 2)  /* MAIR index */
#define AARCH64_PTE_NS         (1ULL <<  5)  /* Non-Secure */
#define AARCH64_PTE_AP_RW_EL1 (0ULL <<  6)  /* AP[2:1]=00: EL1 RW, EL0 none */
#define AARCH64_PTE_AP_RW_ALL (1ULL <<  6)  /* AP[2:1]=01: EL1 RW, EL0 RW */
#define AARCH64_PTE_AP_RO_EL1 (2ULL <<  6)  /* AP[2:1]=10: EL1 RO, EL0 none */
#define AARCH64_PTE_AP_RO_ALL (3ULL <<  6)  /* AP[2:1]=11: EL1 RO, EL0 RO */
#define AARCH64_PTE_SH_NONE   (0ULL <<  8)  /* Non-shareable */
#define AARCH64_PTE_SH_OUTER  (2ULL <<  8)  /* Outer shareable */
#define AARCH64_PTE_SH_INNER  (3ULL <<  8)  /* Inner shareable */
#define AARCH64_PTE_AF        (1ULL << 10)  /* Access Flag (must be set!) */
#define AARCH64_PTE_nG        (1ULL << 11)  /* Not Global (ASID applies) */

/* Upper attribute bits */
#define AARCH64_PTE_CONTIGUOUS (1ULL << 52)  /* Hint: contiguous TLB entry */
#define AARCH64_PTE_PXN        (1ULL << 53)  /* Privileged Execute Never */
#define AARCH64_PTE_UXN        (1ULL << 54)  /* Unprivileged Execute Never */
#define AARCH64_PTE_PBHA(n)    ((uint64_t)((n) & 0xF) << 59)  /* Page Based Hardware Attrs */

#define AARCH64_PTE_OA_MASK    0x0000FFFFFFFFF000ULL  /* Output address [47:12] */

/* Output address bits [51:48] for >48-bit PA (ARMv8.2-LPA) */
#define AARCH64_PTE_OA_51_48(oa) ((uint64_t)((oa) >> 12) & 0xFULL) /* stored in [15:12] */

typedef uint64_t aarch64_pgd_t;  /* Level 0 (PGD) entry */
typedef uint64_t aarch64_pud_t;  /* Level 1 (PUD) entry */
typedef uint64_t aarch64_pmd_t;  /* Level 2 (PMD) entry */
typedef uint64_t aarch64_pte_t;  /* Level 3 (PTE) entry */

/* VA decomposition for 48-bit VA (T0SZ = T1SZ = 16) */
#define AARCH64_VA_L0_IDX(va)   (((uint64_t)(va) >> 39) & 0x1FF)
#define AARCH64_VA_L1_IDX(va)   (((uint64_t)(va) >> 30) & 0x1FF)
#define AARCH64_VA_L2_IDX(va)   (((uint64_t)(va) >> 21) & 0x1FF)
#define AARCH64_VA_L3_IDX(va)   (((uint64_t)(va) >> 12) & 0x1FF)
#define AARCH64_VA_OFFSET(va)   ( (uint64_t)(va)         & 0xFFF)

/* Kernel VA (TTBR1) starts at the top of the address space */
#define AARCH64_KERNEL_VA_START 0xFFFF000000000000ULL
#define AARCH64_USER_VA_END     0x0000FFFFFFFFFFFFULL

/* =========================================================================
 * 7. TLB maintenance
 * ========================================================================= */

/** Invalidate all TLB entries, EL1, all ASID, inner-shareable. */
static inline void aarch64_tlb_flush_all(void)
{
    __asm__ volatile (
        "dsb  ishst\n"
        "tlbi vmalle1is\n"
        "dsb  ish\n"
        "isb\n"
        ::: "memory"
    );
}

/** Invalidate TLB entries for a virtual address (all ASID). */
static inline void aarch64_tlb_flush_va(uint64_t va)
{
    /* TLBI VAE1IS: VA, EL1, inner-shareable, all ASID */
    __asm__ volatile (
        "dsb  ishst\n"
        "tlbi vae1is, %0\n"
        "dsb  ish\n"
        "isb\n"
        :: "r"(va >> 12) : "memory"
    );
}

/** Invalidate TLB entries for a specific ASID. */
static inline void aarch64_tlb_flush_asid(uint16_t asid)
{
    __asm__ volatile (
        "dsb  ishst\n"
        "tlbi aside1is, %0\n"
        "dsb  ish\n"
        "isb\n"
        :: "r"((uint64_t)asid << 48) : "memory"
    );
}

/* =========================================================================
 * 8. Cache maintenance
 * ========================================================================= */

static inline void aarch64_dsb_sy(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
}

static inline void aarch64_isb(void)
{
    __asm__ volatile ("isb" ::: "memory");
}

static inline void aarch64_dmb_sy(void)
{
    __asm__ volatile ("dmb sy" ::: "memory");
}

/** Invalidate instruction cache to PoU (inner-shareable). */
static inline void aarch64_icache_inv_all(void)
{
    __asm__ volatile (
        "ic   ialluis\n"
        "dsb  ish\n"
        "isb\n"
        ::: "memory"
    );
}

/** Clean and invalidate a data cache line to PoC (by virtual address). */
static inline void aarch64_dcache_clean_inv_va(uint64_t va)
{
    __asm__ volatile (
        "dc civac, %0\n"
        "dsb sy\n"
        :: "r"(va) : "memory"
    );
}

/** Clean a data cache line to PoC. */
static inline void aarch64_dcache_clean_va(uint64_t va)
{
    __asm__ volatile (
        "dc cvac, %0\n"
        "dsb sy\n"
        :: "r"(va) : "memory"
    );
}

/* =========================================================================
 * 9. GICv3 - CPU Interface via System Registers (ICC_*)
 *
 * GICv3 exposes the CPU interface as system registers (accessed via
 * MRS/MSR), removing the memory-mapped GICv2 GICC region requirement.
 *
 * These must be enabled by writing ICC_SRE_EL1.SRE = 1 early in init.
 * ========================================================================= */

/* ICC_SRE_EL1 - System Register Enable */
#define ICC_SRE_EL1_SRE   (1ULL << 0)  /* Enable system register interface */
#define ICC_SRE_EL1_DFB   (1ULL << 1)  /* Disable FIQ bypass */
#define ICC_SRE_EL1_DIB   (1ULL << 2)  /* Disable IRQ bypass */

static inline uint64_t aarch64_icc_sre_el1_read(void)
    { return AARCH64_READ_SYSREG(ICC_SRE_EL1); }
static inline void aarch64_icc_sre_el1_write(uint64_t v)
    { AARCH64_WRITE_SYSREG(ICC_SRE_EL1, v); }

/* ICC_CTLR_EL1 - CPU Interface Control */
static inline uint64_t aarch64_icc_ctlr_el1_read(void)
    { return AARCH64_READ_SYSREG(ICC_CTLR_EL1); }
static inline void aarch64_icc_ctlr_el1_write(uint64_t v)
    { AARCH64_WRITE_SYSREG(ICC_CTLR_EL1, v); }

/* ICC_PMR_EL1 - Priority Mask Register (0 = no IRQ; 0xFF = all IRQs) */
static inline uint64_t aarch64_icc_pmr_read(void)
    { return AARCH64_READ_SYSREG(ICC_PMR_EL1); }
static inline void aarch64_icc_pmr_write(uint64_t v)
    { AARCH64_WRITE_SYSREG(ICC_PMR_EL1, v); }

/* ICC_BPR1_EL1 - Binary Point Register (Group 1) */
static inline uint64_t aarch64_icc_bpr1_read(void)
    { return AARCH64_READ_SYSREG(ICC_BPR1_EL1); }
static inline void aarch64_icc_bpr1_write(uint64_t v)
    { AARCH64_WRITE_SYSREG(ICC_BPR1_EL1, v); }

/* ICC_IAR1_EL1 - Interrupt Acknowledge Register (Group 1 IRQ) */
static inline uint32_t aarch64_icc_iar1_read(void)
    { return (uint32_t)AARCH64_READ_SYSREG(ICC_IAR1_EL1); }

/* ICC_EOIR1_EL1 - End Of Interrupt Register (Group 1) */
static inline void aarch64_icc_eoir1_write(uint32_t intid)
    { AARCH64_WRITE_SYSREG(ICC_EOIR1_EL1, (uint64_t)intid); }

/* ICC_HPPIR1_EL1 - Highest Priority Pending IRQ (Group 1) */
static inline uint32_t aarch64_icc_hppir1_read(void)
    { return (uint32_t)AARCH64_READ_SYSREG(ICC_HPPIR1_EL1); }

/* ICC_DIR_EL1 - Deactivate Interrupt Register */
static inline void aarch64_icc_dir_write(uint32_t intid)
    { AARCH64_WRITE_SYSREG(ICC_DIR_EL1, (uint64_t)intid); }

/* ICC_IGRPEN1_EL1 - Interrupt Group 1 Enable */
#define ICC_IGRPEN1_ENABLE  (1ULL << 0)
static inline uint64_t aarch64_icc_igrpen1_read(void)
    { return AARCH64_READ_SYSREG(ICC_IGRPEN1_EL1); }
static inline void aarch64_icc_igrpen1_write(uint64_t v)
    { AARCH64_WRITE_SYSREG(ICC_IGRPEN1_EL1, v); }

/* ICC_SGI1R_EL1 - Generate SGI (Software Generated Interrupt, Group 1) */
static inline void aarch64_icc_sgi1r_write(uint64_t v)
    { AARCH64_WRITE_SYSREG(ICC_SGI1R_EL1, v); }

/**
 * aarch64_gic_sgi - Send an SGI to a set of CPUs.
 *
 * @intid:  SGI INTID (0-15)
 * @aff3:   Affinity level 3
 * @aff2:   Affinity level 2
 * @aff1:   Affinity level 1
 * @target_list: 16-bit bitmask of Aff0 targets within the Aff{3,2,1} cluster
 */
static inline void aarch64_gic_sgi(uint8_t intid,
                                    uint8_t aff3, uint8_t aff2, uint8_t aff1,
                                    uint16_t target_list)
{
    uint64_t val =
        ((uint64_t)aff3        << 48) |
        ((uint64_t)(intid & 0xF) << 24) |
        ((uint64_t)aff2        << 16) |
        ((uint64_t)aff1        <<  8) |
        ((uint64_t)target_list);
    aarch64_icc_sgi1r_write(val);
    __asm__ volatile ("isb" ::: "memory");
}

/* GIC special INTID values */
#define AARCH64_GIC_INTID_SPURIOUS  1023U  /* Returned by IAR when no IRQ */
#define AARCH64_GIC_INTID_MAX_PPI    31U
#define AARCH64_GIC_INTID_MIN_SPI    32U

/* =========================================================================
 * 10. ESR_EL1 - Exception Syndrome Register
 * ========================================================================= */

/* Exception Class field [31:26] */
#define ESR_EL1_EC_SHIFT   26
#define ESR_EL1_EC_MASK    (0x3FUL << ESR_EL1_EC_SHIFT)
#define ESR_EL1_EC(esr)    (((esr) & ESR_EL1_EC_MASK) >> ESR_EL1_EC_SHIFT)

/* Common EC values */
#define EC_UNKNOWN          0x00
#define EC_WF               0x01  /* WFI/WFE trapped */
#define EC_FP_SIMD_ACCESS   0x07
#define EC_LS64             0x0A  /* LD64B/ST64B */
#define EC_BRANCH_TARGET    0x0D  /* Branch target exception */
#define EC_ILL_EXEC_STATE   0x0E
#define EC_SVC_AA32         0x11  /* SVC from AArch32 */
#define EC_SVC_AA64         0x15  /* SVC from AArch64 */
#define EC_HVC_AA64         0x16  /* HVC from AArch64 */
#define EC_SMC_AA64         0x17  /* SMC from AArch64 */
#define EC_SYS_INST         0x18  /* MSR/MRS/System instruction */
#define EC_SVE              0x19
#define EC_INST_ABORT_EL0   0x20  /* Instruction Abort from EL0 */
#define EC_INST_ABORT_EL1   0x21  /* Instruction Abort from EL1 */
#define EC_PC_ALIGN         0x22
#define EC_DATA_ABORT_EL0   0x24  /* Data Abort from EL0 */
#define EC_DATA_ABORT_EL1   0x25  /* Data Abort from EL1 */
#define EC_SP_ALIGN         0x26
#define EC_FP_EXCEPTION_AA64 0x2C
#define EC_SERROR           0x2F  /* SError Interrupt */
#define EC_BREAKPOINT_EL0   0x30
#define EC_BREAKPOINT_EL1   0x31
#define EC_STEP_EL0         0x32
#define EC_STEP_EL1         0x33
#define EC_WATCHPOINT_EL0   0x34
#define EC_WATCHPOINT_EL1   0x35
#define EC_BRK              0x3C  /* BRK instruction */

/* ISS field [24:0] */
#define ESR_EL1_ISS_MASK   0x1FFFFFFUL

/* Data abort ISS bits */
#define ESR_DA_ISV    (1UL << 24)  /* Instruction syndrome valid */
#define ESR_DA_WnR    (1UL <<  6)  /* 1=write, 0=read */
#define ESR_DA_DFSC_MASK 0x3FUL
#define ESR_DA_TRANSL_FAULT_L0  0x04
#define ESR_DA_TRANSL_FAULT_L1  0x05
#define ESR_DA_TRANSL_FAULT_L2  0x06
#define ESR_DA_TRANSL_FAULT_L3  0x07
#define ESR_DA_PERM_FAULT_L1    0x0D
#define ESR_DA_PERM_FAULT_L2    0x0E
#define ESR_DA_PERM_FAULT_L3    0x0F

/* =========================================================================
 * 11. Core affinity from MPIDR
 * ========================================================================= */

static inline uint8_t aarch64_cpu_id(void)
{
    return (uint8_t)(aarch64_read_mpidr_el1() & 0xFF);  /* Aff0 */
}

static inline uint8_t aarch64_cluster_id(void)
{
    return (uint8_t)((aarch64_read_mpidr_el1() >> 8) & 0xFF);  /* Aff1 */
}

/* =========================================================================
 * 12. SVC interface
 * ========================================================================= */

/**
 * aarch64_svc - Issue a Supervisor Call.
 *
 * The immediate is embedded in the SVC instruction (16-bit).
 * x0-x7 carry arguments; x0 carries the return value on return.
 */
#define aarch64_svc(imm16) \
    __asm__ volatile ("svc %[n]" :: [n] "i"((uint16_t)(imm16)) : "memory")

/* =========================================================================
 * 13. ID registers
 * ========================================================================= */

static inline uint64_t aarch64_read_id_aa64mmfr0_el1(void)
    { return AARCH64_READ_SYSREG(ID_AA64MMFR0_EL1); }
static inline uint64_t aarch64_read_id_aa64pfr0_el1(void)
    { return AARCH64_READ_SYSREG(ID_AA64PFR0_EL1); }
static inline uint64_t aarch64_read_id_aa64isar0_el1(void)
    { return AARCH64_READ_SYSREG(ID_AA64ISAR0_EL1); }

/* ID_AA64MMFR0_EL1.PARange field [3:0] */
#define ID_AA64MMFR0_PARANGE_32B   0x0   /* 32-bit PA */
#define ID_AA64MMFR0_PARANGE_36B   0x1
#define ID_AA64MMFR0_PARANGE_40B   0x2
#define ID_AA64MMFR0_PARANGE_42B   0x3
#define ID_AA64MMFR0_PARANGE_44B   0x4
#define ID_AA64MMFR0_PARANGE_48B   0x5
#define ID_AA64MMFR0_PARANGE_52B   0x6   /* ARMv8.2-LPA */

#endif /* FOREST_ARCH_AARCH64_H */
