/*
 * Fern - Cross-Architecture Signal Frame Definitions
 * signal.h
 *
 * Defines the common signal delivery interface and per-architecture
 * signal frame structures.  Each platform provides its own layout
 * for the frame that gets built on the user stack during signal
 * delivery, plus the trampoline code that calls rt_sigreturn.
 *
 * The signal delivery flow is:
 *   1. Scheduler detects pending signal with a handler
 *   2. setup_signal_frame() builds the trampoline frame on user stack
 *   3. Task's saved PC/SP are redirected to the signal handler
 *   4. Handler executes, then calls the trampoline
 *   5. Trampoline invokes rt_sigreturn syscall
 *   6. Kernel restores original register context
 */

#ifndef FOREST_SIGNAL_H
#define FOREST_SIGNAL_H

#include "../arch/arch.h"

/* Signal delivery return codes */
#define SIGNAL_DELIVERED    0   /* Signal frame successfully set up */
#define SIGNAL_NO_HANDLER  1   /* Signal has no user handler (SIG_DFL/IGN) */
#define SIGNAL_NO_STACK    2   /* Not enough user stack space */
#define SIGNAL_BAD_SIGNUM  3   /* Invalid signal number */

/* rt_sigreturn syscall number (Linux ABI, same across architectures) */
#define SYS_RT_SIGRETURN_NR  15

/*
 * Per-task saved user context, used to store the original registers
 * before redirecting to the signal handler.  This is architecture-
 * specific and lives in the signal frame on the user stack.
 */
#if ARCH_X86_64

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rbp;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} signal_saved_ctx_t;

/* x86_64 signal frame layout (pushed onto user stack, high→low) */
typedef struct {
    /* Trampoline return address (points to trampoline code below) */
    uint64_t trampoline_addr;

    /* Saved user context for rt_sigreturn to restore */
    signal_saved_ctx_t saved_ctx;

    /* Signal number passed to handler */
    uint32_t signum;
    uint32_t pad0;

    /* Pointer to the saved_ctx above (for rt_sigreturn) */
    uint64_t saved_ctx_ptr;

    /* Red zone padding (128 bytes mandatory on x86_64 SysV ABI) */
    uint8_t red_zone[128];
} signal_frame_x86_64_t;

/* Offset of trampoline address from frame top (for IRET RIP) */
#define SIGNAL_FRAME_TRAMPOLINE_OFFSET  0

#elif ARCH_ARM64

typedef struct {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29;
    uint64_t x30;       /* link register */
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t tpidr_el0;
} signal_saved_ctx_t;

/* AArch64 signal frame layout */
typedef struct {
    /* Trampoline return address */
    uint64_t trampoline_addr;

    /* Saved user context for rt_sigreturn */
    signal_saved_ctx_t saved_ctx;

    /* Signal number (in x0 for handler) */
    uint32_t signum;
    uint32_t pad0;

    /* Pointer to saved_ctx */
    uint64_t saved_ctx_ptr;

    /* 16-byte alignment padding */
    uint8_t pad1[16];
} signal_frame_aarch64_t;

#define SIGNAL_FRAME_TRAMPOLINE_OFFSET  0

#elif ARCH_RISCV64

typedef struct {
    uint64_t ra;        /* x1 - return address */
    uint64_t sp;        /* x2 - stack pointer */
    uint64_t gp;        /* x3 - global pointer */
    uint64_t tp;        /* x4 - thread pointer */
    uint64_t t0, t1, t2;
    uint64_t s0, s1;    /* x8, x9 - saved registers (fp = s0) */
    uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint64_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t t3, t4, t5, t6;
    uint64_t sstatus;
    uint64_t sepc;
} signal_saved_ctx_t;

/* RISC-V 64 signal frame layout */
typedef struct {
    /* Trampoline return address */
    uint64_t trampoline_addr;

    /* Saved user context for rt_sigreturn */
    signal_saved_ctx_t saved_ctx;

    /* Signal number (in a0 for handler) */
    uint32_t signum;
    uint32_t pad0;

    /* Pointer to saved_ctx */
    uint64_t saved_ctx_ptr;
} signal_frame_riscv64_t;

#define SIGNAL_FRAME_TRAMPOLINE_OFFSET  0

#elif ARCH_ARM32

typedef struct {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12;
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
} signal_saved_ctx_t;

/* ARM32 signal frame layout */
typedef struct {
    /* Trampoline return address (points to trampoline code) */
    uint32_t trampoline_addr;

    /* Saved user context for rt_sigreturn */
    signal_saved_ctx_t saved_ctx;

    /* Signal number (in r0 for handler) */
    uint32_t signum;

    /* Pointer to saved_ctx */
    uint32_t saved_ctx_ptr;
} signal_frame_arm32_t;

#define SIGNAL_FRAME_TRAMPOLINE_OFFSET  0

#endif /* architecture selection */

/*
 * Unified signal frame type - picks the right layout at compile time.
 */
#if ARCH_X86_64
typedef signal_frame_x86_64_t signal_frame_t;
#elif ARCH_ARM64
typedef signal_frame_aarch64_t signal_frame_t;
#elif ARCH_RISCV64
typedef signal_frame_riscv64_t signal_frame_t;
#elif ARCH_ARM32
typedef signal_frame_arm32_t signal_frame_t;
#endif

/*
 * Minimum user stack space required for signal delivery.
 * Must be large enough for the signal frame + trampoline + alignment.
 */
#if ARCH_X86_64
#define SIGNAL_FRAME_SIZE  (sizeof(signal_frame_t) + 64)  /* trampoline bytes */
#elif ARCH_ARM64
#define SIGNAL_FRAME_SIZE  (sizeof(signal_frame_t) + 64)
#elif ARCH_RISCV64
#define SIGNAL_FRAME_SIZE  (sizeof(signal_frame_t) + 64)
#elif ARCH_ARM32
#define SIGNAL_FRAME_SIZE  (sizeof(signal_frame_t) + 64)
#endif

/*
 * setup_signal_frame - Architecture-specific signal frame setup
 *
 * Builds a signal trampoline frame on the user stack and modifies the
 * task's saved context so that when the task returns to user mode, it
 * executes the signal handler instead of the original instruction.
 *
 * @task:       The target task (must be the current task)
 * @signum:     Signal number to deliver
 * @handler:    User-space address of the signal handler
 * @old_sp:     Pointer to the task's saved user stack pointer (modified in-place)
 *
 * Returns SIGNAL_DELIVERED on success, or an error code.
 */
int setup_signal_frame(task_t* task, int signum, void* handler, void** old_sp);

/*
 * get_signal_trampoline_addr - Get the address of the signal trampoline
 *
 * Returns the user-space address where the trampoline code lives.
 * This is called after setup_signal_frame() to redirect the return
 * address to the trampoline instead of directly to the handler.
 */
void* get_signal_trampoline_addr(void);

#endif /* FOREST_SIGNAL_H */
