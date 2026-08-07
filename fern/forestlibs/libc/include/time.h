/*
 * time.h - Time types and functions
 * 
 * C23 / POSIX compatible time functions for Fern libc.
 */
#ifndef _TIME_H
#define _TIME_H

#define __STDC_VERSION_TIME_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>

/* Null pointer constant */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Clocks per second for clock() */
#define CLOCKS_PER_SEC 1000000

/* Clock types for clock_gettime/clock_settime */
#define CLOCK_REALTIME              0   /* System-wide real-time clock */
#define CLOCK_MONOTONIC             1   /* Monotonic time since some unspecified starting point */
#define CLOCK_PROCESS_CPUTIME_ID    2   /* High-resolution per-process timer from the CPU */
#define CLOCK_THREAD_CPUTIME_ID     3   /* Thread-specific CPU-time clock */
#define CLOCK_MONOTONIC_RAW         4   /* Monotonic system-wide clock, not subject to NTP adjustments */
#define CLOCK_REALTIME_COARSE       5   /* Faster but less precise version of CLOCK_REALTIME */
#define CLOCK_MONOTONIC_COARSE      6   /* Faster but less precise version of CLOCK_MONOTONIC */
#define CLOCK_BOOTTIME              7   /* Identical to CLOCK_MONOTONIC, includes any time in suspend */
#define CLOCK_REALTIME_ALARM        8   /* Like CLOCK_REALTIME but will wake the system if suspended */
#define CLOCK_BOOTTIME_ALARM        9   /* Like CLOCK_BOOTTIME but will wake the system if suspended */
#define CLOCK_TAI                   11  /* International Atomic Time */

/* Timer flags */
#define TIMER_ABSTIME 1

/* Time structures */
struct tm {
    int tm_sec;     /* Seconds (0-60) */
    int tm_min;     /* Minutes (0-59) */
    int tm_hour;    /* Hours (0-23) */
    int tm_mday;    /* Day of the month (1-31) */
    int tm_mon;     /* Month (0-11) */
    int tm_year;    /* Year - 1900 */
    int tm_wday;    /* Day of the week (0-6, Sunday = 0) */
    int tm_yday;    /* Day in the year (0-365) */
    int tm_isdst;   /* Daylight saving time flag */
    long tm_gmtoff; /* Seconds east of UTC */
    const char *tm_zone; /* Timezone abbreviation */
};

/* Time value structure (POSIX) */
struct timespec {
    time_t tv_sec;  /* Seconds */
    long tv_nsec;   /* Nanoseconds */
};

/* Time value structure (BSD) */
struct timeval {
    time_t tv_sec;       /* Seconds */
    suseconds_t tv_usec; /* Microseconds */
};

/* Timezone structure */
struct timezone {
    int tz_minuteswest; /* Minutes west of GMT */
    int tz_dsttime;     /* Type of DST correction */
};

/* Timer structure */
struct itimerspec {
    struct timespec it_interval; /* Timer interval */
    struct timespec it_value;    /* Initial expiration */
};

struct itimerval {
    struct timeval it_interval;  /* Timer interval */
    struct timeval it_value;     /* Current value */
};

/* Timer types */
#define ITIMER_REAL    0    /* Decrements in real time */
#define ITIMER_VIRTUAL 1    /* Decrements in process virtual time */
#define ITIMER_PROF    2    /* Decrements both in process virtual time and when the system is running on behalf of the process */

/* Time manipulation functions */
clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm *tm);
time_t time(time_t *tloc);
time_t timegm(struct tm *tm);

/* Time conversion functions */
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *timep);
char *ctime_r(const time_t *timep, char *buf);
struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);

/* Time formatting */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
char *strptime(const char *s, const char *format, struct tm *tm);

/* POSIX clock functions */
int clock_getres(clockid_t clk_id, struct timespec *res);
int clock_gettime(clockid_t clk_id, struct timespec *tp);
int clock_settime(clockid_t clk_id, const struct timespec *tp);
int clock_nanosleep(clockid_t clk_id, int flags,
                    const struct timespec *request,
                    struct timespec *remain);

/* Sleep functions */
int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);

/* Time of day functions */
int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);

/* Timer functions */
int getitimer(int which, struct itimerval *curr_value);
int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value);

/* POSIX timer functions */
int timer_create(clockid_t clockid, struct sigevent *sevp, timer_t *timerid);
int timer_delete(timer_t timerid);
int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *new_value,
                  struct itimerspec *old_value);
int timer_gettime(timer_t timerid, struct itimerspec *curr_value);
int timer_getoverrun(timer_t timerid);

/* Timezone functions */
void tzset(void);

/* Timezone variables */
extern char *tzname[2];
extern long timezone;
extern int daylight;

/* C23 additions */
int timespec_get(struct timespec *ts, int base);
int timespec_getres(struct timespec *res, int base);

#define TIME_UTC 1

#ifdef __cplusplus
}
#endif

#endif /* _TIME_H */
