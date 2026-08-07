#ifndef _FORESTOS_SYSCALLS_H
#define _FORESTOS_SYSCALLS_H

/* Forest OS System Call Interface */

#include <sys/types.h>

/* Forest OS syscall numbers - these must match src/include/syscall.h */
#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_LSEEK     4
#define SYS_GETPID    5
#define SYS_UNLINK    6
#define SYS_TIME      7
#define SYS_NANOSLEEP 8
#define SYS_UNAME     9
#define SYS_BRK      10
#define SYS_EXIT     11
#define SYS_SOCKET   12
#define SYS_BIND     13
#define SYS_SENDTO   14
#define SYS_RECVFROM 15
#define SYS_NETINFO  16
#define SYS_POWER    17

/* Forest OS errno values - these must match src/include/libc/errno.h */
#define EPERM     1   /* Operation not permitted */
#define ENOENT    2   /* No such file or directory */
#define ESRCH     3   /* No such process */
#define EINTR     4   /* Interrupted system call */
#define EIO       5   /* I/O error */
#define ENXIO     6   /* No such device or address */
#define E2BIG     7   /* Argument list too long */
#define ENOEXEC   8   /* Exec format error */
#define EBADF     9   /* Bad file number */
#define ECHILD   10   /* No child processes */
#define EAGAIN   11   /* Try again */
#define ENOMEM   12   /* Out of memory */
#define EACCES   13   /* Permission denied */
#define EFAULT   14   /* Bad address */
#define EBUSY    16   /* Device or resource busy */
#define EEXIST   17   /* File exists */
#define EXDEV    18   /* Cross-device link */
#define ENODEV   19   /* No such device */
#define ENOTDIR  20   /* Not a directory */
#define EISDIR   21   /* Is a directory */
#define EINVAL   22   /* Invalid argument */
#define ENFILE   23   /* File table overflow */
#define EMFILE   24   /* Too many open files */
#define ENOTTY   25   /* Not a typewriter */
#define EFBIG    27   /* File too large */
#define ENOSPC   28   /* No space left on device */
#define ESPIPE   29   /* Illegal seek */
#define EROFS    30   /* Read-only file system */
#define EMLINK   31   /* Too many links */
#define EPIPE    32   /* Broken pipe */
#define EDOM     33   /* Math argument out of domain of func */
#define ERANGE   34   /* Math result not representable */
#define ENOSYS   38   /* Function not implemented */

/* System call wrappers using int 0x80 */
static inline int syscall0(int num) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory");
    return ret;
}

static inline int syscall1(int num, int a1) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1)
        : "memory");
    return ret;
}

static inline int syscall2(int num, int a1, int a2) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2)
        : "memory");
    return ret;
}

static inline int syscall3(int num, int a1, int a2, int a3) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory");
    return ret;
}

static inline int syscall4(int num, int a1, int a2, int a3, int a4) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4)
        : "memory");
    return ret;
}

static inline int syscall5(int num, int a1, int a2, int a3, int a4, int a5) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
        : "memory");
    return ret;
}

static inline int syscall6(int num, int a1, int a2, int a3, int a4, int a5, int a6) {
    int ret;
    __asm__ __volatile__(
        "push %%ebp\n"
        "mov %7, %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3),
          "S"(a4), "D"(a5), "g"(a6)
        : "memory");
    return ret;
}

/* Global errno variable */
extern int errno;

/* ForestOS framebuffer extension syscalls (must match src/include/syscall.h) */
#ifndef SYS_MMAP_FB
#define SYS_MMAP_FB             471
#define SYS_MUNMAP_FB           472
#define SYS_GET_FB_INFO         473
#define SYS_USERCTL             474
#define SYS_SOUND_INIT          475
#define SYS_SOUND_PLAY          476
#define SYS_SOUND_STOP          477
#define SYS_FB_FLUSH            478
#define SYS_FB_VSYNC            479
#define SYS_READ_KBD_EVENT      479
#define SYS_READ_MOUSE_EVENT    480
#define SYS_POLL_INPUT          481
#define SYS_GETTIMEOFDAY        96   /* Linux x86_64 compatible */
#define SYS_SCHED_YIELD         24   /* Linux x86_64 compatible */
#endif

/* Poll-able counter bumped when SYS_SET_FB_MODE succeeds; compare against
 * last-seen value each frame to detect a stale SYS_MMAP_FB mapping and
 * re-map (see libs/leafgfx's gfx_flip()). Number matches the kernel. Kept
 * outside the #ifndef SYS_MMAP_FB guard above since it's independent of
 * that (stale) block. */
#ifndef SYS_GET_FB_GENERATION
#define SYS_GET_FB_GENERATION   531
#endif

#endif /* _FORESTOS_SYSCALLS_H */