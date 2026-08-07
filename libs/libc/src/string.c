/*
 * string.c - String handling functions for Fern libc
 * 
 * This file implements the standard C string and memory functions.
 * These are pure library functions that don't require kernel interaction.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

/* ============================================================================
 * MEMORY FUNCTIONS
 * ============================================================================ */

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    
    /* Use word-sized copies when aligned */
    if (((uintptr_t)d | (uintptr_t)s | n) & (sizeof(unsigned long) - 1)) {
        /* Unaligned or small: byte copy */
        while (n--) {
            *d++ = *s++;
        }
    } else {
        /* Aligned: word copy */
        unsigned long *ld = (unsigned long *)d;
        const unsigned long *ls = (const unsigned long *)s;
        size_t words = n / sizeof(unsigned long);
        while (words--) {
            *ld++ = *ls++;
        }
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dest;
    }

    if (d < s || d >= s + n) {
        /* No overlap or forward copy is safe */
        while (n--) {
            *d++ = *s++;
        }
    } else {
        /* Overlap: copy backwards */
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    unsigned char val = (unsigned char)c;
    
    while (n--) {
        *p++ = val;
    }
    return s;
}

void *memset_explicit(void *s, int c, size_t n) {
    /* Same as memset but guaranteed not to be optimized away */
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    
    while (n--) {
        if (*a != *b) {
            return *a - *b;
        }
        a++;
        b++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char val = (unsigned char)c;
    
    while (n--) {
        if (*p == val) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    unsigned char val = (unsigned char)c;
    
    while (n--) {
        --p;
        if (*p == val) {
            return (void *)p;
        }
    }
    return NULL;
}

void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen) {
    if (needlelen == 0) {
        return (void *)haystack;
    }
    if (haystacklen < needlelen) {
        return NULL;
    }
    
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    const unsigned char *end = h + haystacklen - needlelen + 1;
    
    while (h < end) {
        if (*h == *n && memcmp(h, n, needlelen) == 0) {
            return (void *)h;
        }
        h++;
    }
    return NULL;
}

/* BSD compatibility */
void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

void bcopy(const void *src, void *dest, size_t n) {
    memmove(dest, src, n);
}

int bcmp(const void *s1, const void *s2, size_t n) {
    return memcmp(s1, s2, n);
}

/* ============================================================================
 * STRING LENGTH FUNCTIONS
 * ============================================================================ */

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) {
        p++;
    }
    return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
    const char *p = s;
    while (maxlen-- && *p) {
        p++;
    }
    return p - s;
}

/* ============================================================================
 * STRING COPY FUNCTIONS
 * ============================================================================ */

char *strcpy(char *dest, const char *src) {
    char *ret = dest;
    while ((*dest++ = *src++)) {
        /* Empty body */
    }
    return ret;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copylen = (srclen >= size) ? size - 1 : srclen;
        memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *new = malloc(len);
    if (new) {
        memcpy(new, s, len);
    }
    return new;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *new = malloc(len + 1);
    if (new) {
        memcpy(new, s, len);
        new[len] = '\0';
    }
    return new;
}

/* ============================================================================
 * STRING CONCATENATION FUNCTIONS
 * ============================================================================ */

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++)) {
        /* Empty body */
    }
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        d[i] = src[i];
    }
    d[i] = '\0';
    return dest;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dstlen = strnlen(dst, size);
    size_t srclen = strlen(src);
    
    if (dstlen >= size) {
        return size + srclen;
    }
    
    size_t remaining = size - dstlen - 1;
    size_t copylen = (srclen > remaining) ? remaining : srclen;
    memcpy(dst + dstlen, src, copylen);
    dst[dstlen + copylen] = '\0';
    
    return dstlen + srclen;
}

