/*
 * rm.c - Remove files and directories
 *
 * POSIX-compatible rm for Forest OS.
 * Supports -f, -i, -I, -r/-R, -v, -d, --preserve-root.
 */
#include "forest.h"

static int flag_force = 0;
static int flag_interactive = 0;       /* -i: prompt before each */
static int flag_interactive_once = 0;  /* -I: prompt once */
static int flag_recursive = 0;
static int flag_verbose = 0;
static int flag_dir = 0;               /* -d: remove empty dirs */
static int flag_preserve_root = 1;     /* --preserve-root (default) */

static int had_errors = 0;
static int files_removed = 0;
static int interactive_asked = 0;

static const char *progname = "rm";

static void usage(void) {
    eprint2(progname, "usage: rm [-fiIrRvd] [--preserve-root] file...");
    exit(1);
}

static void vmsg(const char *path) {
    if (flag_verbose) {
        eprint(progname);
        eprint(": removed '");
        eprint(path);
        eprint("'\n");
    }
}

/* Prompt user; return 1 to proceed, 0 to skip */
static int prompt(const char *path) {
    if (flag_force)
        return 1;

    if (flag_interactive) {
        eprint(progname);
        eprint(": remove '");
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

    if (flag_interactive_once && !interactive_asked) {
        interactive_asked = 1;
        eprint(progname);
        eprint(": remove all arguments? ");
        char buf[16];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (buf[0] == 'y' || buf[0] == 'Y')
                return 1;
        }
        return 0;
    }

    /* Check write permission on non-interactive: prompt if write-protected */
    struct stat st;
    if (lstat(path, &st) == 0 && !flag_force) {
        if (!(st.st_mode & S_IWUSR)) {
            eprint(progname);
            eprint(": remove write-protected '");
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
    }

    return 1;
}

/* Recursively remove a directory tree */
static int rm_dir(const char *path);

static int rm_entry(const char *path) {
    struct stat st;

    /* lstat: don't follow symlinks */
    if (lstat(path, &st) != 0) {
        if (!flag_force) {
            eprint(progname);
            eprint(": cannot remove '");
            eprint(path);
            eprint("': No such file or directory\n");
            had_errors = 1;
        }
        return -1;
    }

    if (!prompt(path))
        return 0;

    if (S_ISDIR(st.st_mode)) {
        if (flag_recursive) {
            return rm_dir(path);
        } else if (flag_dir) {
            /* -d: only remove empty dirs */
            if (rmdir(path) == 0) {
                vmsg(path);
                files_removed++;
                return 0;
            }
            if (!flag_force) {
                eprint(progname);
                eprint(": cannot remove '");
                eprint(path);
                eprint("': Directory not empty\n");
                had_errors = 1;
            }
            return -1;
        } else {
            if (!flag_force) {
                eprint(progname);
                eprint(": cannot remove '");
                eprint(path);
                eprint("': Is a directory\n");
                had_errors = 1;
            }
            return -1;
        }
    } else {
        /* Regular file, symlink, fifo, etc. */
        if (unlink(path) != 0) {
            if (!flag_force) {
                eprint(progname);
                eprint(": cannot remove '");
                eprint(path);
                eprint("'\n");
                had_errors = 1;
            }
            return -1;
        }
        vmsg(path);
        files_removed++;
        return 0;
    }
}

static int rm_dir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (!flag_force) {
            eprint(progname);
            eprint(": cannot open directory '");
            eprint(path);
            eprint("'\n");
            had_errors = 1;
        }
        return -1;
    }

    struct dirent *ent;
    char child[4096];

    while ((ent = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0')
                continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0')
                continue;
        }

        /* Build child path */
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);

        if (plen + 1 + nlen >= sizeof(child)) {
            eprint(progname);
            eprint(": path too long\n");
            closedir(dir);
            had_errors = 1;
            return -1;
        }

        memcpy(child, path, plen);
        child[plen] = '/';
        memcpy(child + plen + 1, ent->d_name, nlen + 1);

        rm_entry(child);
    }

    closedir(dir);

    /* Remove the now-empty directory itself */
    if (rmdir(path) != 0) {
        if (!flag_force) {
            eprint(progname);
            eprint(": failed to remove '");
            eprint(path);
            eprint("': Directory not empty or permission denied\n");
            had_errors = 1;
        }
        return -1;
    }

    vmsg(path);
    files_removed++;
    return 0;
}

static int is_root_path(const char *path) {
    if (!flag_preserve_root)
        return 0;

    /* Normalize: skip trailing slashes */
    const char *p = path;
    while (*p == '/')
        p++;

    /* Empty string after stripping slashes = root */
    if (*p == '\0')
        return 1;

    /* Exactly "/" or "//" etc */
    if (strcmp(path, "/") == 0)
        return 1;

    return 0;
}

static void parse_args(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (arg[0] != '-')
            break;

        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }

        if (strcmp(arg, "--preserve-root") == 0) {
            flag_preserve_root = 1;
            continue;
        }
        if (strcmp(arg, "--no-preserve-root") == 0) {
            flag_preserve_root = 0;
            continue;
        }
        if (strcmp(arg, "--help") == 0) {
            usage();
        }
        if (strcmp(arg, "--version") == 0) {
            eprint(progname);
            eprint(" (Forest OS) 1.0\n");
            exit(EXIT_OK);
        }

        /* Parse combined flags */
        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
                case 'f': flag_force = 1; break;
                case 'i': flag_interactive = 1; flag_interactive_once = 0; break;
                case 'I': flag_interactive_once = 1; flag_interactive = 0; break;
                case 'r': case 'R': flag_recursive = 1; break;
                case 'v': flag_verbose = 1; break;
                case 'd': flag_dir = 1; break;
                default:
                    eprint(progname);
                    eprint(": invalid option -- '");
                    char err[2] = {*p, '\0'};
                    eprint(err);
                    eprint("'\n");
                    usage();
            }
            p++;
        }
    }

    /* Process remaining args as file paths */
    if (i >= argc) {
        if (!flag_force) {
            eprint(progname);
            eprint(": missing operand\n");
        }
        usage();
    }

    /* First pass: check --preserve-root */
    for (int j = i; j < argc; j++) {
        if (is_root_path(argv[j])) {
            eprint(progname);
            eprint(": it is dangerous to operate recursively on '/'\n");
            eprint(progname);
            eprint(": use --no-preserve-root to override\n");
            exit(EXIT_FAIL);
        }
    }

    /* Second pass: remove files */
    for (int j = i; j < argc; j++) {
        rm_entry(argv[j]);
    }
}

int main(int argc, char **argv) {
    progname = argv[0] ? argv[0] : "rm";
    parse_args(argc, argv);
    return had_errors ? EXIT_FAIL : EXIT_OK;
}
