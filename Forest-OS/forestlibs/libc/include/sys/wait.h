/*
 * sys/wait.h - Process wait status
 * 
 * POSIX compatible wait definitions for Fern libc.
 */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <signal.h>

/* Options for waitpid */
#define WNOHANG     0x00000001  /* Don't block waiting */
#define WUNTRACED   0x00000002  /* Report status of stopped children */
#define WSTOPPED    WUNTRACED   /* Same as WUNTRACED */
#define WEXITED     0x00000004  /* Report dead child */
#define WCONTINUED  0x00000008  /* Report continued child */
#define WNOWAIT     0x01000000  /* Don't reap, just poll status */

/* Options for clone */
#define __WNOTHREAD 0x20000000  /* Don't wait on children of other threads in this group */
#define __WALL      0x40000000  /* Wait on all children, regardless of type */
#define __WCLONE    0x80000000  /* Wait only on non-SIGCHLD children */

/* Wait status macros */

/* True if child exited normally */
#define WIFEXITED(status)    (((status) & 0x7f) == 0)

/* True if child was signaled */
#define WIFSIGNALED(status)  (((signed char)(((status) & 0x7f) + 1) >> 1) > 0)

/* True if child was stopped */
#define WIFSTOPPED(status)   (((status) & 0xff) == 0x7f)

/* True if child was continued */
#define WIFCONTINUED(status) ((status) == 0xffff)

/* Exit status of normally terminated child */
#define WEXITSTATUS(status)  (((status) & 0xff00) >> 8)

/* Signal number that caused child to terminate */
#define WTERMSIG(status)     ((status) & 0x7f)

/* Signal number that caused child to stop */
#define WSTOPSIG(status)     (((status) & 0xff00) >> 8)

/* True if child produced a core dump */
#define WCOREDUMP(status)    ((status) & 0x80)

/* Construct a status value */
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#define W_STOPCODE(sig)      ((sig) << 8 | 0x7f)

/* Which type of ID to use for waitid */
typedef enum {
    P_ALL = 0,  /* Wait for any child */
    P_PID = 1,  /* Wait for a specific PID */
    P_PGID = 2, /* Wait for any child in a specific process group */
    P_PIDFD = 3 /* Wait for a specific pidfd */
} idtype_t;

/* Wait functions */
pid_t wait(int *wstatus);
pid_t waitpid(pid_t pid, int *wstatus, int options);
int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);

/* BSD compatibility */
pid_t wait3(int *wstatus, int options, struct rusage *rusage);
pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
