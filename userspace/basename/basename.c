/*
 * basename.c - Forest OS userspace basename utility
 * Full POSIX implementation: strip directory and suffix from filename
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "1.0.0"

static const char *progname = "basename";

static void usage(void) {
    fprintf(stderr, "Usage: %s [PATH [SUFFIX]]\n", progname);
    fprintf(stderr, "       %s --help\n", progname);
    fprintf(stderr, "       %s --version\n", progname);
}

static void print_version(void) {
    fprintf(stdout, "%s (Forest OS) %s\n", progname, VERSION);
}

/*
 * Strip trailing slashes from path (in-place).
 * Returns pointer to the modified string.
 * If path is all slashes, returns "/".
 */
static char *strip_trailing_slashes(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    path[len] = '\0';
    return path;
}

/*
 * POSIX basename: extract the last component of a path.
 * Returns pointer to the filename portion.
 */
static const char *base_name(const char *path) {
    const char *p;

    if (!path || !*path)
        return ".";

    /* Strip trailing slashes */
    p = path + strlen(path) - 1;
    while (p > path && *p == '/')
        p--;

    /* If only slashes, return "/" */
    if (p == path && *p == '/')
        return "/";

    /* Find last '/' */
    while (p > path && *p != '/')
        p--;

    if (*p == '/')
        return p + 1;

    return p;
}

/*
 * Remove suffix from filename if it matches.
 * Returns pointer to result (static buffer).
 */
static const char *strip_suffix(const char *name, const char *suffix) {
    static char result[4096];
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    size_t copy_len;

    if (suffix_len == 0 || name_len < suffix_len) {
        copy_len = name_len;
    } else if (strcmp(name + name_len - suffix_len, suffix) == 0) {
        copy_len = name_len - suffix_len;
    } else {
        copy_len = name_len;
    }

    if (copy_len >= sizeof(result))
        copy_len = sizeof(result) - 1;

    memcpy(result, name, copy_len);
    result[copy_len] = '\0';
    return result;
}

int main(int argc, char *argv[]) {
    const char *path;
    const char *suffix = "";
    const char *result;

    progname = argv[0];

    /* Handle --help and --version */
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            print_version();
            return 0;
        }
    }

    /* Check for --help/--version after other args (GNU compat) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
    }

    if (argc < 2 || argc > 3) {
        usage();
        return 1;
    }

    path = argv[1];

    /* Make a mutable copy for trailing slash stripping */
    char pathbuf[4096];
    size_t pathlen = strlen(path);
    if (pathlen >= sizeof(pathbuf))
        pathlen = sizeof(pathbuf) - 1;
    memcpy(pathbuf, path, pathlen);
    pathbuf[pathlen] = '\0';

    strip_trailing_slashes(pathbuf);

    /* If path became empty after stripping slashes, treat as "/" */
    if (pathbuf[0] == '\0')
        pathbuf[0] = '/', pathbuf[1] = '\0';

    result = base_name(pathbuf);

    if (argc == 3) {
        suffix = argv[2];
        result = strip_suffix(result, suffix);
    }

    printf("%s\n", result);
    return 0;
}
