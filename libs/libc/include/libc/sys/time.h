/*
 * sys/time.h - Time types (POSIX extension)
 *
 * Provides gettimeofday(), settimeofday(), and related structures.
 * struct timeval and struct itimerval are defined in time.h.
 */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <time.h>

/* gettimeofday and settimeofday (also declared in time.h) */
int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);

/* adjtime */
int adjtime(const struct timeval *delta, struct timeval *olddelta);

/* Timer macros */
#define timerclear(tvp)    ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timercmp(tvp, uvp, cmp) \
    (((tvp)->tv_sec == (uvp)->tv_sec) ? \
    ((tvp)->tv_usec cmp (uvp)->tv_usec) : \
    ((tvp)->tv_sec cmp (uvp)->tv_sec))
#define timeradd(tvp, uvp, vvp) \
    do { \
        (vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec; \
        (vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec; \
        if ((vvp)->tv_usec >= 1000000) { \
            (vvp)->tv_sec++; \
            (vvp)->tv_usec -= 1000000; \
        } \
    } while (0)
#define timersub(tvp, uvp, vvp) \
    do { \
        (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec; \
        (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec; \
        if ((vvp)->tv_usec < 0) { \
            (vvp)->tv_sec--; \
            (vvp)->tv_usec += 1000000; \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIME_H */
