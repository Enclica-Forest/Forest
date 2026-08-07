/*
 * touch.c - Forest OS touch utility
 *
 * POSIX-compatible touch: create files or update timestamps.
 * Supports -a, -m, -c, -t STAMP, -d STRING, -r FILE.
 */

#include <forest.h>

#define PROGNAME "touch"
#define MAX_PATH 4096

static int flag_a = 0;  /* change access time only */
static int flag_m = 0;  /* change modify time only */
static int flag_c = 0;  /* do not create files */
static int flag_r = 0;  /* use reference file */
static int flag_t = 0;  /* use timestamp string */
static int flag_d = 0;  /* use date string */

static char *ref_file = NULL;
static char *stamp_str = NULL;
static char *date_str = NULL;

/* Parsed timestamp values */
static struct timespec ts_new[2];
static int have_time = 0;

static void usage(void) {
    eprint("usage: " PROGNAME " [-acmrt] [-d string] [-r file] file...\n");
    exit(EXIT_USAGE);
}

/* Parse [[CC]YY]MMDDhhmm[.ss] stamp format */
static int parse_stamp(const char *s) {
    struct tm tm;
    int len = 0;
    const char *p = s;

    while (*p) len++, p++;

    /* Need at least MMDDhhmm (8 chars) */
    if (len < 8) {
        eprint2(PROGNAME, "invalid timestamp format");
        return -1;
    }

    /* Initialize tm */
    tm.tm_sec = 0;
    tm.tm_min = 0;
    tm.tm_hour = 0;
    tm.tm_mday = 0;
    tm.tm_mon = 0;
    tm.tm_year = 0;
    tm.tm_wday = 0;
    tm.tm_yday = 0;
    tm.tm_isdst = 0;

    p = s;

    /* Check for optional century and year prefix */
    if (len >= 10) {
        /* MMDDhhmmss or CCYYMMDDhhmm or YYMMDDhhmm */
        if (len == 10) {
            /* YYMMDDhhmmss -> wait, that's 10, but .ss is separate */
            /* Actually: [[CC]YY]MMDDhhmm[.ss] */
            /* 10 chars could be MMDDhhmmSS (no year) */
            /* Let's be strict: parse from the right side for month/day/time */

            /* Check if the 9th-10th chars form valid seconds */
            /* Actually, the format is [[CC]YY]MMDDhhmm[.ss] */
            /* len=8:  MMDDhhmm */
            /* len=9:  MMDDhhmmS (invalid - needs 2 digit seconds) */
            /* len=10: could be YYMMDDhhmm or MMDDhhmmSS - ambiguous */
            /* Standard touch treats 10-digit as YYMMDDhhmm */
        }
    }

    /* We'll parse based on length:
     * 8  = MMDDhhmm
     * 10 = YYMMDDhhmm
     * 12 = CCYYMMDDhhmm
     * Plus optional .ss appended (adds 3 chars: .SS)
     */

    int dot_pos = -1;
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] == '.') {
            dot_pos = i;
            break;
        }
    }

    int date_part_len = (dot_pos >= 0) ? dot_pos : len;
    int has_seconds = 0;

    if (dot_pos >= 0) {
        /* Has .ss suffix */
        int sec_len = len - dot_pos - 1;
        if (sec_len != 2) {
            eprint2(PROGNAME, "invalid seconds in timestamp");
            return -1;
        }
        has_seconds = 1;
        tm.tm_sec = (s[dot_pos + 1] - '0') * 10 + (s[dot_pos + 2] - '0');
        if (tm.tm_sec < 0 || tm.tm_sec > 60) {
            eprint2(PROGNAME, "invalid seconds in timestamp");
            return -1;
        }
    }

    const char *d = s;

    if (date_part_len == 8) {
        /* MMDDhhmm */
        tm.tm_mon  = (d[0] - '0') * 10 + (d[1] - '0') - 1;
        tm.tm_mday = (d[2] - '0') * 10 + (d[3] - '0');
        tm.tm_hour = (d[4] - '0') * 10 + (d[5] - '0');
        tm.tm_min  = (d[6] - '0') * 10 + (d[7] - '0');
        /* Year defaults to current year - get it */
        time_t now = time(NULL);
        struct tm *cur = localtime(&now);
        if (cur) tm.tm_year = cur->tm_year;
    } else if (date_part_len == 10) {
        /* YYMMDDhhmm */
        int yy = (d[0] - '0') * 10 + (d[1] - '0');
        tm.tm_year = (yy >= 69) ? (1900 + yy) : (2000 + yy);
        tm.tm_year -= 1900;
        tm.tm_mon  = (d[2] - '0') * 10 + (d[3] - '0') - 1;
        tm.tm_mday = (d[4] - '0') * 10 + (d[5] - '0');
        tm.tm_hour = (d[6] - '0') * 10 + (d[7] - '0');
        tm.tm_min  = (d[8] - '0') * 10 + (d[9] - '0');
    } else if (date_part_len == 12) {
        /* CCYYMMDDhhmm */
        int cc = (d[0] - '0') * 10 + (d[1] - '0');
        int yy = (d[2] - '0') * 10 + (d[3] - '0');
        tm.tm_year = ((cc * 100 + yy) - 1900);
        tm.tm_mon  = (d[4] - '0') * 10 + (d[5] - '0') - 1;
        tm.tm_mday = (d[6] - '0') * 10 + (d[7] - '0');
        tm.tm_hour = (d[8] - '0') * 10 + (d[9] - '0');
        tm.tm_min  = (d[10] - '0') * 10 + (d[11] - '0');
    } else {
        eprint2(PROGNAME, "invalid timestamp format");
        return -1;
    }

    /* Validate ranges */
    if (tm.tm_mon < 0 || tm.tm_mon > 11) {
        eprint2(PROGNAME, "invalid month in timestamp");
        return -1;
    }
    if (tm.tm_mday < 1 || tm.tm_mday > 31) {
        eprint2(PROGNAME, "invalid day in timestamp");
        return -1;
    }
    if (tm.tm_hour < 0 || tm.tm_hour > 23) {
        eprint2(PROGNAME, "invalid hour in timestamp");
        return -1;
    }
    if (tm.tm_min < 0 || tm.tm_min > 59) {
        eprint2(PROGNAME, "invalid minute in timestamp");
        return -1;
    }

    /* Convert to time_t then to timespec */
    time_t t = mktime(&tm);
    if (t == (time_t)-1) {
        eprint2(PROGNAME, "invalid date/time in timestamp");
        return -1;
    }

    ts_new[0].tv_sec = t;
    ts_new[0].tv_nsec = has_seconds ? 0 : 0;
    ts_new[1].tv_sec = t;
    ts_new[1].tv_nsec = has_seconds ? 0 : 0;

    have_time = 1;
    return 0;
}

