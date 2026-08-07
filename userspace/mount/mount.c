#include <forest.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/loop.h>

#define MAX_OPTS_LEN 1024
#define MAX_LINE_LEN 1024

typedef struct {
    char *fstype;
    char *options;
    char *device;
    char *mountpoint;
    int read_only;
    int read_write;
    int verbose;
    int fake;
    int no_mtab;
    int mount_all;
} MountOptions;

static MountOptions opts;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-t type] [-o options] [-rvwfn] [-a] [device] [mountpoint]\n", prog);
}

static unsigned long parse_mount_flags(const char *options) {
    unsigned long flags = 0;
    char *buf;
    char *token;
    char *saveptr;

    if (!options)
        return 0;

    buf = strdup(options);
    if (!buf)
        return 0;

    token = strtok_r(buf, ",", &saveptr);
    while (token) {
        if (strcmp(token, "ro") == 0 || strcmp(token, "read-only") == 0)
            flags |= MS_RDONLY;
        else if (strcmp(token, "rw") == 0 || strcmp(token, "read-write") == 0)
            flags &= ~MS_RDONLY;
        else if (strcmp(token, "nosuid") == 0)
            flags |= MS_NOSUID;
        else if (strcmp(token, "nodev") == 0)
            flags |= MS_NODEV;
        else if (strcmp(token, "noexec") == 0)
            flags |= MS_NOEXEC;
        else if (strcmp(token, "sync") == 0)
            flags |= MS_SYNCHRONOUS;
        else if (strcmp(token, "remount") == 0)
            flags |= MS_REMOUNT;
        else if (strcmp(token, "bind") == 0)
            flags |= MS_BIND;
        else if (strcmp(token, "dirsync") == 0)
            flags |= MS_DIRSYNC;
        else if (strcmp(token, "noatime") == 0)
            flags |= MS_NOATIME;
        else if (strcmp(token, "nodiratime") == 0)
            flags |= MS_NODIRATIME;
        else if (strcmp(token, "relatime") == 0)
            flags |= MS_RELATIME;
        else if (strcmp(token, "lazytime") == 0)
            flags |= MS_LAZYTIME;
        else if (strcmp(token, "verbose") == 0)
            opts.verbose = 1;
        else if (strncmp(token, "loop=", 5) == 0) {
            /* loop device specified - handled separately */
        }
        else if (strcmp(token, "loop") == 0) {
            /* auto loop device */
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(buf);
    return flags;
}

static char *find_loop_device(void) {
    char *loopdev = malloc(32);
    if (!loopdev)
        return NULL;

    for (int i = 0; i < 8; i++) {
        snprintf(loopdev, 32, "/dev/loop%d", i);
        struct stat st;
        if (stat(loopdev, &st) == 0 && S_ISBLK(st.st_mode))
            return loopdev;
    }

    free(loopdev);
    return NULL;
}

static char *extract_loop_device(const char *options) {
    const char *p;
    char *dev;

    p = strstr(options, "loop=");
    if (!p)
        return NULL;

    p += 5;
    const char *end = p;
    while (*end && *end != ',')
        end++;

    dev = malloc(end - p + 1);
    if (!dev)
        return NULL;

    memcpy(dev, p, end - p);
    dev[end - p] = '\0';
    return dev;
}

static int do_mount(const char *device, const char *mountpoint,
                    const char *fstype, unsigned long flags, const char *data) {
    if (opts.verbose) {
        printf("mount: mounting %s on %s type %s",
               device ? device : "(none)", mountpoint,
               fstype ? fstype : "(default)");
        if (flags || data)
            printf(" flags=%lu data=%s", flags, data ? data : "");
        printf("\n");
    }

    if (opts.fake) {
        if (opts.verbose)
            printf("mount: (fake) would mount %s on %s\n", device, mountpoint);
        return 0;
    }

    if (mount(device, mountpoint, fstype, flags, data) < 0) {
        fprintf(stderr, "mount: %s on %s failed: %s\n",
                device ? device : "(none)", mountpoint, strerror(errno));
        return -1;
    }

    if (opts.verbose)
        printf("mount: %s mounted on %s\n", device, mountpoint);

    return 0;
}

static int do_umount(const char *target) {
    if (opts.verbose)
        printf("mount: unmounting %s\n", target);

    if (opts.fake)
        return 0;

    if (umount2(target, 0) < 0) {
        fprintf(stderr, "mount: unmounting %s failed: %s\n", target, strerror(errno));
        return -1;
    }

    return 0;
}

static int update_mtab(const char *device, const char *mountpoint,
                       const char *fstype, unsigned long flags) {
    FILE *f;
    char opts_str[MAX_OPTS_LEN] = "";

    if (opts.no_mtab)
        return 0;

    f = fopen("/etc/mtab", "a");
    if (!f) {
        /* Try /proc/mounts as fallback */
        f = fopen("/proc/mounts", "r");
        if (!f)
            return 0;
        /* We can't write to /proc/mounts, so just return */
        fclose(f);
        return 0;
    }

    /* Build options string from flags */
    if (flags & MS_RDONLY) strcat(opts_str, "ro,");
    else strcat(opts_str, "rw,");
    if (flags & MS_NOSUID) strcat(opts_str, "nosuid,");
    if (flags & MS_NODEV) strcat(opts_str, "nodev,");
    if (flags & MS_NOEXEC) strcat(opts_str, "noexec,");

    /* Remove trailing comma */
    size_t len = strlen(opts_str);
    if (len > 0 && opts_str[len - 1] == ',')
        opts_str[len - 1] = '\0';

    fprintf(f, "%s %s %s %s 0 0\n",
            device ? device : "none",
            mountpoint,
            fstype ? fstype : "auto",
            opts_str);

    fclose(f);
    return 0;
}

static int mount_entry(const char *device, const char *mountpoint,
                       const char *fstype, const char *options) {
    unsigned long flags = 0;
    char *data = NULL;
    char *loopdev = NULL;
    int ret = 0;

    if (!device || !mountpoint) {
        fprintf(stderr, "mount: device and mountpoint required\n");
        return -1;
    }

    /* Create mountpoint if it doesn't exist */
    struct stat st;
    if (stat(mountpoint, &st) < 0) {
        if (mkdir(mountpoint, 0755) < 0) {
            fprintf(stderr, "mount: cannot create mountpoint '%s': %s\n",
                    mountpoint, strerror(errno));
            return -1;
        }
    }

    /* Parse options */
    flags = parse_mount_flags(options);

    /* Handle loop mounts */
    if (options && strstr(options, "loop")) {
        loopdev = extract_loop_device(options);
        if (!loopdev)
            loopdev = find_loop_device();

        if (!loopdev) {
            fprintf(stderr, "mount: no free loop device found\n");
            return -1;
        }

        if (opts.verbose)
            printf("mount: using loop device %s\n", loopdev);

        /* Setup loop device */
        int fd = open(loopdev, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "mount: cannot open %s: %s\n", loopdev, strerror(errno));
            free(loopdev);
            return -1;
        }

        int filefd = open(device, O_RDWR);
        if (filefd < 0) {
            fprintf(stderr, "mount: cannot open %s: %s\n", device, strerror(errno));
            close(fd);
            free(loopdev);
            return -1;
        }

        if (ioctl(fd, LOOP_SET_FD, filefd) < 0) {
            fprintf(stderr, "mount: loop set fd failed: %s\n", strerror(errno));
            close(filefd);
            close(fd);
            free(loopdev);
            return -1;
        }

        close(filefd);
        close(fd);

        device = loopdev;
    }

    /* Handle bind mounts */
    if (flags & MS_BIND) {
        if (opts.verbose)
            printf("mount: bind mounting %s on %s\n", device, mountpoint);
    }

    /* Perform the mount */
    ret = do_mount(device, mountpoint, fstype, flags, data);

    /* Update mtab */
    if (ret == 0)
        update_mtab(device, mountpoint, fstype, flags);

    free(loopdev);
    return ret;
}

static int parse_fstab_entry(char *line, char **device, char **mountpoint,
                             char **fstype, char **options) {
    char *saveptr;
    char *token;
    int field = 0;

    /* Initialize output pointers */
    *device = NULL;
    *mountpoint = NULL;
    *fstype = NULL;
    *options = NULL;

    /* Skip comments and empty lines */
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return 0;

    token = strtok_r(line, " \t", &saveptr);
    while (token) {
        switch (field) {
            case 0: *device = token; break;
            case 1: *mountpoint = token; break;
            case 2: *fstype = token; break;
            case 3: *options = token; break;
            default: return 1;
        }
        field++;
        token = strtok_r(NULL, " \t", &saveptr);
    }

    return (field >= 3) ? 1 : 0;
}

static int mount_all(void) {
    FILE *f;
    char line[MAX_LINE_LEN];
    char *device, *mountpoint, *fstype, *options;
    int ret = 0;
    int mounted = 0;

    f = fopen("/etc/fstab", "r");
    if (!f) {
        fprintf(stderr, "mount: cannot open /etc/fstab: %s\n", strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        /* Remove newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (parse_fstab_entry(line, &device, &mountpoint, &fstype, &options)) {
            /* Skip entries marked with noauto */
            if (options && strstr(options, "noauto"))
                continue;

            if (opts.verbose)
                printf("mount: mounting %s from fstab\n", mountpoint);

            if (mount_entry(device, mountpoint, fstype, options) < 0) {
                ret = -1;
            } else {
                mounted++;
            }
        }
    }

    fclose(f);

    if (opts.verbose)
        printf("mount: mounted %d filesystems from fstab\n", mounted);

    return ret;
}

static int show_mounts(void) {
    FILE *f;
    char line[MAX_LINE_LEN];

    f = fopen("/proc/mounts", "r");
    if (!f) {
        f = fopen("/etc/mtab", "r");
        if (!f) {
            fprintf(stderr, "mount: cannot read mount information: %s\n",
                    strerror(errno));
            return -1;
        }
    }

    while (fgets(line, sizeof(line), f)) {
        /* Remove newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        /* Print the line as-is from /proc/mounts or /etc/mtab */
        printf("%s\n", line);
    }

    fclose(f);
    return 0;
}

static int parse_args(int argc, char *argv[]) {
    int opt;

    opts.fstype = NULL;
    opts.options = NULL;
    opts.device = NULL;
    opts.mountpoint = NULL;
    opts.read_only = 0;
    opts.read_write = 0;
    opts.verbose = 0;
    opts.fake = 0;
    opts.no_mtab = 0;
    opts.mount_all = 0;

    while ((opt = getopt(argc, argv, "t:o:rvwfnah")) != -1) {
        switch (opt) {
            case 't':
                opts.fstype = optarg;
                break;
            case 'o':
                opts.options = optarg;
                break;
            case 'r':
                opts.read_only = 1;
                break;
            case 'w':
                opts.read_write = 1;
                break;
            case 'v':
                opts.verbose = 1;
                break;
            case 'f':
                opts.fake = 1;
                break;
            case 'n':
                opts.no_mtab = 1;
                break;
            case 'a':
                opts.mount_all = 1;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    int ret;

    ret = parse_args(argc, argv);
    if (ret < 0)
        return 1;

    /* No arguments: show mounted filesystems */
    if (argc == 1)
        return show_mounts();

    /* Mount all from fstab */
    if (opts.mount_all)
        return mount_all();

    /* Need device and mountpoint for explicit mount */
    if (optind >= argc - 1) {
        if (optind >= argc) {
            /* Just show mounts if no device/mountpoint */
            return show_mounts();
        }
        fprintf(stderr, "mount: mountpoint not specified\n");
        usage(argv[0]);
        return 1;
    }

    opts.device = argv[optind];
    opts.mountpoint = argv[optind + 1];

    /* Apply -r/-w flags */
    if (opts.read_only) {
        /* Append ro to options */
        char newopts[MAX_OPTS_LEN] = "";
        if (opts.options)
            snprintf(newopts, sizeof(newopts), "%s,ro", opts.options);
        else
            strcpy(newopts, "ro");
        opts.options = newopts;
    } else if (opts.read_write) {
        char newopts[MAX_OPTS_LEN] = "";
        if (opts.options)
            snprintf(newopts, sizeof(newopts), "%s,rw", opts.options);
        else
            strcpy(newopts, "rw");
        opts.options = newopts;
    }

    return mount_entry(opts.device, opts.mountpoint, opts.fstype, opts.options);
}
