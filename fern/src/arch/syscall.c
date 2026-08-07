/*
 * syscall.c - Cross-Architecture Syscall Translation and Dispatch
 *
 * Provides translation tables that map architecture-specific Linux syscall
 * numbers to the internal Fern canonical numbers (x86_64 Linux ABI).
 *
 * Design:
 *   - Translation tables are static const arrays indexed by arch-specific
 *     syscall number. Entries map to internal Fern numbers.
 *   - Unmapped entries use FERN_NR_INVALID (-1).
 *   - ARM32 Forest-private syscalls (0xF000+) are handled separately.
 *   - syscall_dispatch_arch() provides a convenience wrapper that translates
 *     and dispatches in one call.
 *
 * Integration:
 *   - x86_64: No translation needed (identity mapping)
 *   - ARM32: Use syscall_translate() before dispatch
 *   - AArch64: Use syscall_translate() before dispatch
 *   - RISC-V: Use syscall_translate() before dispatch
 */

#include "syscall.h"
#include <stdint.h>

/* =========================================================================
 * Forward declarations of platform-independent syscall handlers.
 * These are implemented in src/syscall.c (x86/generic) and called
 * from the translated dispatch.
 * ========================================================================= */

/* Process */
extern void sys_exit(int status);
extern long sys_fork(void);
extern long sys_execve(const char *path, char *const argv[], char *const envp[]);
extern long sys_wait4(int pid, int *status, int options, void *rusage);
extern long sys_getpid(void);
extern long sys_getppid(void);
extern long sys_getuid(void);
extern long sys_getgid(void);
extern long sys_geteuid(void);
extern long sys_getegid(void);
extern long sys_kill(int pid, int sig);
extern void sys_exit_group(int status);
extern long sys_clone(unsigned long flags, void *stack, int *parent_tid,
                      int *child_tid, unsigned long tls);
extern long sys_set_tid_address(int *tidptr);
extern long sys_sched_yield(void);

/* File I/O */
extern long sys_read(int fd, void *buf, size_t count);
extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_open(const char *path, int flags, int mode);
extern long sys_close(int fd);
extern long sys_lseek(int fd, long offset, int whence);
extern long sys_fstat(int fd, void *statbuf);
extern long sys_stat(const char *pathname, void *statbuf);
extern long sys_getcwd(char *buf, size_t size);
extern long sys_chdir(const char *path);
extern long sys_fchdir(int fd);
extern long sys_dup(int oldfd);
extern long sys_dup3(int oldfd, int newfd, int flags);
extern long sys_fcntl(int fd, int cmd, int arg);
extern long sys_ioctl(int fd, unsigned long request, void *arg);
extern long sys_getdents64(int fd, void *dirp, size_t count);
extern long sys_pipe(int pipefd[2]);
extern long sys_readv(int fd, const void *iov, int iovcnt);
extern long sys_writev(int fd, const void *iov, int iovcnt);
extern long sys_pread64(int fd, void *buf, size_t count, long offset);
extern long sys_pwrite64(int fd, const void *buf, size_t count, long offset);
extern long sys_access(const char *pathname, int mode);
extern long sys_faccessat(int dirfd, const char *pathname, int mode, int flags);
extern long sys_unlink(const char *pathname);
extern long sys_mkdir(const char *pathname, int mode);
extern long sys_rmdir(const char *pathname);
extern long sys_rename(const char *oldpath, const char *newpath);
extern long sys_link(const char *oldpath, const char *newpath);
extern long sys_symlink(const char *target, const char *linkpath);
extern long sys_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
extern long sys_chmod(const char *pathname, int mode);
extern long sys_fchmod(int fd, int mode);
extern long sys_chown(const char *pathname, int owner, int group);
extern long sys_fchown(int fd, int owner, int group);
extern long sys_umask(int mode);
extern long sys_fsync(int fd);
extern long sys_ftruncate(int fd, long length);
extern long sys_statfs(const char *path, void *buf);
extern long sys_fstatfs(int fd, void *buf);
extern long sys_unlinkat(int dirfd, const char *pathname, int flags);
extern long sys_mkdirat(int dirfd, const char *pathname, int mode);
extern long sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
extern long sys_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
extern long sys_fchownat(int dirfd, const char *pathname, int owner, int group, int flags);
extern long sys_fchmodat(int dirfd, const char *pathname, int mode, int flags);
extern long sys_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
extern long sys_mknod(const char *pathname, int mode, int dev);
extern long sys_utimensat(int dirfd, const char *pathname, const void *times, int flags);
extern long sys_close_range(unsigned int first, unsigned int last, unsigned int flags);
extern long sys_copy_file_range(int fd_in, long *off_in, int fd_out, long *off_out, size_t len, unsigned int flags);
extern long sys_ppoll(void *fds, unsigned int nfds, const void *timeout, const void *sigmask, size_t sigsetsize);
extern long sys_fadvise64(int fd, long offset, long len, int advice);
extern long sys_memfd_create(const char *name, unsigned int flags);
extern long sys_futex(uint32_t *uaddr, int op, int val, const void *timeout, uint32_t *uaddr2, int val3);
extern long sys_statx(int dirfd, const char *pathname, unsigned flags, unsigned mask, void *statxbuf);
extern long sys_getrandom(void *buf, size_t count, unsigned int flags);

/* Memory */
extern long sys_brk(void *addr);
extern long sys_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
extern long sys_munmap(void *addr, size_t length);
extern long sys_mprotect(void *addr, size_t length, int prot);
extern long sys_madvise(void *addr, size_t length, int advice);

/* Time */
extern long sys_clock_gettime(int clkid, void *tp);
extern long sys_clock_getres(int clkid, void *res);
extern long sys_gettimeofday(void *tv, void *tz);
extern long sys_nanosleep(const void *req, void *rem);

/* System */
extern long sys_uname(void *buf);
extern long sys_getrlimit(int resource, void *rlim);
extern long sys_setrlimit(int resource, const void *rlim);
extern long sys_getrusage(int who, void *usage);
extern long sys_sysinfo(void *info);
extern long sys_times(void *tms);
extern long sys_getpriority(int which, int who);
extern long sys_setpriority(int which, int who, int prio);

/* Networking */
extern long sys_socket(int domain, int type, int protocol);
extern long sys_bind(int fd, const void *addr, int addrlen);
extern long sys_listen(int fd, int backlog);
extern long sys_accept(int fd, void *addr, int *addrlen);
extern long sys_accept4(int fd, void *addr, int *addrlen, int flags);
extern long sys_connect(int fd, const void *addr, int addrlen);
extern long sys_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, int addrlen);
extern long sys_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, int *addrlen);
extern long sys_sendmsg(int fd, const void *msg, int flags);
extern long sys_recvmsg(int fd, void *msg, int flags);
extern long sys_shutdown(int fd, int how);
extern long sys_getsockname(int fd, void *addr, int *addrlen);
extern long sys_getpeername(int fd, void *addr, int *addrlen);
extern long sys_socketpair(int domain, int type, int protocol, int sv[2]);
extern long sys_setsockopt(int fd, int level, int optname, const void *optval, int optlen);
extern long sys_getsockopt(int fd, int level, int optname, void *optval, int *optlen);

/* I/O multiplexing */
extern long sys_epoll_create1(int flags);
extern long sys_epoll_ctl(int epfd, int op, int fd, void *event);
extern long sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout, const void *sigmask, size_t sigsetsize);

/* Signals (stubs) */
extern long sys_rt_sigaction(int signum, const void *act, void *oldact, size_t sigsetsize);
extern long sys_rt_sigprocmask(int how, const void *set, void *oldset, size_t sigsetsize);