/* Parse -d STRING date format: "[[[[[[CC]YY]MM]DD] hh]mm]ss" */
static int parse_date_string(const char *s) {
    struct tm tm;
    int len = 0;
    const char *p = s;

    while (*p) len++, p++;

    /* Initialize tm */
    tm.tm_sec = 0;
    tm.tm_min = 0;
    tm.tm_hour = 0;
    tm.tm_mday = 1;
    tm.tm_mon = 0;
    tm.tm_year = 0;
    tm.tm_wday = 0;
    tm.tm_yday = 0;
    tm.tm_isdst = 0;

    /* Try parsing as timestamp format first (MMDDhhmm[.ss]) */
    if (len >= 8 && len <= 15) {
        int is_stamp = 1;
        int i;
        for (i = 0; i < len; i++) {
            if (s[i] == '.') {
                if (i != 8 && i != 10 && i != 12) {
                    is_stamp = 0;
                    break;
                }
            } else if (s[i] < '0' || s[i] > '9') {
                is_stamp = 0;
                break;
            }
        }
        if (is_stamp) {
            return parse_stamp(s);
        }
    }

    /* Try strptime with common formats */
    const char *fmts[] = {
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%d %H:%M",
        "%Y-%m-%d",
        "%m-%d %H:%M",
        "%m/%d %H:%M",
        "%m/%d %H:%M:%S",
        "%b %d %H:%M:%S %Y",
        "%b %d %H:%M %Y",
        "%b %d, %Y %H:%M:%S",
        "%b %d, %Y %H:%M",
        "%H:%M %m/%d/%Y",
        "%H:%M:%S %m/%d/%Y",
        NULL
    };

    const char **fmt;
    for (fmt = fmts; *fmt; fmt++) {
        char *end = strptime(s, *fmt, &tm);
        if (end && (*end == '\0' || *end == ' ' || *end == '\t')) {
            time_t t = mktime(&tm);
            if (t != (time_t)-1) {
                ts_new[0].tv_sec = t;
                ts_new[0].tv_nsec = 0;
                ts_new[1].tv_sec = t;
                ts_new[1].tv_nsec = 0;
                have_time = 1;
                return 0;
            }
        }
    }

    eprint2(PROGNAME, "invalid date string");
    return -1;
}

/* Get current time as timespec */
static void get_current_timespec(struct timespec *ts) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
}

/* Get timestamps from reference file */
static int get_ref_times(const char *path, struct timespec *ts) {
    struct stat st;
    if (stat(path, &st) < 0) {
        eprint2(PROGNAME, "cannot stat reference file");
        return -1;
    }
    ts[0] = st.st_atim;
    ts[1] = st.st_mtim;
    return 0;
}

/* Set timestamps on a file */
static int set_times(const char *path, struct timespec *ts) {
    struct timespec times[2];

    if (flag_a && !flag_m) {
        /* -a only: set access time, keep modify time */
        struct stat st;
        if (stat(path, &st) < 0)
            return -1;
        times[0] = ts[0];
        times[1] = st.st_mtim;
    } else if (flag_m && !flag_a) {
        /* -m only: set modify time, keep access time */
        struct stat st;
        if (stat(path, &st) < 0)
            return -1;
        times[0] = st.st_atim;
        times[1] = ts[1];
    } else {
        /* Both (default) */
        times[0] = ts[0];
        times[1] = ts[1];
    }

    return utimensat(AT_FDCWD, path, times, 0);
}

/* Process a single file argument */
static int touch_file(const char *path) {
    struct stat st;
    int exists = (stat(path, &st) == 0);

    if (exists) {
        /* File exists: update timestamps */
        struct timespec ts[2];

        if (flag_r) {
            if (get_ref_times(ref_file, ts) < 0)
                return 1;
        } else if (have_time) {
            ts[0] = ts_new[0];
            ts[1] = ts_new[1];
        } else {
            /* Current time */
            struct timespec now;
            get_current_timespec(&now);
            ts[0] = now;
            ts[1] = now;
        }

        if (set_times(path, ts) < 0) {
            eprint2(PROGNAME, "cannot set timestamps");
            return 1;
        }
    } else {
        /* File does not exist */
        if (flag_c) {
            /* -c: do not create */
            return 0;
        }

        /* Create the file */
        int fd = open(path, O_CREAT | O_WRONLY, 0666);
        if (fd < 0) {
            eprint2(PROGNAME, "cannot create file");
            return 1;
        }
        close(fd);

        /* Set timestamps on newly created file */
        struct timespec ts[2];

        if (flag_r) {
            if (get_ref_times(ref_file, ts) < 0)
                return 1;
        } else if (have_time) {
            ts[0] = ts_new[0];
            ts[1] = ts_new[1];
        } else {
            struct timespec now;
            get_current_timespec(&now);
            ts[0] = now;
            ts[1] = now;
        }

        if (set_times(path, ts) < 0) {
            eprint2(PROGNAME, "cannot set timestamps on new file");
            return 1;
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    int i;
    int ret = 0;
    int file_args_start = 1;

    /* Parse flags */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-')
            break;

        /* Handle "-" alone as stdin (POSIX) */
        if (argv[i][1] == '\0') {
            i++;
            break;
        }

        const char *opt = &argv[i][1];
        while (*opt) {
            switch (*opt) {
            case 'a':
                flag_a = 1;
                break;
            case 'm':
                flag_m = 1;
                break;
            case 'c':
                flag_c = 1;
                break;
            case 'r':
                flag_r = 1;
                i++;
                if (i >= argc) {
                    eprint2(PROGNAME, "-r requires a file argument");
                    usage();
                }
                ref_file = argv[i];
                break;
            case 't':
                flag_t = 1;
                i++;
                if (i >= argc) {
                    eprint2(PROGNAME, "-t requires a timestamp argument");
                    usage();
                }
                stamp_str = argv[i];
                break;
            case 'd':
                flag_d = 1;
                i++;
                if (i >= argc) {
                    eprint2(PROGNAME, "-d requires a date string argument");
                    usage();
                }
                date_str = argv[i];
                break;
            case '-':
                /* -- stop option parsing */
                i++;
                goto done_opts;
            default:
                eprint2(PROGNAME, "unknown option");
                usage();
            }
            opt++;
        }
    }

done_opts:
    file_args_start = i;

    /* Validate conflicting options */
    if (flag_t && flag_d) {
        eprint2(PROGNAME, "cannot specify both -t and -d");
        usage();
    }
    if (flag_r && (flag_t || flag_d)) {
        eprint2(PROGNAME, "cannot specify -r with -t or -d");
        usage();
    }

    /* Parse the time specification */
    if (flag_t) {
        if (parse_stamp(stamp_str) < 0)
            return EXIT_FAIL;
    } else if (flag_d) {
        if (parse_date_string(date_str) < 0)
            return EXIT_FAIL;
    }

    /* No files specified: nothing to do */
    if (file_args_start >= argc) {
        return EXIT_OK;
    }

    /* Process each file */
    for (i = file_args_start; i < argc; i++) {
        if (touch_file(argv[i]) != 0)
            ret = 1;
    }

    return ret;
}
