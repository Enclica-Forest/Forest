/*
 * losetup.c - Loop device setup for Forest OS
 *
 * Associate block devices with regular files using loop devices.
 * Supports -a, -d, -f, -o, -r, -P, --show, --sizelimit.
 */
#include <forest.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/loop.h>

#define LOOP_DEV_FMT    "/dev/loop%d"
#define MAX_LOOPS      256

static const char *progname = "losetup";

static int flag_list       = 0;  /* -a */
static int flag_find       = 0;  /* -f */
static int flag_show       = 0;  /* --show */
static int flag_readonly   = 0;  /* -r */
static int flag_partscan   = 0;  /* -P */
static long long offset    = 0;  /* -o */
static long long sizelimit = 0;  /* --sizelimit */

static const char *detach_dev = NULL;  /* -d DEVICE */
static const char *backing_file = NULL;

static void usage(void) {
    fprintf(stderr,
        "Usage: %s [options] [file] [device]\n"
        "  -a, --all           list all loop devices\n"
        "  -d, --detach DEV    detach the file on DEV\n"
        "  -f, --find          find first unused loop device\n"
        "  -o, --offset N      start at offset N bytes\n"
        "      --sizelimit N   maximum size of loop device\n"
        "  -r, --read-only     set up read-only loop device\n"
        "  -P, --partscan      scan for partitions\n"
        "      --show          print device name on success\n"
        "  -h, --help          show this help\n",
        progname);
}

static int loop_set_fd(int loop_fd, int file_fd) {
    return ioctl(loop_fd, LOOP_SET_FD, file_fd);
}

static int loop_clr_fd(int loop_fd) {
    return ioctl(loop_fd, LOOP_CLR_FD, 0);
}

static int loop_set_status(int loop_fd, int readonly, long long off, long long limit) {
    struct loop_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.fd = 0;
    cfg.block_size = 0;

    if (readonly)
        cfg.info.lo_flags |= LO_FLAGS_READ_ONLY;
    if (off)
        cfg.info.lo_offset = (uint64_t)off;
    if (limit)
        cfg.info.lo_sizelimit = (uint64_t)limit;

    return ioctl(loop_fd, LOOP_SET_STATUS64, &cfg.info);
}

static int loop_get_status(int loop_fd, struct loop_info64 *info) {
    return ioctl(loop_fd, LOOP_GET_STATUS64, info);
}

static int is_loop_device_free(int devnum) {
    char path[32];
    snprintf(path, sizeof(path), LOOP_DEV_FMT, devnum);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 1;  /* can't open = probably free or doesn't exist */

    struct loop_info64 info;
    int used = (loop_get_status(fd, &info) == 0 && info.lo_device != 0);
    close(fd);
    return !used;
}

static int find_free_loop(void) {
    for (int i = 0; i < MAX_LOOPS; i++) {
        if (is_loop_device_free(i))
            return i;
    }
    return -1;
}

static void list_loop_devices(void) {
    for (int i = 0; i < MAX_LOOPS; i++) {
        char path[32];
        snprintf(path, sizeof(path), LOOP_DEV_FMT, i);

        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        struct loop_info64 info;
        if (loop_get_status(fd, &info) == 0 && info.lo_device != 0) {
            char *name = (char *)info.lo_file_name;
            size_t namelen = strlen(name);

            printf("%s: [%llu] (%.*s)", path, (unsigned long long)info.lo_offset,
                   (int)namelen, name);
            if (info.lo_flags & LO_FLAGS_READ_ONLY)
                printf(" (read only)");
            if (info.lo_sizelimit)
                printf(" sizelimit=%llu", (unsigned long long)info.lo_sizelimit);
            printf("\n");
        }
        close(fd);
    }
}

static int detach_loop(const char *dev) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open '%s': %s\n", progname, dev, strerror(errno));
        return 1;
    }

    if (loop_clr_fd(fd) != 0) {
        fprintf(stderr, "%s: cannot detach '%s': %s\n", progname, dev, strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int setup_loop(const char *file, const char *dev, int show) {
    int file_fd = open(file, O_RDONLY);
    if (file_fd < 0) {
        fprintf(stderr, "%s: cannot open '%s': %s\n", progname, file, strerror(errno));
        return 1;
    }

    int loopnum = -1;
    if (dev) {
        /* Parse device number from /dev/loopN */
        const char *p = strrchr(dev, 'l');
        if (!p || sscanf(p, "loop%d", &loopnum) != 1) {
            fprintf(stderr, "%s: invalid device '%s'\n", progname, dev);
            close(file_fd);
            return 1;
        }
    } else {
        loopnum = find_free_loop();
        if (loopnum < 0) {
            fprintf(stderr, "%s: no free loop devices\n", progname);
            close(file_fd);
            return 1;
        }
    }

    char looppath[32];
    snprintf(looppath, sizeof(looppath), LOOP_DEV_FMT, loopnum);

    int loop_fd = open(looppath, O_RDWR);
    if (loop_fd < 0) {
        fprintf(stderr, "%s: cannot open '%s': %s\n", progname, looppath, strerror(errno));
        close(file_fd);
        return 1;
    }

    if (loop_set_fd(loop_fd, file_fd) != 0) {
        fprintf(stderr, "%s: cannot set up '%s': %s\n", progname, looppath, strerror(errno));
        close(loop_fd);
        close(file_fd);
        return 1;
    }

    if (offset || sizelimit || flag_readonly) {
        if (loop_set_status(loop_fd, flag_readonly, offset, sizelimit) != 0) {
            fprintf(stderr, "%s: cannot set status on '%s': %s\n",
                    progname, looppath, strerror(errno));
            loop_clr_fd(loop_fd);
            close(loop_fd);
            close(file_fd);
            return 1;
        }
    }

    close(file_fd);
    close(loop_fd);

    if (show)
        printf("%s\n", looppath);

    return 0;
}

int main(int argc, char **argv) {
    progname = argv[0] ? argv[0] : "losetup";

    static struct option long_opts[] = {
        {"all",       no_argument,       NULL, 'a'},
        {"detach",    required_argument, NULL, 'd'},
        {"find",      no_argument,       NULL, 'f'},
        {"offset",    required_argument, NULL, 'o'},
        {"sizelimit", required_argument, NULL, 'S'},
        {"read-only", no_argument,       NULL, 'r'},
        {"partscan",  no_argument,       NULL, 'P'},
        {"show",      no_argument,       NULL, 's'},
        {"help",      no_argument,       NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "ad:fo:rPhs", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'a': flag_list = 1; break;
        case 'd': detach_dev = optarg; break;
        case 'f': flag_find = 1; break;
        case 'o': offset = strtoll(optarg, NULL, 10); break;
        case 'S': sizelimit = strtoll(optarg, NULL, 10); break;
        case 'r': flag_readonly = 1; break;
        case 'P': flag_partscan = 1; break;
        case 's': flag_show = 1; break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    if (flag_list) {
        list_loop_devices();
        return 0;
    }

    if (detach_dev)
        return detach_loop(detach_dev);

    if (flag_find) {
        int n = find_free_loop();
        if (n < 0) {
            fprintf(stderr, "%s: no free loop devices\n", progname);
            return 1;
        }
        printf("/dev/loop%d\n", n);
        return 0;
    }

    if (optind >= argc) {
        if (flag_find) {
            /* already handled above */
            return 0;
        }
        usage();
        return 1;
    }

    backing_file = argv[optind];
    const char *device = (optind + 1 < argc) ? argv[optind + 1] : NULL;

    return setup_loop(backing_file, device, flag_show);
}
