#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <fnmatch.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Predicate types */
typedef enum {
    PRED_NAME,
    PRED_TYPE,
    PRED_SIZE,
    PRED_MTIME,
    PRED_ATIME,
    PRED_CTIME,
    PRED_PERM,
    PRED_USER,
    PRED_GROUP,
    PRED_NEWER,
    PRED_EMPTY,
    PRED_INUM,
    PRED_LINKS,
    PRED_PRINT,
    PRED_PRINT0,
    PRED_LS,
    PRED_PRINTF,
    PRED_EXEC,
    PRED_MAXDEPTH,
    PRED_MINDEPTH,
    PRED_AND,
    PRED_OR,
    PRED_NOT,
    PRED_OPENPAREN,
    PRED_CLOSEPAREN,
    PRED_ACTION
} PredType;

/* Comparison operator for numeric predicates */
typedef enum {
    CMP_EXACT,
    CMP_GT,
    CMP_LT
} CmpOp;

/* Predicate node */
typedef struct Predicate {
    PredType type;
    CmpOp cmp;
    long num_value;
    char *str_value;
    char *format;
    char **exec_args;
    int exec_argc;
    struct Predicate *next;
} Predicate;

/* Global options */
static int opt_maxdepth = -1;
static int opt_mindepth = -1;
static int opt_follow = 0;
static int opt_print_default = 1;
static int exit_status = 0;

/* Parse helpers */
static int arg_index = 1;
static int arg_count;
static char **arg_vec;

static const char *next_arg(void) {
    if (arg_index >= arg_count) return NULL;
    return arg_vec[arg_index++];
}

/* Forward declarations */
static int eval_predicate(Predicate *pred, const char *path,
                          const struct stat *st, int depth);
static void traverse(const char *path, Predicate *preds, int depth);

/* Parse octal mode */
static mode_t parse_mode(const char *s) {
    mode_t mode = 0;
    if (!s) return 0;
    if (*s >= '0' && *s <= '7') {
        while (*s >= '0' && *s <= '7') {
            mode = (mode << 3) | (*s - '0');
            s++;
        }
    }
    return mode;
}

/* Check if path is '.' or '..' */
static int is_dotdir(const char *name) {
    return (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')));
}

/* Format time for -ls and -printf */
static void format_time(time_t t, char *buf, size_t buflen) {
    struct tm *tm = localtime(&t);
    time_t now = time(NULL);
    if (t > now || (now - t) > 15724800) {
        strftime(buf, buflen, "%b %e  %Y", tm);
    } else {
        strftime(buf, buflen, "%b %e %H:%M", tm);
    }
}

/* Mode string for -ls */
static void mode_str(mode_t mode, char *buf) {
    buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISBLK(mode) ? 'b' :
             S_ISCHR(mode) ? 'c' : S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    if (mode & S_ISUID) buf[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) buf[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) buf[9] = (mode & S_IXOTH) ? 't' : 'T';
    buf[10] = '\0';
}

/* Print -ls output for a file */
static void print_ls(const char *path, const struct stat *st) {
    char modebuf[11], timebuf[64];
    struct passwd *pw;
    struct group *gr;

    mode_str(st->st_mode, modebuf);
    format_time(st->st_mtime, timebuf, sizeof(timebuf));
    pw = getpwuid(st->st_uid);
    gr = getgrgid(st->st_gid);

    printf("%8lu %3lu %-8s %-8s ",
           (unsigned long)st->st_ino,
           (unsigned long)st->st_nlink,
           pw ? pw->pw_name : "???",
           gr ? gr->gr_name : "???");

    if (S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode)) {
        printf("%3lu, %2lu ", (unsigned long)major(st->st_rdev),
               (unsigned long)minor(st->st_rdev));
    } else {
        printf("%8lld ", (long long)st->st_size);
    }

    printf("%s %s\n", timebuf, path);
}

