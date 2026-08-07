/*
 * sudo.c - Forest OS userspace sudo implementation
 *
 * Execute commands as root or another user.
 * Usage: sudo [-u user] [-g group] [-i] [-s] [-l] [-k] [-K] [-v] [-n] [-S] [-e] command ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* Forest OS libc lacks sig_atomic_t */
#ifndef _SIG_ATOMIC_T
#define _SIG_ATOMIC_T
typedef int sig_atomic_t;
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>

#define SUDOERS_FILE    "/etc/sudoers"
#define SUDOERS_DIR     "/etc/sudoers.d"
#define SHADOW_FILE     "/etc/shadow"
#define TIMESTAMP_DIR   "/var/run/sudo"
#define LOG_FILE        "/var/log/sudo.log"
#define SUDO_VERSION    "1.0.0"
#define MAX_LINE        1024
#define MAX_ARGS        256
#define TIMESTAMP_TTL   600

static const char *prog = "sudo";

static int opt_run_as_user = 0;
static int opt_run_as_group = 0;
static int opt_login_shell = 0;
static int opt_shell = 0;
static int opt_list = 0;
static int opt_reset_timestamp = 0;
static int opt_remove_timestamp = 0;
static int opt_validate = 0;
static int opt_non_interactive = 0;
static int opt_stdin_password = 0;
static int opt_edit = 0;

static const char *target_user = "root";
static const char *target_group = NULL;
static uid_t target_uid = 0;
static gid_t target_gid = 0;

static void usage(void) {
    fprintf(stderr, "usage: sudo [-u user] [-g group] [-h] [-i] [-s] [-l] [-k] [-K] [-v] [-n] [-S] [-e] command ...\n");
}

static void help(void) {
    printf("sudo version %s\n", SUDO_VERSION);
    printf("Usage: sudo [-u user] [-g group] [-h] [-i] [-s] [-l] [-k] [-K] [-v] [-n] [-S] [-e] command ...\n");
    printf("  -u user    Run command as specified user\n");
    printf("  -g group   Run command as specified group\n");
    printf("  -i         Run login shell\n");
    printf("  -s         Run shell\n");
    printf("  -l         List user's privileges\n");
    printf("  -h         Display help\n");
    printf("  -k         Reset timestamp\n");
    printf("  -K         Remove timestamp\n");
    printf("  -v         Validate credentials\n");
    printf("  -n         Non-interactive mode\n");
    printf("  -S         Read password from stdin\n");
    printf("  -e         Edit file\n");
}

static void log_command(const char *user, const char *command, int success) {
    FILE *logf = fopen(LOG_FILE, "a");
    if (!logf)
        return;

    time_t now = time(NULL);
    char timebuf[64];
    struct tm *tm = localtime(&now);
    if (tm)
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
    else
        snprintf(timebuf, sizeof(timebuf), "unknown");

    fprintf(logf, "%s : %s : TTY=%s ; PWD=%s ; COMMAND=%s ; %s\n",
            user, timebuf,
            ttyname(STDERR_FILENO) ? ttyname(STDERR_FILENO) + 5 : "unknown",
            getcwd(NULL, 0) ? getcwd(NULL, 0) : "/",
            command,
            success ? "SUCCESS" : "FAILED");
    fclose(logf);
}

static char *get_timestamp_path(const char *user) {
    static char path[256];
    snprintf(path, sizeof(path), "%s/%s", TIMESTAMP_DIR, user);
    return path;
}

static int check_timestamp(const char *user) {
    char *path = get_timestamp_path(user);
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;

    time_t now = time(NULL);
    if (now - st.st_mtime > TIMESTAMP_TTL) {
        unlink(path);
        return 0;
    }
    return 1;
}

static void create_timestamp(const char *user) {
    mkdir(TIMESTAMP_DIR, 0755);
    char *path = get_timestamp_path(user);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        write(fd, "", 0);
        close(fd);
    }
}

static void remove_timestamp(const char *user) {
    char *path = get_timestamp_path(user);
    unlink(path);
}

static int match_host(const char *pattern, const char *hostname) {
    if (strcmp(pattern, "ALL") == 0)
        return 1;
    if (strcmp(pattern, hostname) == 0)
        return 1;
    if (pattern[0] == '.' && strcmp(hostname + strlen(hostname) - strlen(pattern), pattern) == 0)
        return 1;
    return 0;
}

