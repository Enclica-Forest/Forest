/*
 * Fern - RISC-V 64-bit (RV64GC) Architecture Header
 * arch_riscv64.h
 *
 * Defines structures, macros, and inline helpers for the RISC-V 64-bit
 * architecture.  Included automatically by arch.h when ARCH_RISCV64 == 1.
 * Do NOT include this file directly.
 *
 * Covers:
 *   - General-purpose register file (x0-x31)
 *   - Supervisor CSRs (sstatus, sepc, scause, stval, sip, sie)
 *   - Virtual memory (Sv39 page table types)
 *   - TLB / cache maintenance
 *   - Interrupt/exception handling
 */

#ifndef FOREST_ARCH_RISCV64_H
#define FOREST_ARCH_RISCV64_H

#ifndef FOREST_ARCH_H
#error "Include arch.h, not arch_riscv64.h directly."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * 1. CSR access macros
 *
 * RISC-V CSRs are accessed via csrr/csrw/csrs/csrc instructions.
 * GCC/Clang accept the named forms directly in extended asm.
 * ========================================================================= */

#define RISCV64_READ_CSR(reg)                           \
    ({                                                   \
        uint64_t _val;                                   \
        __asm__ volatile ("csrr %0, " #reg : "=r"(_val)); \
        _val;                                            \
    })

#define RISCV64_WRITE_CSR(reg, val)                              \
    do {                                                          \
        __asm__ volatile ("csrw " #reg ", %0" :: "r"((uint64_t)(val)) \
                          : "memory");                            \
    } while (0)

/* -------------------------------------------------------------------------
 * sstatus - Supervisor Status Register
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_sstatus(void)
    { return RISCV64_READ_CSR(sstatus); }
static inline void riscv64_write_sstatus(uint64_t v)
    { RISCV64_WRITE_CSR(sstatus, v); }

/* sstatus bits */
#define SSTATUS_SIE     (1ULL <<  1)  /* Supervisor Interrupt Enable */
#define SSTATUS_SPIE    (1ULL <<  5)  /* Previous Interrupt Enable */
#define SSTATUS_SPP     (1ULL <<  8)  /* Previous Privilege (1=S-mode, 0=U-mode) */
#define SSTATUS_FS_MASK (3ULL << 13)  /* Floating-point State */
#define SSTATUS_FS_OFF  (0ULL << 13)
#define SSTATUS_FS_INIT (1ULL << 13)
#define SSTATUS_FS_CLEAN (2ULL << 13)
#define SSTATUS_FS_DIRTY (3ULL << 13)
#define SSTATUS_XS_MASK (3ULL << 15)  /* Extension State */
#define SSTATUS_SUM     (1ULL << 18)  /* Permit Supervisor User Memory access */
#define SSTATUS_MXR     (1ULL << 19)  /* Make eXecutable Readable */

/* -------------------------------------------------------------------------
 * sepc - Supervisor Exception Program Counter
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_sepc(void)
    { return RISCV64_READ_CSR(sepc); }
static inline void riscv64_write_sepc(uint64_t v)
    { RISCV64_WRITE_CSR(sepc, v); }

/* -------------------------------------------------------------------------
 * scause - Supervisor Cause Register (read-only)
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_scause(void)
    { return RISCV64_READ_CSR(scause); }

/* -------------------------------------------------------------------------
 * stval - Supervisor Trap Value Register (read-only)
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_stval(void)
    { return RISCV64_READ_CSR(stval); }

/* -------------------------------------------------------------------------
 * sip / sie - Supervisor Interrupt Pending / Enable
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_sip(void)
    { return RISCV64_READ_CSR(sip); }
static inline void riscv64_write_sip(uint64_t v)
    { RISCV64_WRITE_CSR(sip, v); }

static inline uint64_t riscv64_read_sie(void)
    { return RISCV64_READ_CSR(sie); }
static inline void riscv64_write_sie(uint64_t v)
    { RISCV64_WRITE_CSR(sie, v); }

/* sip/sie bits */
#define SIE_SSIE    (1ULL << 1)  /* Supervisor Software Interrupt */
#define SIE_STIE    (1ULL << 5)  /* Supervisor Timer Interrupt */
#define SIE_SEIE    (1ULL << 9)  /* Supervisor External Interrupt */

/* -------------------------------------------------------------------------
 * satp - Supervisor Address Translation and Protection
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_satp(void)
    { return RISCV64_READ_CSR(satp); }
static inline void riscv64_write_satp(uint64_t v)
    { RISCV64_WRITE_CSR(satp, v); }

/* satp mode field (bits 63:60) */
#define SATP_MODE_BARE  0ULL
#define SATP_MODE_SV39  8ULL
#define SATP_MODE_SV48  9ULL

#define SATP_MODE(mode, asid, ppn) \
    (((uint64_t)(mode) << 60) | ((uint64_t)(asid) << 44) | (uint64_t)(ppn))

/* -------------------------------------------------------------------------
 * sscratch - Scratch register for trap handler
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_sscratch(void)
    { return RISCV64_READ_CSR(sscratch); }
static inline void riscv64_write_sscratch(uint64_t v)
    { RISCV64_WRITE_CSR(sscratch, v); }

/* -------------------------------------------------------------------------
 * medeleg / mideleg - Machine exception/interrupt delegation
 * (used to delegate from M-mode to S-mode)
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_medeleg(void)
    { return RISCV64_READ_CSR(medeleg); }
static inline void riscv64_write_medeleg(uint64_t v)
    { RISCV64_WRITE_CSR(medeleg, v); }

static inline uint64_t riscv64_read_mideleg(void)
    { return RISCV64_READ_CSR(mideleg); }
static inline void riscv64_write_mideleg(uint64_t v)
    { RISCV64_WRITE_CSR(mideleg, v); }

/* -------------------------------------------------------------------------
 * mstatus - Machine Status Register
 * ------------------------------------------------------------------------- */
static inline uint64_t riscv64_read_mstatus(void)
    { return RISCV64_READ_CSR(mstatus); }
static inline void riscv64_write_mstatus(uint64_t v)
    { RISCV64_WRITE_CSR(mstatus, v); }

#define MSTATUS_MIE     (1ULL <<  3)  /* Machine Interrupt Enable */
#define MSTATUS_MPIE    (1ULL <<  7)  /* Previous Interrupt Enable */
#define MSTATUS_MPP     (3ULL << 11)  /* Previous Privilege Mode */

/* =========================================================================
 * 2. Sv39 Page Table Types (4 KB granule, 39-bit VA)
 *
 * Virtual address decomposition:
 *   [38:30] VPN[2] index (9 bits)
 *   [29:21] VPN[1] index (9 bits)
 *   [20:12] VPN[0] index (9 bits)
 *   [11:0]  page offset (12 bits)
 * ========================================================================= */

#define RISCV64_PAGE_SHIFT      12
#define RISCV64_PAGE_SIZE       (1ULL << RISCV64_PAGE_SHIFT)
#define RISCV64_PAGE_MASK       (~(RISCV64_PAGE_SIZE - 1))

#define RISCV64_PGTBL_ENTRIES   512   /* 2^9 entries per level */

/* Page table entry bits */
#define PTE_V       (1ULL <<  0)  /* Valid */
#define PTE_R       (1ULL <<  1)  /* Read */
#define PTE_W       (1ULL <<  2)  /* Write */
#define PTE_X       (1ULL <<  3)  /* Execute */
#define PTE_U       (1ULL <<  4)  /* User accessible */
#define PTE_G       (1ULL <<  5)  /* Global */
#define PTE_A       (1ULL <<  6)  /* Accessed */
#define PTE_D       (1ULL <<  7)  /* Dirty */
#define PTE_RSW(n)  ((uint64_t)(n) << 8)  /* Reserved Software Use (2 bits) */
#define PTE_PPN_MASK 0x0003FFFFFFFFFC00ULL  /* Physical Page Number [53:10] */

typedef uint64_t riscv64_pte_t;

/* VA decomposition for Sv39 */
#define RISCV64_VA_VPN2_IDX(va)  (((uint64_t)(va) >> 30) & 0x1FF)
#define RISCV64_VA_VPN1_IDX(va)  (((uint64_t)(va) >> 21) & 0x1FF)
#define RISCV64_VA_VPN0_IDX(va)  (((uint64_t)(va) >> 12) & 0x1FF)
#define RISCV64_VA_OFFSET(va)    ( (uint64_t)(va)         & 0xFFF)

/* Kernel / user virtual address ranges */
#define RISCV64_KERNEL_VA_START  0xFFFFFFC000000000ULL
#define RISCV64_USER_VA_END      0x0000003FFFFFFFFFULL  /* 39-bit VA */

/* =========================================================================
 * 3. TLB maintenance
 * ========================================================================= */

/** Invalidate all TLB entries (SFENCE.VMA with no operands). */
static inline void riscv64_tlb_flush_all(void)
{
    __asm__ volatile ("sfence.vma" ::: "memory");
}

/** Invalidate TLB entries for a virtual address. */
static inline void riscv64_tlb_flush_va(uint64_t va)
{
    __asm__ volatile ("sfence.vma %0" :: "r"(va) : "memory");
}

/** Invalidate TLB entries for a specific ASID. */
static inline void riscv64_tlb_flush_asid(uint16_t asid)
{
    __asm__ volatile ("sfence.vma zero, %0" :: "r"((uint64_t)asid) : "memory");
}

/** Invalidate TLB entries for a VA + ASID combination. */
static inline void riscv64_tlb_flush_va_asid(uint64_t va, uint16_t asid)
{
    __asm__ volatile ("sfence.vma %0, %1" :: "r"(va), "r"((uint64_t)asid) : "memory");
}

/* =========================================================================
 * 4. Memory barriers
 * ========================================================================= */

static inline void riscv64_rmb(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

static inline void riscv64_wmb(void)
{
    __asm__ volatile ("fence w, w" ::: "memory");
}

static inline void riscv64_mb(void)
{
    __asm__ volatile ("fence" ::: "memory");
}

/* =========================================================================
 * 5. Timer
 * ========================================================================= */

static inline uint64_t riscv64_read_time(void)
{
    /* memory-mapped CLINT mtime; on QEMU virt it's at 0x0200BFF8 */
    volatile uint64_t* mtime = (volatile uint64_t*)0x0200BFF8ULL;
    return *mtime;
}

/* =========================================================================
 * 6. CPU identification
 * ========================================================================= */

static inline uint64_t riscv64_read_mhartid(void)
{
    return RISCV64_READ_CSR(mhartid);
}

static inline uint32_t riscv64_cpu_id(void)
{
    return (uint32_t)riscv64_read_mhartid();
}

/* =========================================================================
 * 7. WFI (Wait For Interrupt)
 * ========================================================================= */

static inline void riscv64_wfi(void)
{
    __asm__ volatile ("wfi" ::: "memory");
}

#endif /* FOREST_ARCH_RISCV64_H */