/* Credentials */
extern long sys_setuid(int uid);
extern long sys_setgid(int gid);

/* Fern private */
extern long sys_power(int action);
extern long sys_netinfo(void *buf, int max_entries);

/* =========================================================================
 * Translation Tables
 *
 * Each table maps architecture-specific syscall number to internal
 * Fern number. Indexed by arch-specific number; FERN_NR_INVALID (-1)
 * for unmapped entries.
 *
 * Table size: 512 entries covers all common Linux syscall numbers.
 * ========================================================================= */

#define SYSCALL_TABLE_SIZE 512

/* ---- ARM32 translation table ----
 * ARM EABI syscall numbers (from uapi/asm/unistd.h).
 * Maps to internal Fern numbers (x86_64 Linux ABI).
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((unused)) static int32_t arm32_xlat[SYSCALL_TABLE_SIZE] = {
    [0 ... (SYSCALL_TABLE_SIZE - 1)] = FERN_NR_INVALID,

    /* Process lifecycle */
    [1]  = FERN_SYS_EXIT,          /* ARM32_NR_exit */
    [2]  = FERN_SYS_FORK,          /* ARM32_NR_fork */
    [7]  = FERN_SYS_WAIT4,         /* ARM32_NR_waitpid */
    [11] = FERN_SYS_EXECVE,        /* ARM32_NR_execve */
    [20] = FERN_SYS_GETPID,        /* ARM32_NR_getpid */
    [24] = FERN_SYS_GETUID,        /* ARM32_NR_getuid */
    [37] = FERN_SYS_KILL,          /* ARM32_NR_kill */
    [158] = FERN_SYS_SCHED_YIELD,  /* ARM32_NR_sched_yield */
    [248] = FERN_SYS_EXIT_GROUP,   /* ARM32_NR_exit_group */
    [256] = FERN_SYS_SET_TID_ADDRESS, /* ARM32_NR_set_tid_address */

    /* File I/O */
    [3]  = FERN_SYS_READ,          /* ARM32_NR_read */
    [4]  = FERN_SYS_WRITE,         /* ARM32_NR_write */
    [5]  = FERN_SYS_OPEN,          /* ARM32_NR_open */
    [6]  = FERN_SYS_CLOSE,         /* ARM32_NR_close */
    [19] = FERN_SYS_LSEEK,         /* ARM32_NR_lseek */
    [33] = FERN_SYS_ACCESS,        /* ARM32_NR_access */
    [42] = FERN_SYS_DUP2,          /* ARM32_NR_dup2 (old) */
    [45] = FERN_SYS_BRK,           /* ARM32_NR_brk */
    [54] = FERN_SYS_IOCTL,         /* ARM32_NR_ioctl */

    /* Memory management */
    [91] = FERN_SYS_MUNMAP,        /* ARM32_NR_munmap */
    [125] = FERN_SYS_MPROTECT,     /* ARM32_NR_mprotect */
    [192] = FERN_SYS_MMAP,         /* ARM32_NR_mmap2 */

    /* Time */
    [78] = FERN_SYS_GETTIMEOFDAY,  /* ARM32_NR_gettimeofday */
    [162] = FERN_SYS_NANOSLEEP,    /* ARM32_NR_nanosleep */

    /* Networking */
    [281] = FERN_SYS_SOCKET,       /* ARM32_NR_socket */
    [282] = FERN_SYS_BIND,         /* ARM32_NR_bind */
    [283] = FERN_SYS_CONNECT,      /* ARM32_NR_connect */

    /* I/O multiplexing */
    [142] = FERN_SYS_SELECT,       /* ARM32_NR_select */

    /* Additional commonly used syscalls */
    [41] = FERN_SYS_DUP,           /* ARM32_NR_dup */
    [55] = FERN_SYS_FCNTL,         /* ARM32_NR_fcntl */
    [106] = FERN_SYS_STAT,         /* ARM32_NR_stat */
    [108] = FERN_SYS_FSTAT,        /* ARM32_NR_fstat */
    [109] = FERN_SYS_LSTAT,        /* ARM32_NR_lstat (same as fstat) */
    [140] = FERN_SYS_GETRLIMIT,    /* ARM32_NR_getrlimit */
    [145] = FERN_SYS_READV,        /* ARM32_NR_readv */
    [146] = FERN_SYS_WRITEV,       /* ARM32_NR_writev */
    [150] = FERN_SYS_MMAP,         /* ARM32_NR_mmap (old) */
    [163] = FERN_SYS_MREMAP,       /* ARM32_NR_mremap */
    [168] = FERN_SYS_POLL,         /* ARM32_NR_poll */
    [172] = FERN_SYS_GETGROUPS,    /* ARM32_NR_getgroups */
    [173] = FERN_SYS_SETGROUPS,    /* ARM32_NR_setgroups */
    [174] = FERN_SYS_SETREUID,     /* ARM32_NR_setreuid */
    [175] = FERN_SYS_SETREGID,     /* ARM32_NR_setregid */
    [180] = FERN_SYS_PREAD64,      /* ARM32_NR_pread64 */
    [181] = FERN_SYS_PWRITE64,     /* ARM32_NR_pwrite64 */
    [182] = FERN_SYS_CHOWN,        /* ARM32_NR_chown */
    [183] = FERN_SYS_GETCWD,       /* ARM32_NR_getcwd */
    [186] = FERN_SYS_SIGALTSTACK,  /* ARM32_NR_sigaltstack (stub) */
    [187] = FERN_SYS_SENDFILE,     /* ARM32_NR_sendfile */
    [190] = FERN_SYS_VFORK,        /* ARM32_NR_vfork */
    [191] = FERN_SYS_TRUNCATE,     /* ARM32_NR_truncate */
    [193] = FERN_SYS_FTRUNCATE,    /* ARM32_NR_ftruncate */
    [194] = FERN_SYS_FCHMOD,       /* ARM32_NR_fchmod */
    [195] = FERN_SYS_FCHOWN,       /* ARM32_NR_fchown */
    [197] = FERN_SYS_FSTAT,        /* ARM32_NR_fstat64 */
    [220] = FERN_SYS_MMAP,         /* ARM32_NR_mmap2 (page-unit) */
    [221] = FERN_SYS_TRUNCATE,     /* ARM32_NR_truncate64 */
    [222] = FERN_SYS_FTRUNCATE,    /* ARM32_NR_ftruncate64 */
    [224] = FERN_SYS_GETTID,       /* ARM32_NR_gettid */
    [238] = FERN_SYS_TKILL,        /* ARM32_NR_tkill */
    [240] = FERN_SYS_FUTEX,        /* ARM32_NR_futex */
    [252] = FERN_SYS_EXIT_GROUP,   /* ARM32_NR_exit_group */
    [264] = FERN_SYS_GETDENTS64,   /* ARM32_NR_getdents64 */
    [265] = FERN_SYS_UNLINKAT,     /* ARM32_NR_unlinkat */
    [266] = FERN_SYS_RENAMEAT,     /* ARM32_NR_renameat */
    [267] = FERN_SYS_LINKAT,       /* ARM32_NR_linkat */
    [268] = FERN_SYS_SYMLINKAT,    /* ARM32_NR_symlinkat */
    [269] = FERN_SYS_READLINKAT,   /* ARM32_NR_readlinkat */
    [270] = FERN_SYS_FCHMODAT,     /* ARM32_NR_fchmodat */
    [271] = FERN_SYS_FACCESSAT,    /* ARM32_NR_faccessat */
    [274] = FERN_SYS_PPOLL,        /* ARM32_NR_ppoll */
    [284] = FERN_SYS_ACCEPT,       /* ARM32_NR_accept */
    [285] = FERN_SYS_GETSOCKNAME,  /* ARM32_NR_getsockname */
    [286] = FERN_SYS_GETPEERNAME,  /* ARM32_NR_getpeername */
    [287] = FERN_SYS_SOCKETPAIR,   /* ARM32_NR_socketpair */
    [288] = FERN_SYS_RECVFROM,     /* ARM32_NR_recvfrom */
    [289] = FERN_SYS_SENDTO,       /* ARM32_NR_sendto */
    [290] = FERN_SYS_SETSOCKOPT,   /* ARM32_NR_setsockopt */
    [291] = FERN_SYS_GETSOCKOPT,   /* ARM32_NR_getsockopt */
    [292] = FERN_SYS_SHUTDOWN,     /* ARM32_NR_shutdown */
    [293] = FERN_SYS_SENDMSG,      /* ARM32_NR_sendmsg */
    [294] = FERN_SYS_RECVMSG,      /* ARM32_NR_recvmsg */
    [295] = FERN_SYS_LISTEN,       /* ARM32_NR_listen */
    [296] = FERN_SYS_ACCEPT4,      /* ARM32_NR_accept4 */
    [300] = FERN_SYS_PRLIMIT64,    /* ARM32_NR_prlimit64 */
    [301] = FERN_SYS_FANOTIFY_INIT, /* ARM32_NR_fanotify_init (stub) */
    [312] = FERN_SYS_GETRANDOM,    /* ARM32_NR_getrandom */
    [313] = FERN_SYS_MEMFD_CREATE, /* ARM32_NR_memfd_create */
    [325] = FERN_SYS_GETDENTS64,   /* ARM32_NR_getdents64 (old) */
    [329] = FERN_SYS_EPOLL_CREATE1, /* ARM32_NR_epoll_create1 */
    [330] = FERN_SYS_DUP3,         /* ARM32_NR_dup3 */
    [331] = FERN_SYS_PIPE2,        /* ARM32_NR_pipe2 */
    [340] = FERN_SYS_SETRESUID,    /* ARM32_NR_setresuid */
    [341] = FERN_SYS_GETRESUID,    /* ARM32_NR_getresuid */
    [342] = FERN_SYS_SETRESGID,    /* ARM32_NR_setresgid */
    [343] = FERN_SYS_GETRESGID,    /* ARM32_NR_getresgid */
    [354] = FERN_SYS_SCHED_SETAFFINITY, /* ARM32_NR_sched_setaffinity */
    [355] = FERN_SYS_SCHED_GETAFFINITY, /* ARM32_NR_sched_getaffinity */
    [364] = FERN_SYS_SETUID,       /* ARM32_NR_setuid32 */
    [365] = FERN_SYS_SETGID,       /* ARM32_NR_setgid32 */
    [369] = FERN_SYS_SETFSUID,     /* ARM32_NR_setfsuid32 */
    [370] = FERN_SYS_SETFSGID,     /* ARM32_NR_setfsgid32 */
    [379] = FERN_SYS_GETPGID,      /* ARM32_NR_getpgid */
    [380] = FERN_SYS_SETSID,       /* ARM32_NR_setsid */
    [381] = FERN_SYS_GETSID,       /* ARM32_NR_getsid */
    [383] = FERN_SYS_GETPGRP,      /* ARM32_NR_getpgrp */
    [398] = FERN_SYS_FCHOWNAT,     /* ARM32_NR_fchownat */
    [399] = FERN_SYS_FUTIMESAT,    /* ARM32_NR_futimesat */
    [400] = FERN_SYS_NEWFSTATAT,   /* ARM32_NR_fstatat64 */
    [403] = FERN_SYS_UNLINKAT,     /* ARM32_NR_unlinkat (old) */
    [404] = FERN_SYS_RENAMEAT,     /* ARM32_NR_renameat (old) */
    [405] = FERN_SYS_LINKAT,       /* ARM32_NR_linkat (old) */
    [406] = FERN_SYS_SYMLINKAT,    /* ARM32_NR_symlinkat (old) */
    [407] = FERN_SYS_READLINKAT,   /* ARM32_NR_readlinkat (old) */
    [408] = FERN_SYS_FCHMODAT,     /* ARM32_NR_fchmodat (old) */
    [409] = FERN_SYS_FACCESSAT,    /* ARM32_NR_faccessat (old) */
    [418] = FERN_SYS_GETTIMEOFDAY, /* ARM32_NR_gettimeofday (old) */
    [419] = FERN_SYS_SETTIMEOFDAY, /* ARM32_NR_settimeofday (old) */
    [423] = FERN_SYS_GETCWD,       /* ARM32_NR_getcwd (old) */
    [428] = FERN_SYS_CHDIR,        /* ARM32_NR_chdir (old) */
    [429] = FERN_SYS_FCHDIR,       /* ARM32_NR_fchdir (old) */
    [430] = FERN_SYS_CHMOD,        /* ARM32_NR_chmod (old) */
    [431] = FERN_SYS_FCHMOD,       /* ARM32_NR_fchmod (old) */
    [432] = FERN_SYS_CHOWN,        /* ARM32_NR_chown (old) */
    [433] = FERN_SYS_FCHOWN,       /* ARM32_NR_fchown (old) */
    [434] = FERN_SYS_CHOWN,        /* ARM32_NR_lchown (old) */
    [435] = FERN_SYS_UMASK,        /* ARM32_NR_umask (old) */
    [436] = FERN_SYS_MKNOD,        /* ARM32_NR_mknod (old) */
    [437] = FERN_SYS_UNLINK,       /* ARM32_NR_unlink (old) */
    [438] = FERN_SYS_MKDIR,        /* ARM32_NR_mkdir (old) */
    [439] = FERN_SYS_RMDIR,        /* ARM32_NR_rmdir (old) */
    [440] = FERN_SYS_LINK,         /* ARM32_NR_link (old) */
    [441] = FERN_SYS_SYMLINK,      /* ARM32_NR_symlink (old) */
    [442] = FERN_SYS_RENAME,       /* ARM32_NR_rename (old) */
    [443] = FERN_SYS_TRUNCATE,     /* ARM32_NR_truncate (old) */
    [444] = FERN_SYS_FTRUNCATE,    /* ARM32_NR_ftruncate (old) */
    [445] = FERN_SYS_CREAT,        /* ARM32_NR_creat (old) */
    [446] = FERN_SYS_OPEN,         /* ARM32_NR_open (old) */
};
#pragma GCC diagnostic pop

