/*
 * Fern - AArch64 Syscall Dispatch
 *
 * Translates AArch64 Linux syscall numbers to Fern internal handlers.
 * The calling convention is the standard arm64 Linux ABI:
 *   x8  = syscall number
 *   x0-x7 = arguments
 *   x0  = return value
 *
 * Unimplemented syscalls return -ENOSYS.
 *
 * Forward declarations for the Fern syscall handlers live in the
 * existing src/syscall.c and are exposed via src/include/syscall.h.
 * We avoid including that header directly to prevent x86 dependencies;
 * we forward-declare only what we use.
 *
 * Two entry points are provided:
 *
 *   aarch64_syscall_dispatch() - legacy signature used by exceptions.S
 *     (x0-x7 = args in order, x8 = nr last; returns long)
 *
 *   aarch64_syscall_handle() - new signature used by vectors.S
 *     (nr = first arg, then a0-a6; returns int64_t)
 *     This is the primary entry point for new code.
 */

#include "syscall.h"
#include "uart.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Forward declarations from the platform-independent Fern layer.
 *
 * These resolve to the implementations in src/syscall.c (x86/generic)
 * or their AArch64-specific equivalents as the port matures.
 * --------------------------------------------------------------------- */

/* VFS / file I/O */
extern long sys_read(int fd, void *buf, size_t count);
extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_open(const char *path, int flags, int mode);
extern long sys_close(int fd);
extern long sys_lseek(int fd, long offset, int whence);
extern long sys_fstat(int fd, void *statbuf);

/* Memory */
extern long sys_brk(void *addr);
extern long sys_mmap(void *addr, size_t length, int prot, int flags,
                     int fd, long offset);
extern long sys_munmap(void *addr, size_t length);
extern long sys_mprotect(void *addr, size_t length, int prot);

/* Process */
extern long sys_getpid(void);
extern long sys_getppid(void);
extern long sys_getuid(void);
extern long sys_getgid(void);
extern long sys_geteuid(void);
extern long sys_getegid(void);
extern long sys_kill(int pid, int sig);
extern void sys_exit(int status) __attribute__((noreturn));
extern void sys_exit_group(int status) __attribute__((noreturn));
extern long sys_execve(const char *path, char *const argv[],
                       char *const envp[]);

/* Time */
extern long sys_clock_gettime(int clkid, void *tp);
extern long sys_clock_getres(int clkid, void *res);
extern long sys_gettimeofday(void *tv, void *tz);
extern long sys_nanosleep(const void *req, void *rem);

/* System */
extern long sys_uname(void *buf);

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */
static long stub_not_implemented(uint64_t nr)
{
    uart_printf("[syscall] unimplemented nr=%lu\n", (unsigned long)nr);
    return AARCH64_ENOSYS;
}

/* -----------------------------------------------------------------------
 * aarch64_syscall_handle
 *
 * Primary syscall entry point called from vectors.S (el0_svc).
 *
 * Signature matches the AArch64 C ABI with syscall number first:
 *   nr  = x8 from userspace (syscall number)
 *   a0  = x0 from userspace (arg0, also receives return value)
 *   a1  = x1 from userspace (arg1)
 *   a2  = x2 from userspace (arg2)
 *   a3  = x3 from userspace (arg3)
 *   a4  = x4 from userspace (arg4)
 *   a5  = x5 from userspace (arg5)
 *   a6  = x6 from userspace (arg6, rarely used)
 *
 * AArch64 Linux syscall numbers (asm-generic/unistd.h):
 *
 *   63  read           64  write          57  close
 *   80  fstat          62  lseek          56  openat
 *   29  ioctl          23  dup            24  dup3
 *   25  fcntl          59  pipe2          61  getdents64
 *   17  getcwd         49  chdir         214  brk
 *  222  mmap          215  munmap        226  mprotect
 *  172  getpid        173  getppid       174  getuid
 *  175  geteuid       176  getgid        177  getegid
 *  178  gettid         93  exit           94  exit_group
 *  129  kill          221  execve        113  clock_gettime
 *  114  clock_getres  169  gettimeofday  101  nanosleep
 *  160  uname         172  getpid
 * --------------------------------------------------------------------- */
