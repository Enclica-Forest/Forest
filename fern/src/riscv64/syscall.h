/*
 * Fern - RISC-V 64-bit Syscall Interface
 *
 * RISC-V Linux syscall convention (RV64 ABI):
 *   a7  = syscall number
 *   a0  = arg0 / return value
 *   a1  = arg1
 *   a2  = arg2
 *   a3  = arg3
 *   a4  = arg4
 *   a5  = arg5
 *
 * Syscalls are triggered by the ECALL instruction from S-mode (or U-mode).
 * The trap handler checks the ecall cause and jumps to
 * riscv64_syscall_dispatch().
 */
#ifndef RISCV64_SYSCALL_H
#define RISCV64_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* ====================================================================
 * RISC-V 64-bit Linux syscall numbers (asm-generic/unistd.h)
 *
 * These differ from x86_64 and AArch64 numbers.  We translate to Fern
 * internal syscall numbers before dispatch.
 * ==================================================================== */

/* ---- I/O ---------------------------------------------------------- */
#define RISCV64_SYS_GETCWD              17
#define RISCV64_SYS_DUP                 23
#define RISCV64_SYS_DUP3                24
#define RISCV64_SYS_FCNTL               25
#define RISCV64_SYS_IOCTL               29
#define RISCV64_SYS_MKNODAT             33
#define RISCV64_SYS_MKDIRAT             34
#define RISCV64_SYS_UNLINKAT            35
#define RISCV64_SYS_SYMLINKAT           36
#define RISCV64_SYS_LINKAT              37
#define RISCV64_SYS_RENAMEAT            38
#define RISCV64_SYS_UMOUNT2             39
#define RISCV64_SYS_STATFS              43
#define RISCV64_SYS_FSTATFS             44
#define RISCV64_SYS_TRUNCATE            46
#define RISCV64_SYS_FTRUNCATE           47
#define RISCV64_SYS_FACCESSAT           48
#define RISCV64_SYS_CHDIR               49
#define RISCV64_SYS_FCHDIR              50
#define RISCV64_SYS_CHROOT              41
#define RISCV64_SYS_OPENAT              56
#define RISCV64_SYS_CLOSE               57
#define RISCV64_SYS_PIPE2               59
#define RISCV64_SYS_GETDENTS64          61
#define RISCV64_SYS_LSEEK               62
#define RISCV64_SYS_READ                63
#define RISCV64_SYS_WRITE               64
#define RISCV64_SYS_READV               65
#define RISCV64_SYS_WRITEV              66
#define RISCV64_SYS_PREAD64             67
#define RISCV64_SYS_PWRITE64            68
#define RISCV64_SYS_PREADV              69
#define RISCV64_SYS_PWRITEV             70
#define RISCV64_SYS_PPOLL               73
#define RISCV64_SYS_RENAMEAT2           76
#define RISCV64_SYS_READLINKAT          78
#define RISCV64_SYS_FSTATAT             79
#define RISCV64_SYS_FSTAT               80
#define RISCV64_SYS_NEWFSTATAT          79  /* alias for FSTATAT */
#define RISCV64_SYS_FACCESSAT2          48  /* alias for FACCESSAT */
#define RISCV64_SYS_COPY_FILE_RANGE     85
#define RISCV64_SYS_UTIMENSAT           88
#define RISCV64_SYS_CLOSE_RANGE         143

/* ---- Memory ------------------------------------------------------- */
#define RISCV64_SYS_MMAP                222
#define RISCV64_SYS_MPROTECT            226
#define RISCV64_SYS_MUNMAP              215
#define RISCV64_SYS_MADVISE             233
#define RISCV64_SYS_MLOCK               234
#define RISCV64_SYS_MUNLOCK             235
#define RISCV64_SYS_MLOCKALL            236
#define RISCV64_SYS_MUNLOCKALL          237
#define RISCV64_SYS_MINCORE             232
#define RISCV64_SYS_MREMAP              216
#define RISCV64_SYS_MSYNC               227
#define RISCV64_SYS_BRK                 214
#define RISCV64_SYS_MSEAL               281

