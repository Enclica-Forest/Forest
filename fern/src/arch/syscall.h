/*
 * syscall.h - Cross-Architecture Syscall Number Translation for Fern
 *
 * Provides a unified interface for translating architecture-specific Linux
 * syscall numbers to the internal Fern canonical numbers (x86_64 Linux ABI).
 *
 * Each architecture uses its own syscall numbering:
 *   - x86_64:  Same as internal (identity mapping, no translation needed)
 *   - ARM32:   ARM EABI numbers (e.g., read=3, write=4)
 *   - AArch64: arm64 numbers (e.g., read=63, write=64)
 *   - RISC-V:  asm-generic numbers (same as AArch64)
 *
 * Usage:
 *   int32_t internal_nr = syscall_translate(arch_nr);
 *   if (internal_nr < 0) return -ENOSYS;
 *   // dispatch on internal_nr
 *
 * Or use the convenience function:
 *   int64_t result = syscall_dispatch_arch(arch_nr, a0, a1, a2, a3, a4, a5);
 */

#ifndef FOREST_ARCH_SYSCALL_H
#define FOREST_ARCH_SYSCALL_H

#include "arch.h"
#include <stdint.h>

/* =========================================================================
 * Internal Fern Syscall Numbers (canonical: x86_64 Linux ABI)
 *
 * These match the enum in src/include/syscall.h. Defined here as macros
 * so the translation tables can reference them without pulling in the
 * full x86-specific header.
 * ========================================================================= */