int64_t aarch64_syscall_handle(uint64_t nr,
                                uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5,
                                uint64_t a6)
{
    (void)a6;

    switch ((unsigned int)nr) {

    /* ---- File I/O ---------------------------------------------------- */
    case 63:  /* __NR_read */
        return sys_read((int)a0, (void *)(uintptr_t)a1, (size_t)a2);

    case 64:  /* __NR_write */
        return sys_write((int)a0, (const void *)(uintptr_t)a1, (size_t)a2);

    case 56:  /* __NR_openat: openat(dirfd, path, flags, mode) */
        /* Treat dirfd as AT_FDCWD (-100) for now; forward to sys_open */
        return sys_open((const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case 57:  /* __NR_close */
        return sys_close((int)a0);

    case 62:  /* __NR_lseek */
        return sys_lseek((int)a0, (long)a1, (int)a2);

    case 80:  /* __NR_fstat */
        return sys_fstat((int)a0, (void *)(uintptr_t)a1);

    case 29:  /* __NR_ioctl */
        return stub_not_implemented(nr);

    case 23:  /* __NR_dup */
    case 24:  /* __NR_dup3 */
    case 25:  /* __NR_fcntl */
    case 59:  /* __NR_pipe2 */
    case 61:  /* __NR_getdents64 */
    case 17:  /* __NR_getcwd */
    case 49:  /* __NR_chdir */
    case 79:  /* __NR_newfstatat */
        return stub_not_implemented(nr);

    /* ---- Memory ------------------------------------------------------ */
    case 214: /* __NR_brk */
        return sys_brk((void *)(uintptr_t)a0);

    case 222: /* __NR_mmap */
        return sys_mmap((void *)(uintptr_t)a0, (size_t)a1,
                        (int)a2, (int)a3, (int)a4, (long)a5);

    case 215: /* __NR_munmap */
        return sys_munmap((void *)(uintptr_t)a0, (size_t)a1);

    case 226: /* __NR_mprotect */
        return sys_mprotect((void *)(uintptr_t)a0, (size_t)a1, (int)a2);

    case 233: /* __NR_madvise — accept and ignore */
        return 0;

    /* ---- Process management ------------------------------------------ */
    case 172: /* __NR_getpid */
        return sys_getpid();

    case 173: /* __NR_getppid */
        return sys_getppid();

    case 174: /* __NR_getuid */
        return sys_getuid();

    case 175: /* __NR_geteuid */
        return sys_geteuid();

    case 176: /* __NR_getgid */
        return sys_getgid();

    case 177: /* __NR_getegid */
        return sys_getegid();

    case 178: /* __NR_gettid */
        return sys_getpid();   /* simplified: TID == PID for single-threaded */

    case 93:  /* __NR_exit */
        sys_exit((int)a0);
        /* noreturn */

    case 94:  /* __NR_exit_group */
        sys_exit_group((int)a0);
        /* noreturn */

    case 129: /* __NR_kill */
        return sys_kill((int)a0, (int)a1);

    case 130: /* __NR_tkill */
        return sys_kill((int)a0, (int)a1);

    case 131: /* __NR_tgkill */
        return sys_kill((int)a1, (int)a2);

    case 220: /* __NR_clone — simplified to fork */
    case 435: /* __NR_clone3 */
        return stub_not_implemented(nr);

    case 221: /* __NR_execve */
        return sys_execve((const char *)(uintptr_t)a0,
                          (char *const *)(uintptr_t)a1,
                          (char *const *)(uintptr_t)a2);

    case 260: /* __NR_wait4 */
    case 95:  /* __NR_waitid */
        return stub_not_implemented(nr);

    /* ---- Credentials (return root for all) --------------------------- */
    case 143: /* __NR_setuid  */
    case 144: /* __NR_setgid  */
    case 145: /* __NR_setreuid */
    case 146: /* __NR_setregid */
    case 147: /* __NR_setresuid */
    case 149: /* __NR_setresgid */
    case 151: /* __NR_setfsuid */
    case 152: /* __NR_setfsgid */
    case 155: /* __NR_setpgid */
    case 157: /* __NR_setsid */
    case 159: /* __NR_setgroups */
        return 0;

    case 148: /* __NR_getresuid */
    case 150: /* __NR_getresgid */ {
        uint32_t *p = (uint32_t *)(uintptr_t)a0;
        if (p) { p[0] = 0; p[1] = 0; p[2] = 0; }
        return 0;
    }

    case 158: /* __NR_getgroups */
        return 0;

    case 155: /* __NR_getpgid */
    case 156: /* __NR_getsid */
        return sys_getpid();

    case 166: /* __NR_umask */
        return 0022;

    /* ---- Time --------------------------------------------------------- */
    case 113: /* __NR_clock_gettime */
        return sys_clock_gettime((int)a0, (void *)(uintptr_t)a1);

    case 114: /* __NR_clock_getres */
        return sys_clock_getres((int)a0, (void *)(uintptr_t)a1);

    case 115: /* __NR_clock_nanosleep */
        /* flags=a1, req=a2, rem=a3 */
        return sys_nanosleep((const void *)(uintptr_t)a2,
                             (void *)(uintptr_t)a3);

    case 101: /* __NR_nanosleep */
        return sys_nanosleep((const void *)(uintptr_t)a0,
                             (void *)(uintptr_t)a1);

    case 169: /* __NR_gettimeofday */
        return sys_gettimeofday((void *)(uintptr_t)a0,
                                (void *)(uintptr_t)a1);

    /* ---- System information ------------------------------------------ */
    case 160: /* __NR_uname */
        return sys_uname((void *)(uintptr_t)a0);

    /* ---- Signals (minimal) ------------------------------------------- */
    case 134: /* __NR_rt_sigaction    */
    case 135: /* __NR_rt_sigprocmask */
    case 139: /* __NR_rt_sigreturn   */
    case 132: /* __NR_sigaltstack    */
        return stub_not_implemented(nr);

    /* ---- Scheduling (accept-silently for single-core kernel) ---------- */
    case 118: /* __NR_sched_setparam     */
    case 119: /* __NR_sched_setscheduler */
    case 120: /* __NR_sched_getscheduler */
    case 121: /* __NR_sched_getparam     */
    case 122: /* __NR_sched_setaffinity  */
    case 123: /* __NR_sched_getaffinity  */
    case 124: /* __NR_sched_yield        */
    case 125: /* __NR_sched_get_priority_max */
    case 126: /* __NR_sched_get_priority_min */
    case 274: /* __NR_sched_setattr      */
    case 275: /* __NR_sched_getattr      */
        return 0;

    /* ---- Futex / robust list ----------------------------------------- */
    case 98:  /* __NR_futex */
    case 99:  /* __NR_set_robust_list */
    case 100: /* __NR_get_robust_list */
        return 0;

    /* ---- Networking (stubs) ------------------------------------------ */
    case 198: /* __NR_socket  */
    case 200: /* __NR_bind    */
    case 201: /* __NR_listen  */
    case 202: /* __NR_accept  */
    case 203: /* __NR_connect */
    case 206: /* __NR_sendto  */
    case 207: /* __NR_recvfrom */
    case 208: /* __NR_setsockopt */
    case 209: /* __NR_getsockopt */
    case 210: /* __NR_shutdown */
    case 211: /* __NR_sendmsg */
    case 212: /* __NR_recvmsg */
        return stub_not_implemented(nr);

    /* ---- epoll / eventfd / inotify / timerfd (stubs) ----------------- */
    case 19:  /* __NR_eventfd2       */
    case 20:  /* __NR_epoll_create1  */
    case 21:  /* __NR_epoll_ctl      */
    case 22:  /* __NR_epoll_pwait    */
    case 26:  /* __NR_inotify_init1  */
    case 27:  /* __NR_inotify_add_watch */
    case 28:  /* __NR_inotify_rm_watch  */
    case 85:  /* __NR_timerfd_create */
    case 86:  /* __NR_timerfd_settime */
    case 87:  /* __NR_timerfd_gettime */
    case 74:  /* __NR_signalfd4      */
        return stub_not_implemented(nr);

    /* ---- Misc --------------------------------------------------------- */
    case 167: /* __NR_prctl */
    case 92:  /* __NR_personality → report PER_LINUX */
        return 0;

    case 96:  /* __NR_set_tid_address */
        return 0;

    case 278: /* __NR_getrandom */
        return stub_not_implemented(nr);

    case 291: /* __NR_statx */
        return stub_not_implemented(nr);

    /* ---- Unimplemented default --------------------------------------- */
    default:
        return stub_not_implemented(nr);
    }
}

/* -----------------------------------------------------------------------
 * aarch64_syscall_dispatch
 *
 * Legacy entry point used by exceptions.S.
 * Argument order: x0-x7 are the first eight parameters, x8 = nr last.
 * Bridges to aarch64_syscall_handle() which takes nr first.
 * --------------------------------------------------------------------- */
long aarch64_syscall_dispatch(uint64_t x0, uint64_t x1, uint64_t x2,
                               uint64_t x3, uint64_t x4, uint64_t x5,
                               uint64_t x6, uint64_t x7, uint64_t nr)
{
    (void)x7;
    return (long)aarch64_syscall_handle(nr, x0, x1, x2, x3, x4, x5, x6);
}

/* -----------------------------------------------------------------------
 * aarch64_syscall_init
 * --------------------------------------------------------------------- */
void aarch64_syscall_init(void)
{
    uart_puts("[syscall] AArch64 syscall dispatcher ready\n");
    /*
     * The SVC exception is caught by the vector table installed in boot.S
     * (VBAR_EL1 = &vectors).  The lower-EL AArch64 sync handler checks
     * ESR_EL1.EC == 0x15 and dispatches to aarch64_syscall_handle().
     * No further initialisation is required here.
     */
}
