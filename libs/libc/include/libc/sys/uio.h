/*
 * sys/uio.h - Scatter/gather I/O
 *
 * POSIX-compatible vector I/O declarations for Fern libc.
 */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct iovec {
    void  *iov_base;  /* Pointer to data buffer */
    size_t iov_len;   /* Length of data buffer */
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UIO_H */
