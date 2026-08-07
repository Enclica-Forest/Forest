/*
 * syscall_table.h - Efficient syscall table definitions for Fern
 */

#ifndef SYSCALL_TABLE_H
#define SYSCALL_TABLE_H

#include "include/types.h"

// Syscall function pointer types
typedef int32 (*syscall_0arg_t)(void);
typedef int32 (*syscall_1arg_t)(uint32 arg1);
typedef int32 (*syscall_2arg_t)(uint32 arg1, uint32 arg2);
typedef int32 (*syscall_3arg_t)(uint32 arg1, uint32 arg2, uint32 arg3);
typedef int32 (*syscall_4arg_t)(uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4);
typedef int32 (*syscall_5arg_t)(uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5);
typedef int32 (*syscall_6arg_t)(uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6);

// Syscall descriptor structure
typedef struct {
    const char *name;
    uint8 arg_count;
    union {
        syscall_0arg_t fn0;
        syscall_1arg_t fn1;
        syscall_2arg_t fn2;
        syscall_3arg_t fn3;
        syscall_4arg_t fn4;
        syscall_5arg_t fn5;
        syscall_6arg_t fn6;
    } func;
    uint8 flags;
} syscall_desc_t;

// Syscall flags
#define SYSCALL_FLAG_NONE      0x00
#define SYSCALL_FLAG_CRITICAL   0x01   // Critical for system operation
#define SYSCALL_FLAG_FILE       0x02   // File I/O operations
#define SYSCALL_FLAG_NET        0x04   // Network operations
#define SYSCALL_FLAG_PROCESS    0x08   // Process management
#define SYSCALL_FLAG_MEMORY     0x10   // Memory management
#define SYSCALL_FLAG_ADMIN      0x20   // Requires admin privileges
#define SYSCALL_FLAG_FOREST     0x40   // Fern specific

// Function declarations for all syscalls
int32 sys_read(uint32 fd, uint32 buf, uint32 count);
int32 sys_write(uint32 fd, uint32 buf, uint32 count);
int32 sys_open(uint32 pathname, uint32 flags, uint32 mode);
int32 sys_close(uint32 fd);
int32 sys_lseek(uint32 fd, int32 offset, uint32 whence);
int32 sys_stat(uint32 pathname, uint32 statbuf);
int32 sys_fstat(uint32 fd, uint32 statbuf);
int32 sys_access(uint32 pathname, uint32 mode);
int32 sys_pipe(uint32 pipefd);
int32 sys_dup(uint32 oldfd);
int32 sys_dup2(uint32 oldfd, uint32 newfd);
int32 sys_ioctl(uint32 fd, uint32 request, uint32 arg);
int32 sys_fcntl(uint32 fd, uint32 cmd, uint32 arg);

// Process management
int32 sys_getpid(void);
int32 sys_getppid(void);
int32 sys_getuid(void);
int32 sys_getgid(void);
int32 sys_geteuid(void);
int32 sys_getegid(void);
int32 sys_setuid(uint32 uid);
int32 sys_setgid(uint32 gid);
int32 sys_seteuid(uint32 uid);
int32 sys_setegid(uint32 gid);
int32 sys_setreuid(uint32 ruid, uint32 euid);
int32 sys_setregid(uint32 rgid, uint32 egid);
int32 sys_getpgrp(void);
int32 sys_setpgid(uint32 pid, uint32 pgid);
int32 sys_setsid(void);
int32 sys_getsid(uint32 pid);
int32 sys_fork(void);
int32 sys_execve(uint32 pathname, uint32 argv, uint32 envp);
int32 sys_exit(uint32 status);
int32 sys_wait4(uint32 pid, uint32 status, uint32 options, uint32 rusage);
int32 sys_waitpid(uint32 pid, uint32 status, uint32 options);
int32 sys_kill(uint32 pid, uint32 sig);

// Memory management
int32 sys_brk(uint32 addr);
int32 sys_mmap(uint32 addr, uint32 length, uint32 prot, uint32 flags, uint32 fd, uint32 offset);
int32 sys_munmap(uint32 addr, uint32 length);
int32 sys_mprotect(uint32 addr, uint32 length, uint32 prot);

// Time management
int32 sys_time(uint32 tloc);
int32 sys_nanosleep(uint32 req, uint32 rem);
int32 sys_gettimeofday(uint32 tv, uint32 tz);
int32 sys_settimeofday(uint32 tv, uint32 tz);

