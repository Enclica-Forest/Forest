/*
 * crossarcinterpret.h - Cross-Architecture Binary Interpreter for Fern
 *
 * Provides kernel-resident user-mode emulation similar to QEMU user-mode.
 * Allows running ELF binaries compiled for foreign CPU architectures directly
 * on a Fern kernel of any supported host architecture.
 *
 * Supported guest architectures:
 *   - x86 (IA-32 / i386)
 *   - x86-64 (AMD64)
 *   - ARM32 (ARMv7)
 *   - AArch64 (ARMv8-A 64-bit)
 *
 * Syscall translation:
 *   Guest Linux-ABI syscalls are intercepted at the emulated trap instruction
 *   (INT 0x80, SYSCALL, SWI #0, SVC #0) and forwarded to the Fern
 *   kernel via the host syscall interface.
 */

#ifndef CROSSARCINTERPRET_H
#define CROSSARCINTERPRET_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../include/types.h"

/* =========================================================================
 * Return codes
 * ========================================================================= */

#define CAI_OK          0       /* Success                                   */
#define CAI_EAGAIN      1       /* Need more steps (run returned mid-exec)   */
#define CAI_EXITED      2       /* Guest called exit()                       */
#define CAI_EFAULT      (-1)    /* Invalid guest memory access               */
#define CAI_EILL        (-2)    /* Illegal / unimplemented instruction       */
#define CAI_EINVAL      (-3)    /* Invalid argument                          */
#define CAI_ENOMEM      (-4)    /* Out of memory                             */
#define CAI_ENOTSUP     (-5)    /* Unsupported feature                       */
#define CAI_ESYSCALL    (-6)    /* Syscall bridge error                      */

/* =========================================================================
 * Architecture identifiers
 * ========================================================================= */

typedef enum {
    CAI_ARCH_X86_32  = 0,   /* IA-32 / i386                                */
    CAI_ARCH_X86_64  = 1,   /* AMD64 / x86-64                              */
    CAI_ARCH_ARM32   = 2,   /* ARMv7 (32-bit, little-endian)               */
    CAI_ARCH_AARCH64 = 3,   /* ARMv8-A 64-bit                              */
    CAI_ARCH_COUNT
} cai_arch_t;

/* =========================================================================
 * Per-architecture CPU register files
 * ========================================================================= */

/* ---- x86-32 register file ---- */
typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, esp, ebp;
    uint32_t eip;
    uint32_t eflags;
    /* Segment registers (flat model – selector values only) */
    uint16_t cs, ds, es, fs, gs, ss;
} cai_x86_32_regs_t;

/* ---- x86-64 register file ---- */
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rsp, rbp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;
    /* Segment bases (64-bit mode uses FS/GS base for TLS) */
    uint64_t fs_base, gs_base;
} cai_x86_64_regs_t;

/* ---- ARM32 register file ---- */
typedef struct {
    uint32_t r[16];     /* r0-r15 (r13=sp, r14=lr, r15=pc)                */
    uint32_t cpsr;      /* Current Program Status Register                 */
    bool     thumb;     /* true when in Thumb state                        */
} cai_arm32_regs_t;

/* ---- AArch64 register file ---- */
typedef struct {
    uint64_t x[31];     /* x0-x30 (x29=fp, x30=lr)                        */
    uint64_t sp;        /* Stack pointer (EL0)                             */
    uint64_t pc;        /* Program counter                                 */
    uint32_t nzcv;      /* Condition flags (N/Z/C/V in bits 31/30/29/28)  */
} cai_aarch64_regs_t;

/* ---- Union of all register files ---- */
typedef union {
    cai_x86_32_regs_t  x86_32;
    cai_x86_64_regs_t  x86_64;
    cai_arm32_regs_t   arm32;
    cai_aarch64_regs_t aarch64;
} cai_cpu_state_t;

/* =========================================================================
 * Guest memory region descriptor
 * ========================================================================= */

#define CAI_MAX_MEM_REGIONS 16

typedef struct {
    uint64_t  gva_base;     /* Guest virtual start address                 */
    uint8_t  *host_ptr;     /* Host kernel pointer to backing store        */
    size_t    size;         /* Region length in bytes                      */
    uint32_t  flags;        /* Protection flags (CAI_MEM_*)                */
} cai_mem_region_t;

