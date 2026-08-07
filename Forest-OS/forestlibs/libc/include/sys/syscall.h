/*
 * sys/syscall.h - System call numbers
 * 
 * Linux x86_64 compatible system call numbers for Fern.
 * These match the Linux kernel syscall ABI for compatibility.
 */
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Architecture detection */
#ifndef ARCH_64BIT
#if defined(__x86_64__) || defined(_M_X64) || defined(__LP64__)
#define ARCH_64BIT 1
#define ARCH_32BIT 0
#else
#define ARCH_64BIT 0
#define ARCH_32BIT 1
#endif
#endif

/* Syscall vector (interrupt number) */
#define SYSCALL_VECTOR 0x80

/*
 * System call numbers - Linux x86_64 compatible
 * 
 * These are the official Linux syscall numbers for x86_64 architecture.
 * Fern implements a subset of these for Unix/Linux compatibility.
 */

/* File operations */
#define SYS_read                    0
#define SYS_write                   1
#define SYS_open                    2
#define SYS_close                   3
#define SYS_stat                    4
#define SYS_fstat                   5
#define SYS_lstat                   6
#define SYS_poll                    7
#define SYS_lseek                   8

/* Memory management */
#define SYS_mmap                    9
#define SYS_mprotect                10
#define SYS_munmap                  11
#define SYS_brk                     12

/* Signals */
#define SYS_rt_sigaction            13
#define SYS_rt_sigprocmask          14
#define SYS_rt_sigreturn            15

/* File control and I/O */
#define SYS_ioctl                   16
#define SYS_pread64                 17
#define SYS_pwrite64                18
#define SYS_readv                   19
#define SYS_writev                  20
#define SYS_access                  21
#define SYS_pipe                    22
#define SYS_select                  23

/* Scheduling */
#define SYS_sched_yield             24

/* Memory mapping extensions */
#define SYS_mremap                  25
#define SYS_msync                   26
#define SYS_mincore                 27
#define SYS_madvise                 28

/* IPC - Shared memory */
#define SYS_shmget                  29
#define SYS_shmat                   30
#define SYS_shmctl                  31

/* File descriptor operations */
#define SYS_dup                     32
#define SYS_dup2                    33
#define SYS_pause                   34
#define SYS_nanosleep               35

/* Timers */
#define SYS_getitimer               36
#define SYS_alarm                   37
#define SYS_setitimer               38

/* Process identification */
#define SYS_getpid                  39

/* File transfer */
#define SYS_sendfile                40

/* Networking */
#define SYS_socket                  41
#define SYS_connect                 42
#define SYS_accept                  43
#define SYS_sendto                  44
#define SYS_recvfrom                45
#define SYS_sendmsg                 46
#define SYS_recvmsg                 47
#define SYS_shutdown                48
#define SYS_bind                    49
#define SYS_listen                  50
#define SYS_getsockname             51
#define SYS_getpeername             52
#define SYS_socketpair              53
#define SYS_setsockopt              54
#define SYS_getsockopt              55

/* Process creation and control */
#define SYS_clone                   56
#define SYS_fork                    57
#define SYS_vfork                   58
#define SYS_execve                  59
#define SYS_exit                    60
#define SYS_wait4                   61
#define SYS_kill                    62

/* System information */
#define SYS_uname                   63

/* IPC - Semaphores */
#define SYS_semget                  64
#define SYS_semop                   65
#define SYS_semctl                  66

/* IPC - Shared memory detach */
#define SYS_shmdt                   67

/* IPC - Message queues */
#define SYS_msgget                  68
#define SYS_msgsnd                  69
#define SYS_msgrcv                  70
#define SYS_msgctl                  71

/* File control */
#define SYS_fcntl                   72
#define SYS_flock                   73
#define SYS_fsync                   74
#define SYS_fdatasync               75
#define SYS_truncate                76
#define SYS_ftruncate               77

/* Directory operations */
#define SYS_getdents                78
#define SYS_getcwd                  79
#define SYS_chdir                   80
#define SYS_fchdir                  81
#define SYS_rename                  82
#define SYS_mkdir                   83
#define SYS_rmdir                   84
#define SYS_creat                   85

/* Link operations */
#define SYS_link                    86
#define SYS_unlink                  87
#define SYS_symlink                 88
#define SYS_readlink                89

/* Permission operations */
#define SYS_chmod                   90
#define SYS_fchmod                  91
#define SYS_chown                   92
#define SYS_fchown                  93
#define SYS_lchown                  94
#define SYS_umask                   95

/* Time operations */
#define SYS_gettimeofday            96
#define SYS_getrlimit               97
#define SYS_getrusage               98
#define SYS_sysinfo                 99
#define SYS_times                   100

/* Debugging */
#define SYS_ptrace                  101

