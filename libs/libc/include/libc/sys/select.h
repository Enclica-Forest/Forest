/*
 * sys/select.h - Select and pselect
 *
 * POSIX-compatible select interface for Fern libc.
 */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>

/* FD_SETSIZE - Maximum number of file descriptors */
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

/* fd_set type */
typedef struct {
    unsigned long __fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

/* FD_SET macros */
#define FD_ZERO(fdset) \
    do { \
        fd_set *__fdset = (fdset); \
        unsigned int __i; \
        for (__i = 0; __i < sizeof(__fdset->__fds_bits) / sizeof(__fdset->__fds_bits[0]); __i++) \
            __fdset->__fds_bits[__i] = 0; \
    } while (0)

#define FD_SET(fd, fdset) \
    ((fdset)->__fds_bits[(fd) / (8 * sizeof(unsigned long))] |= \
     (1UL << ((fd) % (8 * sizeof(unsigned long)))))

#define FD_CLR(fd, fdset) \
    ((fdset)->__fds_bits[(fd) / (8 * sizeof(unsigned long))] &= \
     ~(1UL << ((fd) % (8 * sizeof(unsigned long)))))

#define FD_ISSET(fd, fdset) \
    (((fdset)->__fds_bits[(fd) / (8 * sizeof(unsigned long))] & \
      (1UL << ((fd) % (8 * sizeof(unsigned long))))) != 0)

/* Select functions */
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SELECT_H */
