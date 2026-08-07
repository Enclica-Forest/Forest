/*
 * date.c - Forest OS userspace date implementation
 * Print or set the system date and time.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#ifndef EXIT_USAGE
#define EXIT_USAGE 1
#endif
#ifndef EXIT_FAIL
#define EXIT_FAIL 2
#endif
#ifndef EXIT_OK
#define EXIT_OK 0
#endif

static const char *progname;
static int opt_utc = 0;

static void usage(void) {
    fprintf(stderr, "Usage: %s [OPTION]... [+FORMAT]\n", progname);
    fprintf(stderr, "  -s STRING  set time described by STRING\n");
    fprintf(stderr, "  -d STRING  display time described by STRING\n");
    fprintf(stderr, "  -f FILE    read time from FILE\n");
    fprintf(stderr, "  -R         output RFC 2822 date\n");
    fprintf(stderr, "  --iso-8601[=FMT]  output ISO 8601 format\n");
    fprintf(stderr, "  -u         print UTC/GMT time\n");
    fprintf(stderr, "  +FORMAT    output using strftime FORMAT\n");
    exit(EXIT_USAGE);
}

static void print_date(const char *format) {
    time_t now;
    struct tm *tm;

    time(&now);
    if (now == (time_t)-1) {
        fprintf(stderr, "%s: cannot get current time: %s\n",
                progname, strerror(errno));
        exit(EXIT_FAIL);
    }

    if (opt_utc)
        tm = gmtime(&now);
    else
        tm = localtime(&now);

    if (!tm) {
        fprintf(stderr, "%s: cannot convert time\n", progname);
        exit(EXIT_FAIL);
    }

    char buf[1024];
    if (strftime(buf, sizeof(buf), format, tm) == 0) {
        fprintf(stderr, "%s: format string too long or error\n", progname);
        exit(EXIT_FAIL);
    }
    printf("%s\n", buf);
}

static void print_rfc2822(void) {
    time_t now;
    struct tm *tm;

    time(&now);
    if (now == (time_t)-1) {
        fprintf(stderr, "%s: cannot get current time: %s\n",
                progname, strerror(errno));
        exit(EXIT_FAIL);
    }

    if (opt_utc)
        tm = gmtime(&now);
    else
        tm = localtime(&now);

    if (!tm) {
        fprintf(stderr, "%s: cannot convert time\n", progname);
        exit(EXIT_FAIL);
    }

    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};

#ifdef __tm_gmtoff
    if (!opt_utc && tm->__tm_gmtoff != 0) {
        int abs_off = tm->__tm_gmtoff;
        if (abs_off < 0) abs_off = -abs_off;
        printf("%s, %02d %s %04d %02d:%02d:%02d %c%02d%02d\n",
               days[tm->tm_wday], tm->tm_mday, months[tm->tm_mon],
               tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec,
               tm->__tm_gmtoff >= 0 ? '+' : '-',
               abs_off / 3600, (abs_off % 3600) / 60);
    } else
#endif
    {
        printf("%s, %02d %s %04d %02d:%02d:%02d GMT\n",
               days[tm->tm_wday], tm->tm_mday, months[tm->tm_mon],
               tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
}

static void print_iso8601(const char *fmt) {
    time_t now;
    struct tm *tm;

    time(&now);
    if (now == (time_t)-1) {
        fprintf(stderr, "%s: cannot get current time: %s\n",
                progname, strerror(errno));
        exit(EXIT_FAIL);
    }

    if (opt_utc)
        tm = gmtime(&now);
    else
        tm = localtime(&now);

    if (!tm) {
        fprintf(stderr, "%s: cannot convert time\n", progname);
        exit(EXIT_FAIL);
    }

    if (fmt == NULL || strcmp(fmt, "basic") == 0) {
        /* Basic: 20260807T123456Z or with timezone */
        if (opt_utc) {
            printf("%04d%02d%02dT%02d%02d%02dZ\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {
#ifdef __tm_gmtoff
            int offset = tm->__tm_gmtoff;
            if (offset < 0) offset = -offset;
            printf("%04d%02d%02dT%02d%02d%02d%c%02d%02d\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec,
                   tm->__tm_gmtoff >= 0 ? '+' : '-',
                   offset / 3600, (offset % 3600) / 60);
#else
            printf("%04d%02d%02dT%02d%02d%02dZ\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
        }
    } else if (strcmp(fmt, "seconds") == 0) {
        /* Seconds: 2026-08-07T12:34:56Z */
        if (opt_utc) {
            printf("%04d-%02d-%02dT%02d:%02d:%02dZ\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {
#ifdef __tm_gmtoff
            int offset = tm->__tm_gmtoff;
            if (offset < 0) offset = -offset;
            printf("%04d-%02d-%02dT%02d:%02d:%02d%c%02d%02d\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec,
                   tm->__tm_gmtoff >= 0 ? '+' : '-',
                   offset / 3600, (offset % 3600) / 60);
#else
            printf("%04d-%02d-%02dT%02d:%02d:%02dZ\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
        }
    } else if (strcmp(fmt, "minutes") == 0) {
        /* Minutes: 2026-08-07T12:34Z */
        if (opt_utc) {
            printf("%04d-%02d-%02dT%02d:%02dZ\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min);
        } else {
#ifdef __tm_gmtoff
            int offset = tm->__tm_gmtoff;
            if (offset < 0) offset = -offset;
            printf("%04d-%02d-%02dT%02d:%02d%c%02d%02d\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min,
                   tm->__tm_gmtoff >= 0 ? '+' : '-',
                   offset / 3600, (offset % 3600) / 60);
#else
            printf("%04d-%02d-%02dT%02d:%02dZ\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min);
#endif
        }
    } else if (strcmp(fmt, "date") == 0) {
        /* Date only: 2026-08-07 */
        printf("%04d-%02d-%02d\n",
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    } else {
        fprintf(stderr, "%s: unknown ISO 8601 format '%s'\n", progname, fmt);
        exit(EXIT_USAGE);
    }
}

static void set_time(const char *str) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    /* Try multiple parse formats */
    if (strptime(str, "%Y-%m-%d %H:%M:%S", &tm) != NULL ||
        strptime(str, "%Y-%m-%dT%H:%M:%S", &tm) != NULL ||
        strptime(str, "%Y-%m-%d", &tm) != NULL ||
        strptime(str, "%H:%M:%S", &tm) != NULL) {

        time_t t = mktime(&tm);
        if (t == (time_t)-1) {
            fprintf(stderr, "%s: cannot set date '%s': %s\n",
                    progname, str, strerror(errno));
            exit(EXIT_FAIL);
        }

        struct timeval tv;
        tv.tv_sec = t;
        tv.tv_usec = 0;

        if (settimeofday(&tv, NULL) != 0) {
            fprintf(stderr, "%s: cannot set date (are you root?): %s\n",
                    progname, strerror(errno));
            exit(EXIT_FAIL);
        }
    } else {
        fprintf(stderr, "%s: cannot parse date string '%s'\n",
                progname, str);
        exit(EXIT_FAIL);
    }
}

static void display_string(const char *str) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    if (strptime(str, "%Y-%m-%d %H:%M:%S", &tm) != NULL ||
        strptime(str, "%Y-%m-%dT%H:%M:%S", &tm) != NULL ||
        strptime(str, "%Y-%m-%d", &tm) != NULL) {

        time_t t = mktime(&tm);
        if (t == (time_t)-1) {
            fprintf(stderr, "%s: cannot convert time '%s'\n", progname, str);
            exit(EXIT_FAIL);
        }

        struct tm *out;
        if (opt_utc)
            out = gmtime(&t);
        else
            out = localtime(&t);

        if (!out) {
            fprintf(stderr, "%s: cannot convert time\n", progname);
            exit(EXIT_FAIL);
        }

        char buf[1024];
        if (strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Z %Y", out) != 0)
            printf("%s\n", buf);
    } else {
        fprintf(stderr, "%s: cannot parse date string '%s'\n",
                progname, str);
        exit(EXIT_FAIL);
    }
}

static void read_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "%s: cannot open '%s': %s\n",
                progname, filename, strerror(errno));
        exit(EXIT_FAIL);
    }

    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
        set_time(buf);
        printf("%s\n", buf);
    } else {
        fprintf(stderr, "%s: cannot read '%s'\n", progname, filename);
    }

    fclose(fp);
}

int main(int argc, char *argv[]) {
    const char *set_string = NULL;
    const char *display_string_arg = NULL;
    const char *file_arg = NULL;
    int do_rfc2822 = 0;
    int do_iso8601 = 0;
    const char *iso8601_fmt = NULL;
    int have_format = 0;
    const char *user_format = NULL;

    progname = argv[0];
    if (strncmp(progname, "./", 2) == 0)
        progname += 2;

    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') {
            user_format = argv[i] + 1;
            have_format = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option -s requires an argument\n", progname);
                exit(EXIT_USAGE);
            }
            set_string = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option -d requires an argument\n", progname);
                exit(EXIT_USAGE);
            }
            display_string_arg = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: option -f requires an argument\n", progname);
                exit(EXIT_USAGE);
            }
            file_arg = argv[++i];
        } else if (strcmp(argv[i], "-R") == 0) {
            do_rfc2822 = 1;
        } else if (strcmp(argv[i], "-u") == 0) {
            opt_utc = 1;
        } else if (strncmp(argv[i], "--iso-8601", 10) == 0) {
            do_iso8601 = 1;
            if (argv[i][10] == '=') {
                iso8601_fmt = argv[i] + 11;
            } else if (argv[i][10] != '\0') {
                fprintf(stderr, "%s: unknown option '%s'\n", progname, argv[i]);
                exit(EXIT_USAGE);
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s (Forest OS) 0.1.0\n", progname);
            exit(EXIT_OK);
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            if (i < argc && argv[i][0] == '+') {
                user_format = argv[i] + 1;
                have_format = 1;
            }
            break;
        } else if (argv[i][0] == '-') {
            /* Parse short options */
            for (const char *p = argv[i] + 1; *p; p++) {
                switch (*p) {
                case 's':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "%s: option -s requires an argument\n", progname);
                        exit(EXIT_USAGE);
                    }
                    set_string = argv[++i];
                    break;
                case 'd':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "%s: option -d requires an argument\n", progname);
                        exit(EXIT_USAGE);
                    }
                    display_string_arg = argv[++i];
                    break;
                case 'f':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "%s: option -f requires an argument\n", progname);
                        exit(EXIT_USAGE);
                    }
                    file_arg = argv[++i];
                    break;
                case 'R':
                    do_rfc2822 = 1;
                    break;
                case 'u':
                    opt_utc = 1;
                    break;
                default:
                    fprintf(stderr, "%s: unknown option '-%c'\n", progname, *p);
                    exit(EXIT_USAGE);
                }
            }
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", progname, argv[i]);
            exit(EXIT_USAGE);
        }
    }

    /* Execute requested operation */
    if (set_string) {
        set_time(set_string);
        /* Print the new time */
        if (do_rfc2822)
            print_rfc2822();
        else if (do_iso8601)
            print_iso8601(iso8601_fmt);
        else
            print_date("%a %b %d %H:%M:%S %Z %Y");
    } else if (display_string_arg) {
        display_string(display_string_arg);
    } else if (file_arg) {
        read_file(file_arg);
    } else if (do_rfc2822) {
        print_rfc2822();
    } else if (do_iso8601) {
        print_iso8601(iso8601_fmt);
    } else if (have_format) {
        print_date(user_format);
    } else {
        /* Default: print current date/time */
        print_date("%a %b %d %H:%M:%S %Z %Y");
    }

    return EXIT_OK;
}
