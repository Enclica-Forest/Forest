/*
 * df.c - Report filesystem disk space usage
 *
 * Forest OS implementation of POSIX df.
 * Reads /proc/mounts for mount information and uses statfs syscall
 * to retrieve filesystem usage statistics.
 */

#include <forest.h>
#include <limits.h>

/* statfs structure for Forest OS (Linux x86_64 compatible) */
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct fsid {
    int val[2];
};

struct statfs {
    long f_type;
    long f_bsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    struct fsid f_fsid;
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

/* Filesystem magic numbers */
#define STACK_MAGIC      0x73717368
#define PROC_MAGIC       0x9fa0
#define SYSFS_MAGIC      0x62656572
#define TMPFS_MAGIC      0x01021994
#define DEVPTS_MAGIC     0x1cd1
#define MOUNTED_MAGIC    0x6d446163
#define NONE_MAGIC       0

/* Maximum number of mount entries */
#define MAX_MOUNTS 256

/* Mount entry from /proc/mounts */
typedef struct {
    char device[256];
    char mountpoint[256];
    char fstype[32];
    char options[256];
    int dump;
    int pass;
} MountEntry;

/* Options structure */
typedef struct {
    int human_readable;      /* -h: human readable (1K blocks) */
    int si_units;            /* -H: SI units (powers of 1000) */
    int kilobytes;           /* -k: 1K blocks */
    int megabytes;           /* -m: 1M blocks */
    int show_type;           /* -T: print filesystem type */
    int posix_format;        /* -P: POSIX output */
    int show_all;            /* -a: all filesystems */
    int show_total;          /* --total: display total line */
    char include_type[32];   /* -t TYPE: include only this type */
    char exclude_type[32];   /* -x TYPE: exclude this type */
} Options;

static Options opts;

/* Get statfs information via syscall */
static int do_statfs(const char *path, struct statfs *buf) {
    return syscall(SYS_statfs, path, buf);
}

/* Read /proc/mounts and return mount entries */
static int read_mounts(MountEntry *mounts, int max_entries) {
    FILE *f;
    int count = 0;
    char line[1024];

    f = fopen("/proc/mounts", "r");
    if (!f) {
        /* Try /etc/mtab as fallback */
        f = fopen("/etc/mtab", "r");
        if (!f) {
            fprintf(stderr, "df: cannot open /proc/mounts: %s\n", strerror(errno));
            return 0;
        }
    }

    while (fgets(line, sizeof(line), f) && count < max_entries) {
        MountEntry *m = &mounts[count];
        /* Format: device mountpoint fstype options dump pass */
        if (sscanf(line, "%255s %255s %31s %255s %d %d",
                   m->device, m->mountpoint, m->fstype, m->options,
                   &m->dump, &m->pass) >= 4) {
            count++;
        }
    }

    fclose(f);
    return count;
}

/* Check if filesystem type matches include/exclude filters */
static int type_matches(const char *fstype) {
    if (opts.include_type[0] && strcmp(fstype, opts.include_type) != 0)
        return 0;
    if (opts.exclude_type[0] && strcmp(fstype, opts.exclude_type) == 0)
        return 0;
    return 1;
}

/* Check if filesystem should be shown */
static int should_show(const MountEntry *m) {
    /* Skip pseudo-filesystems unless -a is specified */
    if (!opts.show_all) {
        if (strcmp(m->fstype, "proc") == 0 ||
            strcmp(m->fstype, "sysfs") == 0 ||
            strcmp(m->fstype, "devtmpfs") == 0 ||
            strcmp(m->fstype, "devpts") == 0 ||
            strcmp(m->fstype, "tmpfs") == 0 ||
            strcmp(m->fstype, "securityfs") == 0 ||
            strcmp(m->fstype, "cgroup") == 0 ||
            strcmp(m->fstype, "cgroup2") == 0 ||
            strcmp(m->fstype, "pstore") == 0 ||
            strcmp(m->fstype, "debugfs") == 0 ||
            strcmp(m->fstype, "hugetlbfs") == 0 ||
            strcmp(m->fstype, "mqueue") == 0 ||
            strcmp(m->fstype, "fusectl") == 0 ||
            strcmp(m->fstype, "configfs") == 0 ||
            strcmp(m->fstype, "binfmt_misc") == 0 ||
            strcmp(m->fstype, "autofs") == 0 ||
            strcmp(m->fstype, "tracefs") == 0 ||
            strcmp(m->fstype, "bpf") == 0 ||
            strcmp(m->fstype, "overlay") == 0 ||
            strcmp(m->fstype, "nsfs") == 0 ||
            strcmp(m->fstype, "rpc_pipefs") == 0 ||
            strcmp(m->fstype, "nfsd") == 0 ||
            strcmp(m->fstype, "sunrpc") == 0) {
            return 0;
        }
    }

    /* Apply type filters */
    if (!type_matches(m->fstype))
        return 0;

    /* Skip device nodes and special entries */
    if (strncmp(m->device, "/dev/", 5) != 0 &&
        strcmp(m->fstype, "proc") != 0 &&
        strcmp(m->fstype, "sysfs") != 0 &&
        strcmp(m->fstype, "devtmpfs") != 0 &&
        strcmp(m->fstype, "devpts") != 0) {
        /* For non-device mounts, check if it has actual blocks */
        struct statfs st;
        if (do_statfs(m->mountpoint, &st) == 0) {
            if (st.f_blocks == 0 && !opts.show_all)
                return 0;
        }
    }

    return 1;
}

/* Format a size value with appropriate units */
static void format_size(long long blocks, long block_size, char *buf, size_t buflen) {
    double size;
    const char *unit;

    if (opts.human_readable || opts.si_units) {
        /* Human-readable format */
        const char *units[] = {"B", "K", "M", "G", "T", "P", "E"};
        int base = opts.si_units ? 1000 : 1024;
        size = (double)blocks * block_size;
        int i = 0;
        while (size >= base && i < 6) {
            size /= base;
            i++;
        }
        unit = units[i];
        if (i == 0)
            snprintf(buf, buflen, "%lld", (long long)size);
        else if (size >= 10)
            snprintf(buf, buflen, "%.0f%s", size, unit);
        else
            snprintf(buf, buflen, "%.1f%s", size, unit);
    } else if (opts.megabytes) {
        /* 1M blocks */
        long long mb = (blocks * block_size) / (1024 * 1024);
        snprintf(buf, buflen, "%lld", mb);
    } else if (opts.kilobytes) {
        /* 1K blocks */
        long long kb = (blocks * block_size) / 1024;
        snprintf(buf, buflen, "%lld", kb);
    } else {
        /* Default: 1K blocks */
        long long kb = (blocks * block_size) / 1024;
        snprintf(buf, buflen, "%lld", kb);
    }
}

/* Calculate use percentage */
static int calc_percent(fsblkcnt_t used, fsblkcnt_t total) {
    if (total == 0) return 0;
    return (int)((used * 100ULL) / total);
}

/* Print a single filesystem entry */
static void print_fs(const MountEntry *m, struct statfs *st) {
    char size_buf[64];
    char used_buf[64];
    char avail_buf[64];
    long block_size = st->f_bsize ? st->f_bsize : 1024;
    fsblkcnt_t total = st->f_blocks;
    fsblkcnt_t free_blocks = st->f_bfree;
    fsblkcnt_t avail = st->f_bavail;
    fsblkcnt_t used = total - free_blocks;
    int percent = calc_percent(used, total);

    format_size(total, block_size, size_buf, sizeof(size_buf));
    format_size(used, block_size, used_buf, sizeof(used_buf));
    format_size(avail, block_size, avail_buf, sizeof(avail_buf));

    if (opts.posix_format) {
        /* POSIX format: fixed-width columns */
        if (opts.show_type)
            printf("%-30s %-10s %-10s %-10s %4d%% %s\n",
                   m->device, size_buf, used_buf, avail_buf, percent, m->mountpoint);
        else
            printf("%-30s %-10s %-10s %-10s %4d%% %s\n",
                   m->device, size_buf, used_buf, avail_buf, percent, m->mountpoint);
    } else {
        /* Default format */
        if (opts.show_type)
            printf("%-20s %-6s %-10s %-10s %-10s %4d%% %s\n",
                   m->device, m->fstype, size_buf, used_buf, avail_buf, percent, m->mountpoint);
        else
            printf("%-20s %-10s %-10s %-10s %4d%% %s\n",
                   m->device, size_buf, used_buf, avail_buf, percent, m->mountpoint);
    }
}

/* Print header line */
static void print_header(void) {
    if (opts.posix_format) {
        printf("Filesystem        %s     %s     %s     %s Mounted on\n",
               opts.human_readable || opts.si_units ? "Size" : opts.megabytes ? "1M-blocks" : "1024-blocks",
               opts.human_readable || opts.si_units ? "Used" : opts.megabytes ? "Used" : "Used",
               opts.human_readable || opts.si_units ? "Avail" : opts.megabytes ? "Avail" : "Available",
               "Use%");
    } else {
        if (opts.show_type)
            printf("Filesystem        Type     %s     %s     %s     Use%% Mounted on\n",
                   opts.human_readable || opts.si_units ? "Size" : opts.megabytes ? "1M-blocks" : "1024-blocks",
                   opts.human_readable || opts.si_units ? "Used" : opts.megabytes ? "Used" : "Used",
                   opts.human_readable || opts.si_units ? "Avail" : opts.megabytes ? "Avail" : "Available");
        else
            printf("Filesystem        %s     %s     %s     Use%% Mounted on\n",
                   opts.human_readable || opts.si_units ? "Size" : opts.megabytes ? "1M-blocks" : "1024-blocks",
                   opts.human_readable || opts.si_units ? "Used" : opts.megabytes ? "Used" : "Used",
                   opts.human_readable || opts.si_units ? "Avail" : opts.megabytes ? "Avail" : "Available");
    }
}

/* Print total line */
static void print_total(fsblkcnt_t total_blocks, fsblkcnt_t total_free, fsblkcnt_t total_avail, long block_size) {
    char total_buf[64], used_buf[64], avail_buf[64];
    fsblkcnt_t total_used = total_blocks - total_free;
    int percent = calc_percent(total_used, total_blocks);

    format_size(total_blocks, block_size, total_buf, sizeof(total_buf));
    format_size(total_used, block_size, used_buf, sizeof(used_buf));
    format_size(total_avail, block_size, avail_buf, sizeof(avail_buf));

    printf("%-20s %-10s %-10s %-10s %4d%%\n",
           "total", total_buf, used_buf, avail_buf, percent);
}

/* Parse long option */
static int parse_long_option(const char *arg) {
    if (strcmp(arg, "total") == 0) {
        opts.show_total = 1;
        return 1;
    }
    fprintf(stderr, "df: unrecognized option '--%s'\n", arg);
    return 0;
}

/* Print usage information */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-hHkmaTP] [-t type] [-x type] [--total] [file ...]\n", prog);
    fprintf(stderr, "  -h          human readable (1K blocks)\n");
    fprintf(stderr, "  -H          SI units (powers of 1000)\n");
    fprintf(stderr, "  -k          1K blocks (default)\n");
    fprintf(stderr, "  -m          1M blocks\n");
    fprintf(stderr, "  -T          print filesystem type\n");
    fprintf(stderr, "  -t TYPE     include only filesystems of type TYPE\n");
    fprintf(stderr, "  -x TYPE     exclude filesystems of type TYPE\n");
    fprintf(stderr, "  -a          include pseudo-filesystems\n");
    fprintf(stderr, "  -P          POSIX output format\n");
    fprintf(stderr, "  --total     display a total line\n");
}

