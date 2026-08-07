#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "framebuffer.h"

#ifndef ARCH_64BIT
#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_64BIT 1
#define ARCH_32BIT 0
#else
#define ARCH_64BIT 0
#define ARCH_32BIT 1
#endif
#endif

#define SYSCALL_VECTOR 0x80

// Linux x86_64 syscall numbers - complete table
enum syscall_number {
    SYS_READ                    = 0,
    SYS_WRITE                   = 1,
    SYS_OPEN                    = 2,
    SYS_CLOSE                   = 3,
    SYS_STAT                    = 4,
    SYS_FSTAT                   = 5,
    SYS_LSTAT                   = 6,
    SYS_POLL                    = 7,
    SYS_LSEEK                   = 8,
    SYS_MMAP                    = 9,
    SYS_MPROTECT                = 10,
    SYS_MUNMAP                  = 11,
    SYS_BRK                     = 12,
    SYS_RT_SIGACTION            = 13,
    SYS_RT_SIGPROCMASK          = 14,
    SYS_RT_SIGRETURN            = 15,
    SYS_IOCTL                   = 16,
    SYS_PREAD64                 = 17,
    SYS_PWRITE64                = 18,
    SYS_READV                   = 19,
    SYS_WRITEV                  = 20,
    SYS_ACCESS                  = 21,
    SYS_PIPE                    = 22,
    SYS_SELECT                  = 23,
    SYS_SCHED_YIELD             = 24,
    SYS_MREMAP                  = 25,
    SYS_MSYNC                   = 26,
    SYS_MINCORE                 = 27,
    SYS_MADVISE                 = 28,
    SYS_SHMGET                  = 29,
    SYS_SHMAT                   = 30,
    SYS_SHMCTL                  = 31,
    SYS_DUP                     = 32,
    SYS_DUP2                    = 33,
    SYS_PAUSE                   = 34,
    SYS_NANOSLEEP               = 35,
    SYS_GETITIMER               = 36,
    SYS_ALARM                   = 37,
    SYS_SETITIMER               = 38,
    SYS_GETPID                  = 39,
    SYS_SENDFILE                = 40,
    SYS_SOCKET                  = 41,
    SYS_CONNECT                 = 42,
    SYS_ACCEPT                  = 43,
    SYS_SENDTO                  = 44,
    SYS_RECVFROM                = 45,
    SYS_SENDMSG                 = 46,
    SYS_RECVMSG                 = 47,
    SYS_SHUTDOWN                = 48,
    SYS_BIND                    = 49,
    SYS_LISTEN                  = 50,
    SYS_GETSOCKNAME             = 51,
    SYS_GETPEERNAME             = 52,
    SYS_SOCKETPAIR              = 53,
    SYS_SETSOCKOPT              = 54,
    SYS_GETSOCKOPT              = 55,
    SYS_CLONE                   = 56,
    SYS_FORK                    = 57,
    SYS_VFORK                   = 58,
    SYS_EXECVE                  = 59,
    SYS_EXIT                    = 60,
    SYS_WAIT4                   = 61,
    SYS_KILL                    = 62,
    SYS_UNAME                   = 63,
    SYS_SEMGET                  = 64,
    SYS_SEMOP                   = 65,
    SYS_SEMCTL                  = 66,
    SYS_SHMDT                   = 67,
    SYS_MSGGET                  = 68,
    SYS_MSGSND                  = 69,
    SYS_MSGRCV                  = 70,
    SYS_MSGCTL                  = 71,
    SYS_FCNTL                   = 72,
    SYS_FLOCK                   = 73,
    SYS_FSYNC                   = 74,
    SYS_FDATASYNC               = 75,
    SYS_TRUNCATE                = 76,
    SYS_FTRUNCATE               = 77,
    SYS_GETDENTS                = 78,
    SYS_GETCWD                  = 79,
    SYS_CHDIR                   = 80,
    SYS_FCHDIR                  = 81,
    SYS_RENAME                  = 82,
    SYS_MKDIR                   = 83,
    SYS_RMDIR                   = 84,
    SYS_CREAT                   = 85,
    SYS_LINK                    = 86,
    SYS_UNLINK                  = 87,
    SYS_SYMLINK                 = 88,
    SYS_READLINK                = 89,
    SYS_CHMOD                   = 90,
    SYS_FCHMOD                  = 91,
    SYS_CHOWN                   = 92,
    SYS_FCHOWN                  = 93,
    SYS_LCHOWN                  = 94,
    SYS_UMASK                   = 95,
    SYS_GETTIMEOFDAY            = 96,
    SYS_GETRLIMIT               = 97,
    SYS_GETRUSAGE               = 98,
    SYS_SYSINFO                 = 99,
    SYS_TIMES                   = 100,
    SYS_PTRACE                  = 101,
    SYS_GETUID                  = 102,
    SYS_SYSLOG                  = 103,
    SYS_GETGID                  = 104,
    SYS_SETUID                  = 105,
    SYS_SETGID                  = 106,
    SYS_GETEUID                 = 107,
    SYS_GETEGID                 = 108,
    SYS_SETPGID                 = 109,
    SYS_GETPPID                 = 110,
    SYS_GETPGRP                 = 111,
    SYS_SETSID                  = 112,
    SYS_SETEUID                 = 113,
    SYS_SETEGID                 = 114,
    // NOT 127/128: those collide with the real i386 syscall numbers for
    // create_module/init_module, which i386_dispatch() (tried first on
    // 32-bit builds, see syscall.c) intercepts and answers with ENOSYS
    // before this enum's values are ever consulted -- silently breaking
    // tcgetpgrp()/tcsetpgrp() (see SYS_MAX comment below for where these
    // now live instead).
    SYS_TCGETPGRP               = 525,
    SYS_TCSETPGRP               = 526,

