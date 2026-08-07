/**
 * @file syscall64.h
 * @brief x86_64 SYSCALL/SYSRET fast system call interface for Fern
 *
 * Implements the Intel/AMD SYSCALL instruction path for 64-bit userspace.
 * This is entirely separate from the legacy INT 0x80 path (syscall.c).
 *
 * Hardware behaviour on SYSCALL:
 *   - RIP  saved in RCX, loaded from IA32_LSTAR
 *   - RFLAGS saved in R11, then masked with IA32_FMASK
 *   - CS/SS loaded from IA32_STAR[47:32] (ring 0 selectors)
 *   - CPL set to 0; RSP is NOT changed by hardware
 *
 * Hardware behaviour on SYSRETQ:
 *   - RIP  restored from RCX
 *   - RFLAGS restored from R11
 *   - CS/SS loaded from IA32_STAR[63:48]+16 / IA32_STAR[63:48]+8
 *   - CPL set to 3
 */

#ifndef SYSCALL64_H
#define SYSCALL64_H

#ifdef __x86_64__

#include <stdint.h>
#include "types.h"

/* =========================================================================
 * MSR addresses (x86_64 architectural MSRs)
 * ========================================================================= */

/** Extended Feature Enable Register - bit 0 enables SYSCALL (SCE) */
#define IA32_EFER           0xC0000080U

/**
 * STAR MSR layout:
 *   bits [31:0]  - target EIP for SYSCALL in 32-bit mode (unused in 64-bit)
 *   bits [47:32] - kernel CS selector loaded on SYSCALL entry
 *                  SS is implicitly bits[47:32]+8
 *   bits [63:48] - user CS base: SYSRETQ loads CS from this+16, SS from this+8
 */
#define IA32_STAR           0xC0000081U

/** 64-bit mode SYSCALL target RIP */
#define IA32_LSTAR          0xC0000082U

/** RFLAGS mask applied on SYSCALL entry (bits set here are cleared in RFLAGS) */
#define IA32_FMASK          0xC0000084U

/**
 * FS.base MSR (used by user-space TLS in many ABIs).
 * Not used by the kernel SYSCALL path but listed for completeness.
 */
#define IA32_FS_BASE        0xC0000100U

/**
 * GS.base MSR - current GS base (swapped by SWAPGS).
 * In kernel mode: points to per-CPU data.
 * In user mode: may be 0 or user TLS.
 */
#define IA32_GS_BASE        0xC0000101U

/**
 * Kernel GS base MSR - the value SWAPGS exchanges with IA32_GS_BASE.
 * We store the per-CPU data pointer here so SWAPGS on SYSCALL entry
 * makes GS point to our per_cpu_data_t.
 */
#define IA32_KERNEL_GS_BASE 0xC0000102U

/* =========================================================================
 * RFLAGS bits masked on SYSCALL entry
 * ========================================================================= */

/** Interrupt Enable Flag - we must disable interrupts on entry */
#define RFLAGS_IF   (1U << 9)

/** Direction Flag - must be clear for proper string operations in kernel */
#define RFLAGS_DF   (1U << 10)

/* =========================================================================
 * GDT segment selectors (canonical SYSCALL/SYSRET layout — see gdt64.h)
 *
 *   Index 0  0x00  null
 *   Index 1  0x08  kernel code   (DPL=0, L=1)
 *   Index 2  0x10  kernel data   (DPL=0)
 *   Index 3  0x18  user data     (DPL=3)
 *   Index 4  0x20  user code64   (DPL=3, L=1)
 *   Index 5  0x28  TSS low    \
 *   Index 6  0x30  TSS high   /
 *
 * STAR fields:
 *   STAR[47:32] = 0x08  (SYSCALL: kernel CS = 0x08, kernel SS = 0x08+8 = 0x10)
 *   STAR[63:48] = 0x10  (SYSRET:  user   SS = 0x10+8  = 0x18, user CS = 0x10+16 = 0x20)
 *   The CPU ORs RPL=3 into both CS and SS on SYSRET, yielding
 *   user CS = 0x23 (code64) and user SS = 0x1B (data).
 * ========================================================================= */

/** Raw offset of kernel code descriptor in the GDT (no RPL). */
#define GDT64_KERNEL_CS     0x08U

/** STAR[47:32]: SYSCALL kernel CS base.  SS = base+8 = 0x10 (kernel data). */
#define STAR_KERNEL_CS      GDT64_KERNEL_CS

/** STAR[63:48]: SYSRET user base.  CS = base+16 = 0x20 (user code64),
 *                                  SS = base+8  = 0x18 (user data).  */
#define STAR_USER_BASE      0x10U

/* =========================================================================
 * Per-CPU data area (pointed to by GS.base in kernel mode)
 *
 * CRITICAL layout requirements for the assembly stub:
 *   offset 0x00: kernel_rsp  - loaded into RSP on SYSCALL entry
 *   offset 0x08: cpu_id      - CPU number (informational)
 *   offset 0x10: user_rsp    - where user RSP is saved on SYSCALL entry
 *
 * The assembly stub uses hard-coded offsets; this struct MUST NOT be
 * reordered without updating syscall64_stubs.asm.
 * ========================================================================= */

typedef struct per_cpu_data {
    uint64_t kernel_rsp;    /* offset 0x00: kernel stack pointer (RSP0) */
    uint64_t cpu_id;        /* offset 0x08: logical CPU number */
    uint64_t user_rsp;      /* offset 0x10: saved user RSP on syscall entry */
} __attribute__((packed)) per_cpu_data_t;

/* Verify offsets at compile time */
_Static_assert(__builtin_offsetof(per_cpu_data_t, kernel_rsp) == 0x00,
               "per_cpu_data_t::kernel_rsp must be at offset 0");
_Static_assert(__builtin_offsetof(per_cpu_data_t, cpu_id)     == 0x08,
               "per_cpu_data_t::cpu_id must be at offset 8");
_Static_assert(__builtin_offsetof(per_cpu_data_t, user_rsp)   == 0x10,
               "per_cpu_data_t::user_rsp must be at offset 16");

/* =========================================================================
 * Inline MSR helpers
 *
 * These wrap the existing read_msr/write_msr from cpu_utils.c with
 * names that match the IA32 convention used throughout this file.
 * ========================================================================= */

#include "cpu_ops.h"   /* for read_msr / write_msr */

/**
 * rdmsr - read a model-specific register
 * @param msr   32-bit MSR address
 * @return      64-bit MSR value
 */
static inline uint64_t rdmsr(uint32_t msr) {
    return read_msr(msr);
}

/**
 * wrmsr - write a model-specific register
 * @param msr   32-bit MSR address
 * @param val   64-bit value to write
 */
static inline void wrmsr(uint32_t msr, uint64_t val) {
    write_msr(msr, val);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * syscall64_init - Configure MSRs and per-CPU area for SYSCALL/SYSRET
 *
 * Must be called after GDT/TSS are loaded and before any user-mode code
 * executes.  Sets up:
 *   - IA32_STAR   : kernel CS/SS and user CS/SS selectors
 *   - IA32_LSTAR  : 64-bit entry point (syscall64_entry)
 *   - IA32_FMASK  : RFLAGS bits to clear on entry (IF, DF)
 *   - IA32_EFER   : SCE (System Call Enable) bit
 *   - IA32_KERNEL_GS_BASE : per-CPU area for the bootstrap CPU
 */
void syscall64_init(void);

/**
 * syscall64_handle - C-level dispatcher for 64-bit syscalls
 *
 * Called from syscall64_entry (assembly) with the Linux x86_64 ABI:
 *   rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
 *
 * The 4th argument arrives in r10 (not rcx, which holds the return RIP);
 * the stub moves r10 into rcx before calling this function so the SysV
 * calling convention is satisfied: rdi=a1, rsi=a2, rdx=a3, rcx=a4, r8=a5, r9=a6.
 *
 * @return  syscall return value (written back into rax by the stub)
 */
int64_t syscall64_handle(uint64_t num,
                          uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

/**
 * syscall64_set_kernel_stack - Update the kernel stack pointer in the
 *                              per-CPU area.
 *
 * Must be called on every context switch to a user task so that the
 * SYSCALL entry stub loads the correct per-task kernel stack.
 *
 * @param stack_top  Virtual address of the top of the kernel stack
 */
void syscall64_set_kernel_stack(uint64_t stack_top);

/**
 * syscall64_get_per_cpu - Return a pointer to CPU 0's per_cpu_data_t.
 *
 * Useful for debugging and SMP bringup.
 */
per_cpu_data_t *syscall64_get_per_cpu(void);

/* Assembly entry point (defined in syscall64_stubs.asm) */
extern void syscall64_entry(void);

#endif /* __x86_64__ */
#endif /* SYSCALL64_H */
