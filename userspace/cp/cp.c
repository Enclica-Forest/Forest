/*
 * cp.c - Forest OS userspace cp
 *
 * POSIX-compatible file copy utility.
 * Supports recursive copy, preservation, linking, and more.
 */

#include "forest.h"

#define BUF_SIZE    65536
#define MAX_LINK    4096

/* Runtime flags */
static int flag_recursive  = 0;
static int flag_force      = 0;
static int flag_interactive = 0;
static int flag_preserve   = 0;
static int flag_verbose    = 0;
static int flag_update     = 0;
static int flag_hardlink   = 0;
static int flag_symlink    = 0;
static int flag_no_deref    = 0;

static const char *progname = "cp";
static int exit_status = 0;

static void usage(void) {
    eprint2(progname, "Usage: cp [-adfilprsu] source... target");
    eprint2(progname, "  -r, -R   recursive copy");
    eprint2(progname, "  -f       force (remove dest, no prompt)");
    eprint2(progname, "  -i       interactive prompt before overwrite");
    eprint2(progname, "  -p       preserve mode, ownership, timestamps");
    eprint2(progname, "  -v       verbose (print source -> dest)");
    eprint2(progname, "  -a       archive mode (-dpR)");
    eprint2(progname, "  -u       update (copy only newer)");
    eprint2(progname, "  -l       hard link instead of copy");
    eprint2(progname, "  -s       symbolic link instead of copy");
    exit(EXIT_USAGE);
}

static ssize_t xwrite(int fd, const void *buf, size_t count) {
    const char *p = (const char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)count;
}

static ssize_t xread(int fd, void *buf, size_t count) {
    char *p = (char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)(count - left);
}

static size_t xstrlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static char *xstrcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static char *xstrdup(const char *s) {
    size_t len = xstrlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup) xstrcpy(dup, s);
    return dup;
}

static char *path_join(const char *dir, const char *name) {
    size_t dlen = xstrlen(dir);
    size_t nlen = xstrlen(name);
    int need_sep = (dlen > 0 && dir[dlen - 1] != '/');
    size_t total = dlen + need_sep + nlen + 1;
    char *path = (char *)malloc(total);
    if (!path) return NULL;
    char *p = path;
    xstrcpy(p, dir);
    p += dlen;
    if (need_sep) *p++ = '/';
    xstrcpy(p, name);
    return path;
}

static int is_dir(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int prompt_overwrite(const char *path) {
    eprint(path);
    eprint("? ");
    char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;
    return (c == 'y' || c == 'Y');
}

static int copy_file_data(const char *src, const char *dst, const struct stat *st_src) {
    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) {
        eprint2(progname, "cannot open source file");
        eprint2(progname, src);
        return -1;
    }

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int fd_out = open(dst, flags, st_src->st_mode & 07777);
    if (fd_out < 0) {
        eprint2(progname, "cannot create destination file");
        eprint2(progname, dst);
        close(fd_in);
        return -1;
    }

    char buf[BUF_SIZE];
    ssize_t n;
    int err = 0;

    while ((n = xread(fd_in, buf, BUF_SIZE)) > 0) {
        if (xwrite(fd_out, buf, (size_t)n) != n) {
            eprint2(progname, "write error");
            err = -1;
            break;
        }
    }
    if (n < 0) {
        eprint2(progname, "read error");
        err = -1;
    }

    close(fd_in);
    if (close(fd_out) < 0) {
        eprint2(progname, "close error on destination");
        err = -1;
    }
    return err;
}

