/*
 * dirent.c - Directory entry operations for Fern libc
 * 
 * Implements POSIX compatible directory operations using the
 * getdents and getdents64 system calls.
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "sys/syscall.h"

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

/* Linux dirent structure for getdents */
struct linux_dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    char d_name[256];
};

/* Linux dirent64 structure for getdents64 */
struct linux_dirent64 {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

/* Maximum buffer size for directory entries */
#define DIRENT_BUFFER_SIZE 8192

struct __dirstream {
    int fd;
    char buffer[DIRENT_BUFFER_SIZE];
    int buf_pos;
    int buf_end;
    off_t offset;
};

DIR *opendir(const char *name) {
    if (!name) {
        errno = EFAULT;
        return NULL;
    }

    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return NULL;
    }

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    dir->offset = 0;

    return dir;
}

DIR *fdopendir(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    dir->offset = 0;

    return dir;
}

int closedir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }

    int fd = dirp->fd;
    free(dirp);
    return close(fd);
}

struct dirent *readdir(DIR *dirp) {
    static struct dirent result;

    if (!dirp) {
        errno = EBADF;
        return NULL;
    }

    while (1) {
        if (dirp->buf_pos >= dirp->buf_end) {
            // Refill buffer
            int bytes_read = syscall(SYS_GETDENTS, dirp->fd, dirp->buffer, DIRENT_BUFFER_SIZE);
            if (bytes_read <= 0) {
                if (bytes_read < 0) {
                    errno = -bytes_read;
                }
                return NULL;
            }

            dirp->buf_pos = 0;
            dirp->buf_end = bytes_read;
        }

        // Parse directory entry
        struct linux_dirent *d = (struct linux_dirent *)&dirp->buffer[dirp->buf_pos];
        result.d_ino = d->d_ino;
        result.d_off = d->d_off;
        result.d_reclen = d->d_reclen;
        result.d_type = DT_UNKNOWN; // Not available in 32-bit dirent
        strncpy(result.d_name, d->d_name, sizeof(result.d_name) - 1);
        result.d_name[sizeof(result.d_name) - 1] = '\0';

        dirp->buf_pos += d->d_reclen;
        dirp->offset = d->d_off;

        // Skip . and .. entries
        if (strcmp(result.d_name, ".") == 0 || strcmp(result.d_name, "..") == 0) {
            continue;
        }

        return &result;
    }
}

struct dirent64 *readdir64(DIR *dirp) {
    static struct dirent64 result;

    if (!dirp) {
        errno = EBADF;
        return NULL;
    }

    while (1) {
        if (dirp->buf_pos >= dirp->buf_end) {
            // Refill buffer
            int bytes_read = syscall(SYS_GETDENTS64, dirp->fd, dirp->buffer, DIRENT_BUFFER_SIZE);
            if (bytes_read <= 0) {
                if (bytes_read < 0) {
                    errno = -bytes_read;
                }
                return NULL;
            }

            dirp->buf_pos = 0;
            dirp->buf_end = bytes_read;
        }

        // Parse directory entry
        struct linux_dirent64 *d = (struct linux_dirent64 *)&dirp->buffer[dirp->buf_pos];
        result.d_ino = d->d_ino;
        result.d_off = d->d_off;
        result.d_reclen = d->d_reclen;
        result.d_type = d->d_type;
        strncpy(result.d_name, d->d_name, sizeof(result.d_name) - 1);
        result.d_name[sizeof(result.d_name) - 1] = '\0';

        dirp->buf_pos += d->d_reclen;
        dirp->offset = d->d_off;

        // Skip . and .. entries
        if (strcmp(result.d_name, ".") == 0 || strcmp(result.d_name, "..") == 0) {
            continue;
        }

        return &result;
    }
}

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
    if (!dirp || !entry || !result) {
        errno = EBADF;
        return -1;
    }

    struct dirent *d = readdir(dirp);
    if (d) {
        *entry = *d;
        *result = entry;
        return 0;
    }

    *result = NULL;
    return 0;
}

int readdir64_r(DIR *dirp, struct dirent64 *entry, struct dirent64 **result) {
    if (!dirp || !entry || !result) {
        errno = EBADF;
        return -1;
    }

    struct dirent64 *d = readdir64(dirp);
    if (d) {
        *entry = *d;
        *result = entry;
        return 0;
    }

    *result = NULL;
    return 0;
}

void rewinddir(DIR *dirp) {
    if (!dirp) {
        return;
    }

    lseek(dirp->fd, 0, SEEK_SET);
    dirp->buf_pos = 0;
    dirp->buf_end = 0;
    dirp->offset = 0;
}