/* ---- Process ------------------------------------------------------ */
#define RISCV64_SYS_SET_TID_ADDRESS     96
#define RISCV64_SYS_FUTEX               98
#define RISCV64_SYS_SCHED_YIELD         124
#define RISCV64_SYS_KILL                129
#define RISCV64_SYS_TKILL               130
#define RISCV64_SYS_TGKILL              131
#define RISCV64_SYS_GETSIGINFO          132  /* sigaltstack */
#define RISCV64_SYS_RT_SIGACTION        134
#define RISCV64_SYS_RT_SIGPROCMASK      135
#define RISCV64_SYS_RT_SIGSUSPEND       136
#define RISCV64_SYS_RT_SIGTIMEDWAIT     137
#define RISCV64_SYS_RT_SIGRETURN        139
#define RISCV64_SYS_SETUID              146
#define RISCV64_SYS_SETREUID            113  /* not in RV64, stub */
#define RISCV64_SYS_SETREGID            114  /* not in RV64, stub */
#define RISCV64_SYS_SETRESUID           115  /* not in RV64, stub */
#define RISCV64_SYS_GETRESUID           116
#define RISCV64_SYS_SETRESGID           117  /* not in RV64, stub */
#define RISCV64_SYS_GETRESGID           118
#define RISCV64_SYS_SETGID              144
#define RISCV64_SYS_SETFSUID            113  /* stub: same as setreuid */
#define RISCV64_SYS_SETFSGID            114  /* stub: same as setregid */
#define RISCV64_SYS_SETGROUPS           81
#define RISCV64_SYS_GETGROUPS           80
#define RISCV64_SYS_SETSID              172  /* stub: same as getpid */
#define RISCV64_SYS_GETSID              173  /* stub: same as getppid */
#define RISCV64_SYS_SETEUID             113  /* stub */
#define RISCV64_SYS_SETEGID             114  /* stub */
#define RISCV64_SYS_GETPGID             132  /* stub: same as sigaltstack */
#define RISCV64_SYS_SETPGID             111  /* stub: not in RV64 */
#define RISCV64_SYS_GETPGRP             111  /* stub: same as setpgid */
#define RISCV64_SYS_TCGETPGRP           109  /* stub: not in RV64 */
#define RISCV64_SYS_TCSETPGRP           109  /* stub: same as tcgetpgrp */

/* ---- Scheduling --------------------------------------------------- */
#define RISCV64_SYS_SCHED_SETPARAM      118
#define RISCV64_SYS_SCHED_SETSCHEDULER  119
#define RISCV64_SYS_SCHED_GETSCHEDULER  120
#define RISCV64_SYS_SCHED_GETPARAM      121
#define RISCV64_SYS_SCHED_SETAFFINITY   122
#define RISCV64_SYS_SCHED_GETAFFINITY   123
#define RISCV64_SYS_SCHED_GET_PRIORITY_MAX 125
#define RISCV64_SYS_SCHED_GET_PRIORITY_MIN 126
#define RISCV64_SYS_SCHED_SETATTR       274
#define RISCV64_SYS_SCHED_GETATTR       275

/* ---- Time --------------------------------------------------------- */
#define RISCV64_SYS_NANOSLEEP           101
#define RISCV64_SYS_CLOCK_GETTIME       113
#define RISCV64_SYS_CLOCK_GETRES        114
#define RISCV64_SYS_GETTIMEOFDAY        169
#define RISCV64_SYS_SETTIMEOFDAY        169  /* stub: same as gettimeofday */
#define RISCV64_SYS_UTIME               136  /* stub: same as rt_sigtimedwait */

/* ---- Credentials -------------------------------------------------- */
#define RISCV64_SYS_GETPID              172
#define RISCV64_SYS_GETPPID             173
#define RISCV64_SYS_GETUID              174
#define RISCV64_SYS_GETEUID             175
#define RISCV64_SYS_GETGID              176
#define RISCV64_SYS_GETEGID             177
#define RISCV64_SYS_GETTID              178
#define RISCV64_SYS_UMASK               166

