/*
 * forest.h - Forest OS userspace helper header
 *
 * Common definitions and syscall wrappers for Forest userspace apps.
 * All apps include this plus standard POSIX headers.
 */
#ifndef _FOREST_H
#define _FOREST_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/syscall.h>

/* POSIX types missing from Forest OS libc */
typedef int sig_atomic_t;

/* Forest OS identification */
#define FOREST_OS_NAME "Forest OS"
#define FOREST_OS_VERSION "0.1.0"

/* Utility macros */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* Exit codes */
#define EXIT_OK      0
#define EXIT_USAGE   1
#define EXIT_FAIL    2
#define EXIT_ERROR   128
#define EXIT_SIGBASE 128

/* Output helpers */
static inline void eprint(const char *msg) {
    if (msg) {
        size_t len = 0;
        const char *p = msg;
        while (*p++) len++;
        write(STDERR_FILENO, msg, len);
    }
}

static inline void eprint2(const char *prog, const char *msg) {
    if (prog && msg) {
        size_t len = 0;
        const char *p = prog;
        while (*p++) len++;
        write(STDERR_FILENO, prog, len);
        write(STDERR_FILENO, ": ", 2);
        len = 0;
        p = msg;
        while (*p++) len++;
        write(STDERR_FILENO, msg, len);
        write(STDERR_FILENO, "\n", 1);
    }
}

/* Signal name helper */
static inline const char *sig_name(int sig) {
    switch (sig) {
        case SIGHUP:    return "HUP";
        case SIGINT:    return "INT";
        case SIGQUIT:   return "QUIT";
        case SIGKILL:   return "KILL";
        case SIGTERM:   return "TERM";
        case SIGUSR1:   return "USR1";
        case SIGUSR2:   return "USR2";
        case SIGSTOP:   return "STOP";
        case SIGCONT:   return "CONT";
        case SIGCHLD:   return "CHLD";
        case SIGALRM:   return "ALRM";
        default:        return "??";
    }
}

/* Process state characters (for ps) */
static inline const char *proc_state(char s) {
    switch (s) {
        case 'R': return "Running";
        case 'S': return "Sleeping";
        case 'D': return "Disk Sleep";
        case 'Z': return "Zombie";
        case 'T': return "Stopped";
        case 't': return "Tracing";
        case 'X': return "Dead";
        default:  return "Unknown";
    }
}

/* Permissions string builder */
static inline void mode_string(mode_t mode, char *buf) {
    buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISBLK(mode) ? 'b' :
             S_ISCHR(mode) ? 'c' : S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

#endif /* _FOREST_H */
