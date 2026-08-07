/*
 * Fern - ARM 32-bit (ARMv7-A) Architecture Header
 * arch_arm32.h
 *
 * Defines structures, macros, and inline helpers for the ARMv7-A
 * architecture (Cortex-A series, Thumb-2 capable).  Included automatically
 * by arch.h when ARCH_ARM32 == 1.  Do NOT include directly.
 *
 * Covers:
 *   - Register file (r0-r15, cpsr, spsr)
 *   - CPSR / SPSR bit definitions
 *   - CP15 coprocessor access macros (MRC / MCR)
 *   - ARM exception vector layout
 *   - ARM MMU types (short-descriptor L1 / L2)
 *   - THUMB mode support macros
 *   - Cache / TLB maintenance operations
 *   - Banked register modes
 */

#ifndef FOREST_ARCH_ARM32_H
#define FOREST_ARCH_ARM32_H

#ifndef FOREST_ARCH_H
#error "Include arch.h, not arch_arm32.h directly."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * ARM processor modes (CPSR[4:0])
 * ========================================================================= */

#define ARM32_MODE_USR   0x10   /* User */
#define ARM32_MODE_FIQ   0x11   /* Fast IRQ */
#define ARM32_MODE_IRQ   0x12   /* IRQ */
#define ARM32_MODE_SVC   0x13   /* Supervisor */
#define ARM32_MODE_ABT   0x17   /* Abort */
#define ARM32_MODE_UND   0x1B   /* Undefined */
#define ARM32_MODE_SYS   0x1F   /* System (privileged User) */
#define ARM32_MODE_MON   0x16   /* Monitor (TrustZone, ARMv7 Security Ext.) */
#define ARM32_MODE_HYP   0x1A   /* Hypervisor (Virtualisation Ext., ARMv7-A) */
#define ARM32_MODE_MASK  0x1F

/* =========================================================================
 * CPSR / SPSR bit definitions
 * ========================================================================= */

#define ARM32_CPSR_N     (1U << 31)  /* Negative / Less Than */
#define ARM32_CPSR_Z     (1U << 30)  /* Zero */
#define ARM32_CPSR_C     (1U << 29)  /* Carry / Borrow / Extend */
#define ARM32_CPSR_V     (1U << 28)  /* Overflow */
#define ARM32_CPSR_Q     (1U << 27)  /* Saturation (DSP) */
#define ARM32_CPSR_IT_HI (3U << 25)  /* IT[7:2] bits 26:25 */
#define ARM32_CPSR_J     (1U << 24)  /* Jazelle state */
#define ARM32_CPSR_IT_LO (0x3FU<<10) /* IT[1:0] bits 15:10 */
#define ARM32_CPSR_GE    (0xFU << 16)/* GE[3:0] (SIMD) */
#define ARM32_CPSR_E     (1U <<  9)  /* Endianness (0=LE, 1=BE) */
#define ARM32_CPSR_A     (1U <<  8)  /* Asynchronous abort mask */
#define ARM32_CPSR_I     (1U <<  7)  /* IRQ disable */
#define ARM32_CPSR_F     (1U <<  6)  /* FIQ disable */
#define ARM32_CPSR_T     (1U <<  5)  /* Thumb state */

/* Disable all maskable exceptions (A | I | F) */
#define ARM32_CPSR_MASK_ALL  (ARM32_CPSR_A | ARM32_CPSR_I | ARM32_CPSR_F)

/* =========================================================================
 * CPSR read / write helpers
 * ========================================================================= */

static inline uint32_t arm32_read_cpsr(void)
{
    uint32_t cpsr;
    __asm__ volatile ("mrs %0, cpsr" : "=r"(cpsr));
    return cpsr;
}