static int match_command(const char *pattern, const char *command) {
    if (strcmp(pattern, "ALL") == 0)
        return 1;
    if (strcmp(pattern, command) == 0)
        return 1;
    if (pattern[0] != '/' && strstr(command, pattern) == command)
        return 1;
    return 0;
}

static int check_sudoers(const char *user, const char *host, const char *command) {
    FILE *f = fopen(SUDOERS_FILE, "r");
    if (!f)
        return 1;

    char line[MAX_LINE];
    int allowed = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace(*p))
            p++;
        if (*p == '#' || *p == '\0')
            continue;

        char *colon = strchr(p, ':');
        if (!colon)
            continue;

        *colon = '\0';
        char *user_field = p;

        char *host_colon = strchr(colon + 1, ':');
        if (!host_colon)
            continue;

        *host_colon = '\0';
        char *host_field = colon + 1;

        char *runas_start = strchr(host_colon + 1, '(');
        char *runas_end = strchr(host_colon + 1, ')');

        char *cmd_field;
        if (runas_start && runas_end && runas_end > runas_start) {
            cmd_field = runas_end + 1;
            while (*cmd_field && isspace(*cmd_field))
                cmd_field++;
        } else {
            cmd_field = host_colon + 1;
        }

        while (*cmd_field && isspace(*cmd_field))
            cmd_field++;

        if (strcmp(user_field, user) != 0 && strcmp(user_field, "ALL") != 0)
            continue;

        if (!match_host(host_field, host))
            continue;

        char *cmd = strtok(cmd_field, " \t\n");
        while (cmd) {
            if (match_command(cmd, command)) {
                allowed = 1;
                break;
            }
            cmd = strtok(NULL, " \t\n");
        }
        if (allowed)
            break;
    }

    fclose(f);
    return allowed;
}

static int check_sudoers_dir(const char *user, const char *host, const char *command) {
    DIR *dir = opendir(SUDOERS_DIR);
    if (!dir)
        return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", SUDOERS_DIR, ent->d_name);

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p && isspace(*p))
                p++;
            if (*p == '#' || *p == '\0')
                continue;

            char *colon = strchr(p, ':');
            if (!colon)
                continue;

            *colon = '\0';
            char *user_field = p;

            char *host_colon = strchr(colon + 1, ':');
            if (!host_colon)
                continue;

            *host_colon = '\0';
            char *host_field = colon + 1;

            char *cmd_field = host_colon + 1;
            while (*cmd_field && isspace(*cmd_field))
                cmd_field++;

            if (strcmp(user_field, user) != 0 && strcmp(user_field, "ALL") != 0)
                continue;

            if (!match_host(host_field, host))
                continue;

            char *cmd = strtok(cmd_field, " \t\n");
            while (cmd) {
                if (match_command(cmd, command)) {
                    fclose(f);
                    closedir(dir);
                    return 1;
                }
                cmd = strtok(NULL, " \t\n");
            }
        }
        fclose(f);
    }
    closedir(dir);
    return 0;
}

static int check_privileges(const char *user, const char *host, const char *command) {
    if (check_sudoers(user, host, command))
        return 1;
    return check_sudoers_dir(user, host, command);
}

static int read_password_stdin(char *buf, size_t len) {
    if (!fgets(buf, len, stdin))
        return -1;
    size_t slen = strlen(buf);
    if (slen > 0 && buf[slen - 1] == '\n')
        buf[slen - 1] = '\0';
    return 0;
}

static int prompt_password(const char *user, char *buf, size_t len) {
    FILE *f = fopen(SHADOW_FILE, "r");
    if (!f) {
        if (opt_non_interactive) {
            fprintf(stderr, "%s: no password entry for %s\n", prog, user);
            return -1;
        }
        fprintf(stderr, "[sudo] password for %s: ", user);
        fflush(stderr);
        return read_password_stdin(buf, len);
    }

    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        if (strcmp(line, user) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);

    if (!found) {
        fprintf(stderr, "%s: no password entry for %s\n", prog, user);
        return -1;
    }

    if (opt_stdin_password)
        return read_password_stdin(buf, len);

    if (isatty(STDIN_FILENO)) {
        fprintf(stderr, "[sudo] password for %s: ", user);
        fflush(stderr);
        return read_password_stdin(buf, len);
    }

    fprintf(stderr, "%s: no tty and no askpass program specified\n", prog);
    return -1;
}