/* ---- AArch64 translation table ----
 * arm64 Linux syscall numbers (asm-generic/unistd.h).
 * Maps to internal Fern numbers (x86_64 Linux ABI).
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((unused)) static int32_t aarch64_xlat[SYSCALL_TABLE_SIZE] = {
    [0 ... (SYSCALL_TABLE_SIZE - 1)] = FERN_NR_INVALID,

    /* I/O */
    [17] = FERN_SYS_GETCWD,        /* __NR_getcwd */
    [19] = FERN_SYS_POLL,          /* __NR_eventfd2 (stub, redirect to poll) */
    [20] = FERN_SYS_EPOLL_CREATE1, /* __NR_epoll_create1 */
    [21] = FERN_SYS_EPOLL_CTL,     /* __NR_epoll_ctl */
    [22] = FERN_SYS_EPOLL_PWAIT,   /* __NR_epoll_pwait */
    [23] = FERN_SYS_DUP,           /* __NR_dup */
    [24] = FERN_SYS_DUP3,          /* __NR_dup3 */
    [25] = FERN_SYS_FCNTL,         /* __NR_fcntl */
    [29] = FERN_SYS_IOCTL,         /* __NR_ioctl */
    [33] = FERN_SYS_MKNOD,         /* __NR_mknod */
    [34] = FERN_SYS_MKDIRAT,       /* __NR_mkdirat */
    [35] = FERN_SYS_UNLINKAT,      /* __NR_unlinkat */
    [36] = FERN_SYS_SYMLINKAT,     /* __NR_symlinkat */
    [37] = FERN_SYS_LINKAT,        /* __NR_linkat */
    [38] = FERN_SYS_RENAMEAT,      /* __NR_renameat */
    [43] = FERN_SYS_STATFS,        /* __NR_statfs */
    [44] = FERN_SYS_FSTATFS,       /* __NR_fstatfs */
    [47] = FERN_SYS_FTRUNCATE,     /* __NR_truncate */
    [48] = FERN_SYS_FTRUNCATE,     /* __NR_ftruncate */
    [49] = FERN_SYS_CHDIR,         /* __NR_chdir */
    [50] = FERN_SYS_FCHDIR,        /* __NR_fchdir */
    [54] = FERN_SYS_FCHOWNAT,      /* __NR_fchownat */
    [56] = FERN_SYS_OPENAT,        /* __NR_openat */
    [57] = FERN_SYS_CLOSE,         /* __NR_close */
    [59] = FERN_SYS_PIPE2,         /* __NR_pipe2 */
    [61] = FERN_SYS_GETDENTS64,    /* __NR_getdents64 */
    [62] = FERN_SYS_LSEEK,         /* __NR_lseek */
    [63] = FERN_SYS_READ,          /* __NR_read */
    [64] = FERN_SYS_WRITE,         /* __NR_write */
    [65] = FERN_SYS_READV,         /* __NR_readv */
    [66] = FERN_SYS_WRITEV,        /* __NR_writev */
    [67] = FERN_SYS_PREAD64,       /* __NR_pread64 */
    [68] = FERN_SYS_PWRITE64,      /* __NR_pwrite64 */
    [72] = FERN_SYS_FSYNC,         /* __NR_fsync */
    [73] = FERN_SYS_PPOLL,         /* __NR_ppoll */
    [78] = FERN_SYS_READLINKAT,    /* __NR_readlinkat */
    [79] = FERN_SYS_NEWFSTATAT,    /* __NR_newfstatat */
    [80] = FERN_SYS_FSTAT,         /* __NR_fstat */
    [82] = FERN_SYS_FCNTL,         /* __NR_fcntl (old) */
    [83] = FERN_SYS_FSYNC,         /* __NR_fdatasync */
    [85] = FERN_SYS_COPY_FILE_RANGE, /* __NR_copy_file_range */
    [88] = FERN_SYS_UTIMENSAT,     /* __NR_utimensat */
    [89] = FERN_SYS_SELECT,        /* __NR_pselect6 (stub) */
    [90] = FERN_SYS_CHMOD,         /* __NR_chmod (stub) */
    [91] = FERN_SYS_FCHMOD,        /* __NR_fchmod */
    [92] = FERN_SYS_CHOWN,         /* __NR_chown (stub) */
    [93] = FERN_SYS_FCHOWN,        /* __NR_fchown */
    [95] = FERN_SYS_UMASK,         /* __NR_umask */

    /* Process lifecycle */
    [93] = FERN_SYS_EXIT,          /* __NR_exit */
    [94] = FERN_SYS_EXIT_GROUP,    /* __NR_exit_group */
    [96] = FERN_SYS_SET_TID_ADDRESS, /* __NR_set_tid_address */
    [98] = FERN_SYS_FUTEX,         /* __NR_futex */
    [99] = FERN_SYS_SET_TID_ADDRESS, /* __NR_set_robust_list (stub) */
    [100] = FERN_SYS_SET_TID_ADDRESS, /* __NR_get_robust_list (stub) */
    [113] = FERN_SYS_CLOCK_GETTIME, /* __NR_clock_gettime */
    [114] = FERN_SYS_CLOCK_GETRES, /* __NR_clock_getres */
    [115] = FERN_SYS_CLOCK_NANOSLEEP, /* __NR_clock_nanosleep */
    [118] = FERN_SYS_SCHED_SETPARAM, /* __NR_sched_setparam (stub) */
    [119] = FERN_SYS_SCHED_SETSCHEDULER, /* __NR_sched_setscheduler (stub) */
    [120] = FERN_SYS_SCHED_GETSCHEDULER, /* __NR_sched_getscheduler (stub) */
    [121] = FERN_SYS_SCHED_GETPARAM, /* __NR_sched_getparam (stub) */
    [122] = FERN_SYS_SCHED_SETAFFINITY, /* __NR_sched_setaffinity (stub) */
    [123] = FERN_SYS_SCHED_GETAFFINITY, /* __NR_sched_getaffinity (stub) */
    [124] = FERN_SYS_SCHED_YIELD,  /* __NR_sched_yield */
    [125] = FERN_SYS_SCHED_GET_PRIORITY_MAX, /* __NR_sched_get_priority_max (stub) */
    [126] = FERN_SYS_SCHED_GET_PRIORITY_MIN, /* __NR_sched_get_priority_min (stub) */
    [129] = FERN_SYS_KILL,         /* __NR_kill */
    [130] = FERN_SYS_KILL,         /* __NR_tkill */
    [131] = FERN_SYS_KILL,         /* __NR_tgkill */
    [134] = FERN_SYS_RT_SIGACTION, /* __NR_rt_sigaction (stub) */
    [135] = FERN_SYS_RT_SIGPROCMASK, /* __NR_rt_sigprocmask (stub) */
    [139] = FERN_SYS_RT_SIGRETURN, /* __NR_rt_sigreturn (stub) */
    [143] = FERN_SYS_SETRESUID,    /* __NR_setresuid */
    [144] = FERN_SYS_SETRESGID,    /* __NR_setresgid */
    [145] = FERN_SYS_SETRESUID,    /* __NR_setresuid (old) */
    [146] = FERN_SYS_SETRESGID,    /* __NR_setresgid (old) */
    [147] = FERN_SYS_SETRESUID,    /* __NR_setresuid (alt) */
    [148] = FERN_SYS_GETRESUID,    /* __NR_getresuid */
    [149] = FERN_SYS_SETRESGID,    /* __NR_setresgid (alt) */
    [150] = FERN_SYS_GETRESGID,    /* __NR_getresgid */
    [151] = FERN_SYS_SETFSUID,     /* __NR_setfsuid */
    [152] = FERN_SYS_SETFSGID,     /* __NR_setfsgid */
    [155] = FERN_SYS_SETPGID,      /* __NR_setpgid */
    [156] = FERN_SYS_GETSID,       /* __NR_getsid */
    [157] = FERN_SYS_SETSID,       /* __NR_setsid */
    [158] = FERN_SYS_SETSID,       /* __NR_getgroups (stub) */
    [159] = FERN_SYS_SETGROUPS,    /* __NR_setgroups (stub) */
    [160] = FERN_SYS_UNAME,        /* __NR_uname */
    [166] = FERN_SYS_UMASK,        /* __NR_umask */
    [167] = FERN_SYS_PRCTL,        /* __NR_prctl (stub) */
    [169] = FERN_SYS_GETTIMEOFDAY, /* __NR_gettimeofday */
    [172] = FERN_SYS_GETPID,       /* __NR_getpid */
    [173] = FERN_SYS_GETPPID,      /* __NR_getppid */
    [174] = FERN_SYS_GETUID,       /* __NR_getuid */
    [175] = FERN_SYS_GETEUID,      /* __NR_geteuid */
    [176] = FERN_SYS_GETGID,       /* __NR_getgid */
    [177] = FERN_SYS_GETEGID,      /* __NR_getegid */
    [178] = FERN_SYS_GETTID,       /* __NR_gettid */
    [198] = FERN_SYS_SOCKET,       /* __NR_socket */
    [200] = FERN_SYS_BIND,         /* __NR_bind */
    [201] = FERN_SYS_LISTEN,       /* __NR_listen */
    [202] = FERN_SYS_ACCEPT,       /* __NR_accept */
    [203] = FERN_SYS_CONNECT,      /* __NR_connect */
    [204] = FERN_SYS_GETSOCKNAME,  /* __NR_getsockname */
    [205] = FERN_SYS_GETPEERNAME,  /* __NR_getpeername */
    [206] = FERN_SYS_SENDTO,       /* __NR_sendto */
    [207] = FERN_SYS_RECVFROM,     /* __NR_recvfrom */
    [208] = FERN_SYS_SETSOCKOPT,   /* __NR_setsockopt */
    [209] = FERN_SYS_GETSOCKOPT,   /* __NR_getsockopt */
    [210] = FERN_SYS_SHUTDOWN,     /* __NR_shutdown */
    [211] = FERN_SYS_SENDMSG,      /* __NR_sendmsg */
    [212] = FERN_SYS_RECVMSG,      /* __NR_recvmsg */
    [214] = FERN_SYS_BRK,          /* __NR_brk */
    [215] = FERN_SYS_MUNMAP,       /* __NR_munmap */
    [216] = FERN_SYS_MREMAP,       /* __NR_mremap (stub) */
    [220] = FERN_SYS_CLONE,        /* __NR_clone */
    [221] = FERN_SYS_EXECVE,       /* __NR_execve */
    [222] = FERN_SYS_MMAP,         /* __NR_mmap */
    [226] = FERN_SYS_MPROTECT,     /* __NR_mprotect */
    [233] = FERN_SYS_MADVISE,      /* __NR_madvise */
    [234] = FERN_SYS_MMAP,         /* __NR_mlock (stub) */
    [235] = FERN_SYS_MMAP,         /* __NR_munlock (stub) */
    [236] = FERN_SYS_MMAP,         /* __NR_mlockall (stub) */
    [237] = FERN_SYS_MMAP,         /* __NR_munlockall (stub) */
    [260] = FERN_SYS_WAIT4,        /* __NR_wait4 */
    [274] = FERN_SYS_SCHED_SETATTR, /* __NR_sched_setattr (stub) */
    [275] = FERN_SYS_SCHED_GETATTR, /* __NR_sched_getattr (stub) */
    [278] = FERN_SYS_GETRANDOM,    /* __NR_getrandom */
    [291] = FERN_SYS_STATX,        /* __NR_statx */
    [435] = FERN_SYS_CLONE,        /* __NR_clone3 (stub, same as clone) */
};
#pragma GCC diagnostic pop

/* ---- RISC-V 64 translation table ----
 * RISC-V uses asm-generic/unistd.h (same numbers as AArch64).
 * Maps to internal Fern numbers (x86_64 Linux ABI).
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((unused)) static int32_t riscv64_xlat[SYSCALL_TABLE_SIZE] = {
    [0 ... (SYSCALL_TABLE_SIZE - 1)] = FERN_NR_INVALID,

    /* I/O */
    [17] = FERN_SYS_GETCWD,        /* __NR_getcwd */
    [20] = FERN_SYS_EPOLL_CREATE1, /* __NR_epoll_create1 */
    [21] = FERN_SYS_EPOLL_CTL,     /* __NR_epoll_ctl */
    [22] = FERN_SYS_EPOLL_PWAIT,   /* __NR_epoll_pwait */
    [23] = FERN_SYS_DUP,           /* __NR_dup */
    [24] = FERN_SYS_DUP3,          /* __NR_dup3 */
    [25] = FERN_SYS_FCNTL,         /* __NR_fcntl */
    [29] = FERN_SYS_IOCTL,         /* __NR_ioctl */
    [33] = FERN_SYS_MKNOD,         /* __NR_mknodat */
    [34] = FERN_SYS_MKDIRAT,       /* __NR_mkdirat */
    [35] = FERN_SYS_UNLINKAT,      /* __NR_unlinkat */
    [36] = FERN_SYS_SYMLINKAT,     /* __NR_symlinkat */
    [37] = FERN_SYS_LINKAT,        /* __NR_linkat */
    [38] = FERN_SYS_RENAMEAT,      /* __NR_renameat */
    [39] = FERN_SYS_TRUNCATE,      /* __NR_umount2 (stub) */
    [41] = FERN_SYS_CHDIR,         /* __NR_chroot (stub) */
    [43] = FERN_SYS_STATFS,        /* __NR_statfs */
    [44] = FERN_SYS_FSTATFS,       /* __NR_fstatfs */
    [46] = FERN_SYS_TRUNCATE,      /* __NR_truncate */
    [47] = FERN_SYS_FTRUNCATE,     /* __NR_ftruncate */
    [48] = FERN_SYS_FACCESSAT,     /* __NR_faccessat */
    [49] = FERN_SYS_CHDIR,         /* __NR_chdir */
    [50] = FERN_SYS_FCHDIR,        /* __NR_fchdir */
    [54] = FERN_SYS_FCHOWNAT,      /* __NR_fchownat */
    [56] = FERN_SYS_OPENAT,        /* __NR_openat */
    [57] = FERN_SYS_CLOSE,         /* __NR_close */
    [59] = FERN_SYS_PIPE2,         /* __NR_pipe2 */
    [61] = FERN_SYS_GETDENTS64,    /* __NR_getdents64 */
    [62] = FERN_SYS_LSEEK,         /* __NR_lseek */
    [63] = FERN_SYS_READ,          /* __NR_read */
    [64] = FERN_SYS_WRITE,         /* __NR_write */
    [65] = FERN_SYS_READV,         /* __NR_readv */
    [66] = FERN_SYS_WRITEV,        /* __NR_writev */
    [67] = FERN_SYS_PREAD64,       /* __NR_pread64 */
    [68] = FERN_SYS_PWRITE64,      /* __NR_pwrite64 */
    [72] = FERN_SYS_FSYNC,         /* __NR_fsync */
    [73] = FERN_SYS_PPOLL,         /* __NR_ppoll */
    [76] = FERN_SYS_RENAMEAT,      /* __NR_renameat2 */
    [78] = FERN_SYS_READLINKAT,    /* __NR_readlinkat */
    [79] = FERN_SYS_NEWFSTATAT,    /* __NR_newfstatat */
    [80] = FERN_SYS_FSTAT,         /* __NR_fstat */
    [81] = FERN_SYS_SETGROUPS,     /* __NR_setgroups */
    [85] = FERN_SYS_COPY_FILE_RANGE, /* __NR_copy_file_range */
    [88] = FERN_SYS_UTIMENSAT,     /* __NR_utimensat */
    [90] = FERN_SYS_CHMOD,         /* __NR_chmod */
    [91] = FERN_SYS_FCHMOD,        /* __NR_fchmod */
    [92] = FERN_SYS_CHOWN,         /* __NR_chown */
    [93] = FERN_SYS_FCHOWN,        /* __NR_fchown */

    /* Process lifecycle */
    [93] = FERN_SYS_EXIT,          /* __NR_exit */
    [94] = FERN_SYS_EXIT_GROUP,    /* __NR_exit_group */
    [96] = FERN_SYS_SET_TID_ADDRESS, /* __NR_set_tid_address */
    [98] = FERN_SYS_FUTEX,         /* __NR_futex */
    [101] = FERN_SYS_NANOSLEEP,    /* __NR_nanosleep */
    [113] = FERN_SYS_CLOCK_GETTIME, /* __NR_clock_gettime */
    [114] = FERN_SYS_CLOCK_GETRES, /* __NR_clock_getres */
    [118] = FERN_SYS_SCHED_SETPARAM, /* __NR_sched_setparam (stub) */
    [119] = FERN_SYS_SCHED_SETSCHEDULER, /* __NR_sched_setscheduler (stub) */
    [120] = FERN_SYS_SCHED_GETSCHEDULER, /* __NR_sched_getscheduler (stub) */
    [121] = FERN_SYS_SCHED_GETPARAM, /* __NR_sched_getparam (stub) */
    [122] = FERN_SYS_SCHED_SETAFFINITY, /* __NR_sched_setaffinity (stub) */
    [123] = FERN_SYS_SCHED_GETAFFINITY, /* __NR_sched_getaffinity (stub) */
    [124] = FERN_SYS_SCHED_YIELD,  /* __NR_sched_yield */
    [125] = FERN_SYS_SCHED_GET_PRIORITY_MAX, /* __NR_sched_get_priority_max (stub) */
    [126] = FERN_SYS_SCHED_GET_PRIORITY_MIN, /* __NR_sched_get_priority_min (stub) */
    [129] = FERN_SYS_KILL,         /* __NR_kill */
    [130] = FERN_SYS_KILL,         /* __NR_tkill */
    [131] = FERN_SYS_KILL,         /* __NR_tgkill */
    [134] = FERN_SYS_RT_SIGACTION, /* __NR_rt_sigaction (stub) */
    [135] = FERN_SYS_RT_SIGPROCMASK, /* __NR_rt_sigprocmask (stub) */
    [139] = FERN_SYS_RT_SIGRETURN, /* __NR_rt_sigreturn (stub) */
    [140] = FERN_SYS_SETPRIORITY,  /* __NR_setpriority */
    [141] = FERN_SYS_GETPRIORITY,  /* __NR_getpriority */
    [143] = FERN_SYS_CLOSE_RANGE,  /* __NR_close_range */
    [144] = FERN_SYS_SETRESGID,    /* __NR_setresgid */
    [146] = FERN_SYS_SETUID,       /* __NR_setuid */
    [153] = FERN_SYS_TIMES,        /* __NR_times */
    [160] = FERN_SYS_UNAME,        /* __NR_uname */
    [163] = FERN_SYS_GETRLIMIT,    /* __NR_getrlimit */
    [164] = FERN_SYS_SETRLIMIT,    /* __NR_setrlimit */
    [165] = FERN_SYS_GETRUSAGE,    /* __NR_getrusage */
    [166] = FERN_SYS_UMASK,        /* __NR_umask */
    [169] = FERN_SYS_GETTIMEOFDAY, /* __NR_gettimeofday */
    [172] = FERN_SYS_GETPID,       /* __NR_getpid */
    [173] = FERN_SYS_GETPPID,      /* __NR_getppid */
    [174] = FERN_SYS_GETUID,       /* __NR_getuid */
    [175] = FERN_SYS_GETEUID,      /* __NR_geteuid */
    [176] = FERN_SYS_GETGID,       /* __NR_getgid */
    [177] = FERN_SYS_GETEGID,      /* __NR_getegid */
    [178] = FERN_SYS_GETTID,       /* __NR_gettid */
    [179] = FERN_SYS_SYSINFO,      /* __NR_sysinfo */
    [180] = FERN_SYS_MMAP,         /* __NR_mmap (RV-specific) */
    [198] = FERN_SYS_SOCKET,       /* __NR_socket */
    [199] = FERN_SYS_SOCKETPAIR,   /* __NR_socketpair */
    [200] = FERN_SYS_BIND,         /* __NR_bind */
    [201] = FERN_SYS_LISTEN,       /* __NR_listen */
    [202] = FERN_SYS_ACCEPT,       /* __NR_accept */
    [203] = FERN_SYS_CONNECT,      /* __NR_connect */
    [204] = FERN_SYS_GETSOCKNAME,  /* __NR_getsockname */
    [205] = FERN_SYS_GETPEERNAME,  /* __NR_getpeername */
    [206] = FERN_SYS_SENDTO,       /* __NR_sendto */
    [207] = FERN_SYS_RECVFROM,     /* __NR_recvfrom */
    [208] = FERN_SYS_SETSOCKOPT,   /* __NR_setsockopt */
    [209] = FERN_SYS_GETSOCKOPT,   /* __NR_getsockopt */
    [210] = FERN_SYS_SHUTDOWN,     /* __NR_shutdown */
    [211] = FERN_SYS_SENDMSG,      /* __NR_sendmsg */
    [212] = FERN_SYS_RECVMSG,      /* __NR_recvmsg */
    [214] = FERN_SYS_BRK,          /* __NR_brk */
    [215] = FERN_SYS_MUNMAP,       /* __NR_munmap */
    [216] = FERN_SYS_MREMAP,       /* __NR_mremap */
    [220] = FERN_SYS_CLONE,        /* __NR_clone */
    [221] = FERN_SYS_EXECVE,       /* __NR_execve */
    [222] = FERN_SYS_MMAP,         /* __NR_mmap */
    [226] = FERN_SYS_MPROTECT,     /* __NR_mprotect */
    [227] = FERN_SYS_MSYNC,        /* __NR_msync */
    [232] = FERN_SYS_MINCORE,      /* __NR_mincore */
    [233] = FERN_SYS_MADVISE,      /* __NR_madvise */
    [234] = FERN_SYS_MMAP,         /* __NR_mlock (stub) */
    [235] = FERN_SYS_MMAP,         /* __NR_munlock (stub) */
    [236] = FERN_SYS_MMAP,         /* __NR_mlockall (stub) */
    [237] = FERN_SYS_MMAP,         /* __NR_munlockall (stub) */
    [242] = FERN_SYS_ACCEPT4,      /* __NR_accept4 */
    [243] = FERN_SYS_RSEQ,         /* __NR_rseq */
    [254] = FERN_SYS_FADVISE64,    /* __NR_fadvise64 */
    [260] = FERN_SYS_WAIT4,        /* __NR_wait4 */
    [261] = FERN_SYS_PRLIMIT64,    /* __NR_prlimit64 */
    [274] = FERN_SYS_SCHED_SETATTR, /* __NR_sched_setattr (stub) */
    [275] = FERN_SYS_SCHED_GETATTR, /* __NR_sched_getattr (stub) */
    [278] = FERN_SYS_GETRANDOM,    /* __NR_getrandom */
    [279] = FERN_SYS_MEMFD_CREATE, /* __NR_memfd_create */
    [281] = FERN_SYS_MSEAL,        /* __NR_mseal */
    [291] = FERN_SYS_STATX,        /* __NR_statx */
    [435] = FERN_SYS_CLONE,        /* __NR_clone3 (stub, same as clone) */
};
#pragma GCC diagnostic pop