    // task_schedule()'s "always prefer foreground_task if runnable" priority
    // check (see task.c) means a shell blocked in wait4() (state cycles
    // WAITING/RUNNING) still starves its own child of scheduling time almost
    // entirely, since foreground_task stays pinned to the shell for the
    // task's whole lifetime (set once at session launch, see session.c).
    // This lets a job-control shell hand scheduling priority to whichever
    // job actually has the terminal, mirroring what real tcsetpgrp() does
    // for terminal access -- set_foreground(child_pid) before waiting on a
    // foreground job, set_foreground(getpid()) after reclaiming the prompt.
    SYS_SET_FOREGROUND_TASK     = 527,
    SYS_SETREUID                = 115,
    SYS_SETREGID                = 116,
    SYS_GETGROUPS               = 117,
    SYS_SETGROUPS               = 118,
    SYS_SETRESUID               = 119,
    SYS_GETRESUID               = 120,
    SYS_SETRESGID               = 121,
    SYS_GETRESGID               = 122,
    SYS_SETFSUID                = 123,
    SYS_SETFSGID                = 124,
    SYS_GETPGID                 = 125,
    SYS_GETSID                  = 126,
    SYS_CAPGET                  = 129,
    SYS_CAPSET                  = 130,
    SYS_RT_SIGPENDING           = 131,
    SYS_RT_SIGTIMEDWAIT         = 132,
    SYS_RT_SIGQUEUEINFO         = 133,
    SYS_RT_SIGSUSPEND           = 134,
    SYS_SIGALTSTACK             = 135,
    SYS_UTIME                   = 136,
    SYS_MKNOD                   = 137,
    SYS_STATFS                  = 138,
    SYS_FSTATFS                 = 139,
    SYS_PERSONALITY             = 140,
    SYS_USTAT                   = 141,
    SYS_SYSFS                   = 142,
    SYS_GETPRIORITY             = 143,
    SYS_SETPRIORITY             = 144,
    SYS_SCHED_SETPARAM          = 143,
    SYS_SCHED_GETPARAM          = 144,
    SYS_SCHED_SETSCHEDULER      = 145,
    SYS_SCHED_GETSCHEDULER      = 146,
    SYS_SCHED_GET_PRIORITY_MAX  = 148,
    SYS_SCHED_GET_PRIORITY_MIN  = 149,
    SYS_SCHED_RR_GET_INTERVAL   = 150,
    SYS_MLOCK                   = 151,
    SYS_MUNLOCK                 = 152,
    SYS_MLOCKALL                = 153,
    SYS_MUNLOCKALL              = 154,
    SYS_VHANGUP                 = 155,
    SYS_MODIFY_LDT              = 156,
    SYS_PIVOT_ROOT              = 157,
    SYS_PRCTL                   = 159,
    SYS_ARCH_PRCTL              = 160,
    SYS_ADJTIMEX                = 161,
    SYS_SETRLIMIT               = 162,
    SYS_CHROOT                  = 163,
    SYS_SYNC                    = 164,
    SYS_ACCT                    = 165,
    SYS_SETTIMEOFDAY            = 166,
    SYS_MOUNT                   = 167,
    SYS_UMOUNT2                 = 168,
    SYS_SWAPON                  = 169,
    SYS_SWAPOFF                 = 170,
    SYS_REBOOT                  = 171,
    SYS_SETHOSTNAME             = 172,
    SYS_SETDOMAINNAME           = 173,
    SYS_IOPL                    = 174,
    SYS_IOPERM                  = 175,
    SYS_INIT_MODULE             = 177,
    SYS_DELETE_MODULE           = 178,
    SYS_QUOTACTL                = 181,
    SYS_GETTID                  = 188,
    SYS_READAHEAD               = 189,
    SYS_SETXATTR                = 190,
    SYS_LSETXATTR               = 191,
    SYS_FSETXATTR               = 192,
    SYS_GETXATTR                = 193,
    SYS_LGETXATTR               = 194,
    SYS_FGETXATTR               = 195,
    SYS_LISTXATTR               = 196,
    SYS_LLISTXATTR              = 197,
    SYS_FLISTXATTR              = 198,
    SYS_REMOVEXATTR             = 199,
    SYS_LREMOVEXATTR            = 200,
    SYS_FREMOVEXATTR            = 201,
    SYS_TKILL                   = 202,
    SYS_TIME                    = 203,
    SYS_FUTEX                   = 204,
    SYS_SCHED_SETAFFINITY       = 205,
    SYS_SCHED_GETAFFINITY       = 206,
    SYS_IO_SETUP                = 208,
    SYS_IO_DESTROY              = 209,
    SYS_IO_GETEVENTS            = 210,
    SYS_IO_SUBMIT               = 211,
    SYS_IO_CANCEL               = 212,
    SYS_LOOKUP_DCOOKIE          = 214,
    SYS_EPOLL_CREATE            = 215,
    SYS_REMAP_FILE_PAGES        = 218,
    SYS_GETDENTS64              = 219,
    SYS_SET_TID_ADDRESS         = 220,
    SYS_RESTART_SYSCALL         = 221,
    SYS_SEMTIMEDOP              = 222,
    SYS_FADVISE64               = 223,
    SYS_TIMER_CREATE            = 224,
    SYS_TIMER_SETTIME           = 225,
    SYS_TIMER_GETTIME           = 226,
    SYS_TIMER_GETOVERRUN        = 227,
    SYS_TIMER_DELETE            = 228,
    SYS_CLOCK_SETTIME           = 229,
    SYS_CLOCK_GETTIME           = 230,
    SYS_CLOCK_GETRES            = 231,
    SYS_CLOCK_NANOSLEEP         = 232,
    SYS_EXIT_GROUP              = 233,
    SYS_EPOLL_WAIT              = 234,
    SYS_EPOLL_CTL               = 235,
    SYS_TGKILL                  = 236,
    SYS_UTIMES                  = 237,
    SYS_MBIND                   = 239,
    SYS_SET_MEMPOLICY           = 240,
    SYS_GET_MEMPOLICY           = 241,
    SYS_MQ_OPEN                 = 242,
    SYS_MQ_UNLINK               = 243,
    SYS_MQ_TIMEDSEND            = 244,
    SYS_MQ_TIMEDRECEIVE         = 245,
    SYS_MQ_NOTIFY               = 246,
    SYS_MQ_GETSETATTR           = 247,
    SYS_KEXEC_LOAD              = 248,
    SYS_WAITID                  = 249,
    SYS_ADD_KEY                 = 250,
    SYS_REQUEST_KEY             = 251,
    SYS_KEYCTL                  = 252,
    SYS_IOPRIO_SET              = 253,
    SYS_IOPRIO_GET              = 254,
    SYS_INOTIFY_INIT            = 255,
    SYS_INOTIFY_ADD_WATCH       = 256,
    SYS_INOTIFY_RM_WATCH        = 257,
    SYS_MIGRATE_PAGES           = 258,
    SYS_OPENAT                  = 259,
    SYS_MKDIRAT                 = 260,
    SYS_MKNODAT                 = 261,
    SYS_FCHOWNAT                = 262,
    SYS_FUTIMESAT               = 263,
    SYS_NEWFSTATAT              = 264,
    SYS_UNLINKAT                = 265,
    SYS_RENAMEAT                = 266,
    SYS_LINKAT                  = 267,
    SYS_SYMLINKAT               = 268,
    SYS_READLINKAT              = 269,
    SYS_FCHMODAT                = 270,
    SYS_FACCESSAT               = 271,
    SYS_PSELECT6                = 272,
    SYS_PPOLL                   = 273,
    SYS_UNSHARE                 = 274,
    SYS_SET_ROBUST_LIST         = 275,
    SYS_GET_ROBUST_LIST         = 276,
    SYS_SPLICE                  = 277,
    SYS_TEE                     = 278,
    SYS_SYNC_FILE_RANGE         = 279,
    SYS_VMSPLICE                = 280,
    SYS_MOVE_PAGES              = 281,
    SYS_UTIMENSAT               = 282,
    SYS_EPOLL_PWAIT             = 283,
    SYS_SIGNALFD                = 284,
    SYS_TIMERFD_CREATE          = 285,
    SYS_EVENTFD                 = 286,
    SYS_FALLOCATE               = 287,
    SYS_TIMERFD_SETTIME         = 288,
    SYS_TIMERFD_GETTIME         = 289,
    SYS_ACCEPT4                 = 290,
    SYS_SIGNALFD4               = 291,
    SYS_EVENTFD2                = 292,
    SYS_EPOLL_CREATE1           = 293,
    SYS_DUP3                    = 294,
    SYS_PIPE2                   = 295,
    SYS_INOTIFY_INIT1           = 296,
    SYS_PREADV                  = 297,
    SYS_PWRITEV                 = 298,
    SYS_RT_TGSIGQUEUEINFO       = 299,
    SYS_PERF_EVENT_OPEN         = 300,
    SYS_RECVMMSG                = 301,
    SYS_FANOTIFY_INIT           = 302,
    SYS_FANOTIFY_MARK           = 303,
    SYS_PRLIMIT64               = 304,
    SYS_NAME_TO_HANDLE_AT       = 305,
    SYS_OPEN_BY_HANDLE_AT       = 306,
    SYS_CLOCK_ADJTIME           = 307,
    SYS_SYNCFS                  = 308,
    SYS_SENDMMSG                = 309,
    SYS_SETNS                   = 310,
    SYS_GETCPU                  = 311,
    SYS_PROCESS_VM_READV        = 312,
    SYS_PROCESS_VM_WRITEV       = 313,
    SYS_KCMP                    = 314,
    SYS_FINIT_MODULE            = 315,
    SYS_SCHED_SETATTR           = 316,
    SYS_SCHED_GETATTR           = 317,
    SYS_RENAMEAT2               = 318,
    SYS_SECCOMP                 = 319,
    SYS_GETRANDOM               = 320,
    SYS_MEMFD_CREATE            = 321,
    SYS_KEXEC_FILE_LOAD         = 322,
    SYS_BPF                     = 323,
    SYS_EXECVEAT                = 324,
    SYS_USERFAULTFD             = 325,
    SYS_MEMBARRIER              = 326,
    SYS_MLOCK2                  = 327,
    SYS_COPY_FILE_RANGE         = 328,
    SYS_PREADV2                 = 329,
    SYS_PWRITEV2                = 330,
    SYS_PKEY_MPROTECT           = 331,
    SYS_PKEY_ALLOC              = 332,
    SYS_PKEY_FREE               = 333,
    SYS_STATX                   = 334,
    SYS_IO_PGETEVENTS           = 335,
    SYS_RSEQ                    = 336,
    SYS_PIDFD_SEND_SIGNAL       = 426,
    SYS_IO_URING_SETUP          = 427,
    SYS_IO_URING_ENTER          = 428,
    SYS_IO_URING_REGISTER       = 429,
    SYS_OPEN_TREE               = 430,
    SYS_MOVE_MOUNT              = 431,
    SYS_FSOPEN                  = 432,
    SYS_FSCONFIG                = 433,
    SYS_FSMOUNT                 = 434,
    SYS_FSPICK                  = 435,
    SYS_PIDFD_OPEN              = 436,
    SYS_CLONE3                  = 437,
    SYS_CLOSE_RANGE             = 438,
    SYS_OPENAT2                 = 439,
    SYS_PIDFD_GETFD             = 440,
    SYS_FACCESSAT2              = 441,
    SYS_PROCESS_MADVISE         = 442,
    SYS_EPOLL_PWAIT2            = 443,
    SYS_MOUNT_SETATTR           = 444,
    SYS_QUOTACTL_FD             = 445,
    SYS_LANDLOCK_CREATE_RULESET = 446,
    SYS_LANDLOCK_ADD_RULE       = 447,
    SYS_LANDLOCK_RESTRICT_SELF  = 448,
    SYS_MEMFD_SECRET            = 449,
    SYS_PROCESS_MRELEASE        = 450,
    SYS_FUTEX_WAITV             = 451,
    SYS_SET_MEMPOLICY_HOME_NODE = 452,
    SYS_CACHESTAT               = 453,
    SYS_FCHMODAT2               = 454,
    SYS_MAP_SHADOW_STACK        = 455,
    SYS_FUTEX_WAKE              = 456,
    SYS_FUTEX_WAIT              = 457,
    SYS_FUTEX_REQUEUE           = 458,
    SYS_STATMOUNT               = 459,
    SYS_LISTMOUNT               = 460,
    SYS_LSM_GET_SELF_ATTR       = 461,
    SYS_LSM_SET_SELF_ATTR       = 462,
    SYS_LSM_LIST_MODULES        = 463,
    SYS_MSEAL                   = 464,
    SYS_SETXATTRAT              = 465,
    SYS_GETXATTRAT              = 466,
    SYS_LISTXATTRAT             = 467,
    SYS_REMOVEXATTRAT           = 468,
    SYS_OPEN_TREE_ATTR          = 469,
    SYS_NETINFO                 = 470,
    SYS_MMAP_FB                 = 471,
    SYS_MUNMAP_FB               = 472,
    SYS_GET_FB_INFO             = 473,
    SYS_POWER                   = 474,
    SYS_USERCTL                 = 475,
    SYS_START_FB_WATCHER        = 476,
    SYS_STOP_FB_WATCHER         = 477,
    SYS_FB_FLUSH                = 478,

