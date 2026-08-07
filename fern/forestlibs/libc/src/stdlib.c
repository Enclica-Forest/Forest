/*
 * stdlib.c - General utility functions for Fern libc
 * 
 * This file implements memory allocation, string conversion,
 * random number generation, and other utility functions.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

/* ============================================================================
 * MEMORY ALLOCATION
 * 
 * Simple bump allocator that uses brk() to extend the heap.
 * A more sophisticated allocator would track free blocks.
 * ============================================================================ */

static void *__heap_start = NULL;
static void *__heap_end = NULL;

/* Initialize the heap using brk() */
static int __heap_init(void) {
    if (__heap_start == NULL) {
        __heap_start = (void *)brk(0);
        if (__heap_start == (void *)-1) {
            return -1;
        }
        __heap_end = __heap_start;
    }
    return 0;
}

void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    /* Initialize heap if needed */
    if (__heap_init() < 0) {
        errno = ENOMEM;
        return NULL;
    }
    
    /* Align to 16 bytes */
    size = (size + 15) & ~15;
    
    /* Extend heap */
    void *ptr = __heap_end;
    void *new_end = (char *)__heap_end + size;
    
    if (brk(new_end) < 0) {
        errno = ENOMEM;
        return NULL;
    }
    
    __heap_end = new_end;
    return ptr;
}

void *calloc(size_t nmemb, size_t size) {
    /* Check for overflow */
    if (nmemb && size > SIZE_MAX / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    
    /* Simple implementation: always allocate new block */
    void *new_ptr = malloc(size);
    if (new_ptr) {
        /* We don't know the old size, so copy conservatively */
        memcpy(new_ptr, ptr, size);
        /* Note: free(ptr) would be called in a real implementation */
    }
    return new_ptr;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size) {
    /* Check for overflow */
    if (nmemb && size > SIZE_MAX / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, nmemb * size);
}

void free(void *ptr) {
    /* Simple bump allocator does not actually free memory */
    (void)ptr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    /* Basic validity checks */
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    
    /* Allocate extra space for alignment */
    void *ptr = malloc(size + alignment - 1);
    if (ptr == NULL) {
        return ENOMEM;
    }
    
    /* Align the pointer */
    *memptr = (void *)(((uintptr_t)ptr + alignment - 1) & ~(alignment - 1));
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
    void *ptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

/* ============================================================================
 * STRING CONVERSION FUNCTIONS
 * ============================================================================ */

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}

long long atoll(const char *nptr) {
    return strtoll(nptr, NULL, 10);
}

double atof(const char *nptr) {
    return strtod(nptr, NULL);
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long result = 0;
    int negative = 0;
    
    /* Skip whitespace */
    while (isspace((unsigned char)*s)) {
        s++;
    }
    
    /* Handle sign */
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    /* Auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
    }
    
    /* Convert digits */
    while (*s) {
        int digit;
        if (isdigit((unsigned char)*s)) {
            digit = *s - '0';
        } else if (isalpha((unsigned char)*s)) {
            digit = tolower((unsigned char)*s) - 'a' + 10;
        } else {
            break;
        }
        
        if (digit >= base) {
            break;
        }
        
        result = result * base + digit;
        s++;
    }
    
    if (endptr) {
        *endptr = (char *)s;
    }
    
    return negative ? -result : result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    /* Simple implementation: use strtol and cast */
    return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base) {
    /* Similar to strtol but for long long */
    const char *s = nptr;
    long long result = 0;
    int negative = 0;
    
    while (isspace((unsigned char)*s)) {
        s++;
    }
    
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
    }
    
    while (*s) {
        int digit;
        if (isdigit((unsigned char)*s)) {
            digit = *s - '0';
        } else if (isalpha((unsigned char)*s)) {
            digit = tolower((unsigned char)*s) - 'a' + 10;
        } else {
            break;
        }
        
        if (digit >= base) {
            break;
        }
        
        result = result * base + digit;
        s++;
    }
    
    if (endptr) {
        *endptr = (char *)s;
    }
    
    return negative ? -result : result;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtoll(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    double result = 0.0;
    double fraction = 0.1;
    int negative = 0;
    int has_decimal = 0;
    
    /* Skip whitespace */
    while (isspace((unsigned char)*s)) {
        s++;
    }
    
    /* Handle sign */
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    /* Integer part */
    while (isdigit((unsigned char)*s)) {
        result = result * 10.0 + (*s - '0');
        s++;
    }
    
    /* Decimal part */
    if (*s == '.') {
        s++;
        has_decimal = 1;
        while (isdigit((unsigned char)*s)) {
            result += (*s - '0') * fraction;
            fraction *= 0.1;
            s++;
        }
    }
    
    /* Exponent part */
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_negative = 0;
        int exponent = 0;
        
        if (*s == '-') {
            exp_negative = 1;
            s++;
        } else if (*s == '+') {
            s++;
        }
        
        while (isdigit((unsigned char)*s)) {
            exponent = exponent * 10 + (*s - '0');
            s++;
        }
        
        double multiplier = 1.0;
        while (exponent-- > 0) {
            multiplier *= 10.0;
        }
        
        if (exp_negative) {
            result /= multiplier;
        } else {
            result *= multiplier;
        }
    }
    
    if (endptr) {
        *endptr = (char *)s;
    }
    
    return negative ? -result : result;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

/* Non-standard but common: itoa */
char *itoa(int value, char *str, int base) {
    if (base < 2 || base > 36) {
        str[0] = '\0';
        return str;
    }
    
    char *p = str;
    int negative = 0;
    unsigned int uvalue;
    
    if (value < 0 && base == 10) {
        negative = 1;
        uvalue = (unsigned int)(-value);
    } else {
        uvalue = (unsigned int)value;
    }
    
    /* Generate digits in reverse order */
    do {
        int digit = uvalue % base;
        *p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        uvalue /= base;
    } while (uvalue);
    
    if (negative) {
        *p++ = '-';
    }
    *p = '\0';
    
    /* Reverse string */
    char *start = str;
    char *end = p - 1;
    while (start < end) {
        char tmp = *start;
        *start++ = *end;
        *end-- = tmp;
    }
    
    return str;
}

/* ============================================================================
 * RANDOM NUMBER GENERATION
 * ============================================================================ */

static unsigned int __rand_seed = 1;

int rand(void) {
    __rand_seed = __rand_seed * 1103515245 + 12345;
    return (int)((__rand_seed >> 16) & RAND_MAX);
}

void srand(unsigned int seed) {
    __rand_seed = seed;
}

int rand_r(unsigned int *seedp) {
    *seedp = *seedp * 1103515245 + 12345;
    return (int)((*seedp >> 16) & RAND_MAX);
}

/* ============================================================================
 * ENVIRONMENT FUNCTIONS
 * ============================================================================ */

/* Default environment */
static char *__default_environ[] = { NULL };
char **environ = __default_environ;

char *getenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        return NULL;
    }
    
    size_t len = strlen(name);
    
    for (char **ep = environ; *ep; ep++) {
        if (strncmp(*ep, name, len) == 0 && (*ep)[len] == '=') {
            return (*ep) + len + 1;
        }
    }
    
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    
    /* Check if variable already exists */
    char *existing = getenv(name);
    if (existing && !overwrite) {
        return 0;
    }
    
    /* Create new entry */
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *entry = malloc(name_len + 1 + value_len + 1);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value, value_len + 1);
    
    /* Add to environment (simplified - doesn't handle removal) */
    return putenv(entry);
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    
    size_t len = strlen(name);
    char **ep = environ;
    char **wp = environ;
    
    while (*ep) {
        if (strncmp(*ep, name, len) != 0 || (*ep)[len] != '=') {
            *wp++ = *ep;
        }
        ep++;
    }
    *wp = NULL;
    
    return 0;
}

int putenv(char *string) {
    if (!string || !strchr(string, '=')) {
        errno = EINVAL;
        return -1;
    }
    
    /* Count current environment size */
    size_t count = 0;
    for (char **ep = environ; *ep; ep++) {
        count++;
    }
    
    /* Allocate new environment array */
    char **new_environ = malloc((count + 2) * sizeof(char *));
    if (!new_environ) {
        errno = ENOMEM;
        return -1;
    }
    
    /* Copy existing entries (except if we're replacing) */
    size_t eq_pos = strchr(string, '=') - string;
    size_t i = 0;
    int replaced = 0;
    
    for (char **ep = environ; *ep; ep++) {
        if (strncmp(*ep, string, eq_pos + 1) == 0) {
            new_environ[i++] = string;
            replaced = 1;
        } else {
            new_environ[i++] = *ep;
        }
    }
    
    if (!replaced) {
        new_environ[i++] = string;
    }
    new_environ[i] = NULL;
    
    environ = new_environ;
    return 0;
}

/* ============================================================================
 * PROCESS CONTROL FUNCTIONS
 * ============================================================================ */

void abort(void) {
    /* In a full implementation, this would raise SIGABRT */
    _exit(134);  /* 128 + SIGABRT(6) */
    __builtin_unreachable();
}

void exit(int status) {
    /* TODO: Call atexit handlers */
    _exit(status);
    __builtin_unreachable();
}

void quick_exit(int status) {
    /* TODO: Call at_quick_exit handlers */
    _exit(status);
    __builtin_unreachable();
}

/* atexit handlers (simplified) */
static void (*__atexit_handlers[32])(void);
static int __atexit_count = 0;

int atexit(void (*func)(void)) {
    if (__atexit_count >= 32) {
        return -1;
    }
    __atexit_handlers[__atexit_count++] = func;
    return 0;
}

int at_quick_exit(void (*func)(void)) {
    /* Simplified: same as atexit */
    return atexit(func);
}

int system(const char *command) {
    if (command == NULL) {
        /* Check if shell is available */
        return 0;  /* No shell available in Fern */
    }
    
    errno = ENOSYS;
    return -1;
}