/* =========================================================================
 * syscall_translate
 * ========================================================================= */
int32_t syscall_translate(uint32_t arch_nr)
{
#if ARCH_ARM32
    /* Handle Forest-private syscalls (in ARM32 arch-specific space) */
    if (arch_nr >= ARM32_NR_FOREST_BASE) {
        switch (arch_nr) {
        case ARM32_NR_FOREST_BASE + 0x01: return FERN_SYS_POWER;
        case ARM32_NR_FOREST_BASE + 0x02: return FERN_SYS_NETINFO;
        default: return FERN_NR_INVALID;
        }
    }
    if (arch_nr < SYSCALL_TABLE_SIZE)
        return arm32_xlat[arch_nr];
    return FERN_NR_INVALID;

#elif ARCH_ARM64
    if (arch_nr < SYSCALL_TABLE_SIZE)
        return aarch64_xlat[arch_nr];
    return FERN_NR_INVALID;

#elif ARCH_RISCV64
    if (arch_nr < SYSCALL_TABLE_SIZE)
        return riscv64_xlat[arch_nr];
    return FERN_NR_INVALID;

#elif ARCH_X86_64
    /* x86_64 uses internal numbers directly (identity mapping) */
    (void)arch_nr;
    return (int32_t)arch_nr;

#else
    (void)arch_nr;
    return FERN_NR_INVALID;
#endif
}

