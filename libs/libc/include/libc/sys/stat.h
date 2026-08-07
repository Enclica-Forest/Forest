/*
 * sys/stat.h - File status
 * 
 * POSIX compatible file status definitions for Fern libc.
 */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

/* File type bits */
#define S_IFMT      0170000     /* Mask for file type */
#define S_IFSOCK    0140000     /* Socket */
#define S_IFLNK     0120000     /* Symbolic link */
#define S_IFREG     0100000     /* Regular file */
#define S_IFBLK     0060000     /* Block device */
#define S_IFDIR     0040000     /* Directory */
#define S_IFCHR     0020000     /* Character device */
#define S_IFIFO     0010000     /* FIFO (named pipe) */

/* File type test macros */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)     /* Is regular file? */
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)     /* Is directory? */
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)     /* Is character device? */
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)     /* Is block device? */
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)     /* Is FIFO? */
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)     /* Is symbolic link? */
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)    /* Is socket? */

/* Permission bits */
#define S_ISUID     04000       /* Set-user-ID on execution */
#define S_ISGID     02000       /* Set-group-ID on execution */
#define S_ISVTX     01000       /* Sticky bit */

/* Owner permissions */
#define S_IRWXU     00700       /* Owner read, write, execute */
#define S_IRUSR     00400       /* Owner read */
#define S_IWUSR     00200       /* Owner write */
#define S_IXUSR     00100       /* Owner execute */

/* Group permissions */
#define S_IRWXG     00070       /* Group read, write, execute */
#define S_IRGRP     00040       /* Group read */
#define S_IWGRP     00020       /* Group write */
#define S_IXGRP     00010       /* Group execute */

/* Other permissions */
#define S_IRWXO     00007       /* Others read, write, execute */
#define S_IROTH     00004       /* Others read */
#define S_IWOTH     00002       /* Others write */
#define S_IXOTH     00001       /* Others execute */

/* Compatibility names */
#define S_IREAD     S_IRUSR     /* Read by owner */
#define S_IWRITE    S_IWUSR     /* Write by owner */
#define S_IEXEC     S_IXUSR     /* Execute by owner */

/* Default file creation mask */
#define DEFFILEMODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)

/* Access mode for all */
#define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)

/* All permission bits */
#define ALLPERMS    (S_ISUID|S_ISGID|S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO)

/* Stat structure */
struct stat {
    dev_t     st_dev;       /* ID of device containing file */
    ino_t     st_ino;       /* Inode number */
    mode_t    st_mode;      /* File type and mode */
    nlink_t   st_nlink;     /* Number of hard links */
    uid_t     st_uid;       /* User ID of owner */
    gid_t     st_gid;       /* Group ID of owner */
    dev_t     st_rdev;      /* Device ID (if special file) */
    off_t     st_size;      /* Total size, in bytes */
    blksize_t st_blksize;   /* Block size for filesystem I/O */
    blkcnt_t  st_blocks;    /* Number of 512B blocks allocated */
    
    /* Time of last access */
    struct timespec st_atim;
    /* Time of last modification */
    struct timespec st_mtim;
    /* Time of last status change */
    struct timespec st_ctim;
    
    /* Backward compatibility */
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
};

/* 64-bit stat structure (same as stat on 64-bit systems) */
struct stat64 {
    dev_t     st_dev;
    ino64_t   st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off64_t   st_size;
    blksize_t st_blksize;
    blkcnt64_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

/* Function declarations */
int stat(const char *pathname, struct stat *statbuf);
int fstat(int fd, struct stat *statbuf);
int lstat(const char *pathname, struct stat *statbuf);
int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags);

/* 64-bit variants */
int stat64(const char *pathname, struct stat64 *statbuf);
int fstat64(int fd, struct stat64 *statbuf);
int lstat64(const char *pathname, struct stat64 *statbuf);
int fstatat64(int dirfd, const char *pathname, struct stat64 *statbuf, int flags);

/* File mode creation mask */
mode_t umask(mode_t mask);

/* Create directory */
int mkdir(const char *pathname, mode_t mode);
int mkdirat(int dirfd, const char *pathname, mode_t mode);

/* Create special file */
int mknod(const char *pathname, mode_t mode, dev_t dev);
int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev);

/* Create FIFO */
int mkfifo(const char *pathname, mode_t mode);
int mkfifoat(int dirfd, const char *pathname, mode_t mode);

/* Change file mode */
int chmod(const char *pathname, mode_t mode);
int fchmod(int fd, mode_t mode);
int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags);

/* Update file timestamps */
int futimens(int fd, const struct timespec times[2]);
int utimensat(int dirfd, const char *pathname,
              const struct timespec times[2], int flags);

/* fstatat flags */
#define AT_EMPTY_PATH       0x1000
#define AT_NO_AUTOMOUNT     0x800
#define AT_SYMLINK_NOFOLLOW 0x100

/* Device number macros */
#define major(dev) ((unsigned int)(((dev) >> 8) & 0xFF))
#define minor(dev) ((unsigned int)((dev) & 0xFF))
#define makedev(maj, min) ((dev_t)(((maj) << 8) | (min)))

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STAT_H */