/* ============================================================================
 * SEARCHING AND SORTING
 * ============================================================================ */

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *)) {
    const char *arr = (const char *)base;
    size_t low = 0;
    size_t high = nmemb;
    
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const void *mid_elem = arr + mid * size;
        int cmp = compar(key, mid_elem);
        
        if (cmp == 0) {
            return (void *)mid_elem;
        } else if (cmp < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    
    return NULL;
}

/* Simple quicksort implementation */
static void __swap(void *a, void *b, size_t size) {
    char *pa = (char *)a;
    char *pb = (char *)b;
    
    while (size--) {
        char tmp = *pa;
        *pa++ = *pb;
        *pb++ = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2) {
        return;
    }
    
    /* Simple insertion sort for small arrays */
    if (nmemb <= 10) {
        char *arr = (char *)base;
        for (size_t i = 1; i < nmemb; i++) {
            for (size_t j = i; j > 0; j--) {
                void *prev = arr + (j - 1) * size;
                void *curr = arr + j * size;
                if (compar(prev, curr) <= 0) {
                    break;
                }
                __swap(prev, curr, size);
            }
        }
        return;
    }
    
    /* Quicksort for larger arrays */
    char *arr = (char *)base;
    size_t pivot_idx = nmemb / 2;
    
    /* Move pivot to end */
    __swap(arr + pivot_idx * size, arr + (nmemb - 1) * size, size);
    
    size_t store_idx = 0;
    for (size_t i = 0; i < nmemb - 1; i++) {
        if (compar(arr + i * size, arr + (nmemb - 1) * size) < 0) {
            __swap(arr + i * size, arr + store_idx * size, size);
            store_idx++;
        }
    }
    
    /* Move pivot to final position */
    __swap(arr + store_idx * size, arr + (nmemb - 1) * size, size);
    
    /* Recursively sort partitions */
    qsort(arr, store_idx, size, compar);
    qsort(arr + (store_idx + 1) * size, nmemb - store_idx - 1, size, compar);
}

/* ============================================================================
 * INTEGER ARITHMETIC FUNCTIONS
 * ============================================================================ */

int abs(int j) {
    return j < 0 ? -j : j;
}

long labs(long j) {
    return j < 0 ? -j : j;
}

long long llabs(long long j) {
    return j < 0 ? -j : j;
}

div_t div(int numer, int denom) {
    div_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    result.quot = numer / denom;
    result.rem = numer % denom;
    return result;
}

/* ============================================================================
 * MISCELLANEOUS FUNCTIONS
 * ============================================================================ */

char *realpath(const char *path, char *resolved_path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    
    /* Allocate buffer if not provided */
    int allocated = 0;
    if (!resolved_path) {
        resolved_path = malloc(PATH_MAX);
        if (!resolved_path) {
            errno = ENOMEM;
            return NULL;
        }
        allocated = 1;
    }
    
    /* Fern VFS is simple - just copy absolute paths */
    if (path[0] != '/') {
        /* TODO: Handle relative paths properly */
        if (allocated) {
            free(resolved_path);
        }
        errno = EINVAL;
        return NULL;
    }
    
    size_t len = strlen(path);
    if (len >= PATH_MAX) {
        if (allocated) {
            free(resolved_path);
        }
        errno = ENAMETOOLONG;
        return NULL;
    }
    
    memcpy(resolved_path, path, len + 1);
    return resolved_path;
}

int mkstemp(char *template) {
    /* Find the XXXXXX suffix */
    size_t len = strlen(template);
    if (len < 6) {
        errno = EINVAL;
        return -1;
    }
    
    char *suffix = template + len - 6;
    if (strcmp(suffix, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    
    /* Generate random suffix */
    static unsigned int counter = 0;
    counter++;
    
    for (int i = 0; i < 6; i++) {
        int r = (rand() + counter) % 36;
        suffix[i] = (r < 10) ? ('0' + r) : ('a' + r - 10);
    }
    
    /* Create the file */
    int fd = open(template, 0x42 | 0x80, 0600);  /* O_RDWR | O_CREAT | O_EXCL */
    return fd;
}

char *mkdtemp(char *template) {
    /* Similar to mkstemp but creates a directory */
    size_t len = strlen(template);
    if (len < 6) {
        errno = EINVAL;
        return NULL;
    }
    
    char *suffix = template + len - 6;
    if (strcmp(suffix, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }
    
    static unsigned int counter = 0;
    counter++;
    
    for (int i = 0; i < 6; i++) {
        int r = (rand() + counter) % 36;
        suffix[i] = (r < 10) ? ('0' + r) : ('a' + r - 10);
    }
    
    if (mkdir(template, 0700) < 0) {
        return NULL;
    }
    
    return template;
}

int getloadavg(double loadavg[], int nelem) {
    /* Not implemented in Fern */
    (void)loadavg;
    (void)nelem;
    errno = ENOSYS;
    return -1;
}