/* Process -printf format */
static void do_printf(const char *fmt, const char *path,
                      const struct stat *st, int depth) {
    while (*fmt) {
        if (*fmt == '\\') {
            fmt++;
            switch (*fmt) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case '0': putchar('\0'); break;
                case '\\': putchar('\\'); break;
                case 'a': printf("%c", '\a'); break;
                case 'b': printf("%c", '\b'); break;
                case 'f': printf("%c", '\f'); break;
                case 'r': printf("%c", '\r'); break;
                default: putchar(*fmt); break;
            }
        } else if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'p': printf("%s", path); break;
                case 'f': {
                    const char *base = strrchr(path, '/');
                    printf("%s", base ? base + 1 : path);
                    break;
                }
                case 'P': {
                    printf("%s", path);
                    break;
                }
                case 'd': printf("%d", depth); break;
                case 'i': printf("%lu", (unsigned long)st->st_ino); break;
                case 's': printf("%lld", (long long)st->st_size); break;
                case 'l':
                    if (S_ISLNK(st->st_mode)) {
                        char linkbuf[PATH_MAX];
                        ssize_t len = readlink(path, linkbuf, sizeof(linkbuf) - 1);
                        if (len > 0) { linkbuf[len] = '\0'; printf("%s", linkbuf); }
                    }
                    break;
                case 'n': printf("%lu", (unsigned long)st->st_nlink); break;
                case 'u': {
                    struct passwd *pw = getpwuid(st->st_uid);
                    printf("%s", pw ? pw->pw_name : "???");
                    break;
                }
                case 'g': {
                    struct group *gr = getgrgid(st->st_gid);
                    printf("%s", gr ? gr->gr_name : "???");
                    break;
                }
                case 'U': printf("%lu", (unsigned long)st->st_uid); break;
                case 'G': printf("%lu", (unsigned long)st->st_gid); break;
                case 'm': {
                    char mb[11];
                    mode_str(st->st_mode, mb);
                    printf("%s", mb);
                    break;
                }
                case 't': {
                    char tb[64];
                    format_time(st->st_mtime, tb, sizeof(tb));
                    printf("%s", tb);
                    break;
                }
                case 'S': printf("%c",
                    S_ISDIR(st->st_mode) ? 'd' :
                    S_ISLNK(st->st_mode) ? 'l' :
                    S_ISBLK(st->st_mode) ? 'b' :
                    S_ISCHR(st->st_mode) ? 'c' :
                    S_ISFIFO(st->st_mode) ? 'p' :
                    S_ISSOCK(st->st_mode) ? 's' : '-');
                    break;
                case '%': putchar('%'); break;
                default: printf("%%%c", *fmt); break;
            }
        } else {
            putchar(*fmt);
        }
        fmt++;
    }
}

/* Evaluate a single predicate against a path */
static int eval_predicate(Predicate *pred, const char *path,
                          const struct stat *st, int depth) {
    (void)depth;

    switch (pred->type) {
    case PRED_NAME: {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        return fnmatch(pred->str_value, base, FNM_PATHNAME | FNM_PERIOD) == 0;
    }
    case PRED_TYPE: {
        char c = pred->str_value[0];
        if (c == 'f') return S_ISREG(st->st_mode);
        if (c == 'd') return S_ISDIR(st->st_mode);
        if (c == 'l') return S_ISLNK(st->st_mode);
        if (c == 'c') return S_ISCHR(st->st_mode);
        if (c == 'b') return S_ISBLK(st->st_mode);
        if (c == 'p') return S_ISFIFO(st->st_mode);
        if (c == 's') return S_ISSOCK(st->st_mode);
        return 0;
    }
    case PRED_SIZE: {
        off_t cmp_size = pred->num_value;
        const char *s = pred->str_value;
        while (*s == '+' || *s == '-') s++;
        size_t len = strlen(s);
        if (len > 0) {
            char last = s[len - 1];
            if (last == 'k' || last == 'K') cmp_size *= 1024;
            else if (last == 'M' || last == 'm') cmp_size *= 1024 * 1024;
            else if (last == 'G' || last == 'g') cmp_size *= 1024L * 1024 * 1024;
        }
        if (pred->cmp == CMP_GT) return st->st_size > cmp_size;
        if (pred->cmp == CMP_LT) return st->st_size < cmp_size;
        return st->st_size == cmp_size;
    }
    case PRED_MTIME: {
        time_t now = time(NULL);
        time_t age = (now - st->st_mtime) / 86400;
        if (pred->cmp == CMP_GT) return age > pred->num_value;
        if (pred->cmp == CMP_LT) return age < pred->num_value;
        return age == pred->num_value;
    }
    case PRED_ATIME: {
        time_t now = time(NULL);
        time_t age = (now - st->st_atime) / 86400;
        if (pred->cmp == CMP_GT) return age > pred->num_value;
        if (pred->cmp == CMP_LT) return age < pred->num_value;
        return age == pred->num_value;
    }
    case PRED_CTIME: {
        time_t now = time(NULL);
        time_t age = (now - st->st_ctime) / 86400;
        if (pred->cmp == CMP_GT) return age > pred->num_value;
        if (pred->cmp == CMP_LT) return age < pred->num_value;
        return age == pred->num_value;
    }
    case PRED_PERM: {
        if (pred->cmp == CMP_GT) return (st->st_mode & 07777) > pred->num_value;
        if (pred->cmp == CMP_LT) return (st->st_mode & 07777) < pred->num_value;
        return (st->st_mode & 07777) == pred->num_value;
    }
    case PRED_USER: {
        struct passwd *pw = getpwnam(pred->str_value);
        if (!pw) return 0;
        return st->st_uid == pw->pw_uid;
    }
    case PRED_GROUP: {
        struct group *gr = getgrnam(pred->str_value);
        if (!gr) return 0;
        return st->st_gid == gr->gr_gid;
    }
    case PRED_NEWER: {
        struct stat nst;
        if (stat(pred->str_value, &nst) < 0) return 0;
        return st->st_mtime > nst.st_mtime;
    }
    case PRED_EMPTY:
        return S_ISDIR(st->st_mode) ? (st->st_size == 0) : (st->st_size == 0);
    case PRED_INUM:
        return st->st_ino == (ino_t)pred->num_value;
    case PRED_LINKS: {
        nlink_t n = st->st_nlink;
        long v = pred->num_value;
        if (pred->cmp == CMP_GT) return n > (nlink_t)v;
        if (pred->cmp == CMP_LT) return n < (nlink_t)v;
        return n == (nlink_t)v;
    }
    default:
        return 1;
    }
}

/* Execute -exec command */
static void do_exec(const char *path, Predicate *pred) {
    pid_t pid = fork();
    if (pid == 0) {
        char **argv = malloc((pred->exec_argc + 2) * sizeof(char *));
        int j = 0;
        for (int i = 0; i < pred->exec_argc; i++) {
            if (strcmp(pred->exec_args[i], "{}") == 0) {
                argv[j++] = (char *)path;
            } else {
                argv[j++] = pred->exec_args[i];
            }
        }
        argv[j] = NULL;
        execvp(argv[0], argv);
        fprintf(stderr, "find: exec '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            exit_status = 1;
        }
    } else {
        perror("fork");
    }
}

/* Evaluate the predicate list */
static int eval_predicates(Predicate *preds, const char *path,
                           const struct stat *st, int depth) {
    int result = 1;
    int negate = 0;

    for (Predicate *p = preds; p; p = p->next) {
        if (p->type == PRED_AND) continue;
        if (p->type == PRED_OR) continue;
        if (p->type == PRED_NOT) {
            negate = !negate;
            continue;
        }

        if (p->type == PRED_PRINT || p->type == PRED_PRINT0 ||
            p->type == PRED_LS || p->type == PRED_PRINTF ||
            p->type == PRED_EXEC || p->type == PRED_ACTION) {
            continue;
        }

        int r = eval_predicate(p, path, st, depth);
        if (negate) { r = !r; negate = 0; }
        result = r;
        if (!result) break;
    }
    return result;
}

/* Execute actions for a matched file */
static void do_actions(Predicate *preds, const char *path,
                       const struct stat *st, int depth) {
    int has_action = 0;

    for (Predicate *p = preds; p; p = p->next) {
        if (p->type == PRED_PRINT) {
            printf("%s\n", path);
            has_action = 1;
        } else if (p->type == PRED_PRINT0) {
            printf("%s", path);
            putchar('\0');
            has_action = 1;
        } else if (p->type == PRED_LS) {
            print_ls(path, st);
            has_action = 1;
        } else if (p->type == PRED_PRINTF) {
            do_printf(p->format, path, st, depth);
            has_action = 1;
        } else if (p->type == PRED_EXEC) {
            do_exec(path, p);
            has_action = 1;
        }
    }

    if (!has_action && opt_print_default) {
        printf("%s\n", path);
    }
}

/* Recursively traverse directory */
static void traverse(const char *path, Predicate *preds, int depth) {
    struct stat st;
    DIR *dir;
    struct dirent *dp;

    if (opt_maxdepth >= 0 && depth > opt_maxdepth) return;

    if (lstat(path, &st) < 0) {
        fprintf(stderr, "find: '%s': %s\n", path, strerror(errno));
        return;
    }

    if (opt_mindepth >= 0 && depth < opt_mindepth) {
        goto descend;
    }

    if (eval_predicates(preds, path, &st, depth)) {
        do_actions(preds, path, &st, depth);
    }

    if (!S_ISDIR(st.st_mode)) return;

descend:
    dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "find: '%s': %s\n", path, strerror(errno));
        return;
    }

    while ((dp = readdir(dir)) != NULL) {
        if (is_dotdir(dp->d_name)) continue;

        char childpath[PATH_MAX];
        snprintf(childpath, sizeof(childpath), "%s/%s", path, dp->d_name);

        traverse(childpath, preds, depth + 1);
    }
    closedir(dir);
}

