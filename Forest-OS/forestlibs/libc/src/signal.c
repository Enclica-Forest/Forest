/*
 * signal.c - Signal handling for Fern libc
 * 
 * Signal handling requires kernel support. These are the userspace
 * wrappers that call into the kernel's signal subsystem.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>

/* Signal names for strsignal() */
static const char * const __sys_siglist[] = {
    [0] = "Unknown signal 0",
    [SIGHUP] = "Hangup",
    [SIGINT] = "Interrupt",
    [SIGQUIT] = "Quit",
    [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap",
    [SIGABRT] = "Aborted",
    [SIGBUS] = "Bus error",
    [SIGFPE] = "Floating point exception",
    [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2",
    [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock",
    [SIGTERM] = "Terminated",
    [SIGSTKFLT] = "Stack fault",
    [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued",
    [SIGSTOP] = "Stopped (signal)",
    [SIGTSTP] = "Stopped",
    [SIGTTIN] = "Stopped (tty input)",
    [SIGTTOU] = "Stopped (tty output)",
    [SIGURG] = "Urgent I/O condition",
    [SIGXCPU] = "CPU time limit exceeded",
    [SIGXFSZ] = "File size limit exceeded",
    [SIGVTALRM] = "Virtual timer expired",
    [SIGPROF] = "Profiling timer expired",
    [SIGWINCH] = "Window changed",
    [SIGIO] = "I/O possible",
    [SIGPWR] = "Power failure",
    [SIGSYS] = "Bad system call",
};

const char * const sys_siglist[] = {
    [0] = "Unknown signal 0",
    [SIGHUP] = "Hangup",
    [SIGINT] = "Interrupt",
    [SIGQUIT] = "Quit",
    [SIGILL] = "Illegal instruction",
    [SIGTRAP] = "Trace/breakpoint trap",
    [SIGABRT] = "Aborted",
    [SIGBUS] = "Bus error",
    [SIGFPE] = "Floating point exception",
    [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2",
    [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock",
    [SIGTERM] = "Terminated",
};

/* ============================================================================
 * SIGNAL SET MANIPULATION
 * ============================================================================ */

int sigemptyset(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    memset(set, 0, sizeof(*set));
    return 0;
}

int sigfillset(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    memset(set, 0xFF, sizeof(*set));
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (!set || signum <= 0 || signum > NSIG) {
        errno = EINVAL;
        return -1;
    }
    
    unsigned long word = (signum - 1) / (8 * sizeof(unsigned long));
    unsigned long bit = (signum - 1) % (8 * sizeof(unsigned long));
    set->__val[word] |= (1UL << bit);
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (!set || signum <= 0 || signum > NSIG) {
        errno = EINVAL;
        return -1;
    }
    
    unsigned long word = (signum - 1) / (8 * sizeof(unsigned long));
    unsigned long bit = (signum - 1) % (8 * sizeof(unsigned long));
    set->__val[word] &= ~(1UL << bit);
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (!set || signum <= 0 || signum > NSIG) {
        errno = EINVAL;
        return -1;
    }
    
    unsigned long word = (signum - 1) / (8 * sizeof(unsigned long));
    unsigned long bit = (signum - 1) % (8 * sizeof(unsigned long));
    return (set->__val[word] & (1UL << bit)) != 0;
}

/* ============================================================================
 * SIMPLE SIGNAL HANDLING
 * ============================================================================ */

/* Storage for simple signal handlers */
static sighandler_t __signal_handlers[NSIG] = {0};

sighandler_t signal(int signum, sighandler_t handler) {
    if (signum <= 0 || signum >= NSIG) {
        errno = EINVAL;
        return SIG_ERR;
    }
    
    /* Can't catch SIGKILL or SIGSTOP */
    if (signum == SIGKILL || signum == SIGSTOP) {
        errno = EINVAL;
        return SIG_ERR;
    }
    
    sighandler_t old = __signal_handlers[signum];
    __signal_handlers[signum] = handler;
    
    /* In a full implementation, this would call sigaction */
    return old;
}

/* sigaction is implemented in syscalls.c */

/* ============================================================================
 * SIGNAL SENDING
 * ============================================================================ */

/* kill is implemented in syscalls.c */

int killpg(pid_t pgrp, int sig) {
    return kill(-pgrp, sig);
}

int raise(int sig) {
    return kill(getpid(), sig);
}

int sigqueue(pid_t pid, int sig, const union sigval value) {
    /* Simplified: just use kill */
    (void)value;
    return kill(pid, sig);
}

/* ============================================================================
 * SIGNAL WAITING
 * ============================================================================ */

int sigwait(const sigset_t *set, int *sig) {
    if (!set || !sig) {
        return EINVAL;
    }
    
    /* Not fully implemented */
    errno = ENOSYS;
    return -1;
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info) {
    (void)set;
    (void)info;
    errno = ENOSYS;
    return -1;
}

int sigtimedwait(const sigset_t *set, siginfo_t *info,
                 const struct timespec *timeout) {
    (void)set;
    (void)info;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

/* ============================================================================
 * ALTERNATE SIGNAL STACK
 * ============================================================================ */

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    (void)ss;
    (void)old_ss;
    /* Not implemented */
    errno = ENOSYS;
    return -1;
}

/* ============================================================================
 * SIGNAL INFORMATION
 * ============================================================================ */

const char *strsignal(int signum) {
    if (signum < 0 || signum >= NSIG) {
        return "Unknown signal";
    }
    
    if (__sys_siglist[signum]) {
        return __sys_siglist[signum];
    }
    
    return "Unknown signal";
}

void psignal(int signum, const char *s) {
    const char *sig_str = strsignal(signum);
    
    if (s && *s) {
        write(2, s, strlen(s));
        write(2, ": ", 2);
    }
    write(2, sig_str, strlen(sig_str));
    write(2, "\n", 1);
}

void psiginfo(const siginfo_t *pinfo, const char *s) {
    if (pinfo) {
        psignal(pinfo->si_signo, s);
    }
}