static int copy_one(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) {
        eprint2(progname, "cannot read directory");
        eprint2(progname, src);
        return -1;
    }

    struct stat st_src;
    if (stat(src, &st_src) < 0) {
        closedir(d);
        return -1;
    }

    if (mkdir(dst, st_src.st_mode & 07777) < 0 && errno != EEXIST) {
        eprint2(progname, "cannot create directory");
        eprint2(progname, dst);
        closedir(d);
        return -1;
    }

    struct dirent *ent;
    int ret = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char *src_child = path_join(src, ent->d_name);
        char *dst_child = path_join(dst, ent->d_name);

        if (!src_child || !dst_child) {
            free(src_child);
            free(dst_child);
            ret = -1;
            continue;
        }

        if (copy_one(src_child, dst_child) < 0)
            ret = -1;

        free(src_child);
        free(dst_child);
    }

    closedir(d);

    if (flag_preserve) {
        struct timespec times[2];
        times[0].tv_sec = st_src.st_atim.tv_sec;
        times[0].tv_nsec = st_src.st_atim.tv_nsec;
        times[1].tv_sec = st_src.st_mtim.tv_sec;
        times[1].tv_nsec = st_src.st_mtim.tv_nsec;
        utimensat(AT_FDCWD, dst, times, 0);
        chmod(dst, st_src.st_mode & 07777);
        chown(dst, st_src.st_uid, st_src.st_gid);
    }

    return ret;
}

static int copy_one(const char *src, const char *dst) {
    struct stat st_src;
    if (lstat(src, &st_src) < 0) {
        eprint2(progname, "cannot stat");
        eprint2(progname, src);
        return -1;
    }

    if (flag_symlink) {
        char link_target[MAX_LINK];
        ssize_t len = readlink(src, link_target, sizeof(link_target) - 1);
        if (len < 0) {
            eprint2(progname, "cannot read link");
            return -1;
        }
        link_target[len] = '\0';
        unlink(dst);
        if (symlink(link_target, dst) < 0) {
            eprint2(progname, "cannot create symlink");
            return -1;
        }
        if (flag_verbose) {
            eprint(src);
            eprint(" -> ");
            eprint(dst);
            eprint("\n");
        }
        return 0;
    }

    if (flag_hardlink) {
        unlink(dst);
        if (link(src, dst) < 0) {
            eprint2(progname, "cannot create hard link");
            return -1;
        }
        if (flag_verbose) {
            eprint(src);
            eprint(" -> ");
            eprint(dst);
            eprint("\n");
        }
        return 0;
    }

    if (S_ISLNK(st_src.st_mode)) {
        char link_target[MAX_LINK];
        ssize_t len = readlink(src, link_target, sizeof(link_target) - 1);
        if (len < 0) {
            eprint2(progname, "cannot read link");
            return -1;
        }
        link_target[len] = '\0';
        unlink(dst);
        if (symlink(link_target, dst) < 0) {
            eprint2(progname, "cannot create symlink");
            return -1;
        }
        if (flag_preserve) {
            struct timespec times[2];
            times[0].tv_sec = st_src.st_atim.tv_sec;
            times[0].tv_nsec = st_src.st_atim.tv_nsec;
            times[1].tv_sec = st_src.st_mtim.tv_sec;
            times[1].tv_nsec = st_src.st_mtim.tv_nsec;
            utimensat(AT_FDCWD, dst, times, AT_SYMLINK_NOFOLLOW);
        }
        if (flag_verbose) {
            eprint(src);
            eprint(" -> ");
            eprint(dst);
            eprint("\n");
        }
        return 0;
    }

    if (S_ISDIR(st_src.st_mode)) {
        if (!flag_recursive) {
            eprint2(progname, "omitting directory");
            eprint2(progname, src);
            return -1;
        }
        return copy_dir(src, dst);
    }

    if (flag_interactive && !flag_force) {
        struct stat st_dst;
        if (stat(dst, &st_dst) == 0) {
            if (!prompt_overwrite(dst)) {
                return 0;
            }
        }
    }

    if (flag_update) {
        struct stat st_dst;
        if (stat(dst, &st_dst) == 0) {
            if (st_dst.st_mtim.tv_sec >= st_src.st_mtim.tv_sec &&
                (st_dst.st_mtim.tv_sec > st_src.st_mtim.tv_sec ||
                 st_dst.st_mtim.tv_nsec >= st_src.st_mtim.tv_nsec)) {
                return 0;
            }
        }
    }

    if (S_ISBLK(st_src.st_mode) || S_ISCHR(st_src.st_mode) ||
        S_ISFIFO(st_src.st_mode) || S_ISSOCK(st_src.st_mode)) {
        struct stat st_dst;
        if (stat(dst, &st_dst) == 0) {
            if (st_dst.st_rdev == st_src.st_rdev) return 0;
        }
        unlink(dst);
        if (mknod(dst, st_src.st_mode, st_src.st_rdev) < 0) {
            eprint2(progname, "cannot create special file");
            return -1;
        }
        if (flag_preserve) {
            chown(dst, st_src.st_uid, st_src.st_gid);
        }
        if (flag_verbose) {
            eprint(src);
            eprint(" -> ");
            eprint(dst);
            eprint("\n");
        }
        return 0;
    }

    if (S_ISREG(st_src.st_mode)) {
        if (flag_force) unlink(dst);
    }

    if (copy_file_data(src, dst, &st_src) < 0)
        return -1;

    if (flag_preserve) {
        chmod(dst, st_src.st_mode & 07777);
        chown(dst, st_src.st_uid, st_src.st_gid);
        struct timespec times[2];
        times[0].tv_sec = st_src.st_atim.tv_sec;
        times[0].tv_nsec = st_src.st_atim.tv_nsec;
        times[1].tv_sec = st_src.st_mtim.tv_sec;
        times[1].tv_nsec = st_src.st_mtim.tv_nsec;
        utimensat(AT_FDCWD, dst, times, 0);
    }

    if (flag_verbose) {
        eprint(src);
        eprint(" -> ");
        eprint(dst);
        eprint("\n");
    }

    return 0;
}

static int is_dir_target(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

static char *basename_component(const char *path) {
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    return (char *)last;
}

int main(int argc, char *argv[]) {
    int opt;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') break;

        const char *arg = argv[i] + 1;

        if (arg[0] == '-' && arg[1] == '\0') {
            i++;
            break;
        }

        for (int j = 0; arg[j]; j++) {
            switch (arg[j]) {
            case 'r': case 'R':
                flag_recursive = 1;
                break;
            case 'f':
                flag_force = 1;
                flag_interactive = 0;
                break;
            case 'i':
                flag_interactive = 1;
                flag_force = 0;
                break;
            case 'p':
                flag_preserve = 1;
                break;
            case 'v':
                flag_verbose = 1;
                break;
            case 'a':
                flag_recursive = 1;
                flag_preserve = 1;
                flag_force = 1;
                break;
            case 'u':
                flag_update = 1;
                break;
            case 'l':
                flag_hardlink = 1;
                break;
            case 's':
                flag_symlink = 1;
                break;
            case 'd':
                flag_no_deref = 1;
                break;
            default:
                eprint2(progname, "unknown option");
                usage();
            }
        }
    }

    int remaining = argc - i;
    if (remaining < 2) {
        usage();
    }

    char *target = argv[argc - 1];
    int sources = argc - i - 1;

    if (sources > 1 && !is_dir_target(target)) {
        eprint2(progname, "target is not a directory");
        return EXIT_FAIL;
    }

    int ret = 0;

    for (int idx = i; idx < argc - 1; idx++) {
        char *src = argv[idx];
        char *dst;

        if (is_dir_target(target)) {
            char *base = basename_component(src);
            dst = path_join(target, base);
        } else {
            dst = target;
        }

        if (!dst) {
            eprint2(progname, "out of memory");
            return EXIT_ERROR;
        }

        if (copy_one(src, dst) < 0) {
            ret = 1;
        }

        if (dst != target) free(dst);
    }

    return ret;
}