static void list_privileges(const char *user) {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    FILE *f = fopen(SUDOERS_FILE, "r");
    if (!f) {
        printf("User %s may run the following commands:\n", user);
        printf("    (ALL) ALL\n");
        return;
    }

    printf("User %s may run the following commands on %s:\n", user, hostname);

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace(*p))
            p++;
        if (*p == '#' || *p == '\0')
            continue;

        char *colon = strchr(p, ':');
        if (!colon)
            continue;

        *colon = '\0';
        char *user_field = p;

        if (strcmp(user_field, user) != 0 && strcmp(user_field, "ALL") != 0)
            continue;

        char *host_colon = strchr(colon + 1, ':');
        if (!host_colon)
            continue;

        *host_colon = '\0';
        char *host_field = colon + 1;

        char *cmd_field = host_colon + 1;
        while (*cmd_field && isspace(*cmd_field))
            cmd_field++;

        char *nl = strchr(cmd_field, '\n');
        if (nl)
            *nl = '\0';

        printf("    (%s) %s\n", host_field, cmd_field);
    }
    fclose(f);
}

static volatile sig_atomic_t received_signal = 0;

static void signal_handler(int sig) {
    received_signal = sig;
}

static void setup_signal_forwarding(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void forward_signals(pid_t child) {
    if (received_signal && child > 0) {
        kill(child, received_signal);
        received_signal = 0;
    }
}

static uid_t resolve_user(const char *name) {
    if (isdigit(name[0])) {
        return (uid_t)atoi(name);
    }
    struct passwd *pw = getpwnam(name);
    if (!pw) {
        fprintf(stderr, "%s: unknown user: %s\n", prog, name);
        exit(1);
    }
    return pw->pw_uid;
}

static gid_t resolve_group(const char *name) {
    if (isdigit(name[0])) {
        return (gid_t)atoi(name);
    }
    struct group *gr = getgrnam(name);
    if (!gr) {
        fprintf(stderr, "%s: unknown group: %s\n", prog, name);
        exit(1);
    }
    return gr->gr_gid;
}

static void build_command_string(char *buf, size_t len, int argc, char *argv[], int start) {
    buf[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (i > start)
            strncat(buf, " ", len - strlen(buf) - 1);
        strncat(buf, argv[i], len - strlen(buf) - 1);
    }
}

int main(int argc, char *argv[]) {
    int argi;

    for (argi = 1; argi < argc; argi++) {
        if (argv[argi][0] != '-')
            break;

        if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        }

        if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
            help();
            return 0;
        }

        if (strcmp(argv[argi], "-V") == 0 || strcmp(argv[argi], "--version") == 0) {
            printf("Sudo version %s\n", SUDO_VERSION);
            return 0;
        }

        if (argv[argi][1] == 'u' && argv[argi][2] == '\0') {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "%s: option -u requires an argument\n", prog);
                return 1;
            }
            target_user = argv[argi];
            opt_run_as_user = 1;
            continue;
        }

        if (argv[argi][1] == 'g' && argv[argi][2] == '\0') {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "%s: option -g requires an argument\n", prog);
                return 1;
            }
            target_group = argv[argi];
            opt_run_as_group = 1;
            continue;
        }

        if (strcmp(argv[argi], "-e") == 0) {
            opt_edit = 1;
            argi++;
            break;
        }

        const char *p = &argv[argi][1];
        while (*p) {
            switch (*p) {
                case 'i': opt_login_shell = 1; break;
                case 's': opt_shell = 1; break;
                case 'l': opt_list = 1; break;
                case 'k': opt_reset_timestamp = 1; break;
                case 'K': opt_remove_timestamp = 1; break;
                case 'v': opt_validate = 1; break;
                case 'n': opt_non_interactive = 1; break;
                case 'S': opt_stdin_password = 1; break;
                default:
                    fprintf(stderr, "%s: unknown option: -%c\n", prog, *p);
                    usage();
                    return 1;
            }
            p++;
        }
    }

    target_uid = resolve_user(target_user);
    if (opt_run_as_group)
        target_gid = resolve_group(target_group);
    else
        target_gid = target_uid;

    if (opt_remove_timestamp) {
        remove_timestamp(target_user);
        return 0;
    }

    if (opt_reset_timestamp) {
        remove_timestamp(target_user);
        if (argi >= argc)
            return 0;
    }

    if (opt_list) {
        if (getuid() == 0) {
            list_privileges(target_user);
        } else {
            list_privileges(getenv("USER") ? getenv("USER") : "unknown");
        }
        return 0;
    }

    uid_t current_uid = getuid();

    if (current_uid == 0 && !opt_run_as_user && !opt_run_as_group) {
        if (argi >= argc) {
            usage();
            return 1;
        }

        log_command("root", argv[argi], 1);

        if (opt_shell || opt_login_shell) {
            const char *shell = "/bin/sh";
            struct passwd *pw = getpwuid(0);
            if (pw && pw->pw_shell)
                shell = pw->pw_shell;

            if (opt_login_shell) {
                execlp(shell, shell, "-l", NULL);
            } else {
                execlp(shell, shell, NULL);
            }
        } else if (opt_edit) {
            fprintf(stderr, "%s: edit mode not fully implemented\n", prog);
            return 1;
        } else {
            execvp(argv[argi], &argv[argi]);
        }

        fprintf(stderr, "%s: %s: %s\n", prog, argv[argi], strerror(errno));
        return 1;
    }

    if (argi >= argc && !opt_validate && !opt_shell && !opt_login_shell) {
        usage();
        return 1;
    }

    if (!check_timestamp(target_user)) {
        char password[256];
        if (prompt_password(target_user, password, sizeof(password)) < 0) {
            log_command(target_user, argi < argc ? argv[argi] : "timestamp", 0);
            return 1;
        }

        if (opt_validate) {
            log_command(target_user, "validate", 1);
            create_timestamp(target_user);
            printf("sudo credentials validated\n");
            return 0;
        }

        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        const char *cmd_name = argi < argc ? argv[argi] : "shell";
        if (!check_privileges(target_user, hostname, cmd_name)) {
            fprintf(stderr, "%s: %s is not allowed to run sudo on %s\n",
                    prog, target_user, hostname);
            log_command(target_user, cmd_name, 0);
            return 1;
        }

        create_timestamp(target_user);
    }

    if (opt_validate) {
        log_command(target_user, "validate", 1);
        return 0;
    }

    setup_signal_forwarding();

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        if (opt_run_as_group) {
            if (setgid(target_gid) < 0) {
                fprintf(stderr, "%s: cannot set gid to %d: %s\n", prog, target_gid, strerror(errno));
                _exit(1);
            }
        }

        if (setuid(target_uid) < 0) {
            fprintf(stderr, "%s: cannot set uid to %d: %s\n", prog, target_uid, strerror(errno));
            _exit(1);
        }

        char cmdbuf[MAX_LINE * 4];
        if (opt_edit) {
            if (argi >= argc) {
                fprintf(stderr, "%s: -e requires a file argument\n", prog);
                _exit(1);
            }
            snprintf(cmdbuf, sizeof(cmdbuf), "vi %s", argv[argi]);
        } else if (opt_shell || opt_login_shell) {
            const char *shell = getenv("SHELL");
            if (!shell)
                shell = "/bin/sh";
            struct passwd *pw = getpwuid(target_uid);
            if (pw && pw->pw_shell)
                shell = pw->pw_shell;

            if (opt_login_shell) {
                snprintf(cmdbuf, sizeof(cmdbuf), "%s -l", shell);
            } else {
                snprintf(cmdbuf, sizeof(cmdbuf), "%s", shell);
            }
        } else {
            build_command_string(cmdbuf, sizeof(cmdbuf), argc, argv, argi);
        }

        log_command(target_user, cmdbuf, 1);

        if (opt_shell || opt_login_shell || opt_edit) {
            const char *shell;
            if (opt_shell || opt_login_shell) {
                shell = getenv("SHELL");
                if (!shell)
                    shell = "/bin/sh";
                struct passwd *pw = getpwuid(target_uid);
                if (pw && pw->pw_shell)
                    shell = pw->pw_shell;
            } else {
                shell = getenv("SHELL");
                if (!shell)
                    shell = "/bin/sh";
            }

            if (opt_login_shell) {
                setenv("HOME", getpwuid(target_uid)->pw_dir, 1);
                setenv("USER", target_user, 1);
                setenv("LOGNAME", target_user, 1);
                execlp(shell, shell, "-l", NULL);
            } else if (opt_edit) {
                execlp(shell, shell, "-c", cmdbuf, NULL);
            } else {
                execlp(shell, shell, NULL);
            }
        } else {
            execvp(argv[argi], &argv[argi]);
        }

        fprintf(stderr, "%s: %s: %s\n", prog, argv[argi], strerror(errno));
        _exit(1);
    }

    int status;
    while (1) {
        pid_t result = waitpid(child, &status, 0);
        if (result < 0) {
            if (errno == EINTR) {
                forward_signals(child);
                continue;
            }
            perror("waitpid");
            return 1;
        }
        break;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}
