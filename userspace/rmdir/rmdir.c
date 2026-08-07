#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static int verbose = 0;

static int is_empty_dir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir)
        return -1;

    struct dirent *entry;
    int empty = 1;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        empty = 0;
        break;
    }

    closedir(dir);
    return empty;
}

static int do_rmdir(const char *path) {
    struct stat st;

    if (stat(path, &st) < 0) {
        fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "rmdir: failed to remove '%s': Not a directory\n",
                path);
        return 1;
    }

    int empty = is_empty_dir(path);
    if (empty < 0) {
        fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    if (!empty) {
        fprintf(stderr, "rmdir: failed to remove '%s': Directory not empty\n",
                path);
        return 1;
    }

    if (rmdir(path) < 0) {
        fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                path, strerror(errno));
        return 1;
    }

    if (verbose)
        printf("rmdir: removed directory '%s'\n", path);

    return 0;
}

static int do_rmdir_p(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) {
        perror("rmdir: strdup");
        return 1;
    }

    size_t len = strlen(tmp);

    /* strip trailing slashes */
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    int ret = 0;

    for (;;) {
        ret = do_rmdir(tmp);
        if (ret != 0)
            break;

        /* find parent */
        char *slash = strrchr(tmp, '/');
        if (!slash || slash == tmp) {
            /* reached root or relative path */
            if (strcmp(tmp, ".") != 0) {
                ret = do_rmdir(tmp);
            }
            break;
        }

        *slash = '\0';
        len = slash - tmp;

        /* strip trailing slashes */
        while (len > 1 && tmp[len - 1] == '/')
            tmp[--len] = '\0';
    }

    free(tmp);
    return ret;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-pv] dir...\n", prog);
}

int main(int argc, char *argv[]) {
    int opt;
    int remove_parents = 0;

    while ((opt = getopt(argc, argv, "pv")) != -1) {
        switch (opt) {
            case 'p': remove_parents = 1; break;
            case 'v': verbose = 1; break;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    int ret = 0;

    for (int i = optind; i < argc; i++) {
        if (remove_parents) {
            if (do_rmdir_p(argv[i]) != 0)
                ret = 1;
        } else {
            if (do_rmdir(argv[i]) != 0)
                ret = 1;
        }
    }

    return ret;
}
