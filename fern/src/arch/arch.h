/*
 * Fern - Multi-Architecture Abstraction Layer
 * arch.h - Main architecture detection and abstraction header
 *
 * This header is the single entry point for all architecture-specific
 * definitions. Include this instead of any arch-specific header directly.
 *
 * Supported architectures:
 *   ARCH_X86_32  - i686 / IA-32 (32-bit x86)
 *   ARCH_X86_64  - x86-64 / AMD64 (64-bit x86)
 *   ARCH_ARM32   - ARMv7-A (32-bit ARM, Thumb-2 capable)
 *   ARCH_ARM64   - AArch64 / ARMv8-A (64-bit ARM)
 */

#ifndef FOREST_ARCH_H
#define FOREST_ARCH_H

/* =========================================================================
 * 1. Architecture Detection
 * ========================================================================= */

#if defined(__x86_64__) || defined(_M_X64)
#   define ARCH_X86_64   1
#   define ARCH_X86_32   0
#   define ARCH_ARM32    0
#   define ARCH_ARM64    0
#   define ARCH_BITS     64
#   define ARCH_NAME     "x86_64"
#   define ARCH_LITTLE_ENDIAN 1

#elif defined(__i386__) || defined(_M_IX86)
#   define ARCH_X86_32   1
#   define ARCH_X86_64   0
#   define ARCH_ARM32    0
#   define ARCH_ARM64    0
#   define ARCH_BITS     32
#   define ARCH_NAME     "x86_32"
#   define ARCH_LITTLE_ENDIAN 1

#elif defined(__aarch64__) || defined(_M_ARM64)
#   define ARCH_ARM64    1
#   define ARCH_X86_64   0
#   define ARCH_X86_32   0
#   define ARCH_ARM32    0
#   define ARCH_BITS     64
#   define ARCH_NAME     "aarch64"
#   define ARCH_LITTLE_ENDIAN 1  /* AArch64 is LE by default; BE variant uncommon */

#elif defined(__arm__) || defined(_M_ARM)
#   define ARCH_ARM32    1
#   define ARCH_ARM64    0
#   define ARCH_X86_64   0
#   define ARCH_X86_32   0
#   define ARCH_BITS     32
#   define ARCH_NAME     "arm32"
#   define ARCH_LITTLE_ENDIAN 1  /* Assumes ARMv7 LE configuration */

#else
#   error "Fern: unsupported target architecture."
#endif

/* Convenience combined macros */
#define ARCH_IS_X86      (ARCH_X86_32 || ARCH_X86_64)
#define ARCH_IS_ARM      (ARCH_ARM32  || ARCH_ARM64)
#define ARCH_IS_64BIT    (ARCH_BITS == 64)
#define ARCH_IS_32BIT    (ARCH_BITS == 32)

/* =========================================================================
 * 2. Standard type pull-ins
 * ========================================================================= */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * 3. Pointer-width abstract types
 *
 *   arch_word_t  - unsigned integer exactly as wide as a pointer
 *   arch_sword_t - signed counterpart
 *   arch_reg_t   - a general-purpose register value (same width as pointer)
 *   arch_paddr_t - physical address (always 64-bit to handle PAE / 40-bit PA)
 *   arch_vaddr_t - virtual address (same as arch_word_t)
 * ========================================================================= */

#if ARCH_IS_64BIT
typedef uint64_t  arch_word_t;
typedef int64_t   arch_sword_t;
typedef uint64_t  arch_reg_t;
typedef uint64_t  arch_vaddr_t;
#else
typedef uint32_t  arch_word_t;
typedef int32_t   arch_sword_t;
typedef uint32_t  arch_reg_t;
typedef uint32_t  arch_vaddr_t;
#endif

/* Physical addresses are always 64-bit so PAE / LPAE / 52-bit PA work */
typedef uint64_t  arch_paddr_t;

/* =========================================================================
 * 4. Abstract CPU state
 *
 * arch_cpu_state_t captures enough context to save/restore a CPU thread.
 * The union picks the right register file at compile time.
 * ========================================================================= */

#if ARCH_X86_32
typedef struct {
    /* General-purpose registers (pushed by pusha / ISR stub) */
    uint32_t edi, esi, ebp, esp_dummy;
    uint32_t ebx, edx, ecx, eax;
    /* Interrupt / exception frame */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
    uint32_t esp, ss;           /* only valid on privilege change */
    /* Segment registers */
    uint16_t ds, es, fs, gs;
} arch_cpu_state_x86_32_t;

typedef arch_cpu_state_x86_32_t arch_cpu_state_t;