#define CAI_MEM_READ    0x1
#define CAI_MEM_WRITE   0x2
#define CAI_MEM_EXEC    0x4

/* =========================================================================
 * Interpreter context
 * ========================================================================= */

typedef struct cai_context {
    /* Architecture identification */
    cai_arch_t      target_arch;    /* Architecture being emulated         */
    cai_arch_t      host_arch;      /* Architecture running the host       */

    /* CPU state */
    cai_cpu_state_t cpu;            /* Emulated CPU registers              */

    /* Convenience PC accessor (mirrors arch-specific pc field) */
    uint64_t        pc;             /* Program counter (guest)             */

    /* Guest memory */
    uint8_t        *mem_base;       /* Flat guest physical memory pool     */
    size_t          mem_size;       /* Size of the pool                    */

    /* Region map (for ELF segment tracking) */
    cai_mem_region_t regions[CAI_MAX_MEM_REGIONS];
    int              n_regions;

    /* Guest stack */
    uint64_t        stack_top;      /* Guest virtual address of stack top  */
    size_t          stack_size;     /* Stack size in bytes                 */

    /* Guest entry point (set by cai_load_elf) */
    uint64_t        entry_point;

    /* Exit code set when CAI_EXITED is returned */
    int             exit_code;

    /* Running flag */
    bool            running;

    /* Pending syscall injection: when the bridge has finished handling a
     * syscall it writes the result here and sets pending to false. */
    bool            syscall_pending;
    int64_t         syscall_result;

    /* Opaque per-arch decode state (e.g. prefetch buffer) */
    void           *arch_state;
} cai_context_t;

/* =========================================================================
 * Core public API
 * ========================================================================= */

/*
 * cai_create - Allocate and initialise an interpreter context.
 *
 * @target    : Architecture to emulate.
 * @mem_size  : Size (bytes) of the flat guest memory pool to allocate.
 *
 * Returns a pointer to the new context, or NULL on allocation failure.
 * The caller owns the context and must eventually call cai_destroy().
 */
cai_context_t *cai_create(cai_arch_t target, size_t mem_size);

/*
 * cai_destroy - Release all resources associated with @ctx.
 */
void cai_destroy(cai_context_t *ctx);

/*
 * cai_load_elf - Parse an ELF binary and map its segments into guest memory.
 *
 * The ELF class must match @ctx->target_arch.  PT_LOAD segments are copied
 * into the flat memory pool at the guest virtual addresses they request.
 * The program counter is set to the ELF entry point.
 *
 * Returns CAI_OK on success, negative CAI_E* on failure.
 */
int cai_load_elf(cai_context_t *ctx, const uint8_t *elf_data, size_t elf_size);

/*
 * cai_run - Execute up to @max_insns guest instructions.
 *
 * Returns:
 *   CAI_OK      – ran successfully and guest has not exited yet
 *   CAI_EAGAIN  – max_insns reached, caller should call again
 *   CAI_EXITED  – guest called exit(); ctx->exit_code holds the value
 *   negative    – fatal decode/memory error
 */
int cai_run(cai_context_t *ctx, uint32_t max_insns);

/*
 * cai_step - Execute exactly one guest instruction.
 *
 * Returns CAI_OK, CAI_EXITED, or a negative error code.
 */
int cai_step(cai_context_t *ctx);

/*
 * cai_inject_syscall_result - Supply the return value of a host syscall back
 * to the guest register file and resume execution.
 *
 * Call this after the syscall bridge has completed the host-side operation.
 */
void cai_inject_syscall_result(cai_context_t *ctx, int64_t result);

/* =========================================================================
 * Memory helpers (used by arch emulators)
 * ========================================================================= */

/*
 * Translate a guest virtual address to a host pointer.
 * Returns NULL if the address is outside all registered regions or if the
 * requested access flags are not permitted.
 */
uint8_t *cai_mem_gva_to_host(cai_context_t *ctx, uint64_t gva, size_t len,
                              uint32_t access_flags);

/* Byte-granularity read/write helpers with bounds checking.
 * Return CAI_OK or CAI_EFAULT. */
int cai_mem_read8  (cai_context_t *ctx, uint64_t gva, uint8_t  *out);
int cai_mem_read16 (cai_context_t *ctx, uint64_t gva, uint16_t *out);
int cai_mem_read32 (cai_context_t *ctx, uint64_t gva, uint32_t *out);
int cai_mem_read64 (cai_context_t *ctx, uint64_t gva, uint64_t *out);