// File system operations
int32 sys_unlink(uint32 pathname);
int32 sys_mknod(uint32 pathname, uint32 mode, uint32 dev);
int32 sys_chdir(uint32 pathname);
int32 sys_fchdir(uint32 fd);
int32 sys_getcwd(uint32 buf, uint32 size);
int32 sys_rename(uint32 oldpath, uint32 newpath);
int32 sys_mkdir(uint32 pathname, uint32 mode);
int32 sys_rmdir(uint32 pathname);
int32 sys_creat(uint32 pathname, uint32 mode);
int32 sys_link(uint32 oldpath, uint32 newpath);
int32 sys_symlink(uint32 oldpath, uint32 newpath);
int32 sys_readlink(uint32 path, uint32 buf, uint32 bufsiz);
int32 sys_chmod(uint32 pathname, uint32 mode);
int32 sys_fchmod(uint32 fd, uint32 mode);
int32 sys_chown(uint32 pathname, uint32 owner, uint32 group);
int32 sys_fchown(uint32 fd, uint32 owner, uint32 group);
int32 sys_lchown(uint32 pathname, uint32 owner, uint32 group);
int32 sys_umask(uint32 mode);

// Vectorized I/O
int32 sys_readv(uint32 fd, uint32 iov, uint32 iovcnt);
int32 sys_writev(uint32 fd, uint32 iov, uint32 iovcnt);
int32 sys_pread64(uint32 fd, uint32 buf, uint32 count, uint32 offset);
int32 sys_pwrite64(uint32 fd, uint32 buf, uint32 count, uint32 offset);

// File synchronization
int32 sys_fsync(uint32 fd);
int32 sys_fdatasync(uint32 fd);
int32 sys_ftruncate(uint32 fd, uint32 length);

// System information
int32 sys_uname(uint32 utsname);

// Network operations
int32 sys_socket(uint32 domain, uint32 type, uint32 protocol);
int32 sys_bind(uint32 fd, uint32 addr, uint32 addrlen);
int32 sys_connect(uint32 fd, uint32 addr, uint32 addrlen);
int32 sys_sendto(uint32 fd, uint32 buf, uint32 len, uint32 flags, uint32 addr, uint32 addrlen);
int32 sys_recvfrom(uint32 fd, uint32 buf, uint32 len, uint32 flags, uint32 addr, uint32 addrlen);
int32 sys_sendmsg(uint32 fd, uint32 msg, uint32 flags);
int32 sys_recvmsg(uint32 fd, uint32 msg, uint32 flags);
int32 sys_shutdown(uint32 fd, uint32 how);
int32 sys_listen(uint32 fd, uint32 backlog);
int32 sys_accept(uint32 fd, uint32 addr, uint32 addrlen);
int32 sys_getsockname(uint32 fd, uint32 addr, uint32 addrlen);
int32 sys_getpeername(uint32 fd, uint32 addr, uint32 addrlen);
int32 sys_setsockopt(uint32 fd, uint32 level, uint32 optname, uint32 optval, uint32 optlen);
int32 sys_getsockopt(uint32 fd, uint32 level, uint32 optname, uint32 optval, uint32 optlen);

// I/O multiplexing
int32 sys_select(uint32 nfds, uint32 readfds, uint32 writefds, uint32 exceptfds, uint32 timeout);
int32 sys_poll(uint32 fds, uint32 nfds, uint32 timeout);
int32 sys_ppoll(uint32 fds, uint32 nfds, uint32 timeout, uint32 sigmask);

// Advanced I/O (stubs for now)
int32 sys_epoll_create(uint32 size);
int32 sys_epoll_create1(uint32 flags);
int32 sys_epoll_ctl(uint32 epfd, uint32 op, uint32 fd, uint32 event);
int32 sys_epoll_wait(uint32 epfd, uint32 events, uint32 maxevents, uint32 timeout);
int32 sys_epoll_pwait(uint32 epfd, uint32 events, uint32 maxevents, uint32 timeout, uint32 sigmask);

// Fern specific syscalls
int32 sys_netinfo(uint32 buffer, uint32 max_entries);
int32 sys_power(uint32 action);
int32 sys_user(uint32 op, uint32 user, uint32 pass, uint32 aux, uint32 out, uint32 max_entries);

// Framebuffer operations
long sys_mmap_fb(void);
long sys_munmap_fb(void* addr);
long sys_get_fb_info(void* user_info);

// Scheduler
int32 sys_sched_yield(void);
int32 sys_sched_getscheduler(uint32 pid);
int32 sys_sched_setscheduler(uint32 pid, uint32 policy, uint32 param);
int32 sys_sched_getparam(uint32 pid, uint32 param);
int32 sys_sched_setparam(uint32 pid, uint32 param);
int32 sys_sched_get_priority_min(uint32 policy);
int32 sys_sched_get_priority_max(uint32 policy);

// External syscall table
extern const syscall_desc_t syscall_table[];

// Syscall initialization
void syscall_table_init(void);
int32 syscall_dispatch(uint32 num, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4, uint32 arg5, uint32 arg6);

// Syscall validation and debugging
const char* syscall_get_name(uint32 num);
bool syscall_is_valid(uint32 num);
void syscall_stats_print(void);

#endif