#elif ARCH_X86_64
typedef struct {
    /* Callee-saved + scratch GP registers (pushed by ISR stub) */
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9,  r8;
    uint64_t rdi, rsi, rbp, rbx;
    uint64_t rdx, rcx, rax;
    /* Exception info */
    uint64_t int_no, err_code;
    /* CPU-pushed interrupt frame */
    uint64_t rip, cs, rflags;
    uint64_t rsp, ss;
} arch_cpu_state_x86_64_t;

typedef arch_cpu_state_x86_64_t arch_cpu_state_t;

#elif ARCH_ARM32
typedef struct {
    /* r0-r12 general purpose, r13=sp, r14=lr, r15=pc */
    uint32_t r0,  r1,  r2,  r3;
    uint32_t r4,  r5,  r6,  r7;
    uint32_t r8,  r9,  r10, r11;
    uint32_t r12;
    uint32_t sp;            /* r13 - stack pointer */
    uint32_t lr;            /* r14 - link register */
    uint32_t pc;            /* r15 - program counter */
    uint32_t cpsr;          /* current program status register */
    uint32_t spsr;          /* saved program status register */
} arch_cpu_state_arm32_t;

typedef arch_cpu_state_arm32_t arch_cpu_state_t;

#elif ARCH_ARM64
typedef struct {
    /* x0-x29 general purpose (x30 = link register) */
    uint64_t x0,  x1,  x2,  x3;
    uint64_t x4,  x5,  x6,  x7;
    uint64_t x8,  x9,  x10, x11;
    uint64_t x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19;
    uint64_t x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27;
    uint64_t x28, x29;
    uint64_t x30;           /* link register */
    uint64_t sp;            /* stack pointer (EL1) */
    uint64_t pc;            /* program counter */
    uint64_t spsr_el1;      /* saved program status register (EL1) */
    uint64_t elr_el1;       /* exception link register (EL1) */
    uint64_t esr_el1;       /* exception syndrome register */
    uint64_t far_el1;       /* fault address register */
} arch_cpu_state_arm64_t;

typedef arch_cpu_state_arm64_t arch_cpu_state_t;
#endif /* arch cpu state */

/* =========================================================================
 * 5. Abstract inline operations
 *
 * These thin wrappers give portable names to core CPU operations.  They
 * compile to a single instruction (or a very tight sequence) on each arch.
 * ========================================================================= */

/* ----- Stack pointer ----- */
static inline arch_word_t arch_get_sp(void)
{
#if ARCH_X86_32
    arch_word_t sp;
    __asm__ volatile ("movl %%esp, %0" : "=r"(sp));
    return sp;
#elif ARCH_X86_64
    arch_word_t sp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(sp));
    return sp;
#elif ARCH_ARM32
    arch_word_t sp;
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
    return sp;
#elif ARCH_ARM64
    arch_word_t sp;
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
    return sp;
#endif
}

/* ----- Instruction pointer ----- */
static inline arch_word_t arch_get_ip(void)
{
#if ARCH_X86_32
    arch_word_t ip;
    __asm__ volatile ("call 1f\n1: popl %0" : "=r"(ip));
    return ip;
#elif ARCH_X86_64
    arch_word_t ip;
    __asm__ volatile ("leaq (%%rip), %0" : "=r"(ip));
    return ip;
#elif ARCH_ARM32
    arch_word_t ip;
    __asm__ volatile ("mov %0, pc" : "=r"(ip));
    return ip;
#elif ARCH_ARM64
    arch_word_t ip;
    __asm__ volatile ("adr %0, ." : "=r"(ip));
    return ip;
#endif
}

/* ----- Halt (stop executing until next interrupt) ----- */
static inline void arch_halt(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("hlt");
#elif ARCH_ARM32
    __asm__ volatile ("wfi");   /* Wait For Interrupt */
#elif ARCH_ARM64
    __asm__ volatile ("wfi");
#endif
}

/* ----- Enable hardware interrupts ----- */
static inline void arch_enable_irq(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("sti" ::: "memory");
#elif ARCH_ARM32
    /* Clear I bit in CPSR */
    __asm__ volatile (
        "mrs r0, cpsr\n"
        "bic r0, r0, #0x80\n"
        "msr cpsr_c, r0\n"
        ::: "r0", "memory"
    );
#elif ARCH_ARM64
    __asm__ volatile ("msr daifclr, #2" ::: "memory"); /* clear IRQ mask */
#endif
}

