/**
 * Forest-OS Unix/Linux Compatibility Layer Header
 * 
 * This header provides declarations for all Unix compatibility features
 * implemented to enable running Linux/Unix applications on Forest-OS.
 */

#ifndef UNIX_COMPAT_H
#define UNIX_COMPAT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct vfs_node vfs_node_t;

// ============================================
// /proc Filesystem
// ============================================

// Initialize procfs
bool procfs_init(void);
vfs_node_t* procfs_get_root(void);
void procfs_add_process(uint32_t pid);
void procfs_remove_process(uint32_t pid);

// ============================================
// /sys Filesystem  
// ============================================

// Initialize sysfs
bool sysfs_init(void);
vfs_node_t* sysfs_get_root(void);

// ============================================
// System V Semaphores
// ============================================

// Initialize semaphore subsystem
void sysv_sem_init(void);

// Semaphore operations
int sysv_semget(key_t key, int nsems, int semflg);
int sysv_semop(int semid, struct sembuf* sops, int nsops);
int sysv_semctl(int semid, int semnum, int cmd, union semun* arg);
void sysv_sem_close_all_for_task(uint32_t pid);

// ============================================
// System V Message Queues
// ============================================

// Initialize message queue subsystem
void sysv_msg_init(void);

// Message queue operations
int sysv_msgget(key_t key, int msgflg);
int sysv_msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg);
int sysv_msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg);
int sysv_msgctl(int msqid, int cmd, struct msqid_ds* buf);
void sysv_msg_close_all_for_task(uint32_t pid);

// ============================================
// epoll - I/O Multiplexing
// ============================================

// Initialize epoll subsystem
void epoll_init(void);

// epoll operations
int epoll_create(int size);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout, const sigset_t* sigmask);
int epoll_pwait2(int epfd, struct epoll_event* events, int maxevents, const struct timespec* timeout, const sigset_t* sigmask);
int epoll_close(int fd);
void epoll_close_all_for_task(uint32_t pid);

// ============================================
// inotify - File Monitoring
// ============================================

// Initialize inotify subsystem
void inotify_subsystem_init(void);

// inotify operations
int inotify_init(void);
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char* path, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
void inotify_generate_event(const char* path, uint32_t mask);
int inotify_read(int fd, void* buf, size_t count);
int inotify_close(int fd);
void inotify_close_all_for_task(uint32_t pid);

// ============================================
// POSIX Shared Memory (/dev/shm)
// ============================================

// Initialize POSIX shared memory
void posix_shm_init(void);

// Shared memory operations
int shm_open(const char* name, int oflag, mode_t mode);
int shm_unlink(const char* name);
int shm_ftruncate(int fd, off_t length);
int shm_fstat(int fd, struct stat* statbuf);
void shm_close(int fd);
void posix_shm_close_all_for_task(uint32_t pid);
void* shm_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int shm_munmap(void* addr, size_t length);

// ============================================
// eventfd
// ============================================

// Initialize eventfd subsystem
void eventfd_init(void);

// eventfd operations
int eventfd_create(unsigned int initval, int flags);
int eventfd_read(int fd, uint64_t* value);
int eventfd_write(int fd, uint64_t value);
int eventfd_close(int fd);
void eventfd_close_all_for_task(uint32_t pid);

// ============================================
// signalfd
// ============================================

// Initialize signalfd subsystem
void signalfd_init(void);

// signalfd operations
int signalfd_create(int fd, const sigset_t* mask, int flags);
int signalfd_read(int fd, struct signalfd_siginfo* info);
int signalfd_close(int fd);

// ============================================
// timerfd
// ============================================

// Initialize timerfd subsystem
void timerfd_init(void);

// timerfd operations
int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec* new_value, struct itimerspec* old_value);
int timerfd_gettime(int fd, struct itimerspec* curr_value);
int timerfd_read(int fd, uint64_t* expirations);
int timerfd_close(int fd);

// ============================================
// Master Initialization Function
// ============================================

/**
 * Initialize all Unix compatibility subsystems
 * Call this during kernel startup after basic VFS is ready
 */
static inline void unix_compat_init_all(void) {
    // Filesystems
    procfs_init();
    sysfs_init();
    
    // IPC
    sysv_sem_init();
    sysv_msg_init();
    posix_shm_init();
    
    // I/O Multiplexing
    epoll_init();
    inotify_subsystem_init();
    
    // File descriptors
    eventfd_init();
    signalfd_init();
    timerfd_init();
}

#endif // UNIX_COMPAT_H
