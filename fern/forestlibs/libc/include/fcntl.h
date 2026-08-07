/*
 * fcntl.h - File control options
 * 
 * POSIX compatible file control for Fern libc.
 */
#ifndef _FCNTL_H
#define _FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* File access modes (O_ACCMODE mask) */
#define O_RDONLY    0x0000      /* Open for reading only */
#define O_WRONLY    0x0001      /* Open for writing only */
#define O_RDWR      0x0002      /* Open for reading and writing */
#define O_ACCMODE   0x0003      /* Mask for file access modes */

/* File creation flags */
#define O_CREAT     0x0040      /* Create file if it does not exist */
#define O_EXCL      0x0080      /* Fail if file already exists (with O_CREAT) */
#define O_NOCTTY    0x0100      /* Don't assign controlling terminal */
#define O_TRUNC     0x0200      /* Truncate file to zero length */

/* File status flags */
#define O_APPEND    0x0400      /* Append on each write */
#define O_NONBLOCK  0x0800      /* Non-blocking mode */
#define O_NDELAY    O_NONBLOCK  /* Non-blocking (POSIX name) */
#define O_DSYNC     0x1000      /* Synchronized I/O data integrity completion */
#define O_SYNC      0x4000      /* Synchronized I/O file integrity completion */
#define O_RSYNC     O_SYNC      /* Synchronized read operations */

/* Linux-specific flags */
#define O_DIRECTORY 0x10000     /* Must be a directory */
#define O_NOFOLLOW  0x20000     /* Don't follow symbolic links */
#define O_CLOEXEC   0x80000     /* Close on exec */
#define O_DIRECT    0x4000      /* Direct disk access (hint) */
#define O_NOATIME   0x40000     /* Do not update access time */
#define O_PATH      0x200000    /* Resolve pathname but do not open file */
#define O_TMPFILE   0x400000    /* Create unnamed temporary file */

/* fcntl() commands */
#define F_DUPFD         0   /* Duplicate file descriptor */
#define F_GETFD         1   /* Get file descriptor flags */
#define F_SETFD         2   /* Set file descriptor flags */
#define F_GETFL         3   /* Get file status flags */
#define F_SETFL         4   /* Set file status flags */
#define F_GETLK         5   /* Get record locking information */
#define F_SETLK         6   /* Set record locking information */
#define F_SETLKW        7   /* Set record locking information; wait if blocked */
#define F_SETOWN        8   /* Set process or process group to receive SIGURG signals */
#define F_GETOWN        9   /* Get process or process group ID to receive SIGURG signals */
#define F_SETSIG        10  /* Set signal to be sent */
#define F_GETSIG        11  /* Get signal to be sent */
#define F_DUPFD_CLOEXEC 1030 /* Duplicate file descriptor with close-on-exec set */

/* File descriptor flags */
#define FD_CLOEXEC  1       /* Close on exec flag */

/* flock() structure for record locking */
struct flock {
    short l_type;       /* Type of lock: F_RDLCK, F_WRLCK, F_UNLCK */
    short l_whence;     /* How to interpret l_start: SEEK_SET, SEEK_CUR, SEEK_END */
    off_t l_start;      /* Starting offset for lock */
    off_t l_len;        /* Number of bytes to lock */
    pid_t l_pid;        /* PID of process blocking our lock (F_GETLK only) */
};

/* Lock types */
#define F_RDLCK     0   /* Shared or read lock */
#define F_WRLCK     1   /* Exclusive or write lock */
#define F_UNLCK     2   /* Unlock */

/* Advisory record locking flags (POSIX) */
#define POSIX_FADV_NORMAL       0
#define POSIX_FADV_RANDOM       1
#define POSIX_FADV_SEQUENTIAL   2
#define POSIX_FADV_WILLNEED     3
#define POSIX_FADV_DONTNEED     4
#define POSIX_FADV_NOREUSE      5

/* openat() flags */
#define AT_FDCWD            -100    /* Special value for dirfd */
#define AT_SYMLINK_NOFOLLOW 0x100   /* Do not follow symbolic links */
#define AT_REMOVEDIR        0x200   /* Remove directory instead of file */
#define AT_SYMLINK_FOLLOW   0x400   /* Follow symbolic links */
#define AT_EACCESS          0x200   /* Test access permitted for effective IDs */
#define AT_EMPTY_PATH       0x1000  /* Allow empty relative pathname */

/* Function declarations */
int open(const char *pathname, int flags, ...);
int openat(int dirfd, const char *pathname, int flags, ...);
int creat(const char *pathname, mode_t mode);
int fcntl(int fd, int cmd, ...);
int posix_fadvise(int fd, off_t offset, off_t len, int advice);
int posix_fallocate(int fd, off_t offset, off_t len);
int fallocate(int fd, int mode, off_t offset, off_t len);

#ifdef __cplusplus
}
#endif

#endif /* _FCNTL_H */
