/*
 * Fern - RISC-V 64-bit Syscall Dispatch
 *
 * Translates RISC-V 64-bit Linux syscall numbers to Fern internal handlers.
 * The calling convention is the standard RISC-V Linux ABI:
 *   a7  = syscall number
 *   a0-a5 = arguments
 *   a0  = return value
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
 *   riscv64_syscall_dispatch() - legacy signature used by trap entry.S
 *     (a0-a5 = args in order, a7 = nr last; returns long)
 *
 *   riscv64_syscall_handle() - new signature used by trap vectors
 *     (nr = first arg, then a0-a5; returns int64_t)
 *     This is the primary entry point for new code.
 */

#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Forward declarations from the platform-independent Fern layer.
 *
 * These resolve to the implementations in src/syscall.c (x86/generic)
 * or their RISC-V-specific equivalents as the port matures.
 * --------------------------------------------------------------------- */

/* VFS / file I/O */
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
extern long sys_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
extern long sys_access(const char *pathname, int mode);
extern long sys_faccessat(int dirfd, const char *pathname, int mode, int flags);
extern long sys_unlink(const char *pathname);
extern long sys_mkdir(const char *pathname, int mode);
extern long sys_rmdir(const char *pathname);
extern long sys_rename(const char *oldpath, const char *newpath);
extern long sys_link(const char *oldpath, const char *newpath);
extern long sys_symlink(const char *target, const char *linkpath);
extern long sys_chmod(const char *pathname, int mode);
extern long sys_fchmod(int fd, int mode);
extern long sys_chown(const char *pathname, int owner, int group);
extern long sys_fchown(int fd, int owner, int group);
extern long sys_umask(int mode);
extern long sys_fsync(int fd);
extern long sys_ftruncate(int fd, long length);
extern long sys_statfs(const char *path, void *buf);
extern long sys_fstatfs(int fd, void *buf);
extern long sys_fchownat(int dirfd, const char *pathname, int owner, int group, int flags);
extern long sys_fchmodat(int dirfd, const char *pathname, int mode, int flags);
extern long sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
extern long sys_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
extern long sys_unlinkat(int dirfd, const char *pathname, int flags);
extern long sys_mknod(const char *pathname, int mode, int dev);
extern long sys_mkdirat(int dirfd, const char *pathname, int mode);
extern long sys_utimensat(int dirfd, const char *pathname, const void *times, int flags);
extern long sys_close_range(unsigned int first, unsigned int last, unsigned int flags);
extern long sys_getrandom(void *buf, size_t count, unsigned int flags);
extern long sys_statx(int dirfd, const char *pathname, unsigned flags, unsigned mask, void *statxbuf);
extern long sys_copy_file_range(int fd_in, long *off_in, int fd_out, long *off_out, size_t len, unsigned int flags);
extern long sys_ppoll(void *fds, unsigned int nfds, const void *timeout, const void *sigmask, size_t sigsetsize);
extern long sys_fadvise64(int fd, long offset, long len, int advice);
extern long sys_memfd_create(const char *name, unsigned int flags);
extern long sys_futex(uint32_t *uaddr, int op, int val, const void *timeout, uint32_t *uaddr2, int val3);

/* Memory */
extern long sys_brk(void *addr);
extern long sys_mmap(void *addr, size_t length, int prot, int flags,
                     int fd, long offset);
extern long sys_munmap(void *addr, size_t length);
extern long sys_mprotect(void *addr, size_t length, int prot);
extern long sys_madvise(void *addr, size_t length, int advice);
extern long sys_mlock(const void *addr, size_t length);
extern long sys_munlock(const void *addr, size_t length);
extern long sys_mlockall(int flags);
extern long sys_munlockall(void);
extern long sys_mincore(void *addr, size_t length, unsigned char *vec);
extern long sys_mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address);
extern long sys_msync(void *addr, size_t length, int flags);

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
extern long sys_wait4(int pid, int *status, int options, void *rusage);
extern long sys_set_tid_address(int *tidptr);
extern long sys_clone(unsigned long flags, void *stack, int *parent_tid,
                      int *child_tid, unsigned long tls);

/* Credentials */
extern long sys_setuid(int uid);
extern long sys_setgid(int gid);
extern long sys_setreuid(int ruid, int euid);
extern long sys_setregid(int rgid, int egid);
extern long sys_setresuid(int ruid, int euid, int suid);
extern long sys_getresuid(int *ruid, int *euid, int *suid);
extern long sys_setresgid(int rgid, int egid, int sgid);
extern long sys_getresgid(int *rgid, int *egid, int *sgid);
extern long sys_setgroups(int size, const uint32_t *list);
extern long sys_getgroups(int size, uint32_t *list);

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
extern long sys_umask(int mask);

/* Signals (minimal stubs) */
extern long sys_rt_sigaction(int signum, const void *act, void *oldact, size_t sigsetsize);
extern long sys_rt_sigprocmask(int how, const void *set, void *oldset, size_t sigsetsize);
extern long sys_rt_sigreturn(void);

/* Networking (stubs) */
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

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */
static long stub_not_implemented(uint64_t nr)
{
    /* TODO: replace with proper kprintf */
    (void)nr;
    return RISCV64_ENOSYS;
}

/* -----------------------------------------------------------------------
 * riscv64_syscall_handle
 *
 * Primary syscall entry point called from trap vector (ecall handler).
 *
 * Signature matches the RISC-V C ABI with syscall number first:
 *   nr  = a7 from userspace (syscall number)
 *   a0  = a0 from userspace (arg0, also receives return value)
 *   a1  = a1 from userspace (arg1)
 *   a2  = a2 from userspace (arg2)
 *   a3  = a3 from userspace (arg3)
 *   a4  = a4 from userspace (arg4)
 *   a5  = a5 from userspace (arg5)
 * --------------------------------------------------------------------- */
int64_t riscv64_syscall_handle(uint64_t nr,
                                uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5)
{
    switch ((unsigned int)nr) {

    /* ---- File I/O ---------------------------------------------------- */
    case RISCV64_SYS_READ:  /* 63 */
        return sys_read((int)a0, (void *)(uintptr_t)a1, (size_t)a2);

    case RISCV64_SYS_WRITE:  /* 64 */
        return sys_write((int)a0, (const void *)(uintptr_t)a1, (size_t)a2);

    case RISCV64_SYS_OPENAT:  /* 56 */
        return sys_open((const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case RISCV64_SYS_CLOSE:  /* 57 */
        return sys_close((int)a0);

    case RISCV64_SYS_LSEEK:  /* 62 */
        return sys_lseek((int)a0, (long)a1, (int)a2);

    case RISCV64_SYS_FSTAT:  /* 80 */
        return sys_fstat((int)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_NEWFSTATAT:  /* 79 */
        /* Simplified: treat dirfd as AT_FDCWD, forward to sys_stat */
        return sys_stat((const char *)(uintptr_t)a1, (void *)(uintptr_t)a2);

    case RISCV64_SYS_GETCWD:  /* 17 */
        return sys_getcwd((char *)(uintptr_t)a0, (size_t)a1);

    case RISCV64_SYS_CHDIR:  /* 49 */
        return sys_chdir((const char *)(uintptr_t)a0);

    case RISCV64_SYS_FCHDIR:  /* 50 */
        return sys_fchdir((int)a0);

    case RISCV64_SYS_DUP:  /* 23 */
        return sys_dup((int)a0);

    case RISCV64_SYS_DUP3:  /* 24 */
        return sys_dup3((int)a0, (int)a1, (int)a2);

    case RISCV64_SYS_FCNTL:  /* 25 */
        return sys_fcntl((int)a0, (int)a1, (int)a2);

    case RISCV64_SYS_IOCTL:  /* 29 */
        return sys_ioctl((int)a0, (unsigned long)a1, (void *)(uintptr_t)a2);

    case RISCV64_SYS_GETDENTS64:  /* 61 */
        return sys_getdents64((int)a0, (void *)(uintptr_t)a1, (size_t)a2);

    case RISCV64_SYS_PIPE2:  /* 59 */
        return sys_pipe((int *)(uintptr_t)a0);

    case RISCV64_SYS_READV:  /* 65 */
        return sys_readv((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_WRITEV:  /* 66 */
        return sys_writev((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_PREAD64:  /* 67 */
        return sys_pread64((int)a0, (void *)(uintptr_t)a1, (size_t)a2, (long)a3);

    case RISCV64_SYS_PWRITE64:  /* 68 */
        return sys_pwrite64((int)a0, (const void *)(uintptr_t)a1, (size_t)a2, (long)a3);

    case RISCV64_SYS_READLINKAT:  /* 78 */
        return sys_readlinkat((int)a0, (const char *)(uintptr_t)a1,
                              (char *)(uintptr_t)a2, (size_t)a3);

    case RISCV64_SYS_ACCESS:  /* 8 */
    case RISCV64_SYS_FACCESSAT:  /* 48 */
        return sys_faccessat((int)a0, (const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case RISCV64_SYS_UNLINKAT:  /* 35 */
        return sys_unlinkat((int)a0, (const char *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_MKDIRAT:  /* 34 */
        return sys_mkdirat((int)a0, (const char *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_RENAMEAT:  /* 38 */
        return sys_renameat((int)a0, (const char *)(uintptr_t)a1,
                            (int)a2, (const char *)(uintptr_t)a3);

    case RISCV64_SYS_LINKAT:  /* 37 */
        return sys_linkat((int)a0, (const char *)(uintptr_t)a1,
                          (int)a2, (const char *)(uintptr_t)a3, (int)a4);

    case RISCV64_SYS_SYMLINKAT:  /* 36 */
        return sys_symlink((const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);

    case RISCV64_SYS_MKNODAT:  /* 33 */
        return sys_mknod((const char *)(uintptr_t)a0, (int)a1, (int)a2);

    case RISCV64_SYS_FCHOWNAT:  /* 54 */
        return sys_fchownat((int)a0, (const char *)(uintptr_t)a1,
                            (int)a2, (int)a3, (int)a4);

    case RISCV64_SYS_FCHMODAT:  /* 48 */
        return sys_fchmodat((int)a0, (const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case RISCV64_SYS_FACCESSAT2:  /* 48 */
        return sys_faccessat((int)a0, (const char *)(uintptr_t)a1, (int)a2, (int)a3);

    case RISCV64_SYS_UMOUNT2:  /* 39 */
        return stub_not_implemented(nr);

    case RISCV64_SYS_STATFS:  /* 43 */
        return sys_statfs((const char *)(uintptr_t)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_FSTATFS:  /* 44 */
        return sys_fstatfs((int)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_TRUNCATE:  /* 46 */
        return sys_ftruncate((int)a0, (long)a1);

    case RISCV64_SYS_FTRUNCATE:  /* 47 */
        return sys_ftruncate((int)a0, (long)a1);

    case RISCV64_SYS_CHMOD:  /* 90 */
        return sys_chmod((const char *)(uintptr_t)a0, (int)a1);

    case RISCV64_SYS_FCHMOD:  /* 91 */
        return sys_fchmod((int)a0, (int)a1);

    case RISCV64_SYS_CHOWN:  /* 92 */
        return sys_chown((const char *)(uintptr_t)a0, (int)a1, (int)a2);

    case RISCV64_SYS_FCHOWN:  /* 93 */
        return sys_fchown((int)a0, (int)a1, (int)a2);

    case RISCV64_SYS_UMASK:  /* 166 */
        return sys_umask((int)a0);

    case RISCV64_SYS_FSYNC:  /* 72 */
    case RISCV64_SYS_FDATASYNC:  /* 72 */
        return sys_fsync((int)a0);

    case RISCV64_SYS_CLOSE_RANGE:  /* 143 */
        return sys_close_range((unsigned int)a0, (unsigned int)a1, (unsigned int)a2);

    case RISCV64_SYS_COPY_FILE_RANGE:  /* 85 */
        return sys_copy_file_range((int)a0, (long *)(uintptr_t)a1,
                                   (int)a2, (long *)(uintptr_t)a3,
                                   (size_t)a4, (unsigned int)a5);

    case RISCV64_SYS_PPOLL:  /* 73 */
        return sys_ppoll((void *)(uintptr_t)a0, (unsigned int)a1,
                         (const void *)(uintptr_t)a2, (const void *)(uintptr_t)a3,
                         (size_t)a4);

    case RISCV64_SYS_FADVISE64:  /* 254 */
        return sys_fadvise64((int)a0, (long)a1, (long)a2, (int)a3);

    case RISCV64_SYS_MEMFD_CREATE:  /* 279 */
        return sys_memfd_create((const char *)(uintptr_t)a0, (unsigned int)a1);

    case RISCV64_SYS_STATX:  /* 291 */
        return sys_statx((int)a0, (const char *)(uintptr_t)a1,
                         (unsigned)a2, (unsigned)a3, (void *)(uintptr_t)a4);

    case RISCV64_SYS_UTIMENSAT:  /* 88 */
    case RISCV64_SYS_FUTIMESAT:  /* 88 */
        return sys_utimensat((int)a0, (const char *)(uintptr_t)a1,
                             (const void *)(uintptr_t)a2, (int)a3);

    case RISCV64_SYS_CHROOT:  /* 41 */
        return stub_not_implemented(nr);

    /* ---- Memory ------------------------------------------------------ */
    case RISCV64_SYS_BRK:  /* 214 */
        return sys_brk((void *)(uintptr_t)a0);

    case RISCV64_SYS_MMAP:  /* 222 */
        return sys_mmap((void *)(uintptr_t)a0, (size_t)a1,
                        (int)a2, (int)a3, (int)a4, (long)a5);

    case RISCV64_SYS_MUNMAP:  /* 215 */
        return sys_munmap((void *)(uintptr_t)a0, (size_t)a1);

    case RISCV64_SYS_MPROTECT:  /* 226 */
        return sys_mprotect((void *)(uintptr_t)a0, (size_t)a1, (int)a2);

    case RISCV64_SYS_MADVISE:  /* 233 */
        return sys_madvise((void *)(uintptr_t)a0, (size_t)a1, (int)a2);

    case RISCV64_SYS_MLOCK:  /* 234 */
        return sys_mlock((const void *)(uintptr_t)a0, (size_t)a1);

    case RISCV64_SYS_MUNLOCK:  /* 235 */
        return sys_munlock((const void *)(uintptr_t)a0, (size_t)a1);

    case RISCV64_SYS_MLOCKALL:  /* 236 */
        return sys_mlockall((int)a0);

    case RISCV64_SYS_MUNLOCKALL:  /* 237 */
        return sys_munlockall();

    case RISCV64_SYS_MINCORE:  /* 232 */
        return sys_mincore((void *)(uintptr_t)a0, (size_t)a1,
                           (unsigned char *)(uintptr_t)a2);

    case RISCV64_SYS_MREMAP:  /* 216 */
        return sys_mremap((void *)(uintptr_t)a0, (size_t)a1, (size_t)a2,
                          (int)a3, (void *)(uintptr_t)a4);

    case RISCV64_SYS_MSYNC:  /* 227 */
        return sys_msync((void *)(uintptr_t)a0, (size_t)a1, (int)a2);

    case RISCV64_SYS_MSEAL:  /* 281 */
        return stub_not_implemented(nr);

    /* ---- Process management ------------------------------------------ */
    case RISCV64_SYS_GETPID:  /* 172 */
        return sys_getpid();

    case RISCV64_SYS_GETPPID:  /* 173 */
        return sys_getppid();

    case RISCV64_SYS_GETUID:  /* 174 */
        return sys_getuid();

    case RISCV64_SYS_GETEUID:  /* 175 */
        return sys_geteuid();

    case RISCV64_SYS_GETGID:  /* 176 */
        return sys_getgid();

    case RISCV64_SYS_GETEGID:  /* 177 */
        return sys_getegid();

    case RISCV64_SYS_GETTID:  /* 178 */
        return sys_getpid();   /* simplified: TID == PID for single-threaded */

    case RISCV64_SYS_EXIT:  /* 93 */
        sys_exit((int)a0);
        /* noreturn */

    case RISCV64_SYS_EXIT_GROUP:  /* 94 */
        sys_exit_group((int)a0);
        /* noreturn */

    case RISCV64_SYS_KILL:  /* 129 */
        return sys_kill((int)a0, (int)a1);

    case RISCV64_SYS_TKILL:  /* 130 */
        return sys_kill((int)a0, (int)a1);

    case RISCV64_SYS_TGKILL:  /* 131 */
        return sys_kill((int)a1, (int)a2);

    case RISCV64_SYS_CLONE:  /* 220 */
        return sys_clone((unsigned long)a0, (void *)(uintptr_t)a1,
                         (int *)(uintptr_t)a2, (int *)(uintptr_t)a3,
                         (unsigned long)a4);

    case RISCV64_SYS_CLONE3:  /* 435 */
        return stub_not_implemented(nr);

    case RISCV64_SYS_EXECVE:  /* 221 */
        return sys_execve((const char *)(uintptr_t)a0,
                          (char *const *)(uintptr_t)a1,
                          (char *const *)(uintptr_t)a2);

    case RISCV64_SYS_WAIT4:  /* 260 */
        return sys_wait4((int)a0, (int *)(uintptr_t)a1, (int)a2,
                         (void *)(uintptr_t)a3);

    case RISCV64_SYS_SET_TID_ADDRESS:  /* 96 */
        return sys_set_tid_address((int *)(uintptr_t)a0);

    case RISCV64_SYS_FUTEX:  /* 98 */
        return sys_futex((uint32_t *)(uintptr_t)a0, (int)a1, (int)a2,
                         (const void *)(uintptr_t)a3,
                         (uint32_t *)(uintptr_t)a4, (int)a5);

    /* ---- Credentials ------------------------------------------------ */
    case RISCV64_SYS_SETUID:  /* 146 */
        return sys_setuid((int)a0);

    case RISCV64_SYS_SETGID:  /* 144 */
        return sys_setgid((int)a0);

    case RISCV64_SYS_SETREUID:  /* stub */
    case RISCV64_SYS_SETEUID:  /* stub */
        return 0;

    case RISCV64_SYS_SETREGID:  /* stub */
    case RISCV64_SYS_SETEGID:  /* stub */
        return 0;

    case RISCV64_SYS_SETRESUID:  /* stub */
        return 0;

    case RISCV64_SYS_GETRESUID:  /* 116 */ {
        uint32_t *p = (uint32_t *)(uintptr_t)a0;
        if (p) { p[0] = 0; p[1] = 0; p[2] = 0; }
        return 0;
    }

    case RISCV64_SYS_SETRESGID:  /* stub */
        return 0;

    case RISCV64_SYS_GETRESGID:  /* 118 */ {
        uint32_t *p = (uint32_t *)(uintptr_t)a0;
        if (p) { p[0] = 0; p[1] = 0; p[2] = 0; }
        return 0;
    }

    case RISCV64_SYS_SETGROUPS:  /* 81 */
        return 0;

    case RISCV64_SYS_GETGROUPS:  /* 80 */
        return 0;

    case RISCV64_SYS_GETPGID:  /* stub */
    case RISCV64_SYS_GETSID:  /* stub */
        return sys_getpid();

    case RISCV64_SYS_SETPGID:  /* stub */
    case RISCV64_SYS_SETSID:  /* stub */
        return 0;

    case RISCV64_SYS_TCGETPGRP:  /* stub */
    case RISCV64_SYS_TCSETPGRP:  /* stub */
        return 1;

    case RISCV64_SYS_SETFSUID:  /* stub */
    case RISCV64_SYS_SETFSGID:  /* stub */
        return 0;

    /* ---- Time ------------------------------------------------------- */
    case RISCV64_SYS_CLOCK_GETTIME:  /* 113 */
        return sys_clock_gettime((int)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_CLOCK_GETRES:  /* 114 */
        return sys_clock_getres((int)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_NANOSLEEP:  /* 101 */
        return sys_nanosleep((const void *)(uintptr_t)a0,
                             (void *)(uintptr_t)a1);

    case RISCV64_SYS_GETTIMEOFDAY:  /* 169 */
        return sys_gettimeofday((void *)(uintptr_t)a0,
                                (void *)(uintptr_t)a1);

    case RISCV64_SYS_SETTIMEOFDAY:  /* stub */
        return 0;

    /* ---- Scheduling ------------------------------------------------- */
    case RISCV64_SYS_SCHED_YIELD:  /* 124 */
    case RISCV64_SYS_SCHED_SETPARAM:  /* 118 */
    case RISCV64_SYS_SCHED_SETSCHEDULER:  /* 119 */
    case RISCV64_SYS_SCHED_GETSCHEDULER:  /* 120 */
    case RISCV64_SYS_SCHED_GETPARAM:  /* 121 */
    case RISCV64_SYS_SCHED_SETAFFINITY:  /* 122 */
    case RISCV64_SYS_SCHED_GETAFFINITY:  /* 123 */
    case RISCV64_SYS_SCHED_GET_PRIORITY_MAX:  /* 125 */
    case RISCV64_SYS_SCHED_GET_PRIORITY_MIN:  /* 126 */
    case RISCV64_SYS_SCHED_SETATTR:  /* 274 */
    case RISCV64_SYS_SCHED_GETATTR:  /* 275 */
        return 0;

    /* ---- Resource limits -------------------------------------------- */
    case RISCV64_SYS_GETRLIMIT:  /* 163 */
        return sys_getrlimit((int)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_SETRLIMIT:  /* 164 */
        return sys_setrlimit((int)a0, (const void *)(uintptr_t)a1);

    case RISCV64_SYS_GETRUSAGE:  /* 165 */
        return sys_getrusage((int)a0, (void *)(uintptr_t)a1);

    case RISCV64_SYS_PRLIMIT64:  /* 261 */
        return stub_not_implemented(nr);

    /* ---- System info ------------------------------------------------ */
    case RISCV64_SYS_UNAME:  /* 160 */
        return sys_uname((void *)(uintptr_t)a0);

    case RISCV64_SYS_SYSINFO:  /* 179 */
        return sys_sysinfo((void *)(uintptr_t)a0);

    case RISCV64_SYS_TIMES:  /* 153 */
        return sys_times((void *)(uintptr_t)a0);

    /* ---- Priority --------------------------------------------------- */
    case RISCV64_SYS_GETPRIORITY:  /* 141 */
        return sys_getpriority((int)a0, (int)a1);

    case RISCV64_SYS_SETPRIORITY:  /* 140 */
        return sys_setpriority((int)a0, (int)a1, (int)a2);

    /* ---- Signals (minimal) ------------------------------------------ */
    case RISCV64_SYS_RT_SIGACTION:  /* 134 */
    case RISCV64_SYS_RT_SIGPROCMASK:  /* 135 */
    case RISCV64_SYS_SIGPROCMASK:  /* 135 */
    case RISCV64_SYS_SIGACTION:  /* 134 */
    case RISCV64_SYS_RT_SIGRETURN:  /* 139 */
    case RISCV64_SYS_RT_SIGSUSPEND:  /* 136 */
    case RISCV64_SYS_RT_SIGTIMEDWAIT:  /* 137 */
    case RISCV64_SYS_GETSIGINFO:  /* 132 (sigaltstack) */
        return stub_not_implemented(nr);

    /* ---- Networking (stubs) ----------------------------------------- */
    case RISCV64_SYS_SOCKET:  /* 198 */
        return sys_socket((int)a0, (int)a1, (int)a2);

    case RISCV64_SYS_BIND:  /* 200 */
        return sys_bind((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_LISTEN:  /* 201 */
        return sys_listen((int)a0, (int)a1);

    case RISCV64_SYS_ACCEPT:  /* 202 */
        return sys_accept((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2);

    case RISCV64_SYS_ACCEPT4:  /* 242 */
        return sys_accept4((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2, (int)a3);

    case RISCV64_SYS_CONNECT:  /* 203 */
        return sys_connect((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_SENDTO:  /* 206 */
        return sys_sendto((int)a0, (const void *)(uintptr_t)a1, (size_t)a2,
                          (int)a3, (const void *)(uintptr_t)a4, (int)a5);

    case RISCV64_SYS_RECVFROM:  /* 207 */
        return sys_recvfrom((int)a0, (void *)(uintptr_t)a1, (size_t)a2,
                            (int)a3, (void *)(uintptr_t)a4, (int *)(uintptr_t)a5);

    case RISCV64_SYS_SENDMSG:  /* 211 */
        return sys_sendmsg((int)a0, (const void *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_RECVMSG:  /* 212 */
        return sys_recvmsg((int)a0, (void *)(uintptr_t)a1, (int)a2);

    case RISCV64_SYS_SHUTDOWN:  /* 210 */
        return sys_shutdown((int)a0, (int)a1);

    case RISCV64_SYS_GETSOCKNAME:  /* 204 */
        return sys_getsockname((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2);

    case RISCV64_SYS_GETPEERNAME:  /* 205 */
        return sys_getpeername((int)a0, (void *)(uintptr_t)a1, (int *)(uintptr_t)a2);

    case RISCV64_SYS_SOCKETPAIR:  /* 199 */
        return sys_socketpair((int)a0, (int)a1, (int)a2, (int *)(uintptr_t)a3);

    case RISCV64_SYS_SETSOCKOPT:  /* 208 */
        return sys_setsockopt((int)a0, (int)a1, (int)a2, (const void *)(uintptr_t)a3, (int)a4);

    case RISCV64_SYS_GETSOCKOPT:  /* 209 */
        return sys_getsockopt((int)a0, (int)a1, (int)a2, (void *)(uintptr_t)a3, (int *)(uintptr_t)a4);

    /* ---- I/O multiplexing ------------------------------------------- */
    case RISCV64_SYS_EPOLL_CREATE1:  /* 20 */
        return sys_epoll_create1((int)a0);

    case RISCV64_SYS_EPOLL_CTL:  /* 21 */
        return sys_epoll_ctl((int)a0, (int)a1, (int)a2, (void *)(uintptr_t)a3);

    case RISCV64_SYS_EPOLL_PWAIT:  /* 22 */
    case RISCV64_SYS_EPOLL_WAIT:  /* 22 */
        return sys_epoll_pwait((int)a0, (void *)(uintptr_t)a1, (int)a2,
                               (int)a3, (const void *)(uintptr_t)a4, (size_t)a5);

    case RISCV64_SYS_POLL:  /* 7 */
        return sys_ppoll((void *)(uintptr_t)a0, (unsigned int)a1,
                         (const void *)(uintptr_t)a2, NULL, 0);

    case RISCV64_SYS_SELECT:  /* stub */
    case RISCV64_SYS_PSELECT6:  /* stub */
        return stub_not_implemented(nr);

    /* ---- Misc ------------------------------------------------------- */
    case RISCV64_SYS_PRCTL:  /* stub */
    case RISCV64_SYS_UMASK:  /* 166 */
        return sys_umask((int)a0);

    case RISCV64_SYS_GETRANDOM:  /* 278 */
        return sys_getrandom((void *)(uintptr_t)a0, (size_t)a1, (unsigned int)a2);

    case RISCV64_SYS_RSEQ:  /* 243 */
        return stub_not_implemented(nr);

    /* ---- Unimplemented default -------------------------------------- */
    default:
        return stub_not_implemented(nr);
    }
}

/* -----------------------------------------------------------------------
 * riscv64_syscall_dispatch
 *
 * Legacy entry point used by trap entry code.
 * Argument order: a0-a5 are the first six parameters, a7 = nr last.
 * Bridges to riscv64_syscall_handle() which takes nr first.
 * --------------------------------------------------------------------- */
long riscv64_syscall_dispatch(uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5,
                               uint64_t nr)
{
    return (long)riscv64_syscall_handle(nr, a0, a1, a2, a3, a4, a5);
}

/* -----------------------------------------------------------------------
 * riscv64_syscall_init
 * --------------------------------------------------------------------- */
void riscv64_syscall_init(void)
{
    /*
     * The ECALL exception is caught by the trap vector installed in boot.S.
     * The RISC-V trap handler checks mcause/scause for ecall from U-mode
     * (or S-mode) and dispatches to riscv64_syscall_handle().
     * No further initialisation is required here.
     */
}
