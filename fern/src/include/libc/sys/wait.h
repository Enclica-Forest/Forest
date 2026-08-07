/*
 * sys/wait.h - Process waiting and child status macros
 *
 * POSIX-style wait interfaces for Fern libc.
 */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* waitpid() options */
#define WNOHANG      0x00000001
#define WUNTRACED    0x00000002
#define WCONTINUED   0x00000008
#define WNOWAIT      0x01000000

/* Child status inspection macros */
#define WEXITSTATUS(status)   (((status) >> 8) & 0xFF)
#define WTERMSIG(status)      ((status) & 0x7F)
#define WSTOPSIG(status)      WEXITSTATUS(status)
#define WCOREDUMP(status)     ((status) & 0x80)

#define WIFEXITED(status)     (WTERMSIG(status) == 0)
#define WIFSIGNALED(status)   (WTERMSIG(status) != 0 && WTERMSIG(status) != 0x7F)
#define WIFSTOPPED(status)    (((status) & 0xFF) == 0x7F)
#define WIFCONTINUED(status)  ((status) == 0xFFFF)

int wait(int *status);
int waitpid(pid_t pid, int *status, int options);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
