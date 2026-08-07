#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <getopt.h>
#include <signal.h>

#define DEFAULT_BS 512
#define MAX_BS (1 << 24)

static volatile int interrupted = 0;

static void handle_signal(int sig) {
    (void)sig;
    interrupted = 1;
}

struct conv_flags {
    unsigned noerror : 1;
    unsigned sync : 1;
    unsigned notrunc : 1;
    unsigned excl : 1;
    unsigned fdatasync : 1;
    unsigned fsync : 1;
};

struct flags {
    unsigned append : 1;
    unsigned direct : 1;
    unsigned dsync : 1;
    unsigned sync_meta : 1;
    unsigned nonblock : 1;
};

struct config {
    const char *if_name;
    const char *of_name;
    size_t bs;
    size_t ibs;
    size_t obs;
    size_t cbs;
    long long count;
    long long skip;
    long long seek;
    struct conv_flags conv;
    struct flags iflag;
    struct flags oflag;
    int partial;
};

static size_t parse_size(const char *s) {
    char *end;
    unsigned long long val = strtoull(s, &end, 10);
    if (end == s) return 0;
    switch (*end) {
    case 'k': case 'K': val *= 1024; break;
    case 'm': case 'M': val *= 1024 * 1024; break;
    case 'g': case 'G': val *= 1024ULL * 1024 * 1024; break;
    case 'c': case 'C': break;
    case 'w': case 'W': val *= 2; break;
    case 'b': case 'B': val *= 512; break;
    default: break;
    }
    return (size_t)val;
}

static long long parse_number(const char *s) {
    return strtoll(s, NULL, 10);
}

