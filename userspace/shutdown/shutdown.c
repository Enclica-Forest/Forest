/*
 * Forest OS Shutdown - System Power Control
 *
 * Usage: shutdown [OPTIONS] TIME [MESSAGE]
 *
 * TIME: now | +MINUTES | HH:MM
 *
 * Options:
 *   -h   Halt the system (alias for --halt)
 *   -r   Reboot (alias for --reboot)
 *   -P   Power off (default, alias for --poweroff)
 *   -c   Cancel a pending shutdown
 *   -k   Dry run: only send warnings, don't shutdown
 *   -f   Fast: skip scripts and warnings
 *   -F   Force: skip kill phase, immediate reboot
 */

#include "forest.h"
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

/* ---- Reboot command constants (Linux-compatible) ---- */
#ifndef RB_AUTOBOOT
#define RB_AUTOBOOT   0x01234567
#endif
#ifndef RB_HALT_SYSTEM
#define RB_HALT_SYSTEM  0xCDEF0123
#endif
#ifndef RB_POWER_OFF
#define RB_POWER_OFF  0x4321FEDC
#endif
#ifndef RB_SWsuspend
#define RB_SWsuspend   0xD000FCE2
#endif

#define SHUTDOWN_PIDFILE   "/var/run/shutdown.pid"
#define SHUTDOWN_DIR       "/etc/shutdown.d"
#define WTMP_PATH          "/var/log/wtmp"
#define DEV_CONSOLE        "/dev/console"

#define WARN_INTERVAL      60      /* seconds between warnings */
#define WARN_FIRST_DELAY   60      /* first warning delay */
#define KILL_TIMEOUT       5       /* seconds between SIGTERM and SIGKILL */

#define MAX_MSG_LEN        256
#define MAX_LINE           512

/* ---- Shutdown action enum ---- */
enum shutdown_action {
    ACTION_HALT,
    ACTION_REBOOT,
    ACTION_POWEROFF,
    ACTION_CANCEL,
    ACTION_DRYRUN
};

/* ---- utmp/wtmp record (simplified, Linux-compatible) ---- */
#define UT_LINESIZE  32
#define UT_NAMESIZE  32
#define UT_HOSTSIZE 256

struct utmp {
    short   ut_type;
    int32_t ut_pid;
    char    ut_line[UT_LINESIZE];
    char    ut_id[4];
    char    ut_user[UT_NAMESIZE];
    char    ut_host[UT_HOSTSIZE];
    int32_t ut_exit;       /* exit status */
    int32_t ut_session;    /* session ID */
    int32_t ut_time;       /* seconds since epoch */
    int32_t ut_addr[4];    /* IPv4 address */
    char    __unused[20];
};

/* ut_type values */
#define UT_UNKNOWN   0
#define RUN_LVL      1
#define BOOT_TIME    2
#define NEW_TIME     8
#define OLD_TIME     9
#define INIT_PROCESS 5
#define LOGIN_PROCESS 6
#define USER_PROCESS 7
#define DEAD_PROCESS 8

/* ---- Globals ---- */
static enum shutdown_action g_action = ACTION_POWEROFF;
static int g_time_now = 0;       /* 1 = shutdown now */
static int g_minutes = 0;        /* minutes from now */
static char g_message[MAX_MSG_LEN] = "The system is going down for maintenance NOW!";
static volatile sig_atomic_t g_cancelled = 0;
static time_t g_shutdown_time = 0;
static int g_dry_run = 0;
static int g_fast = 0;
static int g_force = 0;
static uid_t g_uid = 0;

/* ---- Forward declarations ---- */
static void usage(const char *prog);
static void broadcast(const char *msg);
static void write_wtmp(int type, const char *line, const char *user);
static void warn_users(void);
static void run_shutdown_scripts(void);
static void kill_all_processes(void);
static void do_reboot(void);
static int parse_time(const char *arg);
static int parse_args(int argc, char *argv[]);
static void signal_handler(int sig);