void seekdir(DIR *dirp, long loc) {
    if (!dirp) {
        return;
    }

    lseek(dirp->fd, loc, SEEK_SET);
    dirp->buf_pos = 0;
    dirp->buf_end = 0;
    dirp->offset = loc;
}

long telldir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }

    return dirp->offset;
}

int dirfd(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }

    return dirp->fd;
}

ssize_t getdents(int fd, void *dirp, size_t count) {
    return syscall(SYS_GETDENTS, fd, dirp, count);
}

ssize_t getdents64(int fd, void *dirp, size_t count) {
    return syscall(SYS_GETDENTS64, fd, dirp, count);
}

/* Helper for scandir comparison */
static int alphasort_compare(const void *a, const void *b) {
    const struct dirent * const *da = (const struct dirent * const *)a;
    const struct dirent * const *db = (const struct dirent * const *)b;
    return strcmp((*da)->d_name, (*db)->d_name);
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    if (!dirp || !namelist) {
        errno = EFAULT;
        return -1;
    }

    DIR *dir = opendir(dirp);
    if (!dir) {
        return -1;
    }

    struct dirent **list = NULL;
    int capacity = 0;
    int count = 0;

    struct dirent entry;
    struct dirent *result;

    while (readdir_r(dir, &entry, &result) == 0 && result) {
        if (!filter || filter(result)) {
            if (count >= capacity) {
                capacity = capacity ? capacity * 2 : 10;
                struct dirent **new_list = realloc(list, capacity * sizeof(struct dirent *));
                if (!new_list) {
                    closedir(dir);
                    for (int i = 0; i < count; i++) {
                        free(list[i]);
                    }
                    free(list);
                    errno = ENOMEM;
                    return -1;
                }
                list = new_list;
            }

            struct dirent *copy = malloc(sizeof(struct dirent));
            if (!copy) {
                closedir(dir);
                for (int i = 0; i < count; i++) {
                    free(list[i]);
                }
                free(list);
                errno = ENOMEM;
                return -1;
            }

            *copy = entry;
            list[count++] = copy;
        }
    }

    closedir(dir);

    if (compar) {
        qsort(list, count, sizeof(struct dirent *),
              (int (*)(const void *, const void *))compar);
    } else {
        qsort(list, count, sizeof(struct dirent *), alphasort_compare);
    }

    *namelist = list;
    return count;
}

int scandir64(const char *dirp, struct dirent64 ***namelist,
              int (*filter)(const struct dirent64 *),
              int (*compar)(const struct dirent64 **, const struct dirent64 **)) {
    if (!dirp || !namelist) {
        errno = EFAULT;
        return -1;
    }

    DIR *dir = opendir(dirp);
    if (!dir) {
        return -1;
    }

    struct dirent64 **list = NULL;
    int capacity = 0;
    int count = 0;

    struct dirent64 entry;
    struct dirent64 *result;

    while (readdir64_r(dir, &entry, &result) == 0 && result) {
        if (!filter || filter(result)) {
            if (count >= capacity) {
                capacity = capacity ? capacity * 2 : 10;
                struct dirent64 **new_list = realloc(list, capacity * sizeof(struct dirent64 *));
                if (!new_list) {
                    closedir(dir);
                    for (int i = 0; i < count; i++) {
                        free(list[i]);
                    }
                    free(list);
                    errno = ENOMEM;
                    return -1;
                }
                list = new_list;
            }

            struct dirent64 *copy = malloc(sizeof(struct dirent64));
            if (!copy) {
                closedir(dir);
                for (int i = 0; i < count; i++) {
                    free(list[i]);
                }
                free(list);
                errno = ENOMEM;
                return -1;
            }

            *copy = entry;
            list[count++] = copy;
        }
    }

    closedir(dir);

    if (compar) {
        qsort(list, count, sizeof(struct dirent64 *),
              (int (*)(const void *, const void *))compar);
    } else {
        qsort(list, count, sizeof(struct dirent64 *),
              (int (*)(const void *, const void *))alphasort_compare);
    }

    *namelist = list;
    return count;
}

int scandirat(int dirfd, const char *dirp, struct dirent ***namelist,
              int (*filter)(const struct dirent *),
              int (*compar)(const struct dirent **, const struct dirent **)) {
    (void)dirfd;
    return scandir(dirp, namelist, filter, compar);
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int alphasort64(const struct dirent64 **a, const struct dirent64 **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int versionsort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int versionsort64(const struct dirent64 **a, const struct dirent64 **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}