int main(int argc, char *argv[]) {
    MountEntry mounts[MAX_MOUNTS];
    int mount_count;
    int i;
    fsblkcnt_t grand_total = 0, grand_free = 0, grand_avail = 0;
    long block_size = 1024;
    int show_header = 1;
    int error = 0;

    /* Initialize options */
    memset(&opts, 0, sizeof(opts));

    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            break; /* End of options */
        }

        if (argv[i][1] == '-') {
            /* Long option */
            if (parse_long_option(argv[i] + 2) == 0) {
                usage(argv[0]);
                return 1;
            }
            continue;
        }

        /* Short options */
        const char *p = argv[i] + 1;
        while (*p) {
            switch (*p) {
                case 'h':
                    opts.human_readable = 1;
                    opts.si_units = 0;
                    break;
                case 'H':
                    opts.si_units = 1;
                    opts.human_readable = 0;
                    break;
                case 'k':
                    opts.kilobytes = 1;
                    opts.megabytes = 0;
                    opts.human_readable = 0;
                    opts.si_units = 0;
                    break;
                case 'm':
                    opts.megabytes = 1;
                    opts.kilobytes = 0;
                    opts.human_readable = 0;
                    opts.si_units = 0;
                    break;
                case 'T':
                    opts.show_type = 1;
                    break;
                case 't':
                    if (p[1] == '\0') {
                        /* Next argument is the type */
                        if (i + 1 < argc) {
                            strncpy(opts.include_type, argv[++i], sizeof(opts.include_type) - 1);
                        } else {
                            fprintf(stderr, "df: option requires an argument -- 't'\n");
                            usage(argv[0]);
                            return 1;
                        }
                    } else {
                        strncpy(opts.include_type, p + 1, sizeof(opts.include_type) - 1);
                    }
                    goto next_arg;
                case 'x':
                    if (p[1] == '\0') {
                        /* Next argument is the type */
                        if (i + 1 < argc) {
                            strncpy(opts.exclude_type, argv[++i], sizeof(opts.exclude_type) - 1);
                        } else {
                            fprintf(stderr, "df: option requires an argument -- 'x'\n");
                            usage(argv[0]);
                            return 1;
                        }
                    } else {
                        strncpy(opts.exclude_type, p + 1, sizeof(opts.exclude_type) - 1);
                    }
                    goto next_arg;
                case 'a':
                    opts.show_all = 1;
                    break;
                case 'P':
                    opts.posix_format = 1;
                    break;
                case '-':
                    if (p[1] == '\0') {
                        i++;
                        goto parse_done;
                    }
                    if (parse_long_option(p + 1) == 0) {
                        usage(argv[0]);
                        return 1;
                    }
                    goto next_arg;
                default:
                    fprintf(stderr, "df: invalid option -- '%c'\n", *p);
                    usage(argv[0]);
                    return 1;
            }
            p++;
        }
        next_arg:;
    }
    parse_done:

    /* Read mount information */
    mount_count = read_mounts(mounts, MAX_MOUNTS);
    if (mount_count == 0) {
        fprintf(stderr, "df: no mounted filesystems found\n");
        return 1;
    }

    /* Print header */
    if (show_header) {
        print_header();
    }

    /* Process each mount */
    for (i = 0; i < mount_count; i++) {
        MountEntry *m = &mounts[i];
        struct statfs st;

        if (!should_show(m))
            continue;

        if (do_statfs(m->mountpoint, &st) < 0) {
            /* Cannot stat this filesystem, skip it */
            continue;
        }

        /* Get block size for total calculation */
        if (st.f_bsize > 0)
            block_size = st.f_bsize;

        /* Accumulate totals */
        grand_total += st.f_blocks;
        grand_free += st.f_bfree;
        grand_avail += st.f_bavail;

        /* Print filesystem info */
        print_fs(m, &st);
    }

    /* Print total if requested and if multiple filesystems shown */
    if (opts.show_total && mount_count > 1) {
        print_total(grand_total, grand_free, grand_avail, block_size);
    }

    return error;
}