int cai_mem_write8 (cai_context_t *ctx, uint64_t gva, uint8_t  val);
int cai_mem_write16(cai_context_t *ctx, uint64_t gva, uint16_t val);
int cai_mem_write32(cai_context_t *ctx, uint64_t gva, uint32_t val);
int cai_mem_write64(cai_context_t *ctx, uint64_t gva, uint64_t val);

/* Push / pop helpers for stack emulation */
int cai_stack_push32(cai_context_t *ctx, uint32_t val);
int cai_stack_pop32 (cai_context_t *ctx, uint32_t *out);
int cai_stack_push64(cai_context_t *ctx, uint64_t val);
int cai_stack_pop64 (cai_context_t *ctx, uint64_t *out);

/* =========================================================================
 * Syscall bridge (internal, called by arch emulators)
 * ========================================================================= */

/*
 * cai_syscall_dispatch - Translate and execute a guest syscall.
 *
 * Called by the architecture-specific step functions after decoding a
 * trap instruction (INT 0x80, SYSCALL, SWI #0, SVC #0).
 *
 * Reads all arguments from the appropriate guest calling-convention
 * registers, forwards the call to the Fern host, sets ctx->running
 * to false and ctx->exit_code when the guest calls exit(), and returns
 * the host-side result value.
 *
 * @ctx        : Interpreter context – provides register file and arch ID.
 * @syscall_nr : Raw guest syscall number (may be ignored; re-read from regs).
 */
int64_t cai_syscall_dispatch(cai_context_t *ctx, uint64_t syscall_nr);

/* Stack size constant used by the ELF loader */
#define CAI_STACK_SIZE_CONST  (1024 * 1024)   /* 1 MiB */

/* =========================================================================
 * Architecture-specific step functions (internal linkage, called by cai_step)
 * ========================================================================= */

int cai_x86_32_step (cai_context_t *ctx);
int cai_x86_64_step (cai_context_t *ctx);
int cai_arm32_step  (cai_context_t *ctx);
int cai_aarch64_step(cai_context_t *ctx);

/* =========================================================================
 * Host architecture detection
 * ========================================================================= */

static inline cai_arch_t cai_host_arch(void)
{
#if defined(__x86_64__)
    return CAI_ARCH_X86_64;
#elif defined(__i386__)
    return CAI_ARCH_X86_32;
#elif defined(__aarch64__)
    return CAI_ARCH_AARCH64;
#elif defined(__arm__)
    return CAI_ARCH_ARM32;
#else
    return CAI_ARCH_X86_32; /* Fallback */
#endif
}

/* =========================================================================
 * ELF machine type constants needed by the loader
 * ========================================================================= */

#define CAI_EM_386    3     /* Intel 80386                                  */
#define CAI_EM_ARM    40    /* ARM                                          */
#define CAI_EM_X86_64 62    /* AMD x86-64                                   */
#define CAI_EM_AARCH64 183  /* ARM 64-bit architecture                      */

/* =========================================================================
 * Region registration helper (used by ELF loader)
 * ========================================================================= */

/*
 * cai_mem_add_region - Register a new guest memory region in ctx->regions[].
 *
 * @ctx        : Interpreter context.
 * @gva_base   : Guest virtual start address.
 * @host_ptr   : Host kernel pointer to the backing store.
 * @size       : Region length in bytes.
 * @flags      : CAI_MEM_READ | CAI_MEM_WRITE | CAI_MEM_EXEC.
 *
 * Returns CAI_OK or CAI_ENOMEM if the region table is full.
 */
int cai_mem_add_region(cai_context_t *ctx, uint64_t gva_base,
                       uint8_t *host_ptr, size_t size, uint32_t flags);

/* =========================================================================
 * High-level one-shot runner
 * ========================================================================= */

/*
 * crossarcinterpret_run_elf - Load and execute a static ELF binary.
 *
 * Detects the ELF architecture automatically, creates an interpreter context,
 * loads the ELF, sets up argc/argv, and runs until SYS_EXIT or a fatal error.
 *
 * Returns the guest exit code on success, or a negative CAI_E* on error.
 */
int crossarcinterpret_run_elf(const uint8_t *elf_data, size_t elf_size,
                               int argc, char **argv);

#endif /* CROSSARCINTERPRET_H */
