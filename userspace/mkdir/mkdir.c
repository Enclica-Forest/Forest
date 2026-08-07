/*
 * mkdir.c - Forest OS mkdir implementation
 *
 * Creates directories with optional mode, parent creation, and verbose output.
 * Usage: mkdir [-m MODE] [-p] [-v] DIRECTORY...
 */
#include <forest.h>
#include <limits.h>

static const char *prog = "mkdir";
static int flag_verbose = 0;
static int flag_parents = 0;
static int flag_mode_set = 0;
static mode_t dir_mode = 0755;

static void usage(void) {
    eprint2(prog, "Usage: mkdir [-m mode] [-p] [-v] dir...");
    _exit(EXIT_USAGE);
}

static mode_t parse_mode(const char *s) {
    mode_t m = 0;
    const char *p = s;

    while (*p) {
        if (*p >= '0' && *p <= '7') {
            m = (m << 3) | (*p - '0');
            p++;
        } else {
            eprint2(prog, "invalid mode");
            _exit(EXIT_USAGE);
        }
    }
    return m;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static int make_parents(const char *path, mode_t mode) {
    char buf[FILENAME_MAX];
    size_t len = slen(path);
    if (len >= FILENAME_MAX) {
        eprint2(prog, "path too long");
        return -1;
    }

    memcpy(buf, path, len + 1);

    /* Strip trailing slash except root */
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    struct stat st;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, mode) == 0) {
                if (flag_verbose) {
                    eprint2(prog, "created directory '");
                    write(STDERR_FILENO, buf, slen(buf));
                    eprint("'\n");
                }
            } else if (errno != EEXIST) {
                eprint2(prog, "cannot create directory '");
                write(STDERR_FILENO, buf, slen(buf));
                eprint("': ");
                eprint(strerror(errno));
                eprint("\n");
                return -1;
            } else if (stat(buf, &st) == 0 && !S_ISDIR(st.st_mode)) {
                eprint2(prog, "'");
                write(STDERR_FILENO, buf, slen(buf));
                eprint("' exists but is not a directory\n");
                return -1;
            }
            *p = '/';
        }
    }

    return 0;
}

static int make_dir(const char *path, mode_t mode) {
    struct stat st;

    if (mkdir(path, mode) == 0) {
        if (flag_verbose) {
            eprint2(prog, "created directory '");
            write(STDERR_FILENO, path, slen(path));
            eprint("'\n");
        }
        return 0;
    }

    if (errno == EEXIST) {
        if (flag_parents) {
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                return 0;
            eprint2(prog, "'");
            write(STDERR_FILENO, path, slen(path));
            eprint("' exists but is not a directory\n");
            return -1;
        }
        eprint2(prog, "cannot create directory '");
        write(STDERR_FILENO, path, slen(path));
        eprint("': File exists\n");
        return -1;
    }

    if (flag_parents) {
        if (make_parents(path, mode) != 0)
            return -1;
        if (mkdir(path, mode) == 0) {
            if (flag_verbose) {
                eprint2(prog, "created directory '");
                write(STDERR_FILENO, path, slen(path));
                eprint("'\n");
            }
            return 0;
        }
        if (errno == EEXIST) {
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                return 0;
        }
    }

    eprint2(prog, "cannot create directory '");
    write(STDERR_FILENO, path, slen(path));
    eprint("': ");
    eprint(strerror(errno));
    eprint("\n");
    return -1;
}

int main(int argc, char *argv[]) {
    int errors = 0;

    int opt;
    while ((opt = getopt(argc, argv, "m:pv")) != -1) {
        switch (opt) {
            case 'm':
                dir_mode = parse_mode(optarg);
                flag_mode_set = 1;
                break;
            case 'p':
                flag_parents = 1;
                break;
            case 'v':
                flag_verbose = 1;
                break;
            default:
                usage();
        }
    }

    if (optind >= argc)
        usage();

    mode_t mode = dir_mode & ~0002;

    for (int i = optind; i < argc; i++) {
        if (make_dir(argv[i], mode) != 0)
            errors++;
    }

    _exit(errors ? EXIT_FAIL : EXIT_OK);
}
