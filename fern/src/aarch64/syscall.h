/*
 * Fern - AArch64 syscall interface
 *
 * AArch64 Linux syscall convention (matching Linux arm64 ABI):
 *   x8  = syscall number
 *   x0  = arg0 / return value
 *   x1  = arg1
 *   x2  = arg2
 *   x3  = arg3
 *   x4  = arg4
 *   x5  = arg5
 *   x6  = arg6
 *   x7  = arg7  (rarely used, but reserved)
 *
 * Syscalls are triggered by the SVC #0 instruction from EL0.
 * The exception vector (vec_lower64_sync) checks ESR_EL1.EC == 0x15
 * and jumps to aarch64_syscall_dispatch().
 */
#ifndef AARCH64_SYSCALL_H
#define AARCH64_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/*
 * AArch64 Linux syscall numbers (used by user-space binaries).
 * These differ from the x86_64 numbers in the main syscall.h.
 * We translate to Fern internal syscall numbers before dispatch.
 */
#define AARCH64_SYS_READ            63
#define AARCH64_SYS_WRITE           64
#define AARCH64_SYS_OPEN            56   /* openat on arm64 */
#define AARCH64_SYS_OPENAT          56
#define AARCH64_SYS_CLOSE           57
#define AARCH64_SYS_FSTAT           80
#define AARCH64_SYS_NEWFSTATAT      79
#define AARCH64_SYS_LSEEK           62
#define AARCH64_SYS_MMAP            222
#define AARCH64_SYS_MPROTECT        226
#define AARCH64_SYS_MUNMAP          215
#define AARCH64_SYS_BRK             214
#define AARCH64_SYS_GETPID          172
#define AARCH64_SYS_EXIT            93
#define AARCH64_SYS_EXIT_GROUP      94
#define AARCH64_SYS_NANOSLEEP       101
#define AARCH64_SYS_CLOCK_GETTIME   113
#define AARCH64_SYS_CLOCK_GETRES    114
#define AARCH64_SYS_GETTIMEOFDAY    169
#define AARCH64_SYS_GETUID          174
#define AARCH64_SYS_GETGID          176
#define AARCH64_SYS_GETEUID         175
#define AARCH64_SYS_GETEGID         177
#define AARCH64_SYS_KILL            129
#define AARCH64_SYS_CLONE           220
#define AARCH64_SYS_EXECVE          221
#define AARCH64_SYS_WAIT4           260
#define AARCH64_SYS_SOCKET          198
#define AARCH64_SYS_BIND            200
#define AARCH64_SYS_LISTEN          201
#define AARCH64_SYS_ACCEPT          202
#define AARCH64_SYS_CONNECT         203
#define AARCH64_SYS_SENDTO          206
#define AARCH64_SYS_RECVFROM        207
#define AARCH64_SYS_IOCTL           29
#define AARCH64_SYS_FCNTL           25
#define AARCH64_SYS_PIPE2           59
#define AARCH64_SYS_DUP3            24
#define AARCH64_SYS_GETCWD          17
#define AARCH64_SYS_CHDIR           49
#define AARCH64_SYS_MKDIR           34   /* mkdirat */
#define AARCH64_SYS_MKDIRAT         34
#define AARCH64_SYS_UNLINKAT        35
#define AARCH64_SYS_RENAMEAT        38
#define AARCH64_SYS_GETDENTS64      61

/* Error codes (POSIX, as returned in x0) */
#define AARCH64_ENOSYS              (-38)
#define AARCH64_EINVAL              (-22)
#define AARCH64_EPERM               (-1)
#define AARCH64_ENOENT              (-2)
#define AARCH64_ENOMEM              (-12)

/* ------------------------------------------------------------------ */
/* Saved register frame passed to aarch64_syscall_dispatch()           */
/* This matches the layout built by SAVE_REGS in exceptions.S          */
/* ------------------------------------------------------------------ */
struct aarch64_regs {
    uint64_t x[31];     /* x0 – x30 */
    uint64_t sp_el0;    /* saved user stack pointer */
    uint64_t elr_el1;   /* saved ELR (PC at time of SVC) */
    uint64_t spsr_el1;  /* saved SPSR */
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * aarch64_syscall_handle - Primary syscall entry point for vectors.S.
 *
 * Called from el0_svc in vectors.S with the AArch64 C ABI:
 *   @nr     : syscall number (from userspace x8), passed as x0
 *   @a0-a6  : syscall arguments (from userspace x0-x6), passed as x1-x7
 *
 * Returns the syscall return value; vectors.S stores it into the saved
 * x0 frame slot so the user process receives it after eret.
 */
int64_t aarch64_syscall_handle(uint64_t nr,
                                uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5,
                                uint64_t a6);

/**
 * aarch64_syscall_dispatch - Legacy syscall entry point for exceptions.S.
 *
 * Called from the exception vector with:
 *   @x0-x7  : syscall arguments (already in registers at call site)
 *   @x8     : syscall number (passed last as @nr)
 *
 * Bridges to aarch64_syscall_handle().
 * Returns the syscall return value (written back to saved x0).
 */
long aarch64_syscall_dispatch(uint64_t x0, uint64_t x1, uint64_t x2,
                               uint64_t x3, uint64_t x4, uint64_t x5,
                               uint64_t x6, uint64_t x7, uint64_t nr);

/**
 * aarch64_syscall_init - Register the SVC handler and set up syscall table.
 */
void aarch64_syscall_init(void);

#endif /* AARCH64_SYSCALL_H */