/* Build predicates from arguments */
static Predicate *parse_predicates(void) {
    Predicate *head = NULL;
    Predicate *tail = NULL;
    const char *arg;

    while ((arg = next_arg()) != NULL) {
        Predicate *p = calloc(1, sizeof(Predicate));
        if (!p) { perror("calloc"); exit(1); }

        if (strcmp(arg, "-name") == 0) {
            p->type = PRED_NAME;
            p->str_value = strdup(next_arg());
            if (!p->str_value) { fprintf(stderr, "find: missing argument to -name\n"); exit(1); }
        } else if (strcmp(arg, "-type") == 0) {
            p->type = PRED_TYPE;
            const char *t = next_arg();
            if (!t) { fprintf(stderr, "find: missing argument to -type\n"); exit(1); }
            p->str_value = strdup(t);
        } else if (strcmp(arg, "-size") == 0) {
            p->type = PRED_SIZE;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -size\n"); exit(1); }
            p->str_value = strdup(s);
            if (*s == '+') { p->cmp = CMP_GT; s++; }
            else if (*s == '-') { p->cmp = CMP_LT; s++; }
            else { p->cmp = CMP_EXACT; }
            p->num_value = atol(s);
        } else if (strcmp(arg, "-mtime") == 0 || strcmp(arg, "-mmin") == 0) {
            p->type = PRED_MTIME;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -mtime\n"); exit(1); }
            p->str_value = strdup(s);
            if (*s == '+') { p->cmp = CMP_GT; s++; }
            else if (*s == '-') { p->cmp = CMP_LT; s++; }
            else { p->cmp = CMP_EXACT; }
            p->num_value = atol(s);
        } else if (strcmp(arg, "-atime") == 0 || strcmp(arg, "-amin") == 0) {
            p->type = PRED_ATIME;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -atime\n"); exit(1); }
            p->str_value = strdup(s);
            if (*s == '+') { p->cmp = CMP_GT; s++; }
            else if (*s == '-') { p->cmp = CMP_LT; s++; }
            else { p->cmp = CMP_EXACT; }
            p->num_value = atol(s);
        } else if (strcmp(arg, "-ctime") == 0 || strcmp(arg, "-cmin") == 0) {
            p->type = PRED_CTIME;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -ctime\n"); exit(1); }
            p->str_value = strdup(s);
            if (*s == '+') { p->cmp = CMP_GT; s++; }
            else if (*s == '-') { p->cmp = CMP_LT; s++; }
            else { p->cmp = CMP_EXACT; }
            p->num_value = atol(s);
        } else if (strcmp(arg, "-perm") == 0) {
            p->type = PRED_PERM;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -perm\n"); exit(1); }
            p->str_value = strdup(s);
            if (*s == '/') { p->cmp = CMP_GT; s++; }
            else if (*s == '-') { p->cmp = CMP_LT; s++; }
            else { p->cmp = CMP_EXACT; }
            p->num_value = parse_mode(s);
        } else if (strcmp(arg, "-user") == 0) {
            p->type = PRED_USER;
            p->str_value = strdup(next_arg());
            if (!p->str_value) { fprintf(stderr, "find: missing argument to -user\n"); exit(1); }
        } else if (strcmp(arg, "-group") == 0) {
            p->type = PRED_GROUP;
            p->str_value = strdup(next_arg());
            if (!p->str_value) { fprintf(stderr, "find: missing argument to -group\n"); exit(1); }
        } else if (strcmp(arg, "-newer") == 0) {
            p->type = PRED_NEWER;
            p->str_value = strdup(next_arg());
            if (!p->str_value) { fprintf(stderr, "find: missing argument to -newer\n"); exit(1); }
        } else if (strcmp(arg, "-empty") == 0) {
            p->type = PRED_EMPTY;
        } else if (strcmp(arg, "-inum") == 0) {
            p->type = PRED_INUM;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -inum\n"); exit(1); }
            p->num_value = atol(s);
        } else if (strcmp(arg, "-links") == 0 || strcmp(arg, "-link") == 0) {
            p->type = PRED_LINKS;
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -links\n"); exit(1); }
            if (*s == '+') { p->cmp = CMP_GT; s++; }
            else if (*s == '-') { p->cmp = CMP_LT; s++; }
            else { p->cmp = CMP_EXACT; }
            p->num_value = atol(s);
        } else if (strcmp(arg, "-print") == 0) {
            p->type = PRED_PRINT;
            opt_print_default = 0;
        } else if (strcmp(arg, "-print0") == 0) {
            p->type = PRED_PRINT0;
            opt_print_default = 0;
        } else if (strcmp(arg, "-ls") == 0) {
            p->type = PRED_LS;
            opt_print_default = 0;
        } else if (strcmp(arg, "-printf") == 0) {
            p->type = PRED_PRINTF;
            p->format = strdup(next_arg());
            if (!p->format) { fprintf(stderr, "find: missing argument to -printf\n"); exit(1); }
            opt_print_default = 0;
        } else if (strcmp(arg, "-exec") == 0) {
            p->type = PRED_EXEC;
            int cap = 16;
            p->exec_args = malloc(cap * sizeof(char *));
            p->exec_argc = 0;
            const char *a;
            while ((a = next_arg()) != NULL) {
                if (strcmp(a, ";") == 0 || strcmp(a, "+") == 0) {
                    break;
                }
                if (p->exec_argc >= cap) {
                    cap *= 2;
                    p->exec_args = realloc(p->exec_args, cap * sizeof(char *));
                }
                p->exec_args[p->exec_argc++] = strdup(a);
            }
            p->exec_args[p->exec_argc] = NULL;
            opt_print_default = 0;
        } else if (strcmp(arg, "-maxdepth") == 0) {
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -maxdepth\n"); exit(1); }
            opt_maxdepth = atoi(s);
            free(p);
            continue;
        } else if (strcmp(arg, "-mindepth") == 0) {
            const char *s = next_arg();
            if (!s) { fprintf(stderr, "find: missing argument to -mindepth\n"); exit(1); }
            opt_mindepth = atoi(s);
            free(p);
            continue;
        } else if (strcmp(arg, "-and") == 0 || strcmp(arg, "-a") == 0) {
            p->type = PRED_AND;
        } else if (strcmp(arg, "-or") == 0 || strcmp(arg, "-o") == 0) {
            p->type = PRED_OR;
        } else if (strcmp(arg, "-not") == 0 || strcmp(arg, "!") == 0) {
            p->type = PRED_NOT;
        } else if (strcmp(arg, "(") == 0) {
            p->type = PRED_OPENPAREN;
        } else if (strcmp(arg, ")") == 0) {
            p->type = PRED_CLOSEPAREN;
        } else if (strcmp(arg, "-noleaf") == 0) {
            p->type = PRED_ACTION;
            free(p);
            continue;
        } else if (strcmp(arg, "-follow") == 0) {
            opt_follow = 1;
            free(p);
            continue;
        } else if (arg[0] == '-') {
            fprintf(stderr, "find: unknown predicate '%s'\n", arg);
            free(p);
            continue;
        } else {
            free(p);
            arg_index--;
            break;
        }

        p->next = NULL;
        if (!head) { head = p; tail = p; }
        else { tail->next = p; tail = p; }
    }

    return head;
}