/* ---- Usage ---- */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS] TIME [MESSAGE]\n", prog);
    fprintf(stderr, "  TIME: now, +MINUTES, HH:MM\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h   Halt\n");
    fprintf(stderr, "  -r   Reboot\n");
    fprintf(stderr, "  -P   Power off (default)\n");
    fprintf(stderr, "  -c   Cancel pending shutdown\n");
    fprintf(stderr, "  -k   Dry run (warn only)\n");
    fprintf(stderr, "  -f   Fast (skip scripts/warnings)\n");
    fprintf(stderr, "  -F   Force (skip kill, immediate)\n");
}

/* ---- Signal handler for cancel ---- */
static void signal_handler(int sig) {
    (void)sig;
    g_cancelled = 1;
}

/* ---- Broadcast message to all logged-in users ---- */
static void broadcast(const char *msg) {
    /* Try /dev/console first */
    int fd = open(DEV_CONSOLE, O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        write(fd, "\a", 1);  /* bell */
        write(fd, msg, strlen(msg));
        write(fd, "\n", 1);
        close(fd);
    }

    /* Also try /dev/tty0 and /dev/tty1-8 */
    char tty[16];
    for (int i = 0; i < 8; i++) {
        snprintf(tty, sizeof(tty), "/dev/tty%d", i + 1);
        fd = open(tty, O_WRONLY | O_NOCTTY);
        if (fd >= 0) {
            write(fd, "\a", 1);
            write(fd, msg, strlen(msg));
            write(fd, "\n", 1);
            close(fd);
        }
    }
}

/* ---- Write utmp/wtmp record ---- */
static void write_wtmp(int type, const char *line, const char *user) {
    int fd = open(WTMP_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;

    struct utmp entry;
    memset(&entry, 0, sizeof(entry));
    entry.ut_type = type;
    entry.ut_pid = getpid();
    strncpy(entry.ut_line, line, UT_LINESIZE - 1);
    strncpy(entry.ut_user, user, UT_NAMESIZE - 1);
    entry.ut_time = (int32_t)time(NULL);

    write(fd, &entry, sizeof(entry));
    close(fd);
}

/* ---- Parse time argument ---- */
static int parse_time(const char *arg) {
    if (strcmp(arg, "now") == 0) {
        g_time_now = 1;
        return 0;
    }

    /* +MINUTES format */
    if (arg[0] == '+') {
        int mins = atoi(arg + 1);
        if (mins < 0) return -1;
        g_minutes = mins;
        g_shutdown_time = time(NULL) + mins * 60;
        return 0;
    }

    /* HH:MM format */
    int hh, mm;
    if (sscanf(arg, "%d:%d", &hh, &mm) == 2) {
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
        g_shutdown_time = time(NULL);
        struct tm *tm = localtime(&g_shutdown_time);
        tm->tm_hour = hh;
        tm->tm_min = mm;
        tm->tm_sec = 0;
        g_shutdown_time = mktime(tm);
        if (g_shutdown_time <= time(NULL)) {
            g_shutdown_time += 86400;  /* next day */
        }
        g_minutes = (int)((g_shutdown_time - time(NULL)) / 60);
        return 0;
    }

    return -1;
}

/* ---- Parse command-line arguments ---- */
static int parse_args(int argc, char *argv[]) {

    /* Check for -c first (can be used alone) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            g_action = ACTION_CANCEL;
            return 0;
        }
    }

    /* Parse options */
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        const char *opt = argv[i];
        if (strcmp(opt, "-h") == 0) {
            g_action = ACTION_HALT;
        } else if (strcmp(opt, "-r") == 0) {
            g_action = ACTION_REBOOT;
        } else if (strcmp(opt, "-P") == 0) {
            g_action = ACTION_POWEROFF;
        } else if (strcmp(opt, "-k") == 0) {
            g_dry_run = 1;
        } else if (strcmp(opt, "-f") == 0) {
            g_fast = 1;
        } else if (strcmp(opt, "-F") == 0) {
            g_force = 1;
        } else if (strcmp(opt, "--halt") == 0) {
            g_action = ACTION_HALT;
        } else if (strcmp(opt, "--reboot") == 0) {
            g_action = ACTION_REBOOT;
        } else if (strcmp(opt, "--poweroff") == 0) {
            g_action = ACTION_POWEROFF;
        } else if (strcmp(opt, "--help") == 0) {
            usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "shutdown: unknown option: %s\n", opt);
            usage(argv[0]);
            return 1;
        }
        i++;
    }

    /* Time argument is required */
    if (i >= argc) {
        fprintf(stderr, "shutdown: TIME argument required\n");
        usage(argv[0]);
        return 1;
    }

    if (parse_time(argv[i]) < 0) {
        fprintf(stderr, "shutdown: invalid time: %s\n", argv[i]);
        return 1;
    }
    i++;

    /* Optional message */
    if (i < argc) {
        g_message[0] = '\0';
        for (; i < argc; i++) {
            if (g_message[0] != '\0') {
                strncat(g_message, " ", MAX_MSG_LEN - strlen(g_message) - 1);
            }
            strncat(g_message, argv[i], MAX_MSG_LEN - strlen(g_message) - 1);
        }
    }

    return 0;
}