/* =========================================================================
 * syscall_dispatch_arch
 *
 * Translates the arch-specific syscall number and dispatches to the
 * appropriate sys_* handler. Arguments are passed as uint64_t and
 * cast to the correct types at the call site.
 *
 * For special cases (e.g., ARM32 mmap2 page-unit offset), the caller
 * should use syscall_translate() directly and handle the translation
 * in arch-specific code.
 * ========================================================================= */
int64_t syscall_dispatch_arch(uint64_t arch_nr,
                               uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5)
{
    int32_t nr = syscall_translate((uint32_t)arch_nr);
    if (nr == FERN_NR_INVALID)
        return -38; /* -ENOSYS */

    switch (nr) {

    /* ---- Process lifecycle ---- */
    case FERN_SYS_EXIT:
        sys_exit((int)a0);
        return 0; /* noreturn, but silences warning */

    case FERN_SYS_EXIT_GROUP:
        sys_exit_group((int)a0);
        return 0; /* noreturn */

    case FERN_SYS_FORK:
        return sys_fork();

    case FERN_SYS_EXECVE:
        return sys_execve((const char *)(uintptr_t)a0,
                          (char *const *)(uintptr_t)a1,
                          (char *const *)(uintptr_t)a2);

    case FERN_SYS_WAIT4:
        return sys_wait4((int)a0, (int *)(uintptr_t)a1,
                         (int)a2, (void *)(uintptr_t)a3);

    case FERN_SYS_GETPID:
        return sys_getpid();

    case FERN_SYS_GETPPID:
        return sys_getppid();

    case FERN_SYS_GETUID:
        return sys_getuid();

    case FERN_SYS_GETGID:
        return sys_getgid();

    case FERN_SYS_GETEUID:
        return sys_geteuid();

    case FERN_SYS_GETEGID:
        return sys_getegid();

    case FERN_SYS_GETTID:
        return sys_getpid(); /* simplified: TID == PID for single-threaded */

    case FERN_SYS_KILL:
        return sys_kill((int)a0, (int)a1);

    case FERN_SYS_CLONE:
        return sys_clone((unsigned long)a0, (void *)(uintptr_t)a1,
                         (int *)(uintptr_t)a2, (int *)(uintptr_t)a3,
                         (unsigned long)a4);

    case FERN_SYS_SET_TID_ADDRESS:
        return sys_set_tid_address((int *)(uintptr_t)a0);

    case FERN_SYS_SCHED_YIELD:
        return sys_sched_yield();

    /* ---- File I/O ---- */
    case FERN_SYS_READ:
        return sys_read((int)a0, (void *)(uintptr_t)a1, (size_t)a2);

    case FERN_SYS_WRITE:
        return sys_write((int)a0, (const void *)(uintptr_t)a1, (size_t)a2);

    case FERN_SYS_OPEN:
    case FERN_SYS_OPENAT:
        return sys_open((const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case FERN_SYS_CLOSE:
        return sys_close((int)a0);

    case FERN_SYS_LSEEK:
        return sys_lseek((int)a0, (long)a1, (int)a2);

    case FERN_SYS_FSTAT:
    case FERN_SYS_NEWFSTATAT:
        return sys_fstat((int)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_STAT:
        return sys_stat((const char *)(uintptr_t)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_GETCWD:
        return sys_getcwd((char *)(uintptr_t)a0, (size_t)a1);

    case FERN_SYS_CHDIR:
        return sys_chdir((const char *)(uintptr_t)a0);

    case FERN_SYS_FCHDIR:
        return sys_fchdir((int)a0);

    case FERN_SYS_DUP:
        return sys_dup((int)a0);

    case FERN_SYS_DUP2:
    case FERN_SYS_DUP3:
        return sys_dup3((int)a0, (int)a1, (int)a2);

    case FERN_SYS_FCNTL:
        return sys_fcntl((int)a0, (int)a1, (int)a2);

    case FERN_SYS_IOCTL:
        return sys_ioctl((int)a0, (unsigned long)a1, (void *)(uintptr_t)a2);

    case FERN_SYS_GETDENTS64:
        return sys_getdents64((int)a0, (void *)(uintptr_t)a1, (size_t)a2);

    case FERN_SYS_PIPE2:
        return sys_pipe((int *)(uintptr_t)a0);

    case FERN_SYS_READV:
        return sys_readv((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_WRITEV:
        return sys_writev((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_PREAD64:
        return sys_pread64((int)a0, (void *)(uintptr_t)a1, (size_t)a2, (long)a3);

    case FERN_SYS_PWRITE64:
        return sys_pwrite64((int)a0, (const void *)(uintptr_t)a1, (size_t)a2, (long)a3);

    case FERN_SYS_ACCESS:
    case FERN_SYS_FACCESSAT:
        return sys_faccessat((int)a0, (const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case FERN_SYS_UNLINK:
    case FERN_SYS_UNLINKAT:
        return sys_unlinkat((int)a0, (const char *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_MKDIR:
    case FERN_SYS_MKDIRAT:
        return sys_mkdirat((int)a0, (const char *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_RMDIR:
        return sys_rmdir((const char *)(uintptr_t)a0);

    case FERN_SYS_RENAME:
    case FERN_SYS_RENAMEAT:
        return sys_renameat((int)a0, (const char *)(uintptr_t)a1,
                            (int)a2, (const char *)(uintptr_t)a3);

    case FERN_SYS_LINK:
    case FERN_SYS_LINKAT:
        return sys_linkat((int)a0, (const char *)(uintptr_t)a1,
                          (int)a2, (const char *)(uintptr_t)a3, (int)a4);

    case FERN_SYS_SYMLINK:
    case FERN_SYS_SYMLINKAT:
        return sys_symlink((const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);

    case FERN_SYS_READLINKAT:
        return sys_readlinkat((int)a0, (const char *)(uintptr_t)a1,
                              (char *)(uintptr_t)a2, (size_t)a3);

    case FERN_SYS_CHMOD:
        return sys_chmod((const char *)(uintptr_t)a0, (int)a1);

    case FERN_SYS_FCHMOD:
        return sys_fchmod((int)a0, (int)a1);

    case FERN_SYS_CHOWN:
        return sys_chown((const char *)(uintptr_t)a0, (int)a1, (int)a2);

    case FERN_SYS_FCHOWN:
        return sys_fchown((int)a0, (int)a1, (int)a2);

    case FERN_SYS_UMASK:
        return sys_umask((int)a0);

    case FERN_SYS_FSYNC:
    case FERN_SYS_FDATASYNC:
        return sys_fsync((int)a0);

    case FERN_SYS_FTRUNCATE:
    case FERN_SYS_TRUNCATE:
        return sys_ftruncate((int)a0, (long)a1);

    case FERN_SYS_STATFS:
        return sys_statfs((const char *)(uintptr_t)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_FSTATFS:
        return sys_fstatfs((int)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_COPY_FILE_RANGE:
        return sys_copy_file_range((int)a0, (long *)(uintptr_t)a1,
                                   (int)a2, (long *)(uintptr_t)a3,
                                   (size_t)a4, (unsigned int)a5);

    case FERN_SYS_PPOLL:
        return sys_ppoll((void *)(uintptr_t)a0, (unsigned int)a1,
                         (const void *)(uintptr_t)a2, (const void *)(uintptr_t)a3,
                         (size_t)a4);

    case FERN_SYS_FADVISE64:
        return sys_fadvise64((int)a0, (long)a1, (long)a2, (int)a3);

    case FERN_SYS_MEMFD_CREATE:
        return sys_memfd_create((const char *)(uintptr_t)a0, (unsigned int)a1);

    case FERN_SYS_STATX:
        return sys_statx((int)a0, (const char *)(uintptr_t)a1,
                         (unsigned)a2, (unsigned)a3, (void *)(uintptr_t)a4);

    case FERN_SYS_CLOSE_RANGE:
        return sys_close_range((unsigned int)a0, (unsigned int)a1, (unsigned int)a2);

    /* ---- Memory management ---- */
    case FERN_SYS_BRK:
        return sys_brk((void *)(uintptr_t)a0);

    case FERN_SYS_MMAP:
        return sys_mmap((void *)(uintptr_t)a0, (size_t)a1,
                        (int)a2, (int)a3, (int)a4, (long)a5);

    case FERN_SYS_MUNMAP:
        return sys_munmap((void *)(uintptr_t)a0, (size_t)a1);

    case FERN_SYS_MPROTECT:
        return sys_mprotect((void *)(uintptr_t)a0, (size_t)a1, (int)a2);

    case FERN_SYS_MADVISE:
        return sys_madvise((void *)(uintptr_t)a0, (size_t)a1, (int)a2);

    /* ---- Time ---- */
    case FERN_SYS_CLOCK_GETTIME:
        return sys_clock_gettime((int)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_CLOCK_GETRES:
        return sys_clock_getres((int)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_CLOCK_NANOSLEEP:
        return sys_nanosleep((const void *)(uintptr_t)a2, (void *)(uintptr_t)a3);

    case FERN_SYS_NANOSLEEP:
        return sys_nanosleep((const void *)(uintptr_t)a0, (void *)(uintptr_t)a1);

    case FERN_SYS_GETTIMEOFDAY:
        return sys_gettimeofday((void *)(uintptr_t)a0, (void *)(uintptr_t)a1);

    /* ---- System information ---- */
    case FERN_SYS_UNAME:
        return sys_uname((void *)(uintptr_t)a0);

    /* ---- Signals (stubs) ---- */
    case FERN_SYS_RT_SIGACTION:
    case FERN_SYS_RT_SIGPROCMASK:
    case FERN_SYS_RT_SIGRETURN:
        return -38; /* -ENOSYS */

    /* ---- Networking ---- */
    case FERN_SYS_SOCKET:
        return sys_socket((int)a0, (int)a1, (int)a2);

    case FERN_SYS_BIND:
        return sys_bind((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_LISTEN:
        return sys_listen((int)a0, (int)a1);

    case FERN_SYS_ACCEPT:
        return sys_accept((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2);

    case FERN_SYS_ACCEPT4:
        return sys_accept4((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2, (int)a3);

    case FERN_SYS_CONNECT:
        return sys_connect((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_SENDTO:
        return sys_sendto((int)a0, (const void *)(uintptr_t)a1, (size_t)a2,
                          (int)a3, (const void *)(uintptr_t)a4, (int)a5);

    case FERN_SYS_RECVFROM:
        return sys_recvfrom((int)a0, (void *)(uintptr_t)a1, (size_t)a2,
                            (int)a3, (void *)(uintptr_t)a4, (int *)(uintptr_t)a5);

    case FERN_SYS_SENDMSG:
        return sys_sendmsg((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_RECVMSG:
        return sys_recvmsg((int)a0, (void *)(uintptr_t)a1, (int)a2);

    case FERN_SYS_SHUTDOWN:
        return sys_shutdown((int)a0, (int)a1);

    case FERN_SYS_GETSOCKNAME:
        return sys_getsockname((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2);

    case FERN_SYS_GETPEERNAME:
        return sys_getpeername((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2);

    case FERN_SYS_SOCKETPAIR:
        return sys_socketpair((int)a0, (int)a1, (int)a2, (int *)(uintptr_t)a3);

    case FERN_SYS_SETSOCKOPT:
        return sys_setsockopt((int)a0, (int)a1, (int)a2, (const void *)(uintptr_t)a3, (int)a4);

    case FERN_SYS_GETSOCKOPT:
        return sys_getsockopt((int)a0, (int)a1, (int)a2, (void *)(uintptr_t)a3, (int *)(uintptr_t)a4);

    /* ---- I/O multiplexing ---- */
    case FERN_SYS_EPOLL_CREATE1:
        return sys_epoll_create1((int)a0);

    case FERN_SYS_EPOLL_CTL:
        return sys_epoll_ctl((int)a0, (int)a1, (int)a2, (void *)(uintptr_t)a3);

    case FERN_SYS_EPOLL_PWAIT:
        return sys_epoll_pwait((int)a0, (void *)(uintptr_t)a1, (int)a2,
                               (int)a3, (const void *)(uintptr_t)a4, (size_t)a5);

    /* ---- Scheduling (stubs for single-core) ---- */
    case FERN_SYS_SCHED_SETPARAM:
    case FERN_SYS_SCHED_SETSCHEDULER:
    case FERN_SYS_SCHED_GETSCHEDULER:
    case FERN_SYS_SCHED_GETPARAM:
    case FERN_SYS_SCHED_SETAFFINITY:
    case FERN_SYS_SCHED_GETAFFINITY:
    case FERN_SYS_SCHED_GET_PRIORITY_MAX:
    case FERN_SYS_SCHED_GET_PRIORITY_MIN:
    case FERN_SYS_SCHED_SETATTR:
    case FERN_SYS_SCHED_GETATTR:
        return 0;

    /* ---- Credentials (stubs) ---- */
    case FERN_SYS_SETUID:
        return sys_setuid((int)a0);

    case FERN_SYS_SETGID:
        return sys_setgid((int)a0);

    case FERN_SYS_SETREUID:
    case FERN_SYS_SETREGID:
    case FERN_SYS_SETRESUID:
    case FERN_SYS_SETRESGID:
    case FERN_SYS_SETFSUID:
    case FERN_SYS_SETFSGID:
    case FERN_SYS_SETGROUPS:
        return 0;

    case FERN_SYS_GETRESUID:
    case FERN_SYS_GETRESGID: {
        uint32_t *p = (uint32_t *)(uintptr_t)a0;
        if (p) { p[0] = 0; p[1] = 0; p[2] = 0; }
        return 0;
    }

    case FERN_SYS_GETGROUPS:
        return 0;

    case FERN_SYS_GETPGID:
    case FERN_SYS_GETSID:
        return sys_getpid();

    case FERN_SYS_SETPGID:
    case FERN_SYS_SETSID:
        return 0;

    case FERN_SYS_GETPGRP:
        return sys_getpid();

    /* ---- Misc ---- */
    case FERN_SYS_FUTEX:
        return sys_futex((uint32_t *)(uintptr_t)a0, (int)a1, (int)a2,
                         (const void *)(uintptr_t)a3,
                         (uint32_t *)(uintptr_t)a4, (int)a5);

    case FERN_SYS_GETRANDOM:
        return sys_getrandom((void *)(uintptr_t)a0, (size_t)a1, (unsigned int)a2);

    /* ---- Unknown ---- */
    default:
        return -38; /* -ENOSYS */
    }
}
