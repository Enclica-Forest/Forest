/*
 * syscalls.c - System call implementations for Fern libc
 * 
 * This file provides the low-level system call mechanism that acts as a
 * middleman between userspace applications and the Fern kernel.
 * 
 * The libc syscall layer provides:
 * - Architecture abstraction (handles x86, x86_64 differences)
 * - Error translation (kernel errors -> errno)
 * - Data type conversions
 * - POSIX-compatible function signatures
 * 
 * Applications call libc functions (read, write, open, etc.) instead of
 * making raw syscalls. This allows:
 * - Portability across different kernel versions
 * - Consistent error handling via errno
 * - Future optimizations without changing application code
 */

#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/syscall.h>

/* ============================================================================
 * ARCHITECTURE-SPECIFIC SYSCALL MECHANISM
 * 
 * The kernel interface differs between 32-bit and 64-bit systems.
 * We use inline assembly to issue the int 0x80 interrupt which transfers
 * control to the kernel's syscall handler.
 * 
 * Register conventions for Fern (Linux compatible):
 * - eax/rax: syscall number (input) / return value (output)
 * - ebx/rdi: argument 1
 * - ecx/rsi: argument 2
 * - edx/rdx: argument 3
 * - esi/r10: argument 4
 * - edi/r8:  argument 5
 * - ebp/r9:  argument 6
 * ============================================================================ */

/* Return type for syscalls (can be negative on error) */
typedef long syscall_ret_t;

/* Argument type for syscalls */
typedef unsigned long syscall_arg_t;

/*
 * Internal syscall functions for different argument counts.
 * These use inline assembly to invoke the kernel.
 */

#if ARCH_64BIT

/* 64-bit syscall implementations using int 0x80 */

static inline syscall_ret_t __syscall0(syscall_arg_t num) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall1(syscall_arg_t num, syscall_arg_t a1) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall2(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall3(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2, syscall_arg_t a3) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall4(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2, 
                                       syscall_arg_t a3, syscall_arg_t a4) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall5(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2,
                                       syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall6(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2,
                                       syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5,
                                       syscall_arg_t a6) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "push %%rbp\n"
        "mov %[arg6], %%rbp\n"
        "int $0x80\n"
        "pop %%rbp\n"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), [arg6]"r"(a6)
        : "memory"
    );
    return ret;
}

#else /* 32-bit */

/* 32-bit syscall implementations using int 0x80 */

static inline syscall_ret_t __syscall0(syscall_arg_t num) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall1(syscall_arg_t num, syscall_arg_t a1) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall2(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall3(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2, syscall_arg_t a3) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall4(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2,
                                       syscall_arg_t a3, syscall_arg_t a4) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall5(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2,
                                       syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
        : "memory"
    );
    return ret;
}

static inline syscall_ret_t __syscall6(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2,
                                       syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5,
                                       syscall_arg_t a6) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "push %%ebp\n"
        "mov %7, %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), "g"(a6)
        : "memory"
    );
    return ret;
}

#endif /* ARCH_64BIT */

/* ============================================================================
 * ERROR HANDLING AND ERRNO TRANSLATION
 * 
 * The kernel returns negative values on error (e.g., -ENOENT).
 * We translate these to the POSIX convention:
 * - On success: return the actual value (>= 0)
 * - On error: return -1 and set errno to the positive error code
 * ============================================================================ */

/*
 * Convert kernel return value to libc convention.
 * Negative values indicate errors; set errno and return -1.
 */
static inline long __syscall_ret(syscall_ret_t ret) {
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (long)ret;
}

/*
 * Same as above but returns a pointer (for mmap, etc.)
 */
static inline void *__syscall_ret_ptr(syscall_ret_t ret) {
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return (void *)-1;
    }
    return (void *)ret;
}

/* ============================================================================
 * GENERIC SYSCALL FUNCTION
 * 
 * This allows applications to make arbitrary syscalls if needed.
 * Usage: syscall(SYS_xxx, arg1, arg2, ...)
 * ============================================================================ */

