/*
 * unistd.h - Standard symbolic constants and types
 * 
 * POSIX compatible system interface for Fern libc.
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

/* POSIX version constants */
#define _POSIX_VERSION 200809L
#define _POSIX2_VERSION 200809L
#define _XOPEN_VERSION 700

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Null pointer constant */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Access mode flags for access() */
#define F_OK 0  /* Test for existence */
#define X_OK 1  /* Test for execute permission */
#define W_OK 2  /* Test for write permission */
#define R_OK 4  /* Test for read permission */

/* Seek constants (also in stdio.h) */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* File I/O functions */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int fsync(int fd);
int fdatasync(int fd);
int ftruncate(int fd, off_t length);
int truncate(const char *path, off_t length);

/* File descriptor manipulation */
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);

/* Process control */
pid_t fork(void);
pid_t vfork(void);
int execve(const char *pathname, char *const argv[], char *const envp[]);
int execv(const char *pathname, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execvpe(const char *file, char *const argv[], char *const envp[]);
int execl(const char *pathname, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *pathname, const char *arg, ...);
void _exit(int status) __attribute__((noreturn));

/* Process identification */
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
pid_t getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t setsid(void);
pid_t getsid(pid_t pid);
pid_t gettid(void);

/* User and group identification */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t euid);
int setegid(gid_t egid);
int setreuid(uid_t ruid, uid_t euid);
int setregid(gid_t rgid, gid_t egid);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t *list);

/* Working directory */
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int fchdir(int fd);
int chroot(const char *path);

/* File system operations */
int access(const char *pathname, int mode);
int faccessat(int dirfd, const char *pathname, int mode, int flags);
int link(const char *oldpath, const char *newpath);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int symlink(const char *target, const char *linkpath);
int symlinkat(const char *target, int newdirfd, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
int unlink(const char *pathname);
int unlinkat(int dirfd, const char *pathname, int flags);
int rmdir(const char *pathname);

/* File permission operations */
int chmod(const char *pathname, mode_t mode);
int fchmod(int fd, mode_t mode);
int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags);
int chown(const char *pathname, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
int lchown(const char *pathname, uid_t owner, gid_t group);

/* Time functions */
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
unsigned int alarm(unsigned int seconds);
int pause(void);

/* Terminal functions */
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t buflen);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);

/* System configuration */
long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);
size_t confstr(int name, char *buf, size_t len);

/* System configuration names for sysconf() */
#define _SC_ARG_MAX             0
#define _SC_CHILD_MAX           1
#define _SC_CLK_TCK             2
#define _SC_NGROUPS_MAX         3
#define _SC_OPEN_MAX            4
#define _SC_STREAM_MAX          5
#define _SC_TZNAME_MAX          6
#define _SC_JOB_CONTROL         7
#define _SC_SAVED_IDS           8
#define _SC_REALTIME_SIGNALS    9
#define _SC_PRIORITY_SCHEDULING 10
#define _SC_TIMERS              11
#define _SC_ASYNCHRONOUS_IO     12
#define _SC_PRIORITIZED_IO      13
#define _SC_SYNCHRONIZED_IO     14
#define _SC_FSYNC               15
#define _SC_MAPPED_FILES        16
#define _SC_MEMLOCK             17
#define _SC_MEMLOCK_RANGE       18
#define _SC_MEMORY_PROTECTION   19
#define _SC_MESSAGE_PASSING     20
#define _SC_SEMAPHORES          21
#define _SC_SHARED_MEMORY_OBJECTS 22
#define _SC_AIO_LISTIO_MAX      23
#define _SC_AIO_MAX             24
#define _SC_AIO_PRIO_DELTA_MAX  25
#define _SC_DELAYTIMER_MAX      26
#define _SC_MQ_OPEN_MAX         27
#define _SC_MQ_PRIO_MAX         28
#define _SC_VERSION             29
#define _SC_PAGESIZE            30
#define _SC_PAGE_SIZE           _SC_PAGESIZE
#define _SC_RTSIG_MAX           31
#define _SC_SEM_NSEMS_MAX       32
#define _SC_SEM_VALUE_MAX       33
#define _SC_SIGQUEUE_MAX        34
#define _SC_TIMER_MAX           35
#define _SC_NPROCESSORS_CONF    83
#define _SC_NPROCESSORS_ONLN    84
#define _SC_PHYS_PAGES          85
#define _SC_AVPHYS_PAGES        86
#define _SC_HOST_NAME_MAX       180
#define _SC_LOGIN_NAME_MAX      181

/* Hostname operations */
int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);
int getdomainname(char *name, size_t len);
int setdomainname(const char *name, size_t len);

/* Miscellaneous */
int nice(int inc);
void sync(void);
void swab(const void *from, void *to, ssize_t n);
int brk(void *addr);
void *sbrk(intptr_t increment);
int getopt(int argc, char * const argv[], const char *optstring);

/* getopt variables */
extern char *optarg;
extern int optind, opterr, optopt;

/* Encryption (not implemented in Fern) */
char *crypt(const char *key, const char *salt);
void encrypt(char block[64], int edflag);

/* Random data */
int getentropy(void *buffer, size_t length);

/* Fern specific extensions */
int poweroff(void);
int reboot(int howto);

#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
