#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "sys/types.h"
#include "net.h"
#include "poll.h"
#include "sys/uio.h"
#include "time.h"
#include "sys/utsname.h"

/* Standard file descriptors */
#ifndef STDIN_FILENO
#define STDIN_FILENO   0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO  1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO  2
#endif

/* access() mode bits */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

/* lseek() */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

/* select/poll forward-compatible types */
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

struct stat;

ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
int open(const char *pathname, int flags);
int close(int fd);
int lseek(int fd, int offset, int whence);
int fsync(int fd);
int fdatasync(int fd);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int getpid(void);
int getppid(void);
int gettid(void);
int unlink(const char *pathname);
int access(const char *path, int mode);
int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int usleep(useconds_t useconds);
unsigned int sleep(unsigned int seconds);
unsigned int alarm(unsigned int seconds);
int pause(void);
int uname(struct utsname *uts_buffer);
int brk(void *addr);
int time(int *tloc);
void _exit(int status);
int sync(void);

/* Descriptor duplication and pipes */
int dup(int fd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);

/* Process and job control */
int fork(void);
int execve(const char *pathname, char *const argv[], char *const envp[]);
int wait(int *status);
int waitpid(int pid, int *status, int options);
int kill(int pid, int sig);
long times(void *buf); /* raw kernel tick count when buf is NULL, see userspace/libc/syscalls.c */
int sysinfo(void *info); /* real mem/uptime info, see sys_sysinfo() in syscall.c */
long klogctl(int type, char *buf, int len); /* kernel log ring buffer, see sys_syslog() in syscall.c */
int setpgid(int pid, int pgid);
int getpgrp(void);
int getpgid(int pid);
int setsid(void);
int getsid(int pid);

/* User/group operations */
int getuid(void);
int getgid(void);
int geteuid(void);
int getegid(void);
int setuid(int uid);
int setgid(int gid);
int seteuid(int euid);
int setegid(int egid);
int setreuid(int ruid, int euid);
int setregid(int rgid, int egid);
int setresuid(int ruid, int euid, int suid);
int getresuid(int *ruid, int *euid, int *suid);
int setresgid(int rgid, int egid, int sgid);
int getresgid(int *rgid, int *egid, int *sgid);
int getgroups(int size, int *list);
int setgroups(size_t size, const int *list);

/* Host identity */
int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);
int getdomainname(char *name, size_t len);
int setdomainname(const char *name, size_t len);

/* Directory and path operations */
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int mkdir(const char *pathname, mode_t mode);
int rmdir(const char *pathname);
int creat(const char *pathname, int mode);
int link(const char *oldpath, const char *newpath);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
int rename(const char *oldpath, const char *newpath);
int chmod(const char *pathname, mode_t mode);
int fchmod(int fd, mode_t mode);
int chown(const char *pathname, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
mode_t umask(mode_t mask);
ssize_t getdents(int fd, void *dirp, size_t count);

/* Real task-list enumeration (SYS_GET_TASKS) backing `ps`. Copies up to
 * max_entries fixed-size task_info_t records (see task.h in the kernel
 * source, mirrored by callers like userspace/ps.c) into buf. Returns the
 * number of entries written, or -1/errno on failure. */
ssize_t get_tasks(void *buf, size_t max_entries);

/* *at() APIs */
int openat(int dirfd, const char *pathname, int flags, int mode);
int mkdirat(int dirfd, const char *pathname, int mode);
int unlinkat(int dirfd, const char *pathname, int flags);
int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags);
int faccessat(int dirfd, const char *pathname, int mode, int flags);

/* I/O multiplexing and vector I/O */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int fd, const void *addr, int addrlen);
int connect(int sockfd, const void *addr, int addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, void *addr, int *addrlen);
int shutdown(int sockfd, int how);
int getsockname(int sockfd, void *addr, int *addrlen);
int getpeername(int sockfd, void *addr, int *addrlen);
ssize_t sendmsg(int sockfd, const void *msg, int flags);
ssize_t recvmsg(int sockfd, void *msg, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const void *addr, int addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 void *addr, int *addrlen);
int netinfo(net_socket_info_t* buffer, int max_entries);
int poweroff(void);
int reboot(int howto);
int user_syscall(int op, const char* user, const char* pass, const char* aux,
                 void* out, int max_entries);
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int isatty(int fd);
int tcgetattr(int fd, void *termios_p);
int tcsetattr(int fd, int optional_actions, const void *termios_p);
int shmget(int key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int shmctl(int shmid, int cmd, void *buf);
int shmdt(const void *shmaddr);
int clone(int flags, void *stack, void *ptid, void *ctid, unsigned long tls);
int futex(int *uaddr, int op, int val, const void *timeout, int *uaddr2, int val3);

#ifdef __cplusplus
}
#endif

#endif
