/*
 * Forest OS Init - PID 1 Process
 *
 * System initialization process responsible for:
 * - Starting system services
 * - Reaping zombie processes
 * - Handling shutdown/reboot
 * - Mounting essential filesystems
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

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_PROCS 64
#define MAX_LINE 256
#define MAX_ARGS 32

typedef struct {
    pid_t pid;
    char path[128];
    int respawn;
    int active;
} child_proc_t;

static child_proc_t children[MAX_PROCS];
static int num_children = 0;
static volatile sig_atomic_t shutdown_requested = 0;
static volatile sig_atomic_t reboot_requested = 0;
static int current_runlevel = 3;

static void signal_handler(int sig) {
    switch (sig) {
        case SIGCHLD:
            while (waitpid(-1, NULL, WNOHANG) > 0);
            break;
        case SIGTERM:
        case SIGINT:
            shutdown_requested = 1;
            break;
        case SIGUSR1:
            reboot_requested = 1;
            break;
    }
}

static int setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) return -1;

    return 0;
}

static void mount_essential(void) {
    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "init: mount /proc failed: %s\n", strerror(errno));

    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "init: mount /dev failed: %s\n", strerror(errno));

    mkdir("/sys", 0755);
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "init: mount /sys failed: %s\n", strerror(errno));

    mkdir("/tmp", 1755);
}

static void setup_environment(void) {
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "linux", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);

    struct utsname uts;
    if (uname(&uts) == 0) {
        char hostname[256];
        snprintf(hostname, sizeof(hostname), "forest-%s", uts.nodename);
        sethostname(hostname, strlen(hostname));
        setenv("HOSTNAME", hostname, 1);
    }
}

static void spawn_child(const char *path, int respawn) {
    if (num_children >= MAX_PROCS) {
        fprintf(stderr, "init: too many children\n");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: fork failed: %s\n", strerror(errno));
        return;
    }

    if (pid == 0) {
        setsid();
        if (path[0] == '/')
            execl(path, path, NULL);
        else {
            char fullpath[256];
            snprintf(fullpath, sizeof(fullpath), "/bin/%s", path);
            execl(fullpath, path, NULL);
        }
        fprintf(stderr, "init: exec %s failed: %s\n", path, strerror(errno));
        _exit(1);
    }

    children[num_children].pid = pid;
    strncpy(children[num_children].path, path, sizeof(children[num_children].path) - 1);
    children[num_children].respawn = respawn;
    children[num_children].active = 1;
    num_children++;
}

static void check_children(void) {
    for (int i = 0; i < num_children; i++) {
        if (children[i].active) {
            int status;
            pid_t pid = waitpid(children[i].pid, &status, WNOHANG);
            if (pid > 0) {
                children[i].active = 0;
                if (children[i].respawn) {
                    spawn_child(children[i].path, 1);
                }
            }
        }
    }
}

static void parse_inittab(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        char id[32], runlevels[32], action[32], process[128];
        if (sscanf(p, "%31[^:]:%31[^:]:%31[^:]:%127[^\n]", id, runlevels, action, process) == 4) {
            if (strcmp(action, "respawn") == 0 || strcmp(action, "once") == 0) {
                int respawn = (strcmp(action, "respawn") == 0);
                spawn_child(process, respawn);
            }
        }
    }
    fclose(fp);
}

static void start_default_services(void) {
    if (access("/etc/inittab", F_OK) == 0) {
        parse_inittab("/etc/inittab");
        return;
    }

    spawn_child("/bin/sh", 1);
}

static void kill_children(void) {
    for (int i = 0; i < num_children; i++) {
        if (children[i].active) {
            kill(children[i].pid, SIGTERM);
        }
    }

    sleep(2);

    for (int i = 0; i < num_children; i++) {
        if (children[i].active) {
            kill(children[i].pid, SIGKILL);
        }
    }
}

static void halt_system(void) {
    kill_children();
    sync();
    if (reboot(RB_POWER_OFF) < 0) {
        fprintf(stderr, "init: reboot/power_off failed: %s\n", strerror(errno));
        while (1) pause();
    }
}

static void reboot_system(void) {
    kill_children();
    sync();
    if (reboot(RB_AUTOBOOT) < 0) {
        fprintf(stderr, "init: reboot failed: %s\n", strerror(errno));
        while (1) pause();
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (getpid() != 1) {
        fprintf(stderr, "init: must be PID 1\n");
        return 1;
    }

    if (setup_signals() < 0) {
        fprintf(stderr, "init: signal setup failed\n");
        return 1;
    }

    mount_essential();
    setup_environment();

    fprintf(stderr, "Forest OS init starting...\n");
    fprintf(stderr, "Runlevel: %d\n", current_runlevel);

    start_default_services();

    while (1) {
        check_children();

        if (reboot_requested) {
            reboot_system();
        }

        if (shutdown_requested) {
            halt_system();
        }

        usleep(100000);
    }

    return 0;
}