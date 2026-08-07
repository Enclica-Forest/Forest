#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <utmp.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#define WTMP_PATH "/var/log/wtmp"

static void write_wtmp_reboot(void) {
    struct utmp entry;
    int fd;

    memset(&entry, 0, sizeof(entry));
    entry.ut_type = BOOT_TIME;
    entry.ut_pid = getpid();
    strncpy(entry.ut_line, "~", sizeof(entry.ut_line) - 1);
    strncpy(entry.ut_id, "~", sizeof(entry.ut_id) - 1);
    strncpy(entry.ut_user, "reboot", sizeof(entry.ut_user) - 1);
    strncpy(entry.ut_host, "forest", sizeof(entry.ut_host) - 1);
    entry.ut_session = 0;
    time_t now = time(NULL);
    entry.ut_tv.tv_sec = now;
    entry.ut_tv.tv_usec = 0;

    fd = open(WTMP_PATH, O_WRONLY | O_APPEND);
    if (fd < 0) {
        fprintf(stderr, "reboot: cannot open %s: %s\n", WTMP_PATH, strerror(errno));
        return;
    }

    if (write(fd, &entry, sizeof(entry)) != sizeof(entry)) {
        fprintf(stderr, "reboot: failed to write wtmp record\n");
    }

    close(fd);
}

static void usage(void) {
    fprintf(stderr, "usage: reboot [-p] [-h] [-f] [-w] [-b]\n");
    fprintf(stderr, "  -p  poweroff\n");
    fprintf(stderr, "  -h  halt\n");
    fprintf(stderr, "  -f  fast, don't sync\n");
    fprintf(stderr, "  -w  dry run, write wtmp only\n");
    fprintf(stderr, "  -b  boot immediately (Linux reboot)\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    int opt;
    int do_poweroff = 0;
    int do_halt = 0;
    int do_fast = 0;
    int do_wtmp_only = 0;
    int do_boot = 0;

    while ((opt = getopt(argc, argv, "phfwb")) != -1) {
        switch (opt) {
            case 'p':
                do_poweroff = 1;
                break;
            case 'h':
                do_halt = 1;
                break;
            case 'f':
                do_fast = 1;
                break;
            case 'w':
                do_wtmp_only = 1;
                break;
            case 'b':
                do_boot = 1;
                break;
            default:
                usage();
        }
    }

    if (getuid() != 0) {
        fprintf(stderr, "reboot: must be root\n");
        return 1;
    }

    if (do_wtmp_only) {
        write_wtmp_reboot();
        printf("reboot: wtmp record written (dry run)\n");
        return 0;
    }

    if (!do_fast) {
        sync();
    }

    write_wtmp_reboot();

    if (do_poweroff) {
        if (reboot(RB_POWER_OFF) < 0) {
            perror("reboot: poweroff failed");
            return 1;
        }
    } else if (do_halt) {
        if (reboot(RB_HALT_SYSTEM) < 0) {
            perror("reboot: halt failed");
            return 1;
        }
    } else if (do_boot) {
        if (reboot(RB_AUTOBOOT) < 0) {
            perror("reboot: boot failed");
            return 1;
        }
    } else {
        if (reboot(RB_AUTOBOOT) < 0) {
            perror("reboot: reboot failed");
            return 1;
        }
    }

    return 0;
}
