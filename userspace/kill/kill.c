#include "forest.h"
#include <ctype.h>

static const char *signal_names[] = {
    "HUP",    "INT",    "QUIT",   "ILL",    "TRAP",   "ABRT",   "BUS",
    "FPE",    "KILL",   "USR1",   "SEGV",   "USR2",   "PIPE",   "ALRM",
    "TERM",   "STKFLT", "CHLD",   "CONT",   "STOP",   "TSTP",   "TTIN",
    "TTOU",   "URG",    "XCPU",   "XFSZ",   "VTALRM", "PROF",   "WINCH",
    "IO",     "PWR",    "SYS",    NULL
};

static int find_signal(const char *name) {
    for (int i = 0; signal_names[i] != NULL; i++) {
        if (strcasecmp(name, signal_names[i]) == 0)
            return i + 1;
    }
    return -1;
}

static void list_signals(void) {
    for (int i = 0; signal_names[i] != NULL; i++) {
        printf("%2d) %s\n", i + 1, signal_names[i]);
    }
}

static void usage(void) {
    fprintf(stderr, "usage: kill [-s signal | -signal] [-p] pid ...\n");
    fprintf(stderr, "       kill -l [signal]\n");
}

int main(int argc, char *argv[]) {
    int signum = -1;
    int do_list = 0;
    int do_print = 0;
    int argi = 1;

    if (argc < 2) {
        usage();
        return 1;
    }

    while (argi < argc) {
        char *arg = argv[argi];

        if (arg[0] != '-')
            break;

        if (strcmp(arg, "-l") == 0 || strcmp(arg, "-L") == 0) {
            do_list = 1;
            argi++;
            break;
        }

        if (strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        }

        if (strncmp(arg, "--signal=", 9) == 0) {
            const char *sigarg = arg + 9;
            if (isdigit(sigarg[0])) {
                signum = atoi(sigarg);
                if (signum <= 0 || signum >= 64) {
                    fprintf(stderr, "kill: invalid signal '%s'\n", sigarg);
                    return 1;
                }
            } else {
                signum = find_signal(sigarg);
                if (signum < 0) {
                    fprintf(stderr, "kill: unknown signal '%s'\n", sigarg);
                    return 1;
                }
            }
            argi++;
            continue;
        }

        if (arg[1] == 's' && arg[2] == '\0') {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "kill: option requires an argument -- s\n");
                return 1;
            }
            const char *sigarg = argv[argi];
            if (isdigit(sigarg[0])) {
                signum = atoi(sigarg);
                if (signum <= 0 || signum >= 64) {
                    fprintf(stderr, "kill: invalid signal '%s'\n", sigarg);
                    return 1;
                }
            } else {
                signum = find_signal(sigarg);
                if (signum < 0) {
                    fprintf(stderr, "kill: unknown signal '%s'\n", sigarg);
                    return 1;
                }
            }
            argi++;
            continue;
        }

        if (arg[1] == 'p') {
            do_print = 1;
            argi++;
            if (arg[2] != '\0') {
                const char *rest = arg + 2;
                if (isdigit(rest[0])) {
                    signum = atoi(rest);
                    if (signum <= 0 || signum >= 64) {
                        fprintf(stderr, "kill: invalid signal '%s'\n", rest);
                        return 1;
                    }
                } else {
                    signum = find_signal(rest);
                    if (signum < 0) {
                        fprintf(stderr, "kill: unknown signal '%s'\n", rest);
                        return 1;
                    }
                }
            }
            continue;
        }

        if (isdigit(arg[1])) {
            signum = atoi(arg + 1);
            if (signum <= 0 || signum >= 64) {
                fprintf(stderr, "kill: invalid signal '%s'\n", arg + 1);
                return 1;
            }
            argi++;
            continue;
        }

        {
            const char *sigarg = arg + 1;
            int sig = find_signal(sigarg);
            if (sig < 0) {
                fprintf(stderr, "kill: unknown signal '%s'\n", sigarg);
                return 1;
            }
            signum = sig;
            argi++;
            continue;
        }
    }

    if (do_list) {
        if (argi < argc) {
            for (int i = argi; i < argc; i++) {
                int sig;
                if (isdigit(argv[i][0])) {
                    sig = atoi(argv[i]);
                    if (sig > 0 && sig < 64) {
                        if (signal_names[sig - 1])
                            printf("%s\n", signal_names[sig - 1]);
                        else
                            printf("%d\n", sig);
                    } else {
                        fprintf(stderr, "kill: invalid signal '%s'\n", argv[i]);
                        return 1;
                    }
                } else {
                    sig = find_signal(argv[i]);
                    if (sig > 0)
                        printf("%d\n", sig);
                    else {
                        fprintf(stderr, "kill: invalid signal '%s'\n", argv[i]);
                        return 1;
                    }
                }
            }
        } else {
            list_signals();
        }
        return 0;
    }

    if (signum < 0)
        signum = SIGTERM;

    if (argi >= argc) {
        fprintf(stderr, "kill: no process specified\n");
        return 1;
    }

    int ret = 0;
    for (int i = argi; i < argc; i++) {
        pid_t pid;
        char *endptr;

        pid = strtol(argv[i], &endptr, 10);
        if (*endptr != '\0' || endptr == argv[i]) {
            fprintf(stderr, "kill: invalid process id '%s'\n", argv[i]);
            ret = 1;
            continue;
        }

        if (do_print) {
            printf("signal %s pid %d\n", signal_names[signum - 1], pid);
            continue;
        }

        if (kill(pid, signum) < 0) {
            switch (errno) {
            case ESRCH:
                fprintf(stderr, "kill: no such process\n");
                break;
            case EPERM:
                fprintf(stderr, "kill: operation not permitted\n");
                break;
            case EINVAL:
                fprintf(stderr, "kill: invalid signal\n");
                break;
            default:
                perror("kill");
                break;
            }
            ret = 1;
        }
    }

    return ret;
}
