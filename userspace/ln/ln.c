#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <libgen.h>

#define BACKUP_SUFFIX "~"

static int flag_s = 0;
static int flag_f = 0;
static int flag_i = 0;
static int flag_n = 0;
static int flag_v = 0;
static int flag_b = 0;
static int flag_T = 0;
static int flag_P = 0;

static const char *prog;

static void usage(void) {
    fprintf(stderr, "Usage: %s [-sfnivbTP] source_file target\n", prog);
    fprintf(stderr, "       %s [-sfnivbTP] source_file... target_dir\n", prog);
    exit(1);
}

static int make_backup(const char *path) {
    char backup[PATH_MAX];
    size_t len = strlen(path) + sizeof(BACKUP_SUFFIX);

    if (len > sizeof(backup)) {
        fprintf(stderr, "%s: backup path too long: %s\n", prog, path);
        return -1;
    }

    snprintf(backup, sizeof(backup), "%s%s", path, BACKUP_SUFFIX);

    if (rename(path, backup) != 0) {
        fprintf(stderr, "%s: cannot backup '%s': %s\n", prog, path, strerror(errno));
        return -1;
    }
    return 0;
}

static int prompt_remove(const char *path) {
    fprintf(stderr, "%s: replace '%s'? ", prog, path);
    fflush(stderr);

    int c = getchar();
    int newline = 0;

    while (c != EOF && c != '\n')
        c = getchar();

    return (c != EOF && (c == '\n' || newline));
}

static int do_link(const char *source, const char *target) {
    struct stat st;
    int target_exists;

    target_exists = (lstat(target, &st) == 0);

    if (target_exists) {
        if (flag_i) {
            if (!prompt_remove(target))
                return 0;
        }

        if (flag_b) {
            if (make_backup(target) != 0)
                return 1;
        }

        if (flag_f) {
            if (S_ISDIR(st.st_mode)) {
                fprintf(stderr, "%s: cannot remove '%s': Is a directory\n", prog, target);
                return 1;
            }
            if (unlink(target) != 0) {
                fprintf(stderr, "%s: cannot remove '%s': %s\n", prog, target, strerror(errno));
                return 1;
            }
            target_exists = 0;
        }
    }

    if (target_exists && !flag_f && !flag_i && !flag_b) {
        fprintf(stderr, "%s: cannot create link '%s': File exists\n", prog, target);
        return 1;
    }

    if (flag_s) {
        if (symlink(source, target) != 0) {
            fprintf(stderr, "%s: cannot create symbolic link '%s': %s\n",
                    prog, target, strerror(errno));
            return 1;
        }
    } else {
        if (link(source, target) != 0) {
            fprintf(stderr, "%s: cannot create hard link '%s': %s\n",
                    prog, target, strerror(errno));
            return 1;
        }
    }

    if (flag_v) {
        printf("'%s' -> '%s'\n", source, target);
    }

    return 0;
}

static int is_dir_nofollow(const char *path) {
    struct stat st;

    if (flag_n) {
        if (lstat(path, &st) != 0)
            return 0;
        if (S_ISLNK(st.st_mode))
            return 0;
    }

    if (stat(path, &st) != 0)
        return 0;

    return S_ISDIR(st.st_mode);
}

int main(int argc, char *argv[]) {
    int opt;
    int errors = 0;

    prog = basename(argv[0]);

    while ((opt = getopt(argc, argv, "sfinvbTP")) != -1) {
        switch (opt) {
            case 's': flag_s = 1; break;
            case 'f': flag_f = 1; break;
            case 'i': flag_i = 1; break;
            case 'n': flag_n = 1; break;
            case 'v': flag_v = 1; break;
            case 'b': flag_b = 1; break;
            case 'T': flag_T = 1; break;
            case 'P': flag_P = 1; break;
            default:  usage();
        }
    }

    if (flag_i && flag_f) {
        fprintf(stderr, "%s: the -i and -f options are mutually exclusive\n", prog);
        return 1;
    }

    if (argc - optind < 2)
        usage();

    int nsrc = argc - optind - 1;
    char *target = argv[argc - 1];

    if (nsrc > 1 && !flag_T && is_dir_nofollow(target)) {
        for (int i = optind; i < argc - 1; i++) {
            char dest[PATH_MAX];
            const char *name = basename(argv[i]);

            if (snprintf(dest, sizeof(dest), "%s/%s", target, name) >= (int)sizeof(dest)) {
                fprintf(stderr, "%s: path too long: %s/%s\n", prog, target, name);
                errors = 1;
                continue;
            }

            if (do_link(argv[i], dest))
                errors = 1;
        }
    } else {
        if (nsrc > 1 && !flag_T && !is_dir_nofollow(target)) {
            fprintf(stderr, "%s: target '%s' is not a directory\n", prog, target);
            return 1;
        }

        char *source = argv[optind];
        if (do_link(source, target))
            errors = 1;
    }

    return errors ? 1 : 0;
}