/* User/Group identification */
#define SYS_getuid                  102
#define SYS_syslog                  103
#define SYS_getgid                  104
#define SYS_setuid                  105
#define SYS_setgid                  106
#define SYS_geteuid                 107
#define SYS_getegid                 108
#define SYS_setpgid                 109
#define SYS_getppid                 110
#define SYS_getpgrp                 111
#define SYS_setsid                  112
#define SYS_setreuid                113
#define SYS_setregid                114
#define SYS_getgroups               115
#define SYS_setgroups               116
#define SYS_setresuid               117
#define SYS_getresuid               118
#define SYS_setresgid               119
#define SYS_getresgid               120
#define SYS_getpgid                 121
#define SYS_setfsuid                122
#define SYS_setfsgid                123
#define SYS_getsid                  124
#define SYS_capget                  125
#define SYS_capset                  126

/* Signal extensions */
#define SYS_rt_sigpending           127
#define SYS_rt_sigtimedwait         128
#define SYS_rt_sigqueueinfo         129
#define SYS_rt_sigsuspend           130
#define SYS_sigaltstack             131

/* File time operations */
#define SYS_utime                   132
#define SYS_mknod                   133

/* File system operations */
#define SYS_statfs                  137
#define SYS_fstatfs                 138
#define SYS_sysfs                   139

/* Priority and scheduling */
#define SYS_getpriority             140
#define SYS_setpriority             141
#define SYS_sched_setparam          142
#define SYS_sched_getparam          143
#define SYS_sched_setscheduler      144
#define SYS_sched_getscheduler      145
#define SYS_sched_get_priority_max  146
#define SYS_sched_get_priority_min  147
#define SYS_sched_rr_get_interval   148

/* Memory locking */
#define SYS_mlock                   149
#define SYS_munlock                 150
#define SYS_mlockall                151
#define SYS_munlockall              152

/* Terminal */
#define SYS_vhangup                 153

/* Architecture specific */
#define SYS_modify_ldt              154
#define SYS_pivot_root              155
#define SYS_prctl                   157
#define SYS_arch_prctl              158

/* Time adjustment */
#define SYS_adjtimex                159
#define SYS_setrlimit               160

/* Root operations */
#define SYS_chroot                  161
#define SYS_sync                    162
#define SYS_acct                    163
#define SYS_settimeofday            164

/* Mount operations */
#define SYS_mount                   165
#define SYS_umount2                 166

/* Swap */
#define SYS_swapon                  167
#define SYS_swapoff                 168

/* System control */
#define SYS_reboot                  169
#define SYS_sethostname             170
#define SYS_setdomainname           171

/* I/O permissions */
#define SYS_iopl                    172
#define SYS_ioperm                  173

/* Module operations */
#define SYS_init_module             175
#define SYS_delete_module           176

/* Quota */
#define SYS_quotactl                179

/* Thread ID */
#define SYS_gettid                  186

/* Read ahead */
#define SYS_readahead               187

/* Extended attributes */
#define SYS_setxattr                188
#define SYS_lsetxattr               189
#define SYS_fsetxattr               190
#define SYS_getxattr                191
#define SYS_lgetxattr               192
#define SYS_fgetxattr               193
#define SYS_listxattr               194
#define SYS_llistxattr              195
#define SYS_flistxattr              196
#define SYS_removexattr             197
#define SYS_lremovexattr            198
#define SYS_fremovexattr            199

/* Thread kill */
#define SYS_tkill                   200

/* Time */
#define SYS_time                    201

/* Futex */
#define SYS_futex                   202

/* CPU affinity */
#define SYS_sched_setaffinity       203
#define SYS_sched_getaffinity       204

/* Set thread ID */
#define SYS_set_thread_area         205
#define SYS_get_thread_area         211

/* Async I/O */
#define SYS_io_setup                206
#define SYS_io_destroy              207
#define SYS_io_getevents            208
#define SYS_io_submit               209
#define SYS_io_cancel               210

/* Epoll */
#define SYS_epoll_create            213
#define SYS_epoll_ctl_old           214
#define SYS_epoll_wait_old          215

/* Directory entries 64 */
#define SYS_getdents64              217

/* TID address */
#define SYS_set_tid_address         218

/* Restart syscall */
#define SYS_restart_syscall         219

/* Timers (POSIX) */
#define SYS_timer_create            222
#define SYS_timer_settime           223
#define SYS_timer_gettime           224
#define SYS_timer_getoverrun        225
#define SYS_timer_delete            226

/* Clock operations */
#define SYS_clock_settime           227
#define SYS_clock_gettime           228
#define SYS_clock_getres            229
#define SYS_clock_nanosleep         230

/* Exit group */
#define SYS_exit_group              231

/* Epoll wait */
#define SYS_epoll_wait              232
#define SYS_epoll_ctl               233

/* Thread group kill */
#define SYS_tgkill                  234

/* File times */
#define SYS_utimes                  235

/* Memory policy */
#define SYS_mbind                   237
#define SYS_set_mempolicy           238
#define SYS_get_mempolicy           239

/* Message queue */
#define SYS_mq_open                 240
#define SYS_mq_unlink               241
#define SYS_mq_timedsend            242
#define SYS_mq_timedreceive         243
#define SYS_mq_notify               244
#define SYS_mq_getsetattr           245

/* Kexec */
#define SYS_kexec_load              246

/* Waitid */
#define SYS_waitid                  247

/* Keys */
#define SYS_add_key                 248
#define SYS_request_key             249
#define SYS_keyctl                  250

