/*
 * chmod.c - Forest OS userspace chmod implementation
 *
 * Full POSIX-compatible chmod utility supporting:
 *   - Symbolic mode: [ugoa]+[-+=][rwxXst]+
 *   - Octal mode: 0-7777
 *   - Flags: -c, -f, -v, -R, --preserve-root
 */
#include "forest.h"

static const char *progname = "chmod";

/* Option flags */
static int opt_c = 0;  /* report changes */
static int opt_f = 0;  /* suppress errors */
static int opt_v = 0;  /* verbose output */
static int opt_R = 0;  /* recursive */
static int opt_preserve_root = 0;

/* Permission change result */
static int changes_made = 0;
static int errors_occurred = 0;

static void usage(void) {
    eprint2(progname, "Usage: chmod [-cfvR] mode file...");
    eprint2(progname, "       chmod [-cfvR] octal_mode file...");
    eprint2(progname, "       chmod [-cfvR] symbolic_mode file...");
    eprint2(progname, "");
    eprint2(progname, "Modes:");
    eprint2(progname, "  octal       0-7777 (e.g. 755, 0644)");
    eprint2(progname, "  symbolic    [ugoa]+[-+=][rwxXst]+");
    eprint2(progname, "");
    eprint2(progname, "Options:");
    eprint2(progname, "  -c          Report only when changes are made");
    eprint2(progname, "  -f          Suppress error messages");
    eprint2(progname, "  -v          Verbose output");
    eprint2(progname, "  -R          Recurse into directories");
    eprint2(progname, "  --preserve-root  Refuse to operate on '/' (default)");
}

static int is_octal_mode(const char *mode) {
    if (*mode == '\0') return 0;
    if (*mode == '0' && mode[1] != '\0') {
        mode++;
    }
    while (*mode) {
        if (*mode < '0' || *mode > '7') return 0;
        mode++;
    }
    return 1;
}

static mode_t parse_octal_mode(const char *mode) {
    mode_t m = 0;
    if (*mode == '0') mode++;
    while (*mode) {
        m = (m << 3) | (*mode - '0');
        mode++;
    }
    return m & 07777;
}

static int apply_octal_mode(const char *path, mode_t new_mode) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (!opt_f) {
            eprint2(progname, path);
            eprint(": No such file or directory\n");
            errors_occurred++;
        }
        return -1;
    }
    mode_t old_mode = st.st_mode & 07777;
    if (old_mode == new_mode) return 0;
    if (chmod(path, new_mode) < 0) {
        if (!opt_f) {
            eprint2(progname, path);
            eprint(": Permission denied\n");
            errors_occurred++;
        }
        return -1;
    }
    changes_made++;
    if (opt_v || (opt_c && !opt_f)) {
        char obuf[12], nbuf[12];
        obuf[0] = '0'; obuf[1] = '0';
        obuf[2] = '0' + ((old_mode >> 9) & 7);
        obuf[3] = '0' + ((old_mode >> 6) & 7);
        obuf[4] = '0' + ((old_mode >> 3) & 7);
        obuf[5] = '0' + (old_mode & 7);
        obuf[6] = '\0';
        nbuf[0] = '0'; nbuf[1] = '0';
        nbuf[2] = '0' + ((new_mode >> 9) & 7);
        nbuf[3] = '0' + ((new_mode >> 6) & 7);
        nbuf[4] = '0' + ((new_mode >> 3) & 7);
        nbuf[5] = '0' + (new_mode & 7);
        nbuf[6] = '\0';
        write(STDOUT_FILENO, "mode of '", 9);
        write(STDOUT_FILENO, path, strlen(path));
        write(STDOUT_FILENO, "' changed from ", 14);
        write(STDOUT_FILENO, obuf, 6);
        write(STDOUT_FILENO, " to ", 4);
        write(STDOUT_FILENO, nbuf, 6);
        write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}

