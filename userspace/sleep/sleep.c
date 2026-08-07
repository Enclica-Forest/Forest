/*
 * sleep.c - Forest OS userspace sleep
 *
 * Delay for a specified amount of time.
 * Accepts NUMBER[SUFFIX] where SUFFIX is s (seconds), m (minutes),
 * h (hours), or d (days). Supports floating point and multiple arguments.
 */

#define _DEFAULT_SOURCE

#include "forest.h"

#define VERSION "1.0.0"

static const char *progname = "sleep";
static volatile int interrupted = 0;

static void usage(void) {
    eprint2(progname, "usage: sleep NUMBER[SUFFIX]...");
    eprint2(progname, "  SUFFIX: s (seconds, default), m (minutes), h (hours), d (days)");
    eprint2(progname, "  Multiple arguments are summed.");
    exit(1);
}

static void version(void) {
    eprint(progname);
    eprint(" (Forest OS) ");
    eprint(VERSION);
    eprint("\n");
    exit(EXIT_OK);
}

static void sighandler(int sig) {
    (void)sig;
    interrupted = 1;
}

/*
 * Parse a single NUMBER[SUFFIX] argument into seconds.
 * Returns 0 on success, -1 on error.
 */
static int parse_time(const char *arg, double *result) {
    char *endptr;
    double val;

    val = strtod(arg, &endptr);
    if (endptr == arg || *endptr != '\0') {
        size_t len = strlen(arg);
        char last = arg[len - 1];

        char buf[256];
        if (len >= sizeof(buf))
            return -1;
        memcpy(buf, arg, len - 1);
        buf[len - 1] = '\0';

        val = strtod(buf, &endptr);
        if (endptr == buf || *endptr != '\0')
            return -1;

        switch (last) {
        case 's':
            break;
        case 'm':
            val *= 60.0;
            break;
        case 'h':
            val *= 3600.0;
            break;
        case 'd':
            val *= 86400.0;
            break;
        default:
            return -1;
        }
    }

    if (val < 0)
        return -1;

    *result = val;
    return 0;
}

/*
 * Sleep for the specified number of seconds using nanosleep.
 * Handles signals (EINTR) by sleeping the remaining time.
 * Returns 0 on success, 1 if interrupted.
 */
static int do_sleep(double seconds) {
    struct timespec ts;
    struct timespec rem;
    int ret;

    if (seconds <= 0.0)
        return 0;

    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);

    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec = 0;
    }

    interrupted = 0;

    ret = nanosleep(&ts, &rem);
    if (ret < 0 && errno == EINTR && !interrupted) {
        return do_sleep((double)rem.tv_sec +
                        (double)rem.tv_nsec / 1000000000.0);
    }

    return interrupted ? 1 : 0;
}

int main(int argc, char *argv[]) {
    double total = 0.0;
    double val;
    int i;

    progname = argv[0] ? argv[0] : "sleep";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0)
            usage();
        if (strcmp(argv[i], "--version") == 0)
            version();
    }

    if (argc < 2) {
        fprintf(stderr, "%s: missing operand\n", progname);
        usage();
    }

    for (i = 1; i < argc; i++) {
        if (parse_time(argv[i], &val) < 0) {
            fprintf(stderr, "%s: invalid time interval '%s'\n",
                    progname, argv[i]);
            exit(EXIT_FAIL);
        }
        total += val;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return do_sleep(total);
}