static int parse_conv(const char *arg, struct conv_flags *conv) {
    char *copy = strdup(arg);
    if (!copy) return -1;
    char *tok = strtok(copy, ",");
    while (tok) {
        if (strcmp(tok, "noerror") == 0) conv->noerror = 1;
        else if (strcmp(tok, "sync") == 0) conv->sync = 1;
        else if (strcmp(tok, "notrunc") == 0) conv->notrunc = 1;
        else if (strcmp(tok, "excl") == 0) conv->excl = 1;
        else if (strcmp(tok, "fdatasync") == 0) conv->fdatasync = 1;
        else if (strcmp(tok, "fsync") == 0) conv->fsync = 1;
        else {
            fprintf(stderr, "dd: invalid conv '%s'\n", tok);
            free(copy);
            return -1;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return 0;
}

static int parse_flags(const char *arg, struct flags *fl) {
    char *copy = strdup(arg);
    if (!copy) return -1;
    char *tok = strtok(copy, ",");
    while (tok) {
        if (strcmp(tok, "append") == 0) fl->append = 1;
        else if (strcmp(tok, "direct") == 0) fl->direct = 1;
        else if (strcmp(tok, "dsync") == 0) fl->dsync = 1;
        else if (strcmp(tok, "sync") == 0) fl->sync_meta = 1;
        else if (strcmp(tok, "nonblock") == 0) fl->nonblock = 1;
        else {
            fprintf(stderr, "dd: invalid flag '%s'\n", tok);
            free(copy);
            return -1;
        }
        tok = strtok(NULL, ",");
    }
    free(copy);
    return 0;
}

static int open_input(const struct config *cfg) {
    int flags = O_RDONLY;
    if (cfg->iflag.direct) flags |= O_DIRECT;
    if (cfg->iflag.dsync) flags |= O_DSYNC;
    if (cfg->iflag.sync_meta) flags |= O_SYNC;
    if (cfg->iflag.nonblock) flags |= O_NONBLOCK;

    int fd;
    if (!cfg->if_name || strcmp(cfg->if_name, "-") == 0)
        fd = dup(STDIN_FILENO);
    else
        fd = open(cfg->if_name, flags);

    if (fd < 0) {
        fprintf(stderr, "dd: '%s': %s\n", cfg->if_name ? cfg->if_name : "stdin",
                strerror(errno));
        return -1;
    }
    return fd;
}

static int open_output(const struct config *cfg) {
    int flags = O_WRONLY | O_CREAT;
    if (cfg->oflag.append) flags |= O_APPEND;
    else if (cfg->seek >= 0) flags |= O_TRUNC;

    if (cfg->conv.excl) flags |= O_EXCL;
    if (cfg->oflag.direct) flags |= O_DIRECT;
    if (cfg->oflag.dsync) flags |= O_DSYNC;
    if (cfg->oflag.sync_meta) flags |= O_SYNC;
    if (cfg->oflag.nonblock) flags |= O_NONBLOCK;

    int fd;
    if (!cfg->of_name || strcmp(cfg->of_name, "-") == 0)
        fd = dup(STDOUT_FILENO);
    else
        fd = open(cfg->of_name, flags, 0666);

    if (fd < 0) {
        fprintf(stderr, "dd: '%s': %s\n", cfg->of_name ? cfg->of_name : "stdout",
                strerror(errno));
        return -1;
    }
    return fd;
}

static ssize_t xread(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t xwrite(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *p = buf;
    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static double elapsed_sec(struct timespec *start, struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

static void print_stats(long long in_blocks, long long out_blocks,
                        long long bytes, struct timespec *start,
                        struct timespec *end) {
    double secs = elapsed_sec(start, end);
    fprintf(stderr, "%lld+%lld records in\n", in_blocks, in_blocks);
    fprintf(stderr, "%lld+%lld records out\n", out_blocks, out_blocks);
    fprintf(stderr, "%lld bytes", bytes);
    if (secs > 0.0)
        fprintf(stderr, " (%.2f MB/s)", bytes / secs / (1024.0 * 1024.0));
    fprintf(stderr, " copied, %.3f s", secs);
    if (secs > 0.0)
        fprintf(stderr, ", %.2f MB/s", bytes / secs / (1024.0 * 1024.0));
    fprintf(stderr, "\n");
}

static void usage(void) {
    fprintf(stderr,
        "Usage: dd [OPERAND]...\n"
        "  bs=BYTES        read/write up to BYTES bytes at a time\n"
        "  ibs=BYTES       read up to BYTES bytes at a time (default 512)\n"
        "  obs=BYTES       write BYTES bytes at a time (default 512)\n"
        "  cbs=BYTES       convert BYTES bytes at a time\n"
        "  count=N         copy only N input blocks\n"
        "  skip=N          skip N ibs-sized blocks at start of input\n"
        "  seek=N          skip N obs-sized blocks at start of output\n"
        "  if=FILE         read from FILE (default stdin)\n"
        "  of=FILE         write to FILE (default stdout)\n"
        "  conv=CONV       convert the file as per CONV comma list\n"
        "                    noerror  continue after read errors\n"
        "                    sync     pad blocks with NULs\n"
        "                    notrunc  don't truncate output file\n"
        "                    excl     fail if output exists\n"
        "                    fdatasync physically write output before finishing\n"
        "                    fsync    physically write output before finishing\n"
        "  iflag=FLAGS     read as per FLAGS comma list\n"
        "  oflag=FLAGS     write as per FLAGS comma list\n"
        "                    append   append to output\n"
        "                    direct   use direct I/O\n"
        "                    dsync    use synchronous data writes\n"
        "                    sync     use synchronous data+meta writes\n"
        "                    nonblock use non-blocking I/O\n");
}

int main(int argc, char *argv[]) {
    struct config cfg = {
        .bs = DEFAULT_BS,
        .ibs = 0,
        .obs = 0,
        .count = -1,
        .skip = 0,
        .seek = 0,
    };

    static struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h': usage(); return 0;
        default: usage(); return 1;
        }
    }

    for (int i = optind; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            fprintf(stderr, "dd: invalid operand '%s'\n", argv[i]);
            return 1;
        }
        size_t klen = (size_t)(eq - argv[i]);
        const char *val = eq + 1;

        if (klen == 2 && memcmp(argv[i], "if", 2) == 0) cfg.if_name = val;
        else if (klen == 2 && memcmp(argv[i], "of", 2) == 0) cfg.of_name = val;
        else if (klen == 2 && memcmp(argv[i], "bs", 2) == 0) cfg.bs = parse_size(val);
        else if (klen == 3 && memcmp(argv[i], "ibs", 3) == 0) cfg.ibs = parse_size(val);
        else if (klen == 3 && memcmp(argv[i], "obs", 3) == 0) cfg.obs = parse_size(val);
        else if (klen == 3 && memcmp(argv[i], "cbs", 3) == 0) cfg.cbs = parse_size(val);
        else if (klen == 5 && memcmp(argv[i], "count", 5) == 0) cfg.count = parse_number(val);
        else if (klen == 4 && memcmp(argv[i], "skip", 4) == 0) cfg.skip = parse_number(val);
        else if (klen == 4 && memcmp(argv[i], "seek", 4) == 0) cfg.seek = parse_number(val);
        else if (klen == 4 && memcmp(argv[i], "conv", 4) == 0) {
            if (parse_conv(val, &cfg.conv) != 0) return 1;
        }
        else if (klen == 6 && memcmp(argv[i], "iflag", 5) == 0) {
            if (parse_flags(val, &cfg.iflag) != 0) return 1;
        }
        else if (klen == 6 && memcmp(argv[i], "oflag", 5) == 0) {
            if (parse_flags(val, &cfg.oflag) != 0) return 1;
        }
        else {
            fprintf(stderr, "dd: unknown operand '%s'\n", argv[i]);
            return 1;
        }
    }

    if (cfg.ibs == 0) cfg.ibs = cfg.bs;
    if (cfg.obs == 0) cfg.obs = cfg.bs;
    if (cfg.cbs == 0) cfg.cbs = cfg.ibs;

    if (cfg.ibs > MAX_BS || cfg.obs > MAX_BS) {
        fprintf(stderr, "dd: block size too large\n");
        return 1;
    }

    int ifd = open_input(&cfg);
    if (ifd < 0) return 1;

    int ofd = open_output(&cfg);
    if (ofd < 0) { close(ifd); return 1; }

    if (cfg.seek > 0 && lseek(ofd, (off_t)(cfg.seek * cfg.obs), SEEK_CUR) < 0) {
        if (cfg.conv.notrunc == 0) {
            fprintf(stderr, "dd: seek: %s\n", strerror(errno));
            close(ifd); close(ofd); return 1;
        }
    }

    if (cfg.skip > 0) {
        if (lseek(ifd, (off_t)(cfg.skip * cfg.ibs), SEEK_CUR) < 0) {
            long long s = cfg.skip;
            void *skipbuf = malloc(cfg.ibs);
            if (!skipbuf) {
                fprintf(stderr, "dd: out of memory\n");
                close(ifd); close(ofd); return 1;
            }
            while (s > 0) {
                ssize_t n = read(ifd, skipbuf, cfg.ibs);
                if (n <= 0) break;
                s--;
            }
            free(skipbuf);
        }
    }

    signal(SIGINT, handle_signal);

    void *buf = malloc(cfg.ibs > cfg.obs ? cfg.ibs : cfg.obs);
    if (!buf) {
        fprintf(stderr, "dd: out of memory\n");
        close(ifd); close(ofd); return 1;
    }

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    long long in_blocks = 0;
    long long out_blocks = 0;
    long long bytes = 0;

    for (;;) {
        if (interrupted) break;
        if (cfg.count >= 0 && in_blocks >= cfg.count) break;

        ssize_t n = xread(ifd, buf, cfg.ibs);
        if (n < 0) {
            if (cfg.conv.noerror) {
                fprintf(stderr, "dd: input read error: %s\n", strerror(errno));
                if (cfg.conv.sync)
                    memset(buf, 0, cfg.ibs);
                else
                    n = 0;
            } else {
                fprintf(stderr, "dd: input read error: %s\n", strerror(errno));
                break;
            }
        }
        if (n == 0) break;
        in_blocks++;

        ssize_t w;
        if (cfg.obs == cfg.ibs) {
            w = xwrite(ofd, buf, (size_t)n);
        } else {
            w = xwrite(ofd, buf, cfg.obs);
        }

        if (w < 0) {
            fprintf(stderr, "dd: output write error: %s\n", strerror(errno));
            break;
        }
        out_blocks++;
        bytes += w;
    }

    if (cfg.conv.fsync || cfg.conv.fdatasync) {
        if (cfg.conv.fdatasync)
            fdatasync(ofd);
        else
            fsync(ofd);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    free(buf);
    close(ifd);
    close(ofd);

    print_stats(in_blocks, out_blocks, bytes, &t_start, &t_end);

    return interrupted ? 4 : 0;
}