static inline void arm32_write_cpsr_c(uint32_t cpsr)
{
    /* Write only the control field (bits 7:0) */
    __asm__ volatile ("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

static inline void arm32_write_cpsr(uint32_t cpsr)
{
    __asm__ volatile ("msr cpsr_cxsf, %0" :: "r"(cpsr) : "memory");
}

static inline uint32_t arm32_read_spsr(void)
{
    uint32_t spsr;
    __asm__ volatile ("mrs %0, spsr" : "=r"(spsr));
    return spsr;
}

/** Save CPSR and disable IRQ + FIQ + Async abort atomically. */
static inline uint32_t arm32_irq_save(void)
{
    uint32_t cpsr = arm32_read_cpsr();
    arm32_write_cpsr_c(cpsr | ARM32_CPSR_MASK_ALL);
    return cpsr;
}

/** Restore previously saved CPSR (re-enables interrupts if they were on). */
static inline void arm32_irq_restore(uint32_t cpsr)
{
    arm32_write_cpsr_c(cpsr);
}

/* =========================================================================
 * CP15 coprocessor access macros
 *
 * ARMv7-A system control is accessed via MRC/MCR to CP15.  The macro names
 * encode the CRn, Op1, CRm, Op2 fields as per the ARM ARM.
 *
 * Usage:
 *   uint32_t val = ARM32_MRC(c1, 0, c0, 0);   // read SCTLR
 *   ARM32_MCR(c1, 0, c0, 0, val | SCTLR_M);   // write SCTLR
 * ========================================================================= */

#define ARM32_MRC(CRn, Op1, CRm, Op2)                          \
    ({                                                           \
        uint32_t _val;                                           \
        __asm__ volatile (                                       \
            "mrc p15, " #Op1 ", %0, " #CRn ", " #CRm ", " #Op2 \
            : "=r"(_val)                                         \
        );                                                       \
        _val;                                                    \
    })

#define ARM32_MCR(CRn, Op1, CRm, Op2, val)                      \
    do {                                                          \
        __asm__ volatile (                                        \
            "mcr p15, " #Op1 ", %0, " #CRn ", " #CRm ", " #Op2  \
            :: "r"((uint32_t)(val))                               \
            : "memory"                                            \
        );                                                        \
    } while (0)

/* MCRR / MRRC for 64-bit coprocessor registers (e.g. TTBR0 in LPAE) */
#define ARM32_MCRR(CRm, Op1, val64)                              \
    do {                                                          \
        uint32_t _lo = (uint32_t)(val64);                        \
        uint32_t _hi = (uint32_t)((val64) >> 32);                \
        __asm__ volatile (                                        \
            "mcrr p15, " #Op1 ", %0, %1, " #CRm                  \
            :: "r"(_lo), "r"(_hi)                                 \
            : "memory"                                            \
        );                                                        \
    } while (0)

#define ARM32_MRRC(CRm, Op1)                                     \
    ({                                                            \
        uint32_t _lo, _hi;                                        \
        __asm__ volatile (                                        \
            "mrrc p15, " #Op1 ", %0, %1, " #CRm                  \
            : "=r"(_lo), "=r"(_hi)                                \
        );                                                        \
        ((uint64_t)_hi << 32) | _lo;                             \
    })

/* -------------------------------------------------------------------------
 * Common CP15 register accessors
 * -------------------------------------------------------------------------
 * SCTLR  - System Control Register        c1, 0, c0, 0
 * TTBR0  - Translation Table Base 0       c2, 0, c0, 0
 * TTBR1  - Translation Table Base 1       c2, 0, c0, 1
 * TTBCR  - Translation Table Base Control c2, 0, c0, 2
 * DACR   - Domain Access Control          c3, 0, c0, 0
 * DFSR   - Data Fault Status              c5, 0, c0, 0
 * IFSR   - Instruction Fault Status       c5, 0, c0, 1
 * DFAR   - Data Fault Address             c6, 0, c0, 0
 * IFAR   - Instruction Fault Address      c6, 0, c0, 2
 * TLBIALL- TLB Invalidate All             c8, 0, c7, 0  (write-only)
 * VBAR   - Vector Base Address            c12,0, c0, 0
 * MIDR   - Main ID Register               c0, 0, c0, 0
 * MPIDR  - Multiprocessor Affinity        c0, 0, c0, 5
 * CONTEXTIDR - Context ID                 c13,0, c0, 1
 * TPIDRURW   - User R/W Thread ID         c13,0, c0, 2
 * TPIDRURO   - User R/O Thread ID         c13,0, c0, 3
 * TPIDRPRW   - PL1-only Thread ID         c13,0, c0, 4
 * ------------------------------------------------------------------------- */

static inline uint32_t arm32_read_sctlr(void)   { return ARM32_MRC(c1,  0, c0, 0); }
static inline void     arm32_write_sctlr(uint32_t v) { ARM32_MCR(c1,  0, c0, 0, v); }

static inline uint32_t arm32_read_ttbr0(void)   { return ARM32_MRC(c2,  0, c0, 0); }
static inline void     arm32_write_ttbr0(uint32_t v) { ARM32_MCR(c2,  0, c0, 0, v); }

static inline uint32_t arm32_read_ttbr1(void)   { return ARM32_MRC(c2,  0, c0, 1); }
static inline void     arm32_write_ttbr1(uint32_t v) { ARM32_MCR(c2,  0, c0, 1, v); }

static inline uint32_t arm32_read_ttbcr(void)   { return ARM32_MRC(c2,  0, c0, 2); }
static inline void     arm32_write_ttbcr(uint32_t v) { ARM32_MCR(c2,  0, c0, 2, v); }

static inline uint32_t arm32_read_dacr(void)    { return ARM32_MRC(c3,  0, c0, 0); }
static inline void     arm32_write_dacr(uint32_t v)  { ARM32_MCR(c3,  0, c0, 0, v); }

static inline uint32_t arm32_read_dfsr(void)    { return ARM32_MRC(c5,  0, c0, 0); }
static inline uint32_t arm32_read_ifsr(void)    { return ARM32_MRC(c5,  0, c0, 1); }
static inline uint32_t arm32_read_dfar(void)    { return ARM32_MRC(c6,  0, c0, 0); }
static inline uint32_t arm32_read_ifar(void)    { return ARM32_MRC(c6,  0, c0, 2); }

static inline uint32_t arm32_read_vbar(void)    { return ARM32_MRC(c12, 0, c0, 0); }
static inline void     arm32_write_vbar(uint32_t v)  { ARM32_MCR(c12, 0, c0, 0, v); }

static inline uint32_t arm32_read_midr(void)    { return ARM32_MRC(c0,  0, c0, 0); }
static inline uint32_t arm32_read_mpidr(void)   { return ARM32_MRC(c0,  0, c0, 5); }

static inline uint32_t arm32_read_contextidr(void)
    { return ARM32_MRC(c13, 0, c0, 1); }
static inline void arm32_write_contextidr(uint32_t v)
    { ARM32_MCR(c13, 0, c0, 1, v); }

static inline uint32_t arm32_read_tpidrurw(void)
    { return ARM32_MRC(c13, 0, c0, 2); }
static inline void arm32_write_tpidrurw(uint32_t v)
    { ARM32_MCR(c13, 0, c0, 2, v); }

static inline uint32_t arm32_read_tpidrprw(void)
    { return ARM32_MRC(c13, 0, c0, 4); }
static inline void arm32_write_tpidrprw(uint32_t v)
    { ARM32_MCR(c13, 0, c0, 4, v); }

/* =========================================================================
 * SCTLR bit definitions
 * ========================================================================= */

#define SCTLR_M      (1U <<  0)  /* MMU Enable */
#define SCTLR_A      (1U <<  1)  /* Strict Alignment fault enable */
#define SCTLR_C      (1U <<  2)  /* Cache Enable (data/unified) */
#define SCTLR_SW     (1U << 10)  /* SWP/SWPB enable */
#define SCTLR_Z      (1U << 11)  /* Branch Prediction Enable */
#define SCTLR_I      (1U << 12)  /* Instruction Cache Enable */
#define SCTLR_V      (1U << 13)  /* High Vectors (0xFFFF0000 vs 0x00000000) */
#define SCTLR_RR     (1U << 14)  /* Round Robin cache replacement */
#define SCTLR_HA     (1U << 17)  /* Hardware Access Flag update */
#define SCTLR_WXN    (1U << 19)  /* Write XOR Execute */
#define SCTLR_UWXN   (1U << 20)  /* Unprivileged WXN */
#define SCTLR_FI     (1U << 21)  /* Fast interrupt configuration */
#define SCTLR_VE     (1U << 24)  /* Interrupt Vectors Enable (use VBAR) */
#define SCTLR_EE     (1U << 25)  /* Exception Endianness (0=LE) */
#define SCTLR_NMFI   (1U << 27)  /* Non-Maskable FIQ */
#define SCTLR_TRE    (1U << 28)  /* TEX Remap Enable */
#define SCTLR_AFE    (1U << 29)  /* Access Flag Enable */
#define SCTLR_TE     (1U << 30)  /* Thumb Exception Enable */

/* DACR domain access values */
#define DACR_NO_ACCESS    0x0   /* All accesses generate a fault */
#define DACR_CLIENT       0x1   /* Accesses checked against MMU permissions */
#define DACR_MANAGER      0x3   /* All accesses permitted (bypass permission checks) */
#define DACR_DOMAIN(d, v) ((uint32_t)(v) << ((d) * 2))

/* =========================================================================
 * ARM exception vector layout
 *
 * The ARM exception vector table begins at either 0x00000000 (low vectors)
 * or 0xFFFF0000 (high vectors, SCTLR.V=1).  Each slot holds a 32-bit ARM
 * instruction — typically a branch (B) to the actual handler.
 *
 * Fern installs the table at a 32-byte aligned address and sets VBAR.
 * ========================================================================= */

#define ARM32_VEC_RESET         0x00  /* Reset */
#define ARM32_VEC_UNDEF         0x04  /* Undefined Instruction */
#define ARM32_VEC_SWI           0x08  /* Software Interrupt (SVC) */
#define ARM32_VEC_PREFETCH_ABT  0x0C  /* Prefetch Abort */
#define ARM32_VEC_DATA_ABT      0x10  /* Data Abort */
#define ARM32_VEC_RESERVED      0x14  /* Reserved (was 26-bit address exception) */
#define ARM32_VEC_IRQ           0x18  /* IRQ */
#define ARM32_VEC_FIQ           0x1C  /* FIQ */

#define ARM32_VEC_COUNT         8
#define ARM32_VEC_TABLE_SIZE    (ARM32_VEC_COUNT * sizeof(uint32_t))

/**
 * arm32_vector_table_t - 8-entry ARM exception vector table.
 *
 * Each entry is one ARM instruction (typically LDR PC, [PC, #offset] or
 * a branch to a trampolined handler table).
 */
typedef struct __attribute__((aligned(32))) {
    uint32_t reset;
    uint32_t undef;
    uint32_t swi;
    uint32_t prefetch_abort;
    uint32_t data_abort;
    uint32_t reserved;
    uint32_t irq;
    uint32_t fiq;
} arm32_vector_table_t;

/** ARM LDR PC, [PC, #offset] encoding for vector table trampolines. */
#define ARM32_LDR_PC_PC_OFFSET(offset) (0xE59FF000U | ((offset) & 0xFFF))

/* =========================================================================
 * ARM MMU - Short-descriptor format (32-bit PA, ≤32-bit VA)
 *
 * L1 descriptors are 4 bytes and live in a 16 KB aligned L1 table of
 * 4096 entries (VA[31:20] index).  L2 "small page" descriptors are 4 bytes
 * in a 1 KB aligned L2 table of 256 entries (VA[19:12] index).
 * ========================================================================= */

/* ----- L1 descriptor types ----- */
#define ARM32_L1_FAULT        0x0   /* Invalid / fault */
#define ARM32_L1_PAGE_TABLE   0x1   /* Pointer to L2 page table */
#define ARM32_L1_SECTION      0x2   /* 1 MB section */
#define ARM32_L1_SUPERSECTION 0x2   /* 16 MB super-section (L1[18]=1) */

/* ----- L1 section descriptor bits ----- */
#define ARM32_L1_SECT_B       (1U <<  2)  /* Bufferable */
#define ARM32_L1_SECT_C       (1U <<  3)  /* Cacheable */
#define ARM32_L1_SECT_XN      (1U <<  4)  /* Execute Never */
#define ARM32_L1_SECT_DOMAIN(d) ((uint32_t)((d) & 0xF) << 5)
#define ARM32_L1_SECT_AP0     (1U << 10)  /* Access Permission bits [1:0] */
#define ARM32_L1_SECT_AP1     (1U << 11)
#define ARM32_L1_SECT_TEX(t)  ((uint32_t)((t) & 0x7) << 12) /* Type Extension */
#define ARM32_L1_SECT_AP2     (1U << 15)  /* Access Permission bit [2] */
#define ARM32_L1_SECT_S       (1U << 16)  /* Shareable */
#define ARM32_L1_SECT_nG      (1U << 17)  /* Not Global (ASID applies) */
#define ARM32_L1_SECT_SUPER   (1U << 18)  /* Super-section flag */
#define ARM32_L1_SECT_NS      (1U << 19)  /* Non-Secure */
#define ARM32_L1_SECT_BASE_MASK 0xFFF00000U  /* Base address bits [31:20] */

/* ----- L1 page table descriptor bits ----- */
#define ARM32_L1_PT_PXN       (1U <<  2)  /* Privileged Execute Never */
#define ARM32_L1_PT_NS        (1U <<  3)  /* Non-Secure */
#define ARM32_L1_PT_DOMAIN(d) ((uint32_t)((d) & 0xF) << 5)
#define ARM32_L1_PT_BASE_MASK 0xFFFFFC00U  /* L2 table PA bits [31:10] */

typedef uint32_t arm32_l1_desc_t;
typedef uint32_t arm32_l2_desc_t;

/* ----- L2 small page (4 KB) descriptor bits ----- */
#define ARM32_L2_FAULT        0x0
#define ARM32_L2_LARGE_PAGE   0x1   /* 64 KB large page */
#define ARM32_L2_SMALL_PAGE   0x2   /* 4 KB small page */
#define ARM32_L2_XN           (1U <<  0)  /* Execute Never (small page) */
#define ARM32_L2_B            (1U <<  2)
#define ARM32_L2_C            (1U <<  3)
#define ARM32_L2_AP0          (1U <<  4)
#define ARM32_L2_AP1          (1U <<  5)
#define ARM32_L2_TEX(t)       ((uint32_t)((t) & 0x7) << 6)
#define ARM32_L2_AP2          (1U <<  9)
#define ARM32_L2_S            (1U << 10)  /* Shareable */
#define ARM32_L2_nG           (1U << 11)  /* Not Global */
#define ARM32_L2_SMALL_BASE_MASK 0xFFFFF000U

/* Common AP encoding (AP[2:0]) for small pages */
#define ARM32_AP_NO_ACCESS    0x0
#define ARM32_AP_RW_NO        0x1  /* PL1 RW, PL0 none */
#define ARM32_AP_RW_RO        0x2  /* PL1 RW, PL0 RO */
#define ARM32_AP_RW_RW        0x3  /* PL1 RW, PL0 RW */
#define ARM32_AP_RO_NO        0x5  /* PL1 RO, PL0 none (AP2=1, AP[1:0]=01) */
#define ARM32_AP_RO_RO        0x7  /* PL1 RO, PL0 RO  (AP2=1, AP[1:0]=11) */

/* L1 table size */
#define ARM32_L1_ENTRIES      4096
#define ARM32_L1_TABLE_SIZE   (ARM32_L1_ENTRIES * sizeof(arm32_l1_desc_t))  /* 16 KB */

/* L2 table size */
#define ARM32_L2_ENTRIES      256
#define ARM32_L2_TABLE_SIZE   (ARM32_L2_ENTRIES * sizeof(arm32_l2_desc_t))  /* 1 KB */

/* VA decomposition for short-descriptor 4 KB pages */
#define ARM32_VA_L1_IDX(va)    (((uint32_t)(va)) >> 20)
#define ARM32_VA_L2_IDX(va)    ((((uint32_t)(va)) >> 12) & 0xFF)
#define ARM32_VA_OFFSET(va)    (((uint32_t)(va)) & 0xFFF)

/* =========================================================================
 * Page size constants
 * ========================================================================= */

#define ARM32_PAGE_SHIFT       12
#define ARM32_PAGE_SIZE        (1U << ARM32_PAGE_SHIFT)   /* 4096 */
#define ARM32_PAGE_MASK        (~(ARM32_PAGE_SIZE - 1))
#define ARM32_SECTION_SIZE     (1U << 20)                 /* 1 MB section */
#define ARM32_SUPERSECT_SIZE   (16U * ARM32_SECTION_SIZE) /* 16 MB super-section */

/* =========================================================================
 * Cache and TLB maintenance
 * ========================================================================= */

/** Invalidate entire TLB (all ASID, all levels). */
static inline void arm32_tlb_flush_all(void)
{
    ARM32_MCR(c8, 0, c7, 0, 0);   /* TLBIALL */
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb"    ::: "memory");
}

/** Invalidate TLB entry for virtual address (current ASID). */
static inline void arm32_tlb_flush_va(uint32_t va)
{
    ARM32_MCR(c8, 0, c7, 1, va & ARM32_PAGE_MASK);  /* TLBIMVA */
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb"    ::: "memory");
}

/** Invalidate all TLB entries for a given ASID. */
static inline void arm32_tlb_flush_asid(uint8_t asid)
{
    ARM32_MCR(c8, 0, c7, 2, (uint32_t)asid);  /* TLBIASID */
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb"    ::: "memory");
}

/** Data Synchronisation Barrier. */
static inline void arm32_dsb(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
}

/** Instruction Synchronisation Barrier. */
static inline void arm32_isb(void)
{
    __asm__ volatile ("isb" ::: "memory");
}

/** Data Memory Barrier. */
static inline void arm32_dmb(void)
{
    __asm__ volatile ("dmb sy" ::: "memory");
}

/** Invalidate I-cache to PoU. */
static inline void arm32_icache_inv_all(void)
{
    ARM32_MCR(c7, 0, c5, 0, 0);   /* ICIALLU */
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb"    ::: "memory");
}

/** Clean and Invalidate data cache line by MVA to PoC. */
static inline void arm32_dcache_clean_inv_mva(uint32_t mva)
{
    ARM32_MCR(c7, 0, c14, 1, mva & ~0x1FU);  /* DCCIMVAC */
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* =========================================================================
 * THUMB mode support
 * ========================================================================= */

/**
 * ARM32_THUMB_FN - Mark a function as Thumb-2 in a mixed ARM/Thumb file.
 *
 * Placing this before a function definition makes the assembler emit
 * the necessary .thumb_func directive.  Use only in .S files or via
 * __attribute__((target("thumb"))).
 */
#define ARM32_THUMB_FN __attribute__((target("thumb")))

/** Return true if currently executing in Thumb state. */
static inline bool arm32_in_thumb_mode(void)
{
    return !!(arm32_read_cpsr() & ARM32_CPSR_T);
}

/**
 * ARM32_BLX_TO_THUMB - Encode a BLX immediate to a Thumb function.
 *
 * In practice, the compiler generates BLX automatically for inter-working
 * calls.  This macro is provided for hand-written assembly that needs to
 * build the encoding manually.
 */
#define ARM32_THUMB_BIT  1U   /* OR onto a function address to make it Thumb */

/* =========================================================================
 * SVC (supervisor call) interface
 * ========================================================================= */

/**
 * arm32_svc - Issue a software interrupt with an immediate.
 *
 * The immediate (imm24) is encoded in the SWI instruction and can be
 * read by the SVC handler from the instruction word itself.  r0-r6 hold
 * arguments; r0 holds the return value.
 */
#define arm32_svc(imm24)                        \
    __asm__ volatile ("svc %[n]" :: [n] "i"(imm24) : "memory")

/* =========================================================================
 * Core-specific IDs from MPIDR
 * ========================================================================= */

static inline uint32_t arm32_cpu_id(void)
{
    return arm32_read_mpidr() & 0x03;  /* Aff0: core within cluster */
}

static inline uint32_t arm32_cluster_id(void)
{
    return (arm32_read_mpidr() >> 8) & 0xFF;  /* Aff1 */
}

/* =========================================================================
 * Banked stack pointers and link registers
 *
 * Access the banked SP/LR of another mode without switching to it.
 * Only valid on ARMv7 with Virtualisation Extensions or when using
 * the "stm" trick (cps + save/restore).
 * ========================================================================= */

static inline uint32_t arm32_read_sp_irq(void)
{
    uint32_t sp;
    __asm__ volatile (
        "mrs     r1, cpsr\n"
        "cps     #0x12\n"           /* switch to IRQ mode */
        "mov     %0, sp\n"
        "msr     cpsr_c, r1\n"      /* restore original mode */
        : "=r"(sp) :: "r1", "memory"
    );
    return sp;
}

static inline uint32_t arm32_read_sp_svc(void)
{
    uint32_t sp;
    __asm__ volatile (
        "mrs     r1, cpsr\n"
        "cps     #0x13\n"           /* switch to SVC mode */
        "mov     %0, sp\n"
        "msr     cpsr_c, r1\n"
        : "=r"(sp) :: "r1", "memory"
    );
    return sp;
}

#endif /* FOREST_ARCH_ARM32_H */
