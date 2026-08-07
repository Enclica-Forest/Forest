/*
 * id.c - Forest OS userspace id implementation
 *
 * Prints user and group IDs for the current process.
 * Usage: id [-gGnruz] [--name] [--group] [--groups] [--user] [--context]
 */

#include <forest.h>
#include <pwd.h>
#include <grp.h>

#ifndef NGROUPS_MAX
#define NGROUPS_MAX 32
#endif

static const char *prog = "id";

/* Options */
static int opt_group_only  = 0; /* -g: print only the effective group ID */
static int opt_groups_only = 0; /* -G: print all group IDs */
static int opt_names       = 0; /* -n: print names instead of numbers */
static int opt_real        = 0; /* -r: print the real ID instead of effective */
static int opt_user_only   = 0; /* -u: print only the effective user ID */
static int opt_zero        = 0; /* -z: delimit entries with NUL */
static int opt_context     = 0; /* --context: print security context */

static void usage(void) {
    eprint2(prog, "Usage: id [-gGnruz] [--name] [--group] [--groups] [--user] [--context]");
    _exit(EXIT_USAGE);
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static void print_str(const char *s) {
    write(STDOUT_FILENO, s, slen(s));
}

static void print_char(int c) {
    write(STDOUT_FILENO, &c, 1);
}

static void print_uint(unsigned int val) {
    char buf[32];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    if (val == 0) {
        *--p = '0';
    } else {
        while (val > 0) {
            *--p = '0' + (val % 10);
            val /= 10;
        }
    }
    print_str(p);
}

static const char *get_username(uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    return pw ? pw->pw_name : NULL;
}

static const char *get_groupname(gid_t gid) {
    struct group *gr = getgrgid(gid);
    return gr ? gr->gr_name : NULL;
}

static void print_uid_value(uid_t uid, int use_names) {
    if (use_names) {
        const char *name = get_username(uid);
        if (name) {
            print_str(name);
            return;
        }
    }
    print_uint((unsigned int)uid);
}

static void print_gid_value(gid_t gid, int use_names) {
    if (use_names) {
        const char *name = get_groupname(gid);
        if (name) {
            print_str(name);
            return;
        }
    }
    print_uint((unsigned int)gid);
}

static void parse_long_option(const char *arg) {
    if (strcmp(arg, "--name") == 0 || strcmp(arg, "--real") == 0) {
        opt_names = 1;
    } else if (strcmp(arg, "--group") == 0) {
        opt_group_only = 1;
    } else if (strcmp(arg, "--groups") == 0) {
        opt_groups_only = 1;
    } else if (strcmp(arg, "--user") == 0) {
        opt_user_only = 1;
    } else if (strcmp(arg, "--context") == 0) {
        opt_context = 1;
    } else {
        eprint2(prog, "unknown option: ");
        print_str(arg);
        print_char('\n');
        usage();
    }
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == '\0') {
                /* bare '-' not supported */
                eprint2(prog, "invalid option: '-'");
                print_char('\n');
                usage();
            } else if (argv[i][1] == '-') {
                parse_long_option(argv[i]);
            } else {
                const char *p = &argv[i][1];
                while (*p) {
                    switch (*p) {
                        case 'g': opt_group_only = 1;  break;
                        case 'G': opt_groups_only = 1; break;
                        case 'n': opt_names = 1;       break;
                        case 'r': opt_real = 1;        break;
                        case 'u': opt_user_only = 1;   break;
                        case 'z': opt_zero = 1;        break;
                        default:
                            eprint2(prog, "unknown option: -");
                            print_char(*p);
                            print_char('\n');
                            usage();
                    }
                    p++;
                }
            }
        } else {
            eprint2(prog, "unexpected argument: ");
            print_str(argv[i]);
            print_char('\n');
            usage();
        }
    }

    /* Resolve effective vs real IDs */
    uid_t uid = opt_real ? getuid() : geteuid();
    gid_t gid = opt_real ? getgid() : getegid();

    char sep = opt_zero ? '\0' : '\n';

    /* -u: user only */
    if (opt_user_only) {
        print_uid_value(uid, opt_names);
        print_char(sep);
        return EXIT_OK;
    }

    /* -g: group only */
    if (opt_group_only) {
        print_gid_value(gid, opt_names);
        print_char(sep);
        return EXIT_OK;
    }

    /* -G: all group IDs */
    if (opt_groups_only) {
        gid_t groups[NGROUPS_MAX];
        int ngroups = getgroups(NGROUPS_MAX, groups);
        if (ngroups < 0) {
            eprint2(prog, "getgroups failed");
            _exit(EXIT_FAIL);
        }
        for (int i = 0; i < ngroups; i++) {
            if (i > 0)
                print_char(opt_zero ? '\0' : ' ');
            print_gid_value(groups[i], opt_names);
        }
        print_char(sep);
        return EXIT_OK;
    }

    /* --context: security context (unconfined on Forest OS) */
    if (opt_context) {
        print_str("context=\"unconfined\"");
        print_char(sep);
        return EXIT_OK;
    }

    /* Default: uid=... gid=... groups=... */
    print_str("uid=");
    print_uid_value(uid, opt_names);

    print_str(" gid=");
    print_gid_value(gid, opt_names);

    /* Supplementary groups */
    gid_t groups[NGROUPS_MAX];
    int ngroups = getgroups(NGROUPS_MAX, groups);
    if (ngroups > 0) {
        print_str(" groups=");
        for (int i = 0; i < ngroups; i++) {
            if (i > 0)
                print_char(',');
            print_gid_value(groups[i], opt_names);
        }
    }

    print_char(sep);
    return EXIT_OK;
}
