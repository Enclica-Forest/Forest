/*
 * stdlib.h - General utilities
 * 
 * C23 compatible standard library functions for Fern libc.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#define __STDC_VERSION_STDLIB_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Exit codes */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Maximum value returned by rand() */
#define RAND_MAX 32767

/* Division result structures */
typedef struct {
    int quot;   /* Quotient */
    int rem;    /* Remainder */
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

/* String conversion functions */
double atof(const char *nptr);
int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);

double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

/* Non-standard but widely used */
char *itoa(int value, char *str, int base);
char *ltoa(long value, char *str, int base);
char *lltoa(long long value, char *str, int base);

/* Pseudo-random sequence generation functions */
int rand(void);
void srand(unsigned int seed);
int rand_r(unsigned int *seedp);

/* Memory management functions */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

/* POSIX extensions */
void *reallocarray(void *ptr, size_t nmemb, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);
void *aligned_alloc(size_t alignment, size_t size);

/* Communication with the environment */
void abort(void) __attribute__((noreturn));
int atexit(void (*func)(void));
int at_quick_exit(void (*func)(void));
void exit(int status) __attribute__((noreturn));
void quick_exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int system(const char *command);

/* Environment array */
extern char **environ;

/* Searching and sorting utilities */
void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *));
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *), void *arg);

/* Integer arithmetic functions */
int abs(int j);
long labs(long j);
long long llabs(long long j);

div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

/* Multibyte/wide character conversion functions */
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);

/* POSIX extensions */
char *realpath(const char *path, char *resolved_path);
int mkstemp(char *template);
char *mkdtemp(char *template);
int mkstemps(char *template, int suffixlen);
char *mktemp(char *template);

/* Load average (BSD/Linux extension) */
int getloadavg(double loadavg[], int nelem);

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */
