/*
 * signal.h - Signal handling
 * 
 * POSIX compatible signal handling for Fern libc.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

/* Signal handler types */
typedef void (*sighandler_t)(int);
typedef void (*sig_t)(int);

/* Signal handler special values */
#define SIG_DFL ((sighandler_t)0)   /* Default signal handling */
#define SIG_IGN ((sighandler_t)1)   /* Ignore signal */
#define SIG_ERR ((sighandler_t)-1)  /* Error return from signal() */
#define SIG_HOLD ((sighandler_t)2)  /* Block signal */

/* Signal numbers (Linux x86_64 compatible) */
#define SIGHUP      1   /* Hangup */
#define SIGINT      2   /* Terminal interrupt signal */
#define SIGQUIT     3   /* Terminal quit signal */
#define SIGILL      4   /* Illegal instruction */
#define SIGTRAP     5   /* Trace/breakpoint trap */
#define SIGABRT     6   /* Process abort signal */
#define SIGIOT      SIGABRT /* IOT trap (obsolete) */
#define SIGBUS      7   /* Bus error (bad memory access) */
#define SIGFPE      8   /* Floating-point exception */
#define SIGKILL     9   /* Kill (cannot be caught or ignored) */
#define SIGUSR1     10  /* User-defined signal 1 */
#define SIGSEGV     11  /* Invalid memory reference */
#define SIGUSR2     12  /* User-defined signal 2 */
#define SIGPIPE     13  /* Broken pipe */
#define SIGALRM     14  /* Alarm clock */
#define SIGTERM     15  /* Termination signal */
#define SIGSTKFLT   16  /* Stack fault on coprocessor */
#define SIGCHLD     17  /* Child stopped or terminated */
#define SIGCLD      SIGCHLD /* SIGCHLD (obsolete) */
#define SIGCONT     18  /* Continue if stopped */
#define SIGSTOP     19  /* Stop (cannot be caught or ignored) */
#define SIGTSTP     20  /* Terminal stop signal */
#define SIGTTIN     21  /* Background process attempting read */
#define SIGTTOU     22  /* Background process attempting write */
#define SIGURG      23  /* Urgent condition on socket */
#define SIGXCPU     24  /* CPU time limit exceeded */
#define SIGXFSZ     25  /* File size limit exceeded */
#define SIGVTALRM   26  /* Virtual alarm clock */
#define SIGPROF     27  /* Profiling timer expired */
#define SIGWINCH    28  /* Window size change */
#define SIGIO       29  /* I/O now possible */
#define SIGPOLL     SIGIO /* Pollable event (obsolete) */
#define SIGPWR      30  /* Power failure */
#define SIGSYS      31  /* Bad system call */
#define SIGUNUSED   31  /* Unused signal */

/* Real-time signals */
#define SIGRTMIN    32
#define SIGRTMAX    64

/* Number of signals */
#define NSIG        65
#define _NSIG       NSIG

/* Signal set type */
typedef struct {
    unsigned long __val[16];
} sigset_t;

/* sigaction flags */
#define SA_NOCLDSTOP    0x00000001  /* Don't send SIGCHLD when children stop */
#define SA_NOCLDWAIT    0x00000002  /* Don't create zombie on child death */
#define SA_SIGINFO      0x00000004  /* Invoke signal-catching function with three arguments */
#define SA_ONSTACK      0x08000000  /* Use signal stack by using sigaltstack() */
#define SA_RESTART      0x10000000  /* Restart interrupted system calls */
#define SA_NODEFER      0x40000000  /* Don't automatically block the signal when its handler is being executed */
#define SA_RESETHAND    0x80000000  /* Reset to SIG_DFL on entry to handler */

/* Compatibility names */
#define SA_NOMASK   SA_NODEFER
#define SA_ONESHOT  SA_RESETHAND

/* sigprocmask() how values */
#define SIG_BLOCK   0   /* Block signals */
#define SIG_UNBLOCK 1   /* Unblock signals */
#define SIG_SETMASK 2   /* Set the set of blocked signals */

/* sigval union for sigqueue (must precede siginfo_t) */
union sigval {
    int sival_int;
    void *sival_ptr;
};

/* siginfo_t structure (must precede struct sigaction) */
typedef struct {
    int si_signo;       /* Signal number */
    int si_errno;       /* An errno value */
    int si_code;        /* Signal code */
    int si_trapno;      /* Trap number that caused hardware-generated signal */
    pid_t si_pid;       /* Sending process ID */
    uid_t si_uid;       /* Real user ID of sending process */
    int si_status;      /* Exit value or signal */
    clock_t si_utime;   /* User time consumed */
    clock_t si_stime;   /* System time consumed */
    union sigval si_value; /* Signal value */
    int si_int;         /* POSIX.1b signal */
    void *si_ptr;       /* POSIX.1b signal */
    int si_overrun;     /* Timer overrun count */
    int si_timerid;     /* Timer ID */
    void *si_addr;      /* Memory location which caused fault */
    long si_band;       /* Band event */
    int si_fd;          /* File descriptor */
    short si_addr_lsb;  /* Least significant bit of address */
    void *si_lower;     /* Lower bound when address violation occurred */
    void *si_upper;     /* Upper bound when address violation occurred */
    int si_pkey;        /* Protection key on PTE that caused fault */
    void *si_call_addr; /* Address of system call instruction */
    int si_syscall;     /* Number of attempted system call */
    unsigned int si_arch; /* Architecture of attempted system call */
} siginfo_t;

/* sigevent structure (for timer_create) */
struct sigevent {
    int sigev_notify;               /* Notification method */
    int sigev_signo;                /* Signal number */
    union sigval sigev_value;       /* Signal value */
    void (*sigev_notify_function)(union sigval); /* Thread notification function */
    void *sigev_notify_attributes;  /* Thread attributes */
    void *sigev_notify_thread_id;   /* Thread ID */
};

/* sigevent notification methods */
#define SIGEV_SIGNAL  0   /* Notify via signal */
#define SIGEV_NONE    1   /* No notification */
#define SIGEV_THREAD  2   /* Notify via thread creation */
#define SIGEV_THREAD_ID 4 /* Notify via thread ID */

/* sigaction structure */
struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sigaction_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define sa_handler   __sigaction_handler.sa_handler
#define sa_sigaction __sigaction_handler.sa_sigaction

/* si_code values */
#define SI_USER     0       /* Sent by kill(), sigsend(), or raise() */
#define SI_KERNEL   0x80    /* Sent by the kernel from somewhere */
#define SI_QUEUE    -1      /* Sent by sigqueue() */
#define SI_TIMER    -2      /* Sent by timer expiration */
#define SI_MESGQ    -3      /* Sent by message arrival */
#define SI_ASYNCIO  -4      /* Sent by AIO completion */
#define SI_SIGIO    -5      /* Sent by queued SIGIO */
#define SI_TKILL    -6      /* Sent by tkill() or tgkill() */

/* Stack information for signal handlers */
typedef struct {
    void *ss_sp;        /* Stack base or pointer */
    int ss_flags;       /* Flags */
    size_t ss_size;     /* Number of bytes in stack */
} stack_t;

/* ss_flags values */
#define SS_ONSTACK  1
#define SS_DISABLE  2

/* Minimum signal stack size */
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

/* Signal functions */
sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

/* Signal set manipulation */
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);

/* Signal blocking */
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

/* Signal sending */
int kill(pid_t pid, int sig);
int killpg(pid_t pgrp, int sig);
int raise(int sig);
int sigqueue(pid_t pid, int sig, const union sigval value);

/* Signal waiting */
int sigwait(const sigset_t *set, int *sig);
int sigwaitinfo(const sigset_t *set, siginfo_t *info);
int sigtimedwait(const sigset_t *set, siginfo_t *info,
                 const struct timespec *timeout);

/* Alternate signal stack */
int sigaltstack(const stack_t *ss, stack_t *old_ss);

/* Interrupt a system call */
int pause(void);

/* Obsolete functions (provided for compatibility) */
void psignal(int sig, const char *s);
void psiginfo(const siginfo_t *pinfo, const char *s);
const char *strsignal(int sig);
extern const char * const sys_siglist[];

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