/* ============================================================================
 * STRING COMPARISON FUNCTIONS
 * ============================================================================ */

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) {
        return 0;
    }
    while (n-- && *s1 && (*s1 == *s2)) {
        if (n == 0) {
            return 0;
        }
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n-- && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    if (n == (size_t)-1) {
        return 0;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strcoll(const char *s1, const char *s2) {
    /* Simple implementation for C locale */
    return strcmp(s1, s2);
}

size_t strxfrm(char *dest, const char *src, size_t n) {
    /* Simple implementation for C locale */
    size_t len = strlen(src);
    if (n > 0) {
        size_t copylen = (len >= n) ? n - 1 : len;
        memcpy(dest, src, copylen);
        dest[copylen] = '\0';
    }
    return len;
}

/* ============================================================================
 * STRING SEARCH FUNCTIONS
 * ============================================================================ */

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }
    return (ch == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    char ch = (char)c;
    
    while (*s) {
        if (*s == ch) {
            last = s;
        }
        s++;
    }
    return (ch == '\0') ? (char *)s : (char *)last;
}

/* BSD compatibility */
char *index(const char *s, int c) {
    return strchr(s, c);
}

char *rindex(const char *s, int c) {
    return strrchr(s, c);
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) {
        return (char *)haystack;
    }
    
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        
        if (!*n) {
            return (char *)haystack;
        }
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    
    while (*s) {
        if (!strchr(accept, *s)) {
            break;
        }
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    
    while (*s) {
        if (strchr(reject, *s)) {
            break;
        }
        count++;
        s++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        if (strchr(accept, *s)) {
            return (char *)s;
        }
        s++;
    }
    return NULL;
}

/* ============================================================================
 * STRING TOKENIZATION FUNCTIONS
 * ============================================================================ */

static char *__strtok_state = NULL;

char *strtok(char *str, const char *delim) {
    return strtok_r(str, delim, &__strtok_state);
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *start;
    
    if (str) {
        *saveptr = str;
    }
    
    if (!*saveptr) {
        return NULL;
    }
    
    /* Skip leading delimiters */
    *saveptr += strspn(*saveptr, delim);
    if (!**saveptr) {
        *saveptr = NULL;
        return NULL;
    }
    
    start = *saveptr;
    
    /* Find end of token */
    *saveptr = start + strcspn(start, delim);
    if (**saveptr) {
        **saveptr = '\0';
        (*saveptr)++;
    } else {
        *saveptr = NULL;
    }
    
    return start;
}

char *strsep(char **stringp, const char *delim) {
    char *start = *stringp;
    char *p;
    
    if (!start) {
        return NULL;
    }
    
    p = start + strcspn(start, delim);
    if (*p) {
        *p = '\0';
        *stringp = p + 1;
    } else {
        *stringp = NULL;
    }
    
    return start;
}

/* ============================================================================
 * ERROR STRING FUNCTIONS
 * ============================================================================ */

/* Error messages for strerror() */
static const char * const __sys_errlist[] = {
    [0] = "Success",
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "Input/output error",
    [ENXIO] = "No such device or address",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [EXDEV] = "Invalid cross-device link",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [ENFILE] = "Too many open files in system",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Inappropriate ioctl for device",
    [EFBIG] = "File too large",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EMLINK] = "Too many links",
    [EPIPE] = "Broken pipe",
    [EDOM] = "Numerical argument out of domain",
    [ERANGE] = "Numerical result out of range",
    [ENOSYS] = "Function not implemented",
    [ENAMETOOLONG] = "File name too long",
    [ENOTEMPTY] = "Directory not empty",
    [ELOOP] = "Too many levels of symbolic links",
    [ENOTSOCK] = "Socket operation on non-socket",
    [EDESTADDRREQ] = "Destination address required",
    [EMSGSIZE] = "Message too long",
    [EPROTOTYPE] = "Protocol wrong type for socket",
    [ENOPROTOOPT] = "Protocol not available",
    [EPROTONOSUPPORT] = "Protocol not supported",
    [EADDRINUSE] = "Address already in use",
    [EADDRNOTAVAIL] = "Cannot assign requested address",
    [ENETDOWN] = "Network is down",
    [ENETUNREACH] = "Network is unreachable",
    [ECONNABORTED] = "Software caused connection abort",
    [ECONNRESET] = "Connection reset by peer",
    [ENOBUFS] = "No buffer space available",
    [EISCONN] = "Transport endpoint is already connected",
    [ENOTCONN] = "Transport endpoint is not connected",
    [ETIMEDOUT] = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTUNREACH] = "No route to host",
    [EALREADY] = "Operation already in progress",
    [EINPROGRESS] = "Operation now in progress",
};

static const int __sys_nerr = sizeof(__sys_errlist) / sizeof(__sys_errlist[0]);

char *strerror(int errnum) {
    static char buf[64];
    
    if (errnum >= 0 && errnum < __sys_nerr && __sys_errlist[errnum]) {
        return (char *)__sys_errlist[errnum];
    }
    
    /* Generate a generic message for unknown errors */
    char *p = buf;
    const char *prefix = "Unknown error ";
    while (*prefix) {
        *p++ = *prefix++;
    }
    
    /* Convert error number to string */
    if (errnum < 0) {
        *p++ = '-';
        errnum = -errnum;
    }
    
    char numstr[16];
    int i = 0;
    do {
        numstr[i++] = '0' + (errnum % 10);
        errnum /= 10;
    } while (errnum && i < 15);
    
    while (i > 0) {
        *p++ = numstr[--i];
    }
    *p = '\0';
    
    return buf;
}
