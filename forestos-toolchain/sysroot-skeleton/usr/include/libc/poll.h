/*
 * poll.h - I/O multiplexing
 *
 * POSIX-compatible poll interface for Forest OS libc.
 */
#ifndef _POLL_H
#define _POLL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* Poll event flags */
#define POLLIN      0x0001  /* Data other than high-priority data may be read */
#define POLLPRI     0x0002  /* High-priority data may be read */
#define POLLOUT     0x0004  /* Normal data may be written */
#define POLLERR     0x0008  /* Error condition */
#define POLLHUP     0x0010  /* Hung up */
#define POLLNVAL    0x0020  /* Invalid polling request */

/* Common aliases/extensions */
#define POLLRDNORM  POLLIN
#define POLLWRNORM  POLLOUT
#define POLLRDBAND  0x0080
#define POLLWRBAND  0x0100
#define POLLMSG     0x0400
#define POLLREMOVE  0x1000
#define POLLRDHUP   0x2000

struct pollfd {
    int   fd;       /* File descriptor to poll */
    short events;   /* Requested events */
    short revents;  /* Returned events */
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* _POLL_H */