long syscall(long number, ...) {
    va_list ap;
    syscall_arg_t args[6];
    
    va_start(ap, number);
    for (int i = 0; i < 6; i++) {
        args[i] = va_arg(ap, syscall_arg_t);
    }
    va_end(ap);
    
    return __syscall_ret(__syscall6(number, args[0], args[1], args[2], 
                                    args[3], args[4], args[5]));
}

/* ============================================================================
 * FILE I/O SYSCALLS
 * 
 * These are the core file operations that most applications use.
 * ============================================================================ */

ssize_t read(int fd, void *buf, size_t count) {
    return __syscall_ret(__syscall3(SYS_read, fd, (syscall_arg_t)buf, count));
}

ssize_t write(int fd, const void *buf, size_t count) {
    return __syscall_ret(__syscall3(SYS_write, fd, (syscall_arg_t)buf, count));
}

int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & 0x40) { /* O_CREAT */
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return __syscall_ret(__syscall3(SYS_open, (syscall_arg_t)pathname, flags, mode));
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & 0x40) { /* O_CREAT */
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return __syscall_ret(__syscall4(SYS_openat, dirfd, (syscall_arg_t)pathname, flags, mode));
}

int close(int fd) {
    return __syscall_ret(__syscall1(SYS_close, fd));
}

off_t lseek(int fd, off_t offset, int whence) {
    return __syscall_ret(__syscall3(SYS_lseek, fd, offset, whence));
}

int dup(int oldfd) {
    return __syscall_ret(__syscall1(SYS_dup, oldfd));
}

int dup2(int oldfd, int newfd) {
    return __syscall_ret(__syscall2(SYS_dup2, oldfd, newfd));
}

int dup3(int oldfd, int newfd, int flags) {
    return __syscall_ret(__syscall3(SYS_dup3, oldfd, newfd, flags));
}

int pipe(int pipefd[2]) {
    return __syscall_ret(__syscall1(SYS_pipe, (syscall_arg_t)pipefd));
}

int pipe2(int pipefd[2], int flags) {
    return __syscall_ret(__syscall2(SYS_pipe2, (syscall_arg_t)pipefd, flags));
}

/* ============================================================================
 * FILE STATUS SYSCALLS
 * ============================================================================ */

int stat(const char *pathname, struct stat *statbuf) {
    return __syscall_ret(__syscall2(SYS_stat, (syscall_arg_t)pathname, (syscall_arg_t)statbuf));
}

int fstat(int fd, struct stat *statbuf) {
    return __syscall_ret(__syscall2(SYS_fstat, fd, (syscall_arg_t)statbuf));
}

int lstat(const char *pathname, struct stat *statbuf) {
    return __syscall_ret(__syscall2(SYS_lstat, (syscall_arg_t)pathname, (syscall_arg_t)statbuf));
}

int access(const char *pathname, int mode) {
    return __syscall_ret(__syscall2(SYS_access, (syscall_arg_t)pathname, mode));
}

/* ============================================================================
 * FILE CONTROL SYSCALLS
 * ============================================================================ */

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    syscall_arg_t arg = va_arg(ap, syscall_arg_t);
    va_end(ap);
    return __syscall_ret(__syscall3(SYS_fcntl, fd, cmd, arg));
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    syscall_arg_t arg = va_arg(ap, syscall_arg_t);
    va_end(ap);
    return __syscall_ret(__syscall3(SYS_ioctl, fd, request, arg));
}

int fsync(int fd) {
    return __syscall_ret(__syscall1(SYS_fsync, fd));
}

int fdatasync(int fd) {
    return __syscall_ret(__syscall1(SYS_fdatasync, fd));
}

int ftruncate(int fd, off_t length) {
    return __syscall_ret(__syscall2(SYS_ftruncate, fd, length));
}

int truncate(const char *path, off_t length) {
    return __syscall_ret(__syscall2(SYS_truncate, (syscall_arg_t)path, length));
}

/* ============================================================================
 * DIRECTORY SYSCALLS
 * ============================================================================ */

int mkdir(const char *pathname, mode_t mode) {
    return __syscall_ret(__syscall2(SYS_mkdir, (syscall_arg_t)pathname, mode));
}

int rmdir(const char *pathname) {
    return __syscall_ret(__syscall1(SYS_rmdir, (syscall_arg_t)pathname));
}

int chdir(const char *path) {
    return __syscall_ret(__syscall1(SYS_chdir, (syscall_arg_t)path));
}

int fchdir(int fd) {
    return __syscall_ret(__syscall1(SYS_fchdir, fd));
}

char *getcwd(char *buf, size_t size) {
    long ret = __syscall2(SYS_getcwd, (syscall_arg_t)buf, size);
    if (ret < 0) {
        errno = (int)(-ret);
        return NULL;
    }
    return buf;
}

ssize_t getdents(int fd, void *dirp, size_t count) {
    return __syscall_ret(__syscall3(SYS_getdents, fd, (syscall_arg_t)dirp, count));
}

ssize_t getdents64(int fd, void *dirp, size_t count) {
    return __syscall_ret(__syscall3(SYS_getdents64, fd, (syscall_arg_t)dirp, count));
}

/* ============================================================================
 * LINK/UNLINK SYSCALLS
 * ============================================================================ */

int link(const char *oldpath, const char *newpath) {
    return __syscall_ret(__syscall2(SYS_link, (syscall_arg_t)oldpath, (syscall_arg_t)newpath));
}

int unlink(const char *pathname) {
    return __syscall_ret(__syscall1(SYS_unlink, (syscall_arg_t)pathname));
}

int symlink(const char *target, const char *linkpath) {
    return __syscall_ret(__syscall2(SYS_symlink, (syscall_arg_t)target, (syscall_arg_t)linkpath));
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    return __syscall_ret(__syscall3(SYS_readlink, (syscall_arg_t)pathname, (syscall_arg_t)buf, bufsiz));
}

int rename(const char *oldpath, const char *newpath) {
    return __syscall_ret(__syscall2(SYS_rename, (syscall_arg_t)oldpath, (syscall_arg_t)newpath));
}

/* ============================================================================
 * PERMISSION SYSCALLS
 * ============================================================================ */

int chmod(const char *pathname, mode_t mode) {
    return __syscall_ret(__syscall2(SYS_chmod, (syscall_arg_t)pathname, mode));
}

int fchmod(int fd, mode_t mode) {
    return __syscall_ret(__syscall2(SYS_fchmod, fd, mode));
}

int chown(const char *pathname, uid_t owner, gid_t group) {
    return __syscall_ret(__syscall3(SYS_chown, (syscall_arg_t)pathname, owner, group));
}

int fchown(int fd, uid_t owner, gid_t group) {
    return __syscall_ret(__syscall3(SYS_fchown, fd, owner, group));
}

int lchown(const char *pathname, uid_t owner, gid_t group) {
    return __syscall_ret(__syscall3(SYS_lchown, (syscall_arg_t)pathname, owner, group));
}

mode_t umask(mode_t mask) {
    return __syscall1(SYS_umask, mask);  /* umask never fails */
}

/* ============================================================================
 * MEMORY MANAGEMENT SYSCALLS
 * ============================================================================ */

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return __syscall_ret_ptr(__syscall6(SYS_mmap, (syscall_arg_t)addr, length, prot, flags, fd, offset));
}

int munmap(void *addr, size_t length) {
    return __syscall_ret(__syscall2(SYS_munmap, (syscall_arg_t)addr, length));
}

int mprotect(void *addr, size_t len, int prot) {
    return __syscall_ret(__syscall3(SYS_mprotect, (syscall_arg_t)addr, len, prot));
}

int msync(void *addr, size_t length, int flags) {
    return __syscall_ret(__syscall3(SYS_msync, (syscall_arg_t)addr, length, flags));
}

int madvise(void *addr, size_t length, int advice) {
    return __syscall_ret(__syscall3(SYS_madvise, (syscall_arg_t)addr, length, advice));
}

int mlock(const void *addr, size_t len) {
    return __syscall_ret(__syscall2(SYS_mlock, (syscall_arg_t)addr, len));
}

int munlock(const void *addr, size_t len) {
    return __syscall_ret(__syscall2(SYS_munlock, (syscall_arg_t)addr, len));
}

int mlockall(int flags) {
    return __syscall_ret(__syscall1(SYS_mlockall, flags));
}

int munlockall(void) {
    return __syscall_ret(__syscall0(SYS_munlockall));
}

int brk(void *addr) {
    return __syscall_ret(__syscall1(SYS_brk, (syscall_arg_t)addr));
}

/* ============================================================================
 * PROCESS CONTROL SYSCALLS
 * ============================================================================ */

pid_t getpid(void) {
    return __syscall0(SYS_getpid);  /* getpid never fails */
}

pid_t getppid(void) {
    return __syscall0(SYS_getppid);  /* getppid never fails */
}

pid_t getpgrp(void) {
    return __syscall0(SYS_getpgrp);  /* getpgrp never fails */
}

int setpgid(pid_t pid, pid_t pgid) {
    return __syscall_ret(__syscall2(SYS_setpgid, (syscall_arg_t)pid, (syscall_arg_t)pgid));
}

pid_t setsid(void) {
    return __syscall_ret(__syscall0(SYS_setsid));
}

pid_t getsid(pid_t pid) {
    return __syscall_ret(__syscall1(SYS_getsid, (syscall_arg_t)pid));
}

pid_t getpgid(pid_t pid) {
    return __syscall_ret(__syscall1(SYS_getpgid, (syscall_arg_t)pid));
}

pid_t tcgetpgrp(int fd) {
    return __syscall_ret(__syscall1(127, (syscall_arg_t)fd));  // SYS_TCGETPGRP
}

int tcsetpgrp(int fd, pid_t pgrp) {
    return __syscall_ret(__syscall2(128, (syscall_arg_t)fd, (syscall_arg_t)pgrp));  // SYS_TCSETPGRP
}

pid_t gettid(void) {
    return __syscall0(SYS_gettid);  /* gettid never fails */
}

pid_t fork(void) {
    return __syscall_ret(__syscall0(SYS_fork));
}

pid_t vfork(void) {
    return __syscall_ret(__syscall0(SYS_vfork));
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    return __syscall_ret(__syscall3(SYS_execve, (syscall_arg_t)pathname, 
                                    (syscall_arg_t)argv, (syscall_arg_t)envp));
}

void _exit(int status) {
    __syscall1(SYS_exit, status);
    __builtin_unreachable();
}

void _Exit(int status) {
    _exit(status);
}

pid_t wait(int *wstatus) {
    return __syscall_ret(__syscall4(SYS_wait4, -1, (syscall_arg_t)wstatus, 0, 0));
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
    return __syscall_ret(__syscall4(SYS_wait4, pid, (syscall_arg_t)wstatus, options, 0));
}

int kill(pid_t pid, int sig) {
    return __syscall_ret(__syscall2(SYS_kill, pid, sig));
}

/* ============================================================================
 * USER/GROUP IDENTIFICATION SYSCALLS
 * ============================================================================ */

uid_t getuid(void) {
    return __syscall0(SYS_getuid);
}

uid_t geteuid(void) {
    return __syscall0(SYS_geteuid);
}

gid_t getgid(void) {
    return __syscall0(SYS_getgid);
}

gid_t getegid(void) {
    return __syscall0(SYS_getegid);
}

int setuid(uid_t uid) {
    return __syscall_ret(__syscall1(SYS_setuid, uid));
}

int setgid(gid_t gid) {
    return __syscall_ret(__syscall1(SYS_setgid, gid));
}

int seteuid(uid_t euid) {
    return __syscall_ret(__syscall1(SYS_setreuid, -1, euid));
}

int setegid(gid_t egid) {
    return __syscall_ret(__syscall1(SYS_setregid, -1, egid));
}

int setreuid(uid_t ruid, uid_t euid) {
    return __syscall_ret(__syscall2(SYS_setreuid, ruid, euid));
}

int setregid(gid_t rgid, gid_t egid) {
    return __syscall_ret(__syscall2(SYS_setregid, rgid, egid));
}

pid_t getpgrp(void) {
    return __syscall0(SYS_getpgrp);
}

pid_t getpgid(pid_t pid) {
    return __syscall_ret(__syscall1(SYS_getpgid, pid));
}

int setpgid(pid_t pid, pid_t pgid) {
    return __syscall_ret(__syscall2(SYS_setpgid, pid, pgid));
}

pid_t setsid(void) {
    return __syscall_ret(__syscall0(SYS_setsid));
}

pid_t getsid(pid_t pid) {
    return __syscall_ret(__syscall1(SYS_getsid, pid));
}

/* ============================================================================
 * TIME SYSCALLS
 * ============================================================================ */

time_t time(time_t *tloc) {
    return __syscall_ret(__syscall1(SYS_time, (syscall_arg_t)tloc));
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    return __syscall_ret(__syscall2(SYS_gettimeofday, (syscall_arg_t)tv, (syscall_arg_t)tz));
}

int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    return __syscall_ret(__syscall2(SYS_settimeofday, (syscall_arg_t)tv, (syscall_arg_t)tz));
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    return __syscall_ret(__syscall2(SYS_clock_gettime, clk_id, (syscall_arg_t)tp));
}

int clock_settime(clockid_t clk_id, const struct timespec *tp) {
    return __syscall_ret(__syscall2(SYS_clock_settime, clk_id, (syscall_arg_t)tp));
}

int clock_getres(clockid_t clk_id, struct timespec *res) {
    return __syscall_ret(__syscall2(SYS_clock_getres, clk_id, (syscall_arg_t)res));
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return __syscall_ret(__syscall2(SYS_nanosleep, (syscall_arg_t)req, (syscall_arg_t)rem));
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
    struct timespec rem;
    if (nanosleep(&req, &rem) < 0) {
        return rem.tv_sec;
    }
    return 0;
}

int usleep(useconds_t usec) {
    struct timespec req = {
        .tv_sec = usec / 1000000,
        .tv_nsec = (usec % 1000000) * 1000
    };
    return nanosleep(&req, NULL);
}

unsigned int alarm(unsigned int seconds) {
    return __syscall_ret(__syscall1(SYS_alarm, seconds));
}

int pause(void) {
    return __syscall_ret(__syscall0(SYS_pause));
}

/* ============================================================================
 * SIGNAL SYSCALLS
 * ============================================================================ */

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return __syscall_ret(__syscall4(SYS_rt_sigaction, signum, (syscall_arg_t)act, 
                                    (syscall_arg_t)oldact, sizeof(sigset_t)));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return __syscall_ret(__syscall4(SYS_rt_sigprocmask, how, (syscall_arg_t)set,
                                    (syscall_arg_t)oldset, sizeof(sigset_t)));
}

int sigpending(sigset_t *set) {
    return __syscall_ret(__syscall2(SYS_rt_sigpending, (syscall_arg_t)set, sizeof(sigset_t)));
}

int sigsuspend(const sigset_t *mask) {
    return __syscall_ret(__syscall2(SYS_rt_sigsuspend, (syscall_arg_t)mask, sizeof(sigset_t)));
}

/* ============================================================================
 * SOCKET SYSCALLS
 * ============================================================================ */

int socket(int domain, int type, int protocol) {
    return __syscall_ret(__syscall3(SYS_socket, domain, type, protocol));
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return __syscall_ret(__syscall3(SYS_bind, sockfd, (syscall_arg_t)addr, addrlen));
}

int listen(int sockfd, int backlog) {
    return __syscall_ret(__syscall2(SYS_listen, sockfd, backlog));
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return __syscall_ret(__syscall3(SYS_accept, sockfd, (syscall_arg_t)addr, (syscall_arg_t)addrlen));
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    return __syscall_ret(__syscall4(SYS_accept4, sockfd, (syscall_arg_t)addr, (syscall_arg_t)addrlen, flags));
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return __syscall_ret(__syscall3(SYS_connect, sockfd, (syscall_arg_t)addr, addrlen));
}

int shutdown(int sockfd, int how) {
    return __syscall_ret(__syscall2(SYS_shutdown, sockfd, how));
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return __syscall_ret(__syscall6(SYS_sendto, sockfd, (syscall_arg_t)buf, len, flags, 0, 0));
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return __syscall_ret(__syscall6(SYS_recvfrom, sockfd, (syscall_arg_t)buf, len, flags, 0, 0));
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return __syscall_ret(__syscall6(SYS_sendto, sockfd, (syscall_arg_t)buf, len, flags,
                                    (syscall_arg_t)dest_addr, addrlen));
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return __syscall_ret(__syscall6(SYS_recvfrom, sockfd, (syscall_arg_t)buf, len, flags,
                                    (syscall_arg_t)src_addr, (syscall_arg_t)addrlen));
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    return __syscall_ret(__syscall5(SYS_getsockopt, sockfd, level, optname, 
                                    (syscall_arg_t)optval, (syscall_arg_t)optlen));
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    return __syscall_ret(__syscall5(SYS_setsockopt, sockfd, level, optname,
                                    (syscall_arg_t)optval, optlen));
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return __syscall_ret(__syscall3(SYS_getsockname, sockfd, (syscall_arg_t)addr, (syscall_arg_t)addrlen));
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return __syscall_ret(__syscall3(SYS_getpeername, sockfd, (syscall_arg_t)addr, (syscall_arg_t)addrlen));
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    return __syscall_ret(__syscall4(SYS_socketpair, domain, type, protocol, (syscall_arg_t)sv));
}

/* ============================================================================
 * SYSTEM INFORMATION SYSCALLS
 * ============================================================================ */

int uname(struct utsname *buf) {
    return __syscall_ret(__syscall1(SYS_uname, (syscall_arg_t)buf));
}

int gethostname(char *name, size_t len) {
    struct utsname uts;
    if (uname(&uts) < 0) {
        return -1;
    }
    size_t hostname_len = 0;
    while (hostname_len < sizeof(uts.nodename) && uts.nodename[hostname_len]) {
        hostname_len++;
    }
    if (hostname_len >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 0; i <= hostname_len; i++) {
        name[i] = uts.nodename[i];
    }
    return 0;
}

int sethostname(const char *name, size_t len) {
    return __syscall_ret(__syscall2(SYS_sethostname, (syscall_arg_t)name, len));
}

/* ============================================================================
 * SCHEDULING SYSCALLS
 * ============================================================================ */

int sched_yield(void) {
    return __syscall_ret(__syscall0(SYS_sched_yield));
}

int nice(int inc) {
    int prio = __syscall2(SYS_getpriority, 0, 0);  /* PRIO_PROCESS */
    if (prio < 0 && prio > -4096) {
        errno = -prio;
        return -1;
    }
    return __syscall_ret(__syscall3(SYS_setpriority, 0, 0, prio + inc));
}

/* ============================================================================
 * MOUNT/FILESYSTEM SYSCALLS
 * ============================================================================ */

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    return __syscall_ret(__syscall5(SYS_mount, (syscall_arg_t)source, (syscall_arg_t)target,
                                    (syscall_arg_t)fstype, flags, (syscall_arg_t)data));
}

int umount(const char *target) {
    return __syscall_ret(__syscall2(SYS_umount2, (syscall_arg_t)target, 0));
}

int umount2(const char *target, int flags) {
    return __syscall_ret(__syscall2(SYS_umount2, (syscall_arg_t)target, flags));
}

void sync(void) {
    __syscall0(SYS_sync);
}

int chroot(const char *path) {
    return __syscall_ret(__syscall1(SYS_chroot, (syscall_arg_t)path));
}

/* ============================================================================
 * MISCELLANEOUS SYSCALLS
 * ============================================================================ */

int mknod(const char *pathname, mode_t mode, dev_t dev) {
    return __syscall_ret(__syscall3(SYS_mknod, (syscall_arg_t)pathname, mode, dev));
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return __syscall_ret(__syscall3(SYS_poll, (syscall_arg_t)fds, nfds, timeout));
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    return __syscall_ret(__syscall5(SYS_select, nfds, (syscall_arg_t)readfds, 
                                    (syscall_arg_t)writefds, (syscall_arg_t)exceptfds,
                                    (syscall_arg_t)timeout));
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return __syscall_ret(__syscall3(SYS_readv, fd, (syscall_arg_t)iov, iovcnt));
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return __syscall_ret(__syscall3(SYS_writev, fd, (syscall_arg_t)iov, iovcnt));
}

int getrandom(void *buf, size_t buflen, unsigned int flags) {
    return __syscall_ret(__syscall3(SYS_getrandom, (syscall_arg_t)buf, buflen, flags));
}

int reboot(int cmd) {
    /* Magic numbers required by Linux reboot syscall */
    return __syscall_ret(__syscall4(SYS_reboot, 0xfee1dead, 0x28121969, cmd, 0));
}

/* ============================================================================
 * FOREST OS SPECIFIC SYSCALLS
 * 
 * These are extensions for Fern specific functionality.
 * ============================================================================ */

int poweroff(void) {
    return __syscall_ret(__syscall1(SYS_power, 0));  /* POWER_ACTION_SHUTDOWN */
}

void *mmap_fb(size_t *width, size_t *height, size_t *pitch) {
    /* Get framebuffer info first */
    struct {
        size_t width;
        size_t height;
        size_t pitch;
        size_t bpp;
        void *address;
    } fb_info;
    
    long ret = __syscall1(SYS_get_fb_info, (syscall_arg_t)&fb_info);
    if (ret < 0) {
        errno = -ret;
        return NULL;
    }
    
    void *fb = __syscall_ret_ptr(__syscall1(SYS_mmap_fb, (syscall_arg_t)&fb_info));
    if (fb == (void *)-1) {
        return NULL;
    }
    
    if (width) *width = fb_info.width;
    if (height) *height = fb_info.height;
    if (pitch) *pitch = fb_info.pitch;
    
    return fb;
}

int munmap_fb(void *addr) {
    return __syscall_ret(__syscall1(SYS_munmap_fb, (syscall_arg_t)addr));
}

int start_fb_watcher(void) {
    return __syscall_ret(__syscall0(SYS_start_fb_watcher));
}

int stop_fb_watcher(void) {
    return __syscall_ret(__syscall0(SYS_stop_fb_watcher));
}

int fb_flush(void) {
    return __syscall_ret(__syscall0(SYS_fb_flush));
}

ssize_t read_kbd_event(void *event) {
    return __syscall1(SYS_read_kbd_event, (syscall_arg_t)event);
}

ssize_t read_mouse_event(void *event) {
    return __syscall1(SYS_read_mouse_event, (syscall_arg_t)event);
}

int poll_input(void) {
    return __syscall0(SYS_poll_input);
}

int netinfo(void *buffer, int max_entries) {
    return __syscall_ret(__syscall2(SYS_netinfo, (syscall_arg_t)buffer, max_entries));
}

int user_syscall(int op, const char *user, const char *pass, const char *aux,
                 void *out, int max_entries) {
    return __syscall_ret(__syscall6(SYS_userctl, op, (syscall_arg_t)user, (syscall_arg_t)pass,
                                    (syscall_arg_t)aux, (syscall_arg_t)out, max_entries));
}
