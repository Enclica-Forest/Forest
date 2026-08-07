/*
 * dirent.h - Directory entries
 *
 * POSIX compatible directory operations for Fern libc.
 */
#ifndef _DIRENT_H
#define _DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* Maximum filename length */
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* Directory entry structure */
struct dirent {
    ino_t d_ino;                    /* Inode number */
    off_t d_off;                    /* Offset to the next dirent */
    unsigned short d_reclen;        /* Length of this record */
    unsigned char d_type;           /* Type of file */
    char d_name[NAME_MAX + 1];      /* Filename (null-terminated) */
};

/* 64-bit directory entry structure */
struct dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

/* File type values for d_type */
#define DT_UNKNOWN  0   /* Unknown file type */
#define DT_FIFO     1   /* Named pipe (FIFO) */
#define DT_CHR      2   /* Character device */
#define DT_DIR      4   /* Directory */
#define DT_BLK      6   /* Block device */
#define DT_REG      8   /* Regular file */
#define DT_LNK      10  /* Symbolic link */
#define DT_SOCK     12  /* Local-domain socket */
#define DT_WHT      14  /* Whiteout (BSD) */

/* Convert stat mode to d_type */
#define IFTODT(mode)    (((mode) & 0170000) >> 12)
#define DTTOIF(dirtype) ((dirtype) << 12)

/* Opaque directory stream type */
typedef struct __dirstream DIR;

/* Directory operations */
DIR *opendir(const char *name);
DIR *fdopendir(int fd);
int closedir(DIR *dirp);
struct dirent *readdir(DIR *dirp);
struct dirent64 *readdir64(DIR *dirp);
int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
int readdir64_r(DIR *dirp, struct dirent64 *entry, struct dirent64 **result);
void rewinddir(DIR *dirp);
void seekdir(DIR *dirp, long loc);
long telldir(DIR *dirp);
int dirfd(DIR *dirp);

/* Scanning directories */
int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));
int scandir64(const char *dirp, struct dirent64 ***namelist,
              int (*filter)(const struct dirent64 *),
              int (*compar)(const struct dirent64 **, const struct dirent64 **));
int scandirat(int dirfd, const char *dirp, struct dirent ***namelist,
              int (*filter)(const struct dirent *),
              int (*compar)(const struct dirent **, const struct dirent **));

/* Comparison functions for scandir */
int alphasort(const struct dirent **a, const struct dirent **b);
int alphasort64(const struct dirent64 **a, const struct dirent64 **b);
int versionsort(const struct dirent **a, const struct dirent **b);
int versionsort64(const struct dirent64 **a, const struct dirent64 **b);

/* Get current directory entries (low-level) */
ssize_t getdents(int fd, void *dirp, size_t count);
ssize_t getdents64(int fd, void *dirp, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */
