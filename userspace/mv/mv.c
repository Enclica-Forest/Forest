/*
 * mv.c - Move/rename files and directories
 *
 * Forest OS mv implementation.
 * Supports -f, -i, -n, -v, -u flags.
 * Tries rename() first; falls back to copy+delete on EXDEV.
 */
#include <forest.h>
#include <limits.h>

static const char *prog = "mv";

static int flag_force = 0;
static int flag_interactive = 0;
static int flag_no_clobber = 0;
static int flag_verbose = 0;
static int flag_update = 0;

static int had_errors = 0;

static size_t slen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static void usage(void) {
    eprint2(prog, "Usage: mv [-finv] source target");
    eprint2(prog, "       mv [-finv] source... directory");
    _exit(EXIT_USAGE);
}

static void vmsg(const char *src, const char *dst) {
    if (flag_verbose) {
        eprint(prog);
        eprint(": '");
        eprint(src);
        eprint("' -> '");
        eprint(dst);
        eprint("'\n");
    }
}

static int prompt_overwrite(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return 1;

    if (flag_no_clobber)
        return 0;

    if (flag_force)
        return 1;

    if (flag_update && S_ISREG(st.st_mode)) {
        struct stat src_st;
        if (stat(path, &src_st) == 0) {
            if (st.st_mtime >= src_st.st_mtime)
                return 0;
        }
    }

    if (flag_interactive) {
        eprint(prog);
        eprint(": overwrite '");
        eprint(path);
        eprint("'? ");
        char buf[16];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (buf[0] == 'y' || buf[0] == 'Y')
                return 1;
        }
        return 0;
    }

    if (!flag_force) {
        eprint(prog);
        eprint(": overwrite '");
        eprint(path);
        eprint("'? ");
        char buf[16];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (buf[0] == 'y' || buf[0] == 'Y')
                return 1;
        }
        return 0;
    }

    return 1;
}

static int copy_file(const char *src, const char *dst) {
    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) {
        eprint(prog);
        eprint(": cannot read '");
        eprint(src);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    struct stat st;
    if (fstat(fd_in, &st) != 0) {
        eprint(prog);
        eprint(": cannot stat '");
        eprint(src);
        eprint("'\n");
        close(fd_in);
        had_errors = 1;
        return -1;
    }

    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (fd_out < 0) {
        eprint(prog);
        eprint(": cannot create '");
        eprint(dst);
        eprint("'\n");
        close(fd_in);
        had_errors = 1;
        return -1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(fd_out, buf + written, n - written);
            if (w < 0) {
                eprint(prog);
                eprint(": write error\n");
                close(fd_in);
                close(fd_out);
                had_errors = 1;
                return -1;
            }
            written += w;
        }
    }

    close(fd_in);
    close(fd_out);

    chmod(dst, st.st_mode);
    return 0;
}

static int copy_dir(const char *src, const char *dst);

static int copy_entry(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        eprint(prog);
        eprint(": cannot stat '");
        eprint(src);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        return copy_dir(src, dst);
    }

    if (S_ISLNK(st.st_mode)) {
        char linkbuf[PATH_MAX];
        ssize_t len = readlink(src, linkbuf, sizeof(linkbuf) - 1);
        if (len < 0) {
            eprint(prog);
            eprint(": cannot readlink '");
            eprint(src);
            eprint("'\n");
            had_errors = 1;
            return -1;
        }
        linkbuf[len] = '\0';
        unlink(dst);
        if (symlink(linkbuf, dst) != 0) {
            eprint(prog);
            eprint(": cannot create symlink '");
            eprint(dst);
            eprint("'\n");
            had_errors = 1;
            return -1;
        }
        return 0;
    }

    if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
        unlink(dst);
        if (mknod(dst, st.st_mode, st.st_rdev) != 0) {
            eprint(prog);
            eprint(": cannot create special file '");
            eprint(dst);
            eprint("'\n");
            had_errors = 1;
            return -1;
        }
        return 0;
    }

    return copy_file(src, dst);
}

static int copy_dir(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) {
        eprint(prog);
        eprint(": cannot stat '");
        eprint(src);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    if (mkdir(dst, st.st_mode) != 0 && errno != EEXIST) {
        eprint(prog);
        eprint(": cannot create directory '");
        eprint(dst);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    DIR *dir = opendir(src);
    if (!dir) {
        eprint(prog);
        eprint(": cannot open directory '");
        eprint(src);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    struct dirent *ent;
    int ret = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0')
                continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0')
                continue;
        }

        char child_src[PATH_MAX];
        char child_dst[PATH_MAX];
        size_t src_len = slen(src);
        size_t name_len = slen(ent->d_name);

        if (src_len + 1 + name_len >= PATH_MAX) {
            eprint(prog);
            eprint(": path too long\n");
            closedir(dir);
            had_errors = 1;
            return -1;
        }

        memcpy(child_src, src, src_len);
        child_src[src_len] = '/';
        memcpy(child_src + src_len + 1, ent->d_name, name_len + 1);

        size_t dst_len = slen(dst);
        if (dst_len + 1 + name_len >= PATH_MAX) {
            eprint(prog);
            eprint(": path too long\n");
            closedir(dir);
            had_errors = 1;
            return -1;
        }

        memcpy(child_dst, dst, dst_len);
        child_dst[dst_len] = '/';
        memcpy(child_dst + dst_len + 1, ent->d_name, name_len + 1);

        if (copy_entry(child_src, child_dst) != 0)
            ret = -1;
    }

    closedir(dir);
    return ret;
}

static int remove_entry(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir)
            return -1;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') {
                if (ent->d_name[1] == '\0')
                    continue;
                if (ent->d_name[1] == '.' && ent->d_name[2] == '\0')
                    continue;
            }

            char child[PATH_MAX];
            size_t path_len = slen(path);
            size_t name_len = slen(ent->d_name);

            if (path_len + 1 + name_len >= PATH_MAX) {
                closedir(dir);
                return -1;
            }

            memcpy(child, path, path_len);
            child[path_len] = '/';
            memcpy(child + path_len + 1, ent->d_name, name_len + 1);

            remove_entry(child);
        }

        closedir(dir);
        return rmdir(path);
    }

    return unlink(path);
}

static int do_move(const char *src, const char *dst) {
    struct stat src_st;
    if (lstat(src, &src_st) != 0) {
        eprint(prog);
        eprint(": cannot stat '");
        eprint(src);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    if (S_ISLNK(src_st.st_mode)) {
        struct stat link_st;
        if (stat(src, &link_st) != 0) {
            eprint(prog);
            eprint(": cannot stat '");
            eprint(src);
            eprint("'\n");
            had_errors = 1;
            return -1;
        }
        src_st = link_st;
    }

    struct stat dst_st;
    int dst_exists = (lstat(dst, &dst_st) == 0);

    if (dst_exists) {
        if (flag_no_clobber) {
            return 0;
        }

        if (S_ISDIR(src_st.st_mode) && S_ISDIR(dst_st.st_mode)) {
            DIR *dir = opendir(dst);
            if (dir) {
                struct dirent *ent;
                int empty = 1;
                while ((ent = readdir(dir)) != NULL) {
                    if (ent->d_name[0] == '.') {
                        if (ent->d_name[1] == '\0')
                            continue;
                        if (ent->d_name[1] == '.' && ent->d_name[2] == '\0')
                            continue;
                    }
                    empty = 0;
                    break;
                }
                closedir(dir);

                if (!empty) {
                    eprint(prog);
                    eprint(": cannot move '");
                    eprint(src);
                    eprint("' to '");
                    eprint(dst);
                    eprint("': Directory not empty\n");
                    had_errors = 1;
                    return -1;
                }
            }
        }

        if (!prompt_overwrite(dst)) {
            return 0;
        }
    }

    if (rename(src, dst) == 0) {
        vmsg(src, dst);
        return 0;
    }

    if (errno != EXDEV) {
        eprint(prog);
        eprint(": cannot move '");
        eprint(src);
        eprint("' to '");
        eprint(dst);
        eprint("': ");
        eprint(strerror(errno));
        eprint("\n");
        had_errors = 1;
        return -1;
    }

    if (copy_entry(src, dst) != 0) {
        had_errors = 1;
        return -1;
    }

    if (remove_entry(src) != 0) {
        eprint(prog);
        eprint(": cannot remove '");
        eprint(src);
        eprint("'\n");
        had_errors = 1;
        return -1;
    }

    vmsg(src, dst);
    return 0;
}

int main(int argc, char *argv[]) {
    int opt;

    while ((opt = getopt(argc, argv, "finvu")) != -1) {
        switch (opt) {
            case 'f':
                flag_force = 1;
                flag_interactive = 0;
                break;
            case 'i':
                flag_interactive = 1;
                flag_force = 0;
                break;
            case 'n':
                flag_no_clobber = 1;
                flag_force = 0;
                flag_interactive = 0;
                break;
            case 'v':
                flag_verbose = 1;
                break;
            case 'u':
                flag_update = 1;
                break;
            default:
                usage();
        }
    }

    if (optind >= argc - 1) {
        if (optind >= argc)
            eprint2(prog, "missing file operand\n");
        else
            eprint2(prog, "missing destination operand\n");
        usage();
    }

    if (optind == argc - 2) {
        const char *src = argv[optind];
        const char *dst = argv[optind + 1];

        struct stat dst_st;
        if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
            char *dstpath;
            size_t srclen = slen(src);
            size_t dstlen = slen(dst);
            const char *basename = src;
            const char *p = src;
            while (*p) {
                if (*p == '/')
                    basename = p + 1;
                p++;
            }
            size_t namelen = slen(basename);

            if (dstlen + 1 + namelen >= PATH_MAX) {
                eprint2(prog, "path too long\n");
                _exit(EXIT_FAIL);
            }

            dstpath = malloc(dstlen + 1 + namelen + 1);
            if (!dstpath) {
                eprint2(prog, "out of memory\n");
                _exit(EXIT_FAIL);
            }

            memcpy(dstpath, dst, dstlen);
            dstpath[dstlen] = '/';
            memcpy(dstpath + dstlen + 1, basename, namelen + 1);

            do_move(src, dstpath);
            free(dstpath);
        } else {
            do_move(src, dst);
        }
    } else {
        const char *dest = argv[argc - 1];
        struct stat dest_st;
        if (stat(dest, &dest_st) == 0 && S_ISDIR(dest_st.st_mode)) {
            for (int i = optind; i < argc - 1; i++) {
                const char *src = argv[i];
                const char *basename = src;
                const char *p = src;
                while (*p) {
                    if (*p == '/')
                        basename = p + 1;
                    p++;
                }
                size_t destlen = slen(dest);
                size_t namelen = slen(basename);

                if (destlen + 1 + namelen >= PATH_MAX) {
                    eprint2(prog, "path too long\n");
                    had_errors = 1;
                    continue;
                }

                char *dstpath = malloc(destlen + 1 + namelen + 1);
                if (!dstpath) {
                    eprint2(prog, "out of memory\n");
                    _exit(EXIT_FAIL);
                }

                memcpy(dstpath, dest, destlen);
                dstpath[destlen] = '/';
                memcpy(dstpath + destlen + 1, basename, namelen + 1);

                do_move(src, dstpath);
                free(dstpath);
            }
        } else {
            if (argc - optind > 2) {
                eprint2(prog, "target is not a directory\n");
                _exit(EXIT_FAIL);
            }
            do_move(argv[optind], dest);
        }
    }

    _exit(had_errors ? EXIT_FAIL : EXIT_OK);
}