static mode_t calc_symbolic_mode(mode_t old_mode, const char *mode_str) {
    mode_t mask = 0;
    const char *p = mode_str;

    while (*p) {
        mode_t who = 0;
        int first_who = 1;

        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            first_who = 0;
            switch (*p) {
                case 'u': who |= S_IRWXU | S_ISUID; break;
                case 'g': who |= S_IRWXG | S_ISGID; break;
                case 'o': who |= S_IRWXO | S_ISVTX; break;
                case 'a': who |= S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX; break;
            }
            p++;
        }

        if (first_who) who = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;

        int op = *p;
        if (op != '+' && op != '-' && op != '=') {
            eprint2(progname, "invalid mode: bad operator");
            return (mode_t)-1;
        }
        p++;

        mode_t add = 0;
        int have_X = 0;

        while (*p && *p != ',' && *p != '+' && *p != '-' && *p != '=') {
            switch (*p) {
                case 'r':
                    add |= S_IRUSR | S_IRGRP | S_IROTH;
                    break;
                case 'w':
                    add |= S_IWUSR | S_IWGRP | S_IWOTH;
                    break;
                case 'x':
                    add |= S_IXUSR | S_IXGRP | S_IXOTH;
                    break;
                case 'X':
                    have_X = 1;
                    break;
                case 's':
                    add |= S_ISUID | S_ISGID;
                    break;
                case 't':
                    add |= S_ISVTX;
                    break;
                default:
                    eprint2(progname, "invalid mode: bad character");
                    return (mode_t)-1;
            }
            p++;
        }

        switch (op) {
            case '+':
                if (have_X) {
                    if (S_ISDIR(old_mode) || (old_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                        add |= S_IXUSR | S_IXGRP | S_IXOTH;
                }
                mask |= (add & who);
                break;
            case '-':
                mask &= ~(add & who);
                break;
            case '=':
                mask &= ~who;
                mask |= (add & who);
                break;
        }

        if (*p == ',') p++;
    }

    return mask;
}

static int apply_symbolic_mode(const char *path, const char *mode_str) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (!opt_f) {
            eprint2(progname, path);
            eprint(": No such file or directory\n");
            errors_occurred++;
        }
        return -1;
    }
    mode_t old_mode = st.st_mode;
    mode_t perm_only = old_mode & 07777;

    mode_t new_mask = calc_symbolic_mode(perm_only, mode_str);
    if (new_mask == (mode_t)-1) return -1;

    mode_t new_mode = (old_mode & ~07777) | new_mask;
    if (new_mode == old_mode) return 0;

    if (chmod(path, new_mode) < 0) {
        if (!opt_f) {
            eprint2(progname, path);
            eprint(": Permission denied\n");
            errors_occurred++;
        }
        return -1;
    }

    changes_made++;
    if (opt_v || (opt_c && !opt_f)) {
        char obuf[12], nbuf[12];
        mode_string(old_mode, obuf);
        mode_string(new_mode, nbuf);
        write(STDOUT_FILENO, "mode of '", 9);
        write(STDOUT_FILENO, path, strlen(path));
        write(STDOUT_FILENO, "' changed from ", 14);
        write(STDOUT_FILENO, obuf, 10);
        write(STDOUT_FILENO, " to ", 4);
        write(STDOUT_FILENO, nbuf, 10);
        write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}

static int do_chmod(const char *path, const char *mode_str, int is_octal);

static int do_chmod_dir(const char *path, const char *mode_str, int is_octal) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (!opt_f) {
            eprint2(progname, path);
            eprint(": Permission denied\n");
            errors_occurred++;
        }
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        size_t pathlen = strlen(path);
        size_t namelen = strlen(ent->d_name);
        char *child = malloc(pathlen + 1 + namelen + 1);
        if (!child) {
            closedir(dir);
            eprint2(progname, "out of memory");
            return -1;
        }
        memcpy(child, path, pathlen);
        child[pathlen] = '/';
        memcpy(child + pathlen + 1, ent->d_name, namelen);
        child[pathlen + 1 + namelen] = '\0';

        do_chmod(child, mode_str, is_octal);

        if (ent->d_type == DT_DIR) {
            do_chmod_dir(child, mode_str, is_octal);
        }
        free(child);
    }
    closedir(dir);
    return 0;
}

static int do_chmod(const char *path, const char *mode_str, int is_octal) {
    if (opt_preserve_root && strcmp(path, "/") == 0) {
        eprint2(progname, "it is dangerous to operate recursively on '/'");
        errors_occurred++;
        return -1;
    }

    int ret;
    if (is_octal) {
        mode_t mode = parse_octal_mode(mode_str);
        ret = apply_octal_mode(path, mode);
    } else {
        ret = apply_symbolic_mode(path, mode_str);
    }

    if (opt_R && ret == 0) {
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            do_chmod_dir(path, mode_str, is_octal);
        }
    }
    return ret;
}

int main(int argc, char **argv) {
    int argi;
    for (argi = 1; argi < argc; argi++) {
        if (argv[argi][0] != '-') break;
        if (strcmp(argv[argi], "--help") == 0) { usage(); return 0; }
        if (strcmp(argv[argi], "--version") == 0) {
            eprint2(progname, "Forest OS coreutils 0.1.0");
            return 0;
        }
        if (strcmp(argv[argi], "--preserve-root") == 0) {
            opt_preserve_root = 1;
            continue;
        }
        if (strcmp(argv[argi], "--") == 0) { argi++; break; }
        const char *opt = argv[argi] + 1;
        while (*opt) {
            switch (*opt) {
                case 'c': opt_c = 1; break;
                case 'f': opt_f = 1; break;
                case 'v': opt_v = 1; break;
                case 'R': opt_R = 1; break;
                default:
                    eprint2(progname, "invalid option");
                    usage();
                    return 1;
            }
            opt++;
        }
    }

    if (argi >= argc) {
        eprint2(progname, "missing operand");
        usage();
        return 1;
    }

    const char *mode_str = argv[argi];
    int is_octal = is_octal_mode(mode_str);
    if (!is_octal && !mode_str[0]) {
        eprint2(progname, "invalid empty mode");
        return 1;
    }

    argi++;
    if (argi >= argc) {
        eprint2(progname, "missing file operand");
        usage();
        return 1;
    }

    for (int i = argi; i < argc; i++) {
        do_chmod(argv[i], mode_str, is_octal);
    }

    return changes_made ? 0 : (errors_occurred ? 1 : 0);
}
