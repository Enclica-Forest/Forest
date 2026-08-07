#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <libgen.h>
#include <getopt.h>

typedef struct {
    int changes;
    int verbose;
    int no_errors;
    int recursive;
    int no_deref;
    int preserve_root;
} Options;

static Options opts;
static uid_t target_uid = (uid_t)-1;
static gid_t target_gid = (gid_t)-1;
static char *target_user = NULL;
static char *target_group = NULL;
static int has_user = 0;
static int has_group = 0;

static int parse_owner_spec(const char *spec) {
    char *colon = strchr(spec, ':');
    char *dot = strchr(spec, '.');

    char *sep = colon ? colon : dot;

    if (sep) {
        *sep = '\0';
        if (spec[0] != '\0') {
            char *endptr;
            long uid = strtol(spec, &endptr, 10);
            if (*endptr == '\0' && uid >= 0) {
                target_uid = (uid_t)uid;
            } else {
                struct passwd *pw = getpwnam(spec);
                if (!pw) {
                    fprintf(stderr, "chown: invalid user: '%s'\n", spec);
                    return -1;
                }
                target_uid = pw->pw_uid;
            }
            has_user = 1;
            target_user = strdup(spec);
        }

        char *gname = sep + 1;
        if (gname[0] != '\0') {
            char *endptr;
            long gid = strtol(gname, &endptr, 10);
            if (*endptr == '\0' && gid >= 0) {
                target_gid = (gid_t)gid;
            } else {
                struct group *gr = getgrnam(gname);
                if (!gr) {
                    fprintf(stderr, "chown: invalid group: '%s'\n", gname);
                    return -1;
                }
                target_gid = gr->gr_gid;
            }
            has_group = 1;
            target_group = strdup(gname);
        }
    } else {
        char *endptr;
        long uid = strtol(spec, &endptr, 10);
        if (*endptr == '\0' && uid >= 0) {
            target_uid = (uid_t)uid;
        } else {
            struct passwd *pw = getpwnam(spec);
            if (!pw) {
                fprintf(stderr, "chown: invalid user: '%s'\n", spec);
                return -1;
            }
            target_uid = pw->pw_uid;
        }
        has_user = 1;
        target_user = strdup(spec);
    }

    return 0;
}

static void report_change(const char *path, uid_t old_uid, gid_t old_gid,
                          uid_t new_uid, gid_t new_gid) {
    if (!opts.verbose && !opts.changes)
        return;

    struct passwd *old_pw = getpwuid(old_uid);
    struct group *old_gr = getgrgid(old_gid);
    struct passwd *new_pw = (new_uid != (uid_t)-1) ? getpwuid(new_uid) : NULL;
    struct group *new_gr = (new_gid != (gid_t)-1) ? getgrgid(new_gid) : NULL;

    const char *oname = old_pw ? old_pw->pw_name : "???";
    const char *gname = old_gr ? old_gr->gr_name : "???";
    const char *nuname = new_pw ? new_pw->pw_name : "???";
    const char *ngname = new_gr ? new_gr->gr_name : "???";

    if (opts.verbose) {
        printf("changed ownership of '%s' from %s:%s to %s:%s\n",
               path, oname, gname,
               has_user ? nuname : oname,
               has_group ? ngname : gname);
    } else if (opts.changes) {
        printf("changed ownership of '%s' from %s:%s to %s:%s\n",
               path, oname, gname,
               has_user ? nuname : oname,
               has_group ? ngname : gname);
    }
}

static int do_chown(const char *path, const struct stat *st) {
    uid_t new_uid = has_user ? target_uid : st->st_uid;
    gid_t new_gid = has_group ? target_gid : st->st_gid;

    if (new_uid == st->st_uid && new_gid == st->st_gid)
        return 0;

    uid_t old_uid = st->st_uid;
    gid_t old_gid = st->st_gid;

    int ret;
    if (opts.no_deref) {
        ret = lchown(path, new_uid, new_gid);
    } else {
        ret = chown(path, new_uid, new_gid);
    }

    if (ret < 0) {
        if (!opts.no_errors)
            fprintf(stderr, "chown: changing ownership of '%s': %s\n",
                    path, strerror(errno));
        return 1;
    }

    report_change(path, old_uid, old_gid, new_uid, new_gid);
    return 0;
}

static int chown_recursive(const char *path) {
    struct stat st;
    int (*stat_fn)(const char *, struct stat *) = opts.no_deref ? lstat : stat;

    if (stat_fn(path, &st) < 0) {
        if (!opts.no_errors)
            fprintf(stderr, "chown: cannot access '%s': %s\n",
                    path, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode) && opts.recursive) {
        DIR *dir = opendir(path);
        if (!dir) {
            if (!opts.no_errors)
                fprintf(stderr, "chown: cannot open directory '%s': %s\n",
                        path, strerror(errno));
            return 1;
        }

        struct dirent *ent;
        int ret = 0;

        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

            if (chown_recursive(child) != 0)
                ret = 1;
        }
        closedir(dir);

        if (do_chown(path, &st) != 0)
            ret = 1;

        return ret;
    }

    return do_chown(path, &st);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [OPTION]... OWNER FILE...\n"
            "Change the owner and/or group of each FILE.\n\n"
            "  OWNER is [USER][:GROUP] format\n"
            "  -c, --changes          like verbose but report only when a change is made\n"
            "  -f, --silent, --quiet  suppress most error messages\n"
            "  -v, --verbose          output a diagnostic for every file processed\n"
            "  -R, --recursive        operate on files and directories recursively\n"
            "  -h, --no-dereference   do not follow symlinks\n"
            "      --preserve-root    fail silently rather than recursively\n"
            "      --help             display this help and exit\n",
            prog);
}

int main(int argc, char *argv[]) {
    int opt;
    static struct option long_opts[] = {
        {"changes",       no_argument, NULL, 'c'},
        {"silent",        no_argument, NULL, 'f'},
        {"quiet",         no_argument, NULL, 'f'},
        {"verbose",       no_argument, NULL, 'v'},
        {"recursive",     no_argument, NULL, 'R'},
        {"no-dereference", no_argument, NULL, 'h'},
        {"preserve-root", no_argument, NULL, 'P'},
        {"help",          no_argument, NULL, 'H'},
        {NULL,            0,           NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "cfvRh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': opts.changes = 1; break;
            case 'f': opts.no_errors = 1; break;
            case 'v': opts.verbose = 1; break;
            case 'R': opts.recursive = 1; break;
            case 'h': opts.no_deref = 1; break;
            case 'P': opts.preserve_root = 1; break;
            case 'H':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s: missing operand\n", argv[0]);
        usage(argv[0]);
        return 1;
    }

    if (parse_owner_spec(argv[optind]) < 0)
        return 1;
    optind++;

    if (optind >= argc) {
        fprintf(stderr, "%s: missing file operand\n", argv[0]);
        return 1;
    }

    int exit_code = 0;

    for (int i = optind; i < argc; i++) {
        if (opts.preserve_root && strcmp(argv[i], "/") == 0) {
            if (!opts.no_errors)
                fprintf(stderr, "chown: it is dangerous to operate recursively on '/'\n");
            exit_code = 1;
            continue;
        }

        if (chown_recursive(argv[i]) != 0)
            exit_code = 1;
    }

    return exit_code;
}