/* I/O priority */
#define SYS_ioprio_set              251
#define SYS_ioprio_get              252

/* Inotify */
#define SYS_inotify_init            253
#define SYS_inotify_add_watch       254
#define SYS_inotify_rm_watch        255

/* Page migration */
#define SYS_migrate_pages           256

/* *at syscalls */
#define SYS_openat                  257
#define SYS_mkdirat                 258
#define SYS_mknodat                 259
#define SYS_fchownat                260
#define SYS_futimesat               261
#define SYS_newfstatat              262
#define SYS_unlinkat                263
#define SYS_renameat                264
#define SYS_linkat                  265
#define SYS_symlinkat               266
#define SYS_readlinkat              267
#define SYS_fchmodat                268
#define SYS_faccessat               269

/* Select variants */
#define SYS_pselect6                270
#define SYS_ppoll                   271

/* Namespace */
#define SYS_unshare                 272

/* Robust list */
#define SYS_set_robust_list         273
#define SYS_get_robust_list         274

/* Splice */
#define SYS_splice                  275
#define SYS_tee                     276
#define SYS_sync_file_range         277
#define SYS_vmsplice                278

/* Move pages */
#define SYS_move_pages              279

/* File times nanosecond */
#define SYS_utimensat               280

/* Epoll variants */
#define SYS_epoll_pwait             281

/* Signal FD */
#define SYS_signalfd                282

/* Timer FD */
#define SYS_timerfd_create          283
#define SYS_eventfd                 284
#define SYS_fallocate               285
#define SYS_timerfd_settime         286
#define SYS_timerfd_gettime         287

/* Accept4 */
#define SYS_accept4                 288

/* Signalfd4 */
#define SYS_signalfd4               289
#define SYS_eventfd2                290
#define SYS_epoll_create1           291
#define SYS_dup3                    292
#define SYS_pipe2                   293
#define SYS_inotify_init1           294

/* Vectored I/O with offset */
#define SYS_preadv                  295
#define SYS_pwritev                 296

/* Thread signal queue */
#define SYS_rt_tgsigqueueinfo       297

/* Performance monitoring */
#define SYS_perf_event_open         298

/* Receive multiple messages */
#define SYS_recvmmsg                299

/* Fanotify */
#define SYS_fanotify_init           300
#define SYS_fanotify_mark           301

/* Resource limit 64 */
#define SYS_prlimit64               302

/* Name to handle */
#define SYS_name_to_handle_at       303
#define SYS_open_by_handle_at       304

/* Clock adjust */
#define SYS_clock_adjtime           305

/* Syncfs */
#define SYS_syncfs                  306

/* Send multiple messages */
#define SYS_sendmmsg                307

/* Namespace */
#define SYS_setns                   308

/* CPU info */
#define SYS_getcpu                  309

/* Process VM */
#define SYS_process_vm_readv        310
#define SYS_process_vm_writev       311

/* Kernel compare */
#define SYS_kcmp                    312

/* Module from file */
#define SYS_finit_module            313

/* Scheduling attributes */
#define SYS_sched_setattr           314
#define SYS_sched_getattr           315

/* Rename at 2 */
#define SYS_renameat2               316

/* Seccomp */
#define SYS_seccomp                 317

/* Random */
#define SYS_getrandom               318

/* Memfd */
#define SYS_memfd_create            319

/* Kexec file */
#define SYS_kexec_file_load         320

/* BPF */
#define SYS_bpf                     321

/* Execve at */
#define SYS_execveat                322

/* Userfaultfd */
#define SYS_userfaultfd             323

/* Memory barrier */
#define SYS_membarrier              324

/* Mlock 2 */
#define SYS_mlock2                  325

/* Copy file range */
#define SYS_copy_file_range         326

/* Vectored I/O v2 */
#define SYS_preadv2                 327
#define SYS_pwritev2                328

/* Protection keys */
#define SYS_pkey_mprotect           329
#define SYS_pkey_alloc              330
#define SYS_pkey_free               331

/* Statx */
#define SYS_statx                   332

/* IO pgetevents */
#define SYS_io_pgetevents           333

/* Rseq */
#define SYS_rseq                    334

/* =====================================================
 * Fern specific syscalls (start at 470)
 * These extend Linux compatibility with Forest-specific features
 * ===================================================== */

/* Network info (Forest extension) */
#define SYS_netinfo                 470

/* Framebuffer operations (Forest extension) */
#define SYS_mmap_fb                 471
#define SYS_munmap_fb               472
#define SYS_get_fb_info             473

/* Power management (Forest extension) */
#define SYS_power                   474

/* User management (Forest extension) */
#define SYS_userctl                 475

/* Framebuffer watcher (Forest extension) */
#define SYS_start_fb_watcher        476
#define SYS_stop_fb_watcher         477
#define SYS_fb_flush                478

/* Input event reading (Forest extension) */
#define SYS_read_kbd_event          479
#define SYS_read_mouse_event        480
#define SYS_poll_input              481

/* Maximum syscall number */
#define SYS_MAX                     482

/* Convenience macro for invoking syscalls */
long syscall(long number, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSCALL_H */