/* ----- Disable hardware interrupts ----- */
static inline void arch_disable_irq(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("cli" ::: "memory");
#elif ARCH_ARM32
    /* Set I bit in CPSR */
    __asm__ volatile (
        "mrs r0, cpsr\n"
        "orr r0, r0, #0x80\n"
        "msr cpsr_c, r0\n"
        ::: "r0", "memory"
    );
#elif ARCH_ARM64
    __asm__ volatile ("msr daifset, #2" ::: "memory"); /* set IRQ mask */
#endif
}

/* ----- Memory barrier ----- */
static inline void arch_mb(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("mfence" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("dmb sy" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dmb sy" ::: "memory");
#endif
}

/* ----- Read memory barrier ----- */
static inline void arch_rmb(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("lfence" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("dmb ish" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dmb ishld" ::: "memory");
#endif
}

/* ----- Write memory barrier ----- */
static inline void arch_wmb(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("sfence" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("dmb ishst" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("dmb ishst" ::: "memory");
#endif
}

/* ----- CPU relax (spin-wait hint) ----- */
static inline void arch_cpu_relax(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("pause" ::: "memory");
#elif ARCH_ARM32
    __asm__ volatile ("yield" ::: "memory");
#elif ARCH_ARM64
    __asm__ volatile ("yield" ::: "memory");
#endif
}

/* ----- No-op / compiler barrier ----- */
static inline void arch_nop(void)
{
#if ARCH_IS_X86
    __asm__ volatile ("nop");
#elif ARCH_ARM32
    __asm__ volatile ("nop");
#elif ARCH_ARM64
    __asm__ volatile ("nop");
#endif
}

/* =========================================================================
 * 6. Arch-capability feature query constants  (used with arch_supports_feature)
 * ========================================================================= */

#define ARCH_FEAT_FPU            (1U << 0)   /* Hardware floating point */
#define ARCH_FEAT_SIMD           (1U << 1)   /* SIMD / vector unit */
#define ARCH_FEAT_MMU            (1U << 2)   /* Memory Management Unit */
#define ARCH_FEAT_SMP            (1U << 3)   /* Multi-processor capable */
#define ARCH_FEAT_ATOMIC64       (1U << 4)   /* 64-bit atomic ops */
#define ARCH_FEAT_UNALIGNED_MEM  (1U << 5)   /* Efficient unaligned access */
#define ARCH_FEAT_VIRT           (1U << 6)   /* Hardware virtualization */
#define ARCH_FEAT_CRYPTO         (1U << 7)   /* Hardware crypto acceleration */
#define ARCH_FEAT_CRC32          (1U << 8)   /* Hardware CRC32 */
#define ARCH_FEAT_THUMB          (1U << 9)   /* ARM Thumb / Thumb-2 (ARM32 only) */
#define ARCH_FEAT_NEON           (1U << 10)  /* ARM NEON (ARM only) */
#define ARCH_FEAT_SVE            (1U << 11)  /* ARM SVE (AArch64 only) */
#define ARCH_FEAT_TSX            (1U << 12)  /* Intel TSX (x86 only) */
#define ARCH_FEAT_AVX512         (1U << 13)  /* Intel AVX-512 (x86-64 only) */

/* =========================================================================
 * 7. Pull in the right arch-specific header
 * ========================================================================= */

#if ARCH_X86_32
#   include "x86_32/arch_x86_32.h"
#elif ARCH_X86_64
#   include "x86_64/arch_x86_64.h"
#elif ARCH_ARM32
#   include "arm32/arch_arm32.h"
#elif ARCH_ARM64
#   include "aarch64/arch_aarch64.h"
#endif

/* =========================================================================
 * 8. Function declarations implemented in arch_ops.c
 * ========================================================================= */

/**
 * arch_init - Perform architecture-specific early initialisation.
 *
 * Called once from the common kernel entry point before any subsystem
 * (memory, scheduler, devices) is brought up.
 */
void arch_init(void);

/**
 * arch_get_name - Return a short ASCII string identifying the architecture.
 *
 * Returns one of: "x86_32", "x86_64", "arm32", "aarch64".
 */
const char *arch_get_name(void);

/**
 * arch_get_page_size - Return the native page size in bytes.
 *
 * 4096 for all currently supported architectures.
 */
size_t arch_get_page_size(void);

/**
 * arch_supports_feature - Query whether a capability is present on this CPU.
 *
 * @feature: one of the ARCH_FEAT_* constants defined above.
 *
 * Returns true if the feature is available, false otherwise.  Detection is
 * performed lazily on first call and cached for subsequent calls.
 */
bool arch_supports_feature(uint32_t feature);

#endif /* FOREST_ARCH_H */