    // Input device syscalls (for direct input event reading)
    SYS_READ_KBD_EVENT          = 479,
    SYS_READ_MOUSE_EVENT        = 480,
    SYS_POLL_INPUT              = 481,

    // Sound/Audio syscalls (Phloem API)
    SYS_SOUND_PLAY              = 482,  // Play PCM audio data
    SYS_SOUND_STOP              = 483,  // Stop current playback
    SYS_SOUND_BEEP              = 484,  // Generate beep tone
    SYS_SOUND_SET_VOLUME        = 485,  // Set master volume (0-255)
    SYS_SOUND_GET_VOLUME        = 486,  // Get current volume
    SYS_SOUND_GET_INFO          = 487,  // Get sound device info
    SYS_SOUND_GET_CAPS          = 488,  // Get device capabilities
    SYS_SOUND_PLAY_WAV          = 489,  // Queue WAV playback by path (non-blocking)
    SYS_SPAWN_TASK              = 490,  // Spawn ELF from VFS path as a new task
    SYS_DLOPEN                  = 491,  // Dynamic loader open handle
    SYS_DLSYM                   = 492,  // Dynamic loader symbol lookup
    SYS_DLCLOSE                 = 493,  // Dynamic loader close handle

    SYS_FB_LOCK                 = 494,
    SYS_FB_UNLOCK               = 495,
    SYS_FB_DIRTY_RECT           = 496,
    SYS_FB_GET_REGIONS          = 497,

    SYS_IPC_SHM_CREATE          = 498,
    SYS_IPC_SHM_OPEN            = 499,
    SYS_IPC_SHM_CLOSE           = 500,
    SYS_IPC_SHM_DESTROY         = 501,

    SYS_IPC_MSG_CREATE          = 502,
    SYS_IPC_MSG_OPEN            = 503,
    SYS_IPC_MSG_SEND            = 504,
    SYS_IPC_MSG_RECEIVE         = 505,
    SYS_IPC_MSG_DESTROY         = 506,

    SYS_SET_PGRP                = 507,
    SYS_GET_PGRP_EXT            = 508,
    SYS_SET_SESSION             = 509,
    SYS_GET_SESSION_EXT         = 510,

    SYS_SOUND_STREAM_CREATE     = 511,
    SYS_SOUND_STREAM_WRITE      = 512,
    SYS_SOUND_STREAM_PLAY       = 513,
    SYS_SOUND_STREAM_PAUSE      = 514,
    SYS_SOUND_STREAM_STOP       = 515,
    SYS_SOUND_STREAM_DESTROY    = 516,

    /* Fern virtual-terminal management (chvt / vt info). */
    SYS_CHVT                    = 518,  /* switch to VT number (arg1) */
    SYS_GETVT                   = 519,  /* get current VT number */
    SYS_GET_VT_COUNT            = 520,  /* get total VT count */
    SYS_DM_REGISTER        = 521,  /* Display manager: register and claim framebuffer */
    SYS_DM_UNREGISTER      = 522,  /* Display manager: release framebuffer control */
    SYS_DM_AUTH_REPORT     = 523,  /* Display manager: report successful authentication */
    SYS_DM_GET_SESSION     = 524,  /* Display manager: query current session info */

    // SYS_TCGETPGRP=525, SYS_TCSETPGRP=526, SYS_SET_FOREGROUND_TASK=527
    // live up here (see their own comments above) to stay clear of the
    // real i386 syscall number range.

    /* Real task-list enumeration for userspace `ps` (task_get_all() walks
     * the real ready_queue_head under task_scheduler_lock). Signature:
     * (task_info_t* user_buf, uint32 max_entries) -> number of entries
     * written, or -errno. */
    SYS_GET_TASKS                = 528,

    /* Runtime display-mode switching (graphics_manager_v2 gfx_set_mode()
     * hot-swap path). Only actually changes hardware resolution on a
     * BGA/VBE-Extensions-capable device (QEMU/Bochs/VirtualBox std-vga);
     * on bare VESA-only hardware it returns -ENOTSUP since real post-boot
     * VESA mode-setting would require a vm86/real-mode BIOS trampoline
     * this kernel does not have. */
    SYS_SET_FB_MODE              = 529,  /* (width, height, bpp) -> 0 or -errno */
    SYS_GET_FB_MODES              = 530,  /* (video_mode_t* out, uint32_t* count) enumerate settable modes */

    /* Poll-able counter bumped every time SYS_SET_FB_MODE successfully
     * changes hardware resolution/bpp. Userspace graphics clients (see
     * libs/leafgfx) compare this against their last-seen value once per
     * frame to detect that their mmap'd framebuffer (SYS_MMAP_FB) went
     * stale and re-map before touching it again -- there is no signal/IPC
     * push mechanism in this kernel, so this is a deliberately cheap
     * value to poll from an already-running per-frame loop. () -> uint32_t,
     * never fails. */
    SYS_GET_FB_GENERATION         = 531,

    SYS_MAX                     = 532
};

// Time structures (for nanosleep, gettimeofday, etc.)
// These definitions may conflict with libc headers, so we guard them carefully
#ifndef TIME_STRUCTURES_DEFINED
#define TIME_STRUCTURES_DEFINED
struct timeval {
    uint32 tv_sec;   // seconds
    uint32 tv_usec;  // microseconds
};

struct timespec {
    uint32 tv_sec;   // seconds
    uint32 tv_nsec;  // nanoseconds
};
#endif

// System call argument type (based on architecture)
#if ARCH_64BIT
typedef uint64 sys_arg_t;
#else
typedef uint32 sys_arg_t;
#endif

#ifndef USERSPACE_BUILD
typedef struct {
#if ARCH_64BIT
    /* Register snapshot for 64-bit int 0x80 ABI:
     * rax = syscall number / return value
     * rbx, rcx, rdx, rsi, rdi, rbp = arguments 1..6
     * rsp captured for completeness/debugging
     */
    uint64 rdi;
    uint64 rsi;
    uint64 rbp;
    uint64 rsp;
    uint64 rbx;
    uint64 rdx;
    uint64 rcx;
    uint64 rax;
#else
    // pusha pushes EAX first, EDI last. On stack (low to high address):
    // EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX
    // Struct fields must match stack layout (ESP points to EDI after pusha)
    uint32 edi;  // Argument 5 (at lowest address, top of stack)
    uint32 esi;  // Argument 4
    uint32 ebp;  // Argument 6
    uint32 esp;  // Original ESP (saved by pusha, not used)
    uint32 ebx;  // Argument 1
    uint32 edx;  // Argument 3
    uint32 ecx;  // Argument 2
    uint32 eax;  // System call number and return value (at highest address)
#endif
} syscall_frame_t;

// Forward declaration for framebuffer info
// typedef struct fb_info fb_info_t; // Already defined in framebuffer.h

// Framebuffer and power syscalls (Fern extensions)
extern long sys_mmap_fb(void);
extern long sys_munmap_fb(void* addr);
extern long sys_get_fb_info(fb_info_t* user_info);
extern int32 sys_power(int32 action);
extern int32 sys_user(sys_arg_t arg1, sys_arg_t arg2, sys_arg_t arg3, sys_arg_t arg4, sys_arg_t arg5, sys_arg_t arg6);

void syscall_init(void);
void syscall_handle(syscall_frame_t* frame);

// Query the process-wide sigaction disposition set by sys_rt_sigaction().
// Returns true if signum's handler is currently SIG_IGN. Used by the
// signal-delivery layer (task.c) to decide whether to actually generate a
// signal (e.g. SIGTTIN/SIGTTOU gating in task_send_signal_to_pgrp_checked()).
bool signal_is_ignored(int signum);

// Clears the (machine-wide, not per-process) stdio_alias redirection table.
// Used by task.c on task exit/termination so a dead task's dup2()
// redirection doesn't keep pointing every other task's stdio at a
// since-recycled fd for the rest of the session.
void syscall_reset_stdio_redirect(void);

// Closes every open file/pipe/pty/socket/dup-alias fd slot owned by `pid`,
// via the same sys_close() path a normal close() syscall would take (so
// pipe half-close bookkeeping, pty teardown, unix-socketpair peer-close,
// and vfs_close() on the underlying node all run exactly as they would for
// a live process). Without this, a task that dies (crash/SIGKILL/orphan)
// while still holding fds leaks those slots out of the small fixed-size
// system-wide fd table forever. Called from task.c's task_destroy() and
// task_process_deferred_cleanup() -- the two places a task is actually torn
// down -- mirroring the existing net_close_all_for_task() convention.
void syscall_close_all_fds_for_task(uint32 pid);

// Detaches every SysV shm attachment owned by `pid` (decrementing the
// segment's attach_count and freeing the segment's backing buffer if it
// was already marked IPC_RMID and this was the last attachment). Does NOT
// unmap the attachment's user-space pages -- task_destroy() already frees
// every present+user page frame via vmm_destroy_page_directory() before
// this can run, so there is nothing left to unmap. Without this,
// attach_count never returns to zero once every attaching task has died,
// so a segment marked for deletion never actually frees its buffer.
void syscall_detach_all_shm_for_task(uint32 pid);
#endif

// mmap constants (available to both kernel and userspace)
#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// ============================================================================
// Linux i386 (x86 32-bit) syscall numbers for int 0x80 ABI
// These are the canonical numbers used by 32-bit ELF userspace binaries.
// The enum above uses x86_64 numbers; this section provides the i386 set.
// ============================================================================
#define I386_SYS_EXIT           1
#define I386_SYS_FORK           2
#define I386_SYS_READ           3
#define I386_SYS_WRITE          4
#define I386_SYS_OPEN           5
#define I386_SYS_CLOSE          6
#define I386_SYS_WAITPID        7
#define I386_SYS_CREAT          8
#define I386_SYS_LINK           9
#define I386_SYS_UNLINK         10
#define I386_SYS_EXECVE         11
#define I386_SYS_CHDIR          12
#define I386_SYS_TIME           13
#define I386_SYS_MKNOD          14
#define I386_SYS_CHMOD          15
#define I386_SYS_LCHOWN         16
#define I386_SYS_STAT           18
#define I386_SYS_LSEEK          19
#define I386_SYS_GETPID         20
#define I386_SYS_MOUNT          21
#define I386_SYS_UMOUNT         22
#define I386_SYS_SETUID         23
#define I386_SYS_GETUID         24
#define I386_SYS_STIME          25
#define I386_SYS_PTRACE         26
#define I386_SYS_ALARM          27
#define I386_SYS_FSTAT          28
#define I386_SYS_PAUSE          29
#define I386_SYS_UTIME          30
#define I386_SYS_ACCESS         33
#define I386_SYS_NICE           34
#define I386_SYS_SYNC           36
#define I386_SYS_KILL           37
#define I386_SYS_RENAME         38
#define I386_SYS_MKDIR          39
#define I386_SYS_RMDIR          40
#define I386_SYS_DUP            41
#define I386_SYS_PIPE           42
#define I386_SYS_TIMES          43
#define I386_SYS_BRK            45
#define I386_SYS_SETGID         46
#define I386_SYS_GETGID         47
#define I386_SYS_SIGNAL         48
#define I386_SYS_GETEUID        49
#define I386_SYS_GETEGID        50
#define I386_SYS_ACCT           51
#define I386_SYS_UMOUNT2        52
#define I386_SYS_IOCTL          54
#define I386_SYS_FCNTL          55
#define I386_SYS_SETPGID        57
#define I386_SYS_UMASK          60
#define I386_SYS_CHROOT         61
#define I386_SYS_USTAT          62
#define I386_SYS_DUP2           63
#define I386_SYS_GETPPID        64
#define I386_SYS_GETPGRP        65
#define I386_SYS_SETSID         66
#define I386_SYS_SIGACTION      67
#define I386_SYS_SGETMASK       68
#define I386_SYS_SSETMASK       69
#define I386_SYS_SETREUID       70
#define I386_SYS_SETREGID       71
#define I386_SYS_SIGSUSPEND     72
#define I386_SYS_SIGPENDING     73
#define I386_SYS_SETHOSTNAME    74
#define I386_SYS_SETRLIMIT      75
#define I386_SYS_GETRLIMIT      76
#define I386_SYS_GETRUSAGE      77
#define I386_SYS_GETTIMEOFDAY   78
#define I386_SYS_SETTIMEOFDAY   79
#define I386_SYS_GETGROUPS      80
#define I386_SYS_SETGROUPS      81
#define I386_SYS_SELECT         82
#define I386_SYS_SYMLINK        83
#define I386_SYS_LSTAT          84
#define I386_SYS_READLINK       85
#define I386_SYS_USELIB         86
#define I386_SYS_SWAPON         87
#define I386_SYS_REBOOT         88
#define I386_SYS_READDIR        89
#define I386_SYS_MMAP           90
#define I386_SYS_MUNMAP         91
#define I386_SYS_TRUNCATE       92
#define I386_SYS_FTRUNCATE      93
#define I386_SYS_FCHMOD         94
#define I386_SYS_FCHOWN         95
#define I386_SYS_GETPRIORITY    96
#define I386_SYS_SETPRIORITY    97
#define I386_SYS_STATFS         99
#define I386_SYS_FSTATFS        100
#define I386_SYS_SOCKETCALL     102
#define I386_SYS_SYSLOG         103
#define I386_SYS_SETITIMER      104
#define I386_SYS_GETITIMER      105
#define I386_SYS_STAT_OLD       106  /* old stat() - struct stat86 */
#define I386_SYS_LSTAT_OLD      107
#define I386_SYS_FSTAT_OLD      108
#define I386_SYS_UNAME          109
#define I386_SYS_IOPL           110
#define I386_SYS_VHANGUP        111
#define I386_SYS_WAIT4          114
#define I386_SYS_SWAPOFF        115
#define I386_SYS_SYSINFO        116
#define I386_SYS_IPC            117
#define I386_SYS_FSYNC          118
#define I386_SYS_SIGRETURN      119
#define I386_SYS_CLONE          120
#define I386_SYS_SETDOMAINNAME  121
#define I386_SYS_UGETRLIMIT     122
#define I386_SYS_MMAP2          192  /* mmap with page-unit offset */
#define I386_SYS_TRUNCATE64     193
#define I386_SYS_FTRUNCATE64    194
#define I386_SYS_STAT64         195
#define I386_SYS_LSTAT64        196
#define I386_SYS_FSTAT64        197
#define I386_SYS_CHOWN32        198
#define I386_SYS_GETUID32       199
#define I386_SYS_GETGID32       200
#define I386_SYS_GETEUID32      201
#define I386_SYS_GETEGID32      202
#define I386_SYS_SETREUID32     203
#define I386_SYS_SETREGID32     204
#define I386_SYS_GETGROUPS32    205
#define I386_SYS_SETGROUPS32    206
#define I386_SYS_FCHOWN32       207
#define I386_SYS_SETRESUID32    208
#define I386_SYS_GETRESUID32    209
#define I386_SYS_SETRESGID32    210
#define I386_SYS_GETRESGID32    211
#define I386_SYS_LCHOWN32       212
#define I386_SYS_SETUID32       213
#define I386_SYS_SETGID32       214
#define I386_SYS_SETFSUID32     215
#define I386_SYS_SETFSGID32     216
#define I386_SYS_PIVOT_ROOT     217
#define I386_SYS_MINCORE        218
#define I386_SYS_MADVISE        219
#define I386_SYS_GETDENTS64     220
#define I386_SYS_FCNTL64        221
#define I386_SYS_GETTID         224
#define I386_SYS_READAHEAD      225
#define I386_SYS_SETXATTR       226
#define I386_SYS_LSETXATTR      227
#define I386_SYS_FSETXATTR      228
#define I386_SYS_GETXATTR       229
#define I386_SYS_LGETXATTR      230
#define I386_SYS_FGETXATTR      231
#define I386_SYS_LISTXATTR      232
#define I386_SYS_LLISTXATTR     233
#define I386_SYS_FLISTXATTR     234
#define I386_SYS_REMOVEXATTR    235
#define I386_SYS_LREMOVEXATTR   236
#define I386_SYS_FREMOVEXATTR   237
#define I386_SYS_TKILL          238
#define I386_SYS_SENDFILE64     239
#define I386_SYS_FUTEX          240
#define I386_SYS_SCHED_SETAFFINITY  241
#define I386_SYS_SCHED_GETAFFINITY  242
#define I386_SYS_SET_THREAD_AREA    243
#define I386_SYS_GET_THREAD_AREA    244
#define I386_SYS_IO_SETUP       245
#define I386_SYS_IO_DESTROY     246
#define I386_SYS_IO_GETEVENTS   247
#define I386_SYS_IO_SUBMIT      248
#define I386_SYS_IO_CANCEL      249
#define I386_SYS_FADVISE64      250
#define I386_SYS_EXIT_GROUP     252
#define I386_SYS_LOOKUP_DCOOKIE 253
#define I386_SYS_EPOLL_CREATE   254
#define I386_SYS_EPOLL_CTL      255
#define I386_SYS_EPOLL_WAIT     256
#define I386_SYS_REMAP_FILE_PAGES 257
#define I386_SYS_SET_TID_ADDRESS 258
#define I386_SYS_TIMER_CREATE   259
#define I386_SYS_TIMER_SETTIME  260
#define I386_SYS_TIMER_GETTIME  261
#define I386_SYS_TIMER_GETOVERRUN 262
#define I386_SYS_TIMER_DELETE   263
#define I386_SYS_CLOCK_SETTIME  264
#define I386_SYS_CLOCK_GETTIME  265
#define I386_SYS_CLOCK_GETRES   266
#define I386_SYS_CLOCK_NANOSLEEP 267
#define I386_SYS_STATFS64       268
#define I386_SYS_FSTATFS64      269
#define I386_SYS_TGKILL         270
#define I386_SYS_UTIMES         271
#define I386_SYS_FADVISE64_64   272
#define I386_SYS_MBIND          274
#define I386_SYS_GET_MEMPOLICY  275
#define I386_SYS_SET_MEMPOLICY  276
#define I386_SYS_MQ_OPEN        277
#define I386_SYS_MQ_UNLINK      278
#define I386_SYS_MQ_TIMEDSEND   279
#define I386_SYS_MQ_TIMEDRECEIVE 280
#define I386_SYS_MQ_NOTIFY      281
#define I386_SYS_MQ_GETSETATTR  282
#define I386_SYS_KEXEC_LOAD     283
#define I386_SYS_WAITID         284
#define I386_SYS_ADD_KEY        286
#define I386_SYS_REQUEST_KEY    287
#define I386_SYS_KEYCTL         288
#define I386_SYS_IOPRIO_SET     289
#define I386_SYS_IOPRIO_GET     290
#define I386_SYS_INOTIFY_INIT   291
#define I386_SYS_INOTIFY_ADD_WATCH 292
#define I386_SYS_INOTIFY_RM_WATCH  293
#define I386_SYS_MIGRATE_PAGES  294
#define I386_SYS_OPENAT         295
#define I386_SYS_MKDIRAT        296
#define I386_SYS_MKNODAT        297
#define I386_SYS_FCHOWNAT       298
#define I386_SYS_FUTIMESAT      299
#define I386_SYS_FSTATAT64      300
#define I386_SYS_UNLINKAT       301
#define I386_SYS_RENAMEAT       302
#define I386_SYS_LINKAT         303
#define I386_SYS_SYMLINKAT      304
#define I386_SYS_READLINKAT     305
#define I386_SYS_FCHMODAT       306
#define I386_SYS_FACCESSAT      307
#define I386_SYS_PSELECT6       308
#define I386_SYS_PPOLL          309
#define I386_SYS_UNSHARE        310
#define I386_SYS_SET_ROBUST_LIST 311
#define I386_SYS_GET_ROBUST_LIST 312
#define I386_SYS_SPLICE         313
#define I386_SYS_SYNC_FILE_RANGE 314
#define I386_SYS_TEE            315
#define I386_SYS_VMSPLICE       316
#define I386_SYS_MOVE_PAGES     317
#define I386_SYS_GETCPU         318
#define I386_SYS_EPOLL_PWAIT    319
#define I386_SYS_UTIMENSAT      320
#define I386_SYS_SIGNALFD       321
#define I386_SYS_TIMERFD_CREATE 322
#define I386_SYS_EVENTFD        323
#define I386_SYS_FALLOCATE      324
#define I386_SYS_TIMERFD_SETTIME 325
#define I386_SYS_TIMERFD_GETTIME 326
#define I386_SYS_SIGNALFD4      327
#define I386_SYS_EVENTFD2       328
#define I386_SYS_EPOLL_CREATE1  329
#define I386_SYS_DUP3           330
#define I386_SYS_PIPE2          331
#define I386_SYS_INOTIFY_INIT1  332
#define I386_SYS_PREADV         333
#define I386_SYS_PWRITEV        334
#define I386_SYS_RT_TGSIGQUEUEINFO 335
#define I386_SYS_PERF_EVENT_OPEN 336
#define I386_SYS_RECVMMSG       337
#define I386_SYS_FANOTIFY_INIT  338
#define I386_SYS_FANOTIFY_MARK  339
#define I386_SYS_PRLIMIT64      340
#define I386_SYS_NAME_TO_HANDLE_AT 341
#define I386_SYS_OPEN_BY_HANDLE_AT 342
#define I386_SYS_CLOCK_ADJTIME  343
#define I386_SYS_SYNCFS         344
#define I386_SYS_SENDMMSG       345
#define I386_SYS_SETNS          346
#define I386_SYS_PROCESS_VM_READV 347
#define I386_SYS_PROCESS_VM_WRITEV 348
#define I386_SYS_KCMP           349
#define I386_SYS_FINIT_MODULE   350
#define I386_SYS_SCHED_SETATTR  351
#define I386_SYS_SCHED_GETATTR  352
#define I386_SYS_RENAMEAT2      353
#define I386_SYS_SECCOMP        354
#define I386_SYS_GETRANDOM      355
#define I386_SYS_MEMFD_CREATE   356
#define I386_SYS_BPF            357
#define I386_SYS_EXECVEAT       358
#define I386_SYS_SOCKET         359
#define I386_SYS_SOCKETPAIR     360
#define I386_SYS_BIND           361
#define I386_SYS_CONNECT        362
#define I386_SYS_LISTEN         363
#define I386_SYS_ACCEPT4        364
#define I386_SYS_GETSOCKOPT     365
#define I386_SYS_SETSOCKOPT     366
#define I386_SYS_GETSOCKNAME    367
#define I386_SYS_GETPEERNAME    368
#define I386_SYS_SENDTO         369
#define I386_SYS_SENDMSG        370
#define I386_SYS_RECVFROM       371
#define I386_SYS_RECVMSG        372
#define I386_SYS_SHUTDOWN       373
#define I386_SYS_USERFAULTFD    374
#define I386_SYS_MEMBARRIER     375
#define I386_SYS_MLOCK2         376
#define I386_SYS_COPY_FILE_RANGE 377
#define I386_SYS_PREADV2        378
#define I386_SYS_PWRITEV2       379
#define I386_SYS_PKEY_MPROTECT  380
#define I386_SYS_PKEY_ALLOC     381
#define I386_SYS_PKEY_FREE      382
#define I386_SYS_STATX          383
#define I386_SYS_ARCH_PRCTL     384
#define I386_SYS_IO_PGETEVENTS  385
#define I386_SYS_RSEQ           386
#define I386_SYS_SEMGET         393
#define I386_SYS_SEMCTL         394
#define I386_SYS_SHMGET         395
#define I386_SYS_SHMCTL         396
#define I386_SYS_SHMAT          397
#define I386_SYS_SHMDT          398
#define I386_SYS_MSGGET         399
#define I386_SYS_MSGSND         400
#define I386_SYS_MSGRCV         401
#define I386_SYS_MSGCTL         402
/* Note: On i386 Linux, accept() goes through socketcall(SYS_ACCEPT=5,...).
 * There is no standalone accept syscall (403 = clock_gettime64 in newer ABI).
 * I386_SYS_ACCEPT4 (364) is the standalone accept4 in the modern i386 ABI.
 */
#define I386_SYS_CLOCK_GETTIME64        403
#define I386_SYS_RECVMMSG_TIME64        417

/* socketcall() sub-call numbers (i386 Linux ABI) */
#define I386_SOCKETCALL_SOCKET      1
#define I386_SOCKETCALL_BIND        2
#define I386_SOCKETCALL_CONNECT     3
#define I386_SOCKETCALL_LISTEN      4
#define I386_SOCKETCALL_ACCEPT      5
#define I386_SOCKETCALL_GETSOCKNAME 6
#define I386_SOCKETCALL_GETPEERNAME 7
#define I386_SOCKETCALL_SOCKETPAIR  8
#define I386_SOCKETCALL_SEND        9
#define I386_SOCKETCALL_RECV        10
#define I386_SOCKETCALL_SENDTO      11
#define I386_SOCKETCALL_RECVFROM    12
#define I386_SOCKETCALL_SHUTDOWN    13
#define I386_SOCKETCALL_SETSOCKOPT  14
#define I386_SOCKETCALL_GETSOCKOPT  15
#define I386_SOCKETCALL_SENDMSG     16
#define I386_SOCKETCALL_RECVMSG     17
#define I386_SOCKETCALL_ACCEPT4     18
#define I386_SOCKETCALL_RECVMMSG    19
#define I386_SOCKETCALL_SENDMMSG    20

/* i386 sigaction / sigreturn related */
#define I386_SYS_RT_SIGACTION       174
#define I386_SYS_RT_SIGPROCMASK     175
#define I386_SYS_RT_SIGRETURN       173
#define I386_SYS_RT_SIGPENDING      176
#define I386_SYS_RT_SIGTIMEDWAIT    177
#define I386_SYS_RT_SIGQUEUEINFO    178
#define I386_SYS_RT_SIGSUSPEND      179
#define I386_SYS_SIGALTSTACK        186
#define I386_SYS_PRCTL              172
#define I386_SYS_GETDENTS           141
#define I386_SYS_SCHED_SETPARAM     154
#define I386_SYS_SCHED_GETPARAM     155
#define I386_SYS_SCHED_SETSCHEDULER 156
#define I386_SYS_SCHED_GETSCHEDULER 157
#define I386_SYS_SCHED_YIELD        158
#define I386_SYS_SCHED_GET_PRIORITY_MAX 159
#define I386_SYS_SCHED_GET_PRIORITY_MIN 160
#define I386_SYS_SCHED_RR_GET_INTERVAL 161
#define I386_SYS_NANOSLEEP          162
#define I386_SYS_MREMAP             163
#define I386_SYS_SETRESUID          164
#define I386_SYS_GETRESUID          165
#define I386_SYS_SETRESGID          170
#define I386_SYS_GETRESGID          171
#define I386_SYS_POLL               168
#define I386_SYS_SETFSUID           138
#define I386_SYS_SETFSGID           139
#define I386_SYS_GETDENTS_OLD       141
#define I386_SYS_FLOCK              143
#define I386_SYS_MSYNC              144
#define I386_SYS_READV              145
#define I386_SYS_WRITEV             146
#define I386_SYS_GETSID             147
#define I386_SYS_FDATASYNC          148
#define I386_SYS_SYSCTL             149
#define I386_SYS_MLOCK              150
#define I386_SYS_MUNLOCK            151
#define I386_SYS_MLOCKALL           152
#define I386_SYS_MUNLOCKALL         153
#define I386_SYS_PREAD64            180
#define I386_SYS_PWRITE64           181
#define I386_SYS_CHOWN              182
#define I386_SYS_GETCWD             183
#define I386_SYS_CAPGET             184
#define I386_SYS_CAPSET             185
#define I386_SYS_SENDFILE           187
#define I386_SYS_GETPMSG            188
#define I386_SYS_PUTPMSG            189
#define I386_SYS_VFORK              190
#define I386_SYS_MPROTECT           125
#define I386_SYS_SIGPROCMASK        126
#define I386_SYS_CREATE_MODULE      127
#define I386_SYS_INIT_MODULE        128
#define I386_SYS_DELETE_MODULE      129
#define I386_SYS_GET_KERNEL_SYMS    130
#define I386_SYS_QUOTACTL           131
#define I386_SYS_GETPGID            132
#define I386_SYS_FCHDIR             133
#define I386_SYS_BDFLUSH            134
#define I386_SYS_SYSFS              135
#define I386_SYS_PERSONALITY        136
#define I386_SYS_SETFSUID_OLD       138
#define I386_SYS_SETFSGID_OLD       139
#define I386_SYS_LLSEEK             140
#define I386_SYS_GETDENTS_141       141
#define I386_SYS_NEWSELECT          142
#define I386_SYS_SEMOP              166  /* semop() i386 syscall number */

#endif