#define FERN_SYS_READ                   0
#define FERN_SYS_WRITE                  1
#define FERN_SYS_OPEN                   2
#define FERN_SYS_CLOSE                  3
#define FERN_SYS_STAT                   4
#define FERN_SYS_FSTAT                  5
#define FERN_SYS_LSTAT                  6
#define FERN_SYS_POLL                   7
#define FERN_SYS_LSEEK                  8
#define FERN_SYS_MMAP                   9
#define FERN_SYS_MPROTECT              10
#define FERN_SYS_MUNMAP                11
#define FERN_SYS_BRK                   12
#define FERN_SYS_RT_SIGACTION          13
#define FERN_SYS_RT_SIGPROCMASK        14
#define FERN_SYS_RT_SIGRETURN          15
#define FERN_SYS_IOCTL                 16
#define FERN_SYS_PREAD64               17
#define FERN_SYS_PWRITE64              18
#define FERN_SYS_READV                 19
#define FERN_SYS_WRITEV                20
#define FERN_SYS_ACCESS                21
#define FERN_SYS_PIPE                  22
#define FERN_SYS_SELECT                23
#define FERN_SYS_SCHED_YIELD           24
#define FERN_SYS_MREMAP                25
#define FERN_SYS_MSYNC                 26
#define FERN_SYS_MINCORE               27
#define FERN_SYS_MADVISE               28
#define FERN_SYS_SHMGET                29
#define FERN_SYS_SHMAT                 30
#define FERN_SYS_SHMCTL                31
#define FERN_SYS_DUP                   32
#define FERN_SYS_DUP2                  33
#define FERN_SYS_PAUSE                 34
#define FERN_SYS_NANOSLEEP             35
#define FERN_SYS_GETITIMER             36
#define FERN_SYS_ALARM                 37
#define FERN_SYS_SETITIMER             38
#define FERN_SYS_GETPID                39
#define FERN_SYS_SENDFILE              40
#define FERN_SYS_SOCKET                41
#define FERN_SYS_CONNECT               42
#define FERN_SYS_ACCEPT                43
#define FERN_SYS_SENDTO                44
#define FERN_SYS_RECVFROM              45
#define FERN_SYS_SENDMSG               46
#define FERN_SYS_RECVMSG               47
#define FERN_SYS_SHUTDOWN              48
#define FERN_SYS_BIND                  49
#define FERN_SYS_LISTEN                50
#define FERN_SYS_GETSOCKNAME           51
#define FERN_SYS_GETPEERNAME           52
#define FERN_SYS_SOCKETPAIR            53
#define FERN_SYS_SETSOCKOPT            54
#define FERN_SYS_GETSOCKOPT            55
#define FERN_SYS_CLONE                 56
#define FERN_SYS_FORK                  57
#define FERN_SYS_VFORK                 58
#define FERN_SYS_EXECVE                59
#define FERN_SYS_EXIT                  60
#define FERN_SYS_WAIT4                 61
#define FERN_SYS_KILL                  62
#define FERN_SYS_UNAME                 63
#define FERN_SYS_FCNTL                 72
#define FERN_SYS_FSYNC                 74
#define FERN_SYS_FDATASYNC             75
#define FERN_SYS_TRUNCATE              76
#define FERN_SYS_FTRUNCATE             77
#define FERN_SYS_GETDENTS64            78
#define FERN_SYS_GETCWD                79
#define FERN_SYS_CHDIR                 80
#define FERN_SYS_FCHDIR                81
#define FERN_SYS_RENAME                82
#define FERN_SYS_MKDIR                 83
#define FERN_SYS_RMDIR                 84
#define FERN_SYS_CREAT                 85
#define FERN_SYS_LINK                  86
#define FERN_SYS_UNLINK                87
#define FERN_SYS_SYMLINK               88
#define FERN_SYS_READLINK              89
#define FERN_SYS_CHMOD                 90
#define FERN_SYS_FCHMOD                91
#define FERN_SYS_CHOWN                 92
#define FERN_SYS_FCHOWN                93
#define FERN_SYS_LCHOWN                94
#define FERN_SYS_UMASK                 95
#define FERN_SYS_GETTIMEOFDAY          96
#define FERN_SYS_GETRLIMIT             97
#define FERN_SYS_GETRUSAGE             98
#define FERN_SYS_SYSINFO               99
#define FERN_SYS_TIMES                100
#define FERN_SYS_GETUID               102
#define FERN_SYS_GETGID               104
#define FERN_SYS_SETUID               105
#define FERN_SYS_SETGID               106
#define FERN_SYS_GETEUID              107
#define FERN_SYS_GETEGID              108
#define FERN_SYS_SETPGID              109
#define FERN_SYS_GETPPID              110
#define FERN_SYS_GETPGRP              111
#define FERN_SYS_SETSID               112
#define FERN_SYS_SETREUID             115
#define FERN_SYS_SETREGID             116
#define FERN_SYS_GETGROUPS            117
#define FERN_SYS_SETGROUPS            118
#define FERN_SYS_SETRESUID            119
#define FERN_SYS_GETRESUID            120
#define FERN_SYS_SETRESGID            121
#define FERN_SYS_GETRESGID            122
#define FERN_SYS_SETFSUID             123
#define FERN_SYS_SETFSGID             124
#define FERN_SYS_GETPGID              125
#define FERN_SYS_GETSID               126
#define FERN_SYS_SIGALTSTACK          131
#define FERN_SYS_MKNOD                133
#define FERN_SYS_STATFS               137
#define FERN_SYS_FSTATFS              138
#define FERN_SYS_GETPRIORITY          140
#define FERN_SYS_SETPRIORITY          141
#define FERN_SYS_SCHED_SETPARAM       144
#define FERN_SYS_SCHED_GETPARAM       145
#define FERN_SYS_SCHED_SETSCHEDULER   146
#define FERN_SYS_SCHED_GETSCHEDULER   147
#define FERN_SYS_SCHED_GET_PRIORITY_MAX 148
#define FERN_SYS_SCHED_GET_PRIORITY_MIN 149
#define FERN_SYS_PRCTL                157
#define FERN_SYS_SETRLIMIT            160
#define FERN_SYS_SETTIMEOFDAY         163
#define FERN_SYS_GETTID               188
#define FERN_SYS_FUTEX                202
#define FERN_SYS_SCHED_SETAFFINITY    204
#define FERN_SYS_SCHED_GETAFFINITY    205
#define FERN_SYS_EPOLL_CREATE         213
#define FERN_SYS_GETDENTS64_          217
#define FERN_SYS_SET_TID_ADDRESS      218
#define FERN_SYS_FADVISE64            221
#define FERN_SYS_CLOCK_SETTIME        227
#define FERN_SYS_CLOCK_GETTIME        228
#define FERN_SYS_CLOCK_GETRES         229
#define FERN_SYS_CLOCK_NANOSLEEP      230
#define FERN_SYS_EXIT_GROUP           231
#define FERN_SYS_EPOLL_WAIT           232
#define FERN_SYS_EPOLL_CTL            233
#define FERN_SYS_TGKILL               234
#define FERN_SYS_TKILL                200
#define FERN_SYS_TIME                 201
#define FERN_SYS_OPENAT               257
#define FERN_SYS_MKDIRAT              258
#define FERN_SYS_MKNODAT              259
#define FERN_SYS_FCHOWNAT             260
#define FERN_SYS_FUTIMESAT            261
#define FERN_SYS_NEWFSTATAT           262
#define FERN_SYS_UNLINKAT             263
#define FERN_SYS_RENAMEAT             264
#define FERN_SYS_LINKAT               265
#define FERN_SYS_SYMLINKAT            266
#define FERN_SYS_READLINKAT           267
#define FERN_SYS_FCHMODAT             268
#define FERN_SYS_FACCESSAT            269
#define FERN_SYS_PPOLL                271
#define FERN_SYS_SETXATTR             276
#define FERN_SYS_LSETXATTR            277
#define FERN_SYS_FSETXATTR            278
#define FERN_SYS_GETXATTR             279
#define FERN_SYS_LGETXATTR            280
#define FERN_SYS_FGETXATTR            281
#define FERN_SYS_LISTXATTR            282
#define FERN_SYS_LLISTXATTR           283
#define FERN_SYS_FLISTXATTR           284
#define FERN_SYS_REMOVEXATTR          285
#define FERN_SYS_LREMOVEXATTR         286
#define FERN_SYS_FREMOVEXATTR         287
#define FERN_SYS_SCHED_SETAFFINITY_   204
#define FERN_SYS_SCHED_GETAFFINITY_   205
#define FERN_SYS_EPOLL_PWAIT          281
#define FERN_SYS_UTIMENSAT            280
#define FERN_SYS_ACCEPT4              288
#define FERN_SYS_EPOLL_CREATE1        291
#define FERN_SYS_DUP3                 292
#define FERN_SYS_PIPE2                293
#define FERN_SYS_INOTIFY_INIT1        294
#define FERN_SYS_PREADV               295
#define FERN_SYS_PWRITEV              296
#define FERN_SYS_RT_TGSIGQUEUEINFO    297
#define FERN_SYS_PERF_EVENT_OPEN      298
#define FERN_SYS_RECVMMSG             299
#define FERN_SYS_FANOTIFY_INIT        303
#define FERN_SYS_PRLIMIT64            302
#define FERN_SYS_SYNCFS               306
#define FERN_SYS_SETNS                308
#define FERN_SYS_GETCPU               309
#define FERN_SYS_PROCESS_VM_READV     310
#define FERN_SYS_PROCESS_VM_WRITEV    311
#define FERN_SYS_FINIT_MODULE         313
#define FERN_SYS_SCHED_SETATTR        314
#define FERN_SYS_SCHED_GETATTR        315
#define FERN_SYS_RENAMEAT2            316
#define FERN_SYS_GETRANDOM            318
#define FERN_SYS_MEMFD_CREATE         319
#define FERN_SYS_EXECVEAT             322
#define FERN_SYS_COPY_FILE_RANGE      326
#define FERN_SYS_STATX                332
#define FERN_SYS_RSEQ                 334
#define FERN_SYS_CLOSE_RANGE          436
#define FERN_SYS_MSEAL                452