int main(int argc, char *argv[]) {
    Predicate *preds = NULL;
    char *paths[PATH_MAX];
    int path_count = 0;

    arg_count = argc;
    arg_vec = argv;
    arg_index = 1;

    /* Skip options */
    while (arg_index < argc && argv[arg_index][0] == '-' &&
           strcmp(argv[arg_index], "(") != 0 &&
           strcmp(argv[arg_index], "!") != 0) {
        const char *a = argv[arg_index];
        if (strcmp(a, "-H") == 0 || strcmp(a, "-L") == 0 || strcmp(a, "-P") == 0) {
            arg_index++;
        } else if (strcmp(a, "-maxdepth") == 0 || strcmp(a, "-mindepth") == 0 ||
                   strcmp(a, "-depth") == 0 || strcmp(a, "-noleaf") == 0 ||
                   strcmp(a, "-follow") == 0) {
            arg_index++;
            if (strcmp(a, "-maxdepth") == 0 || strcmp(a, "-mindepth") == 0)
                arg_index++;
        } else {
            break;
        }
    }

    /* Collect path arguments before predicates */
    while (arg_index < argc) {
        const char *a = argv[arg_index];
        if (a[0] == '(' || a[0] == '!' ||
            (a[0] == '-' && strcmp(a, "(") != 0 && strcmp(a, "!") != 0)) {
            break;
        }
        paths[path_count++] = strdup(a);
        arg_index++;
    }

    /* Parse predicates */
    preds = parse_predicates();

    /* Default path */
    if (path_count == 0) {
        paths[path_count++] = strdup(".");
    }

    /* Execute */
    for (int i = 0; i < path_count; i++) {
        traverse(paths[i], preds, 0);
        free(paths[i]);
    }

    /* Free predicates */
    while (preds) {
        Predicate *next = preds->next;
        free(preds->str_value);
        free(preds->format);
        if (preds->exec_args) {
            for (int i = 0; i < preds->exec_argc; i++)
                free(preds->exec_args[i]);
            free(preds->exec_args);
        }
        free(preds);
        preds = next;
    }

    return exit_status;
}