/* ---- Warn users at intervals ---- */
static void warn_users(void) {
    char msg[MAX_MSG_LEN + 128];
    int remaining = g_minutes;
    time_t now;

    if (remaining <= 0 || g_fast) return;

    /* First warning */
    snprintf(msg, sizeof(msg),
             "*** %s ***\n\nSystem going down at %s",
             g_message,
             ctime(&g_shutdown_time));
    broadcast(msg);

    /* Warn at intervals */
    while (remaining > 0 && !g_cancelled) {
        sleep(WARN_INTERVAL > remaining ? remaining : WARN_INTERVAL);
        remaining = g_minutes - (int)((time(&now) - (g_shutdown_time - g_minutes * 60)));

        if (remaining > 0 && remaining <= g_minutes) {
            snprintf(msg, sizeof(msg),
                     "*** %s ***\n\nSystem going down in %d minute%s",
                     g_message, remaining, remaining == 1 ? "" : "s");
            broadcast(msg);
        }
    }
}

/* ---- Execute shutdown.d scripts ---- */
static void run_shutdown_scripts(void) {
    DIR *dir = opendir(SHUTDOWN_DIR);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[MAX_LINE];
        snprintf(path, sizeof(path), "%s/%s", SHUTDOWN_DIR, ent->d_name);

        struct stat st;
        if (stat(path, &st) < 0) continue;
        if (!(st.st_mode & S_IXUSR)) continue;  /* must be executable */

        pid_t pid = fork();
        if (pid == 0) {
            /* Child: run script with action as argument */
            const char *action_str;
            switch (g_action) {
                case ACTION_HALT:     action_str = "halt"; break;
                case ACTION_REBOOT:   action_str = "reboot"; break;
                case ACTION_POWEROFF: action_str = "poweroff"; break;
                default:              action_str = "poweroff"; break;
            }
            execl(path, path, action_str, NULL);
            _exit(127);
        } else if (pid > 0) {
            waitpid(pid, NULL, 0);
        }
    }
    closedir(dir);
}

/* ---- Kill all processes ---- */
static void kill_all_processes(void) {
    char msg[MAX_MSG_LEN + 64];

    /* SIGTERM first */
    snprintf(msg, sizeof(msg),
             "Sending SIGTERM to all processes...");
    broadcast(msg);
    kill(-1, SIGTERM);

    sleep(KILL_TIMEOUT);

    /* Then SIGKILL */
    snprintf(msg, sizeof(msg),
             "Sending SIGKILL to all processes...");
    broadcast(msg);
    kill(-1, SIGKILL);

    /* Brief pause to let processes die */
    sleep(1);
}

/* ---- Perform the actual reboot/halt/poweroff ---- */
static void do_reboot(void) {
    sync();

    switch (g_action) {
        case ACTION_HALT:
            if (reboot(RB_HALT_SYSTEM) < 0) {
                fprintf(stderr, "shutdown: halt failed: %s\n", strerror(errno));
            }
            break;
        case ACTION_REBOOT:
            if (reboot(RB_AUTOBOOT) < 0) {
                fprintf(stderr, "shutdown: reboot failed: %s\n", strerror(errno));
            }
            break;
        case ACTION_POWEROFF:
        default:
            if (reboot(RB_POWER_OFF) < 0) {
                fprintf(stderr, "shutdown: poweroff failed: %s\n", strerror(errno));
            }
            break;
    }

    /* If reboot failed, poweroff as fallback */
    if (g_action == ACTION_REBOOT) {
        poweroff();
    }

    /* Should not reach here */
    while (1) pause();
}

/* ---- Cancel a pending shutdown ---- */
static void do_cancel(void) {
    FILE *fp = fopen(SHUTDOWN_PIDFILE, "r");
    if (!fp) {
        fprintf(stderr, "shutdown: no pending shutdown to cancel\n");
        return;
    }

    pid_t pid = 0;
    if (fscanf(fp, "%d", &pid) == 1) {
        if (pid > 0) {
            kill(pid, SIGUSR1);
            fprintf(stderr, "shutdown: cancel signal sent to PID %d\n", pid);
        }
    }
    fclose(fp);
    unlink(SHUTDOWN_PIDFILE);
}

/* ---- Main ---- */
int main(int argc, char *argv[]) {
    int ret = parse_args(argc, argv);
    if (ret != 0) return ret;

    g_uid = getuid();

    /* Only root can shut down */
    if (g_uid != 0 && g_action != ACTION_DRYRUN) {
        fprintf(stderr, "shutdown: must be root to shut down the system\n");
        return EXIT_FAIL;
    }

    /* Cancel mode */
    if (g_action == ACTION_CANCEL) {
        do_cancel();
        return EXIT_OK;
    }

    /* Write PID file for cancel support */
    FILE *fp = fopen(SHUTDOWN_PIDFILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }

    /* Setup signal handler for cancel */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* Log to wtmp */
    write_wtmp(BOOT_TIME, "~", "shutdown");

    /* Announce shutdown */
    char msg[MAX_MSG_LEN + 256];
    const char *action_str;
    switch (g_action) {
        case ACTION_HALT:     action_str = "HALT"; break;
        case ACTION_REBOOT:   action_str = "REBOOT"; break;
        case ACTION_POWEROFF: action_str = "POWER OFF"; break;
        default:              action_str = "POWER OFF"; break;
    }

    if (g_time_now) {
        snprintf(msg, sizeof(msg),
                 "The system is going down for %s NOW!\n\n%s",
                 action_str, g_message);
    } else {
        snprintf(msg, sizeof(msg),
                 "The system is going down for %s at %s\n\n%s",
                 action_str, ctime(&g_shutdown_time), g_message);
    }
    broadcast(msg);

    /* Warn users at intervals */
    warn_users();

    if (g_cancelled) {
        fprintf(stderr, "shutdown: shutdown cancelled\n");
        unlink(SHUTDOWN_PIDFILE);
        return EXIT_OK;
    }

    if (g_dry_run) {
        fprintf(stderr, "shutdown: dry run, not actually shutting down\n");
        unlink(SHUTDOWN_PIDFILE);
        return EXIT_OK;
    }

    /* Run shutdown scripts */
    if (!g_fast) {
        run_shutdown_scripts();
    }

    /* Kill all processes unless force mode */
    if (!g_force) {
        kill_all_processes();
    }

    /* Remove PID file */
    unlink(SHUTDOWN_PIDFILE);

    /* Perform the actual system action */
    do_reboot();

    return EXIT_OK;
}