/* Fern-private syscall numbers (internal, not arch-specific) */
#define FERN_SYS_NETINFO              470
#define FERN_SYS_MMAP_FB              471
#define FERN_SYS_MUNMAP_FB            472
#define FERN_SYS_GET_FB_INFO          473
#define FERN_SYS_POWER                474
#define FERN_SYS_USERCTL              475
#define FERN_SYS_START_FB_WATCHER     476
#define FERN_SYS_STOP_FB_WATCHER      477
#define FERN_SYS_FB_FLUSH             478
#define FERN_SYS_READ_KBD_EVENT       479
#define FERN_SYS_READ_MOUSE_EVENT     480
#define FERN_SYS_POLL_INPUT           481
#define FERN_SYS_SOUND_PLAY           482
#define FERN_SYS_SOUND_STOP           483
#define FERN_SYS_SOUND_BEEP           484
#define FERN_SYS_SOUND_SET_VOLUME     485
#define FERN_SYS_SOUND_GET_VOLUME     486
#define FERN_SYS_SOUND_GET_INFO       487
#define FERN_SYS_SOUND_GET_CAPS       488
#define FERN_SYS_SOUND_PLAY_WAV       489
#define FERN_SYS_SPAWN_TASK           490
#define FERN_SYS_DLOPEN               491
#define FERN_SYS_DLSYM                492
#define FERN_SYS_DLCLOSE              493
#define FERN_SYS_FB_LOCK              494
#define FERN_SYS_FB_UNLOCK            495
#define FERN_SYS_FB_DIRTY_RECT        496
#define FERN_SYS_FB_GET_REGIONS       497
#define FERN_SYS_IPC_SHM_CREATE       498
#define FERN_SYS_IPC_SHM_OPEN         499
#define FERN_SYS_IPC_SHM_CLOSE        500
#define FERN_SYS_IPC_SHM_DESTROY      501
#define FERN_SYS_IPC_MSG_CREATE       502
#define FERN_SYS_IPC_MSG_OPEN         503
#define FERN_SYS_IPC_MSG_SEND         504
#define FERN_SYS_IPC_MSG_RECEIVE      505
#define FERN_SYS_IPC_MSG_DESTROY      506
#define FERN_SYS_SET_PGRP             507
#define FERN_SYS_GET_PGRP_EXT         508
#define FERN_SYS_SET_SESSION          509
#define FERN_SYS_GET_SESSION_EXT      510
#define FERN_SYS_SOUND_STREAM_CREATE  511
#define FERN_SYS_SOUND_STREAM_WRITE   512
#define FERN_SYS_SOUND_STREAM_PLAY    513
#define FERN_SYS_SOUND_STREAM_PAUSE   514
#define FERN_SYS_SOUND_STREAM_STOP    515
#define FERN_SYS_SOUND_STREAM_DESTROY 516
#define FERN_SYS_CHVT                 518
#define FERN_SYS_GETVT                519
#define FERN_SYS_GET_VT_COUNT         520
#define FERN_SYS_DM_REGISTER          521
#define FERN_SYS_DM_UNREGISTER        522
#define FERN_SYS_DM_AUTH_REPORT       523
#define FERN_SYS_DM_GET_SESSION       524
#define FERN_SYS_TCGETPGRP           525
#define FERN_SYS_TCSETPGRP           526
#define FERN_SYS_SET_FOREGROUND_TASK  527
#define FERN_SYS_GET_TASKS            528
#define FERN_SYS_SET_FB_MODE          529
#define FERN_SYS_GET_FB_MODES         530
#define FERN_SYS_GET_FB_GENERATION    531

/* Sentinel for unmapped syscall numbers */
#define FERN_NR_INVALID               (-1)

/* Forest-private base for ARM32 (in arch-specific space) */
#define ARM32_NR_FOREST_BASE         0xF000U

/* =========================================================================
 * Translation API
 * ========================================================================= */

/**
 * syscall_translate - Translate arch-specific syscall number to internal Fern number.
 *
 * @arch_nr: Architecture-specific syscall number (from userspace).
 *
 * Returns the internal Fern syscall number, or FERN_NR_INVALID (-1) if
 * the syscall number is not recognized.
 */
int32_t syscall_translate(uint32_t arch_nr);

/**
 * syscall_dispatch_arch - Translate and dispatch a syscall.
 *
 * @arch_nr: Architecture-specific syscall number.
 * @a0-a5:   Syscall arguments (as uint64_t, cast internally).
 *
 * Returns the syscall result. Calls the appropriate sys_* function
 * based on the translated internal syscall number.
 */
int64_t syscall_dispatch_arch(uint64_t arch_nr,
                               uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5);

#endif /* FOREST_ARCH_SYSCALL_H */