/* ---- Process lifecycle -------------------------------------------- */
#define RISCV64_SYS_EXIT                93
#define RISCV64_SYS_EXIT_GROUP          94
#define RISCV64_SYS_WAIT4               260
#define RISCV64_SYS_WAITPID             260  /* alias for wait4 */
#define RISCV64_SYS_CLONE               220
#define RISCV64_SYS_CLONE3              435
#define RISCV64_SYS_EXECVE              221
#define RISCV64_SYS_FORK                220  /* alias: clone-based */

/* ---- Resource limits ---------------------------------------------- */
#define RISCV64_SYS_GETRLIMIT           163
#define RISCV64_SYS_SETRLIMIT           164
#define RISCV64_SYS_GETRUSAGE           165
#define RISCV64_SYS_PRLIMIT64           261

/* ---- System info -------------------------------------------------- */
#define RISCV64_SYS_SYSINFO             179
#define RISCV64_SYS_TIMES               153
#define RISCV64_SYS_UNAME               160

/* ---- Priority ----------------------------------------------------- */
#define RISCV64_SYS_GETPRIORITY         141
#define RISCV64_SYS_SETPRIORITY         140

/* ---- Networking --------------------------------------------------- */
#define RISCV64_SYS_SOCKET              198
#define RISCV64_SYS_SOCKETPAIR          199
#define RISCV64_SYS_BIND                200
#define RISCV64_SYS_LISTEN              201
#define RISCV64_SYS_ACCEPT              202
#define RISCV64_SYS_ACCEPT4             242
#define RISCV64_SYS_CONNECT             203
#define RISCV64_SYS_GETSOCKNAME         204
#define RISCV64_SYS_GETPEERNAME         205
#define RISCV64_SYS_SENDTO              206
#define RISCV64_SYS_RECVFROM            207
#define RISCV64_SYS_SETSOCKOPT          208
#define RISCV64_SYS_GETSOCKOPT          209
#define RISCV64_SYS_SHUTDOWN            210
#define RISCV64_SYS_SENDMSG             211
#define RISCV64_SYS_RECVMSG             212

/* ---- I/O multiplexing -------------------------------------------- */
#define RISCV64_SYS_POLL                7
#define RISCV64_SYS_PPOLL_RV            73  /* separate from regular ppoll */
#define RISCV64_SYS_EPOLL_CREATE1       20
#define RISCV64_SYS_EPOLL_CTL           21
#define RISCV64_SYS_EPOLL_PWAIT         22
#define RISCV64_SYS_EPOLL_WAIT          22  /* alias: epoll_pwait */
#define RISCV64_SYS_SELECT              23  /* stub: not in RV64 */
#define RISCV64_SYS_PSELECT6            23  /* alias: select */

/* ---- File sync ---------------------------------------------------- */
#define RISCV64_SYS_FSYNC               72
#define RISCV64_SYS_FDATASYNC           72  /* alias: fsync */

/* ---- Signals ------------------------------------------------------ */
#define RISCV64_SYS_SIGPROCMASK         135  /* alias: rt_sigprocmask */
#define RISCV64_SYS_SIGACTION           134  /* alias: rt_sigaction */

/* ---- Misc --------------------------------------------------------- */
#define RISCV64_SYS_PRCTL               167  /* stub */
#define RISCV64_SYS_GETRANDOM           278
#define RISCV64_SYS_MEMFD_CREATE        279
#define RISCV64_SYS_STATX               291
#define RISCV64_SYS_RSEQ                243
#define RISCV64_SYS_FADVISE64           254
#define RISCV64_SYS_FCHMODAT            48   /* alias: faccessat */
#define RISCV64_SYS_FCHOWNAT            54
#define RISCV64_SYS_FUTIMESAT           88   /* alias: utimensat */

/* ====================================================================
 * Error codes (POSIX, as returned in a0)
 * ==================================================================== */
#define RISCV64_ENOSYS          (-38)
#define RISCV64_EINVAL          (-22)
#define RISCV64_EPERM           (-1)
#define RISCV64_ENOENT          (-2)
#define RISCV64_ENOMEM          (-12)
#define RISCV64_EBADF           (-9)
#define RISCV64_EACCES          (-13)
#define RISCV64_EFAULT          (-14)
#define RISCV64_EBUSY           (-16)
#define RISCV64_EEXIST          (-17)
#define RISCV64_ENOTDIR         (-20)
#define RISCV64_EISDIR          (-21)
#define RISCV64_EMFILE          (-24)
#define RISCV64_ENOSPC          (-28)
#define RISCV64_EPIPE           (-32)
#define RISCV64_EAGAIN          (-11)
#define RISCV64_EWOULDBLOCK     (-11)
#define RISCV64_ENOTSOCK        (-88)
#define RISCV64_ENOPROTOOPT     (-92)
#define RISCV64_EAFNOSUPPORT    (-97)
#define RISCV64_EADDRINUSE      (-98)
#define RISCV64_EADDRNOTAVAIL   (-99)
#define RISCV64_ENETDOWN        (-100)
#define RISCV64_ENETUNREACH     (-101)
#define RISCV64_ENETRESET       (-102)
#define RISCV64_ECONNRESET      (-104)
#define RISCV64_ENOBUFS         (-105)
#define RISCV64_EISCONN         (-106)
#define RISCV64_ENOTCONN        (-107)
#define RISCV64_ETIMEDOUT       (-110)
#define RISCV64_ECONNREFUSED    (-111)
#define RISCV64_EALREADY        (-114)
#define RISCV64_EINPROGRESS     (-115)

/* ====================================================================
 * Saved register frame passed to riscv64_syscall_dispatch()
 *
 * On RISC-V, the ECALL instruction traps to S-mode.  The trap handler
 * saves the user-mode registers into this frame before calling C code.
 * ==================================================================== */
struct riscv64_regs {
    uint64_t ra;        /* return address */
    uint64_t sp;        /* user stack pointer */
    uint64_t gp;        /* global pointer */
    uint64_t tp;        /* thread pointer */
    uint64_t t0;        /* temporaries t0-t2 */
    uint64_t t1;
    uint64_t t2;
    uint64_t s0;        /* saved registers s0-s11 (frame pointer = s0) */
    uint64_t s1;
    uint64_t a0;        /* arguments a0-a7 (a0 = return value) */
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;        /* syscall number */
    uint64_t s2;        /* more saved registers */
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t t3;        /* temporaries t3-t6 */
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
    uint64_t sepc;      /* saved PC (return address for sret) */
    uint64_t sstatus;   /* saved status register */
};

/* ====================================================================
 * Public API
 * ==================================================================== */

/**
 * riscv64_syscall_handle - Primary syscall entry point for trap vector.
 *
 * Called from the RISC-V trap handler with the standard C ABI:
 *   @nr     : syscall number (from userspace a7), passed as first arg
 *   @a0-a5  : syscall arguments (from userspace a0-a5)
 *
 * Returns the syscall return value; the trap handler stores it into the
 * saved a0 frame slot so the user process receives it after sret.
 */
int64_t riscv64_syscall_handle(uint64_t nr,
                                uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5);

/**
 * riscv64_syscall_dispatch - Legacy syscall entry point for trap entry.
 *
 * Called from the exception vector with:
 *   @a0-a5  : syscall arguments (already in registers at call site)
 *   @a7     : syscall number (passed last as @nr)
 *
 * Bridges to riscv64_syscall_handle().
 * Returns the syscall return value (written back to saved a0).
 */
long riscv64_syscall_dispatch(uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5,
                               uint64_t nr);

/**
 * riscv64_syscall_init - Register the ECALL handler and set up syscall table.
 */
void riscv64_syscall_init(void);

#endif /* RISCV64_SYSCALL_H */
