/*
 * forest_stubs.c - Stub implementations for missing POSIX functions
 *
 * Provides minimal implementations of functions that Forest OS libc
 * declares but doesn't yet implement, or that are completely missing.
 *
 * Link this with apps that need: fnmatch, basename, dirname,
 * getaddrinfo, getnameinfo, freeaddrinfo, sscanf, getline, etc.
 */

#define _DEFAULT_SOURCE
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fnmatch.h>
#include <netdb.h>
#include <stdio.h>
#include <getopt.h>
#include <wchar.h>
#include <time.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <grp.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <fenv.h>
#include <termios.h>
#include <stdarg.h>

/* --- basename / dirname --- */

char *basename(char *path) {
    char *p;
    if (!path || !*path) return ".";
    p = path + strlen(path) - 1;
    while (p > path && *p == '/') p--;
    if (p == path && *p == '/') return "/";
    while (p > path && *(p - 1) != '/') p--;
    return p;
}

static char __dirname_buf[1024];

char *dirname(char *path) {
    char *p;
    if (!path || !*path) return ".";
    strncpy(__dirname_buf, path, sizeof(__dirname_buf) - 1);
    __dirname_buf[sizeof(__dirname_buf) - 1] = '\0';
    p = __dirname_buf + strlen(__dirname_buf) - 1;
    while (p > __dirname_buf && *p == '/') *p-- = '\0';
    while (p > __dirname_buf && *(p - 1) != '/') p--;
    if (p == __dirname_buf) {
        if (*p == '/') return "/";
        return ".";
    }
    *p = '\0';
    return __dirname_buf;
}

/* --- fnmatch (simple glob-style) --- */

static int fnmatch_impl(const char *pattern, const char *string, int flags) {
    int c;

    while ((c = *pattern++) != '\0') {
        if (c == '*') {
            while (*pattern == '*') pattern++;
            if (*pattern == '\0') return 0;
            while (*string) {
                if (fnmatch_impl(pattern, string, flags) == 0)
                    return 0;
                string++;
            }
            return FNM_NOMATCH;
        }
        if (c == '?') {
            if (*string == '\0') return FNM_NOMATCH;
            if (*string == '/' && (flags & FNM_PATHNAME)) return FNM_NOMATCH;
            string++;
            continue;
        }
        if (c == '[') {
            int negate = 0;
            if (*pattern == '!' || *pattern == '^') {
                negate = 1;
                pattern++;
            }
            int match = 0;
            int closed = 0;
            while (*pattern && !closed) {
                if (*pattern == ']') { closed = 1; break; }
                if (*(pattern + 1) == '-' && *(pattern + 2) != ']') {
                    if (*string >= *pattern && *string <= *(pattern + 2))
                        match = 1;
                    pattern += 3;
                } else {
                    if (*string == *pattern) match = 1;
                    pattern++;
                }
            }
            if (!closed) return FNM_NOMATCH;
            if (negate ? match : !match) return FNM_NOMATCH;
            string++;
            continue;
        }
        if (c == '\\') {
            c = *pattern++;
            if (c == '\0') return FNM_NOMATCH;
        }
        if ((flags & FNM_PATHNAME) && c == '/' && *string != '/')
            return FNM_NOMATCH;
        if (*string == '\0') return FNM_NOMATCH;
        if (c != *string) return FNM_NOMATCH;
        string++;
    }
    return (*string == '\0') ? 0 : FNM_NOMATCH;
}

int fnmatch(const char *pattern, const char *string, int flags) {
    return fnmatch_impl(pattern, string, flags);
}

/* --- getaddrinfo / getnameinfo ---
 * Forest OS doesn't have a networking stack yet. These stubs
 * provide basic hostname resolution using loopback as a placeholder.
 */

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints,
                struct addrinfo **res) {
    (void)service;
    (void)hints;

    if (!node || !res) return EAI_NONAME;

    struct addrinfo *ai = calloc(1, sizeof(struct addrinfo));
    if (!ai) return EAI_NONAME;

    struct sockaddr_in *sin = calloc(1, sizeof(struct sockaddr_in));
    if (!sin) { free(ai); return EAI_NONAME; }

    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */

    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_addr = (struct sockaddr *)sin;
    ai->ai_addrlen = sizeof(struct sockaddr_in);

    if (node)
        ai->ai_canonname = strdup(node);

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)salen;
    (void)flags;

    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)sa;
        if (sin->sin_addr.s_addr == htonl(0x7f000001)) {
            if (host && hostlen > 0) {
                strncpy(host, "localhost", hostlen - 1);
                host[hostlen - 1] = '\0';
            }
        } else {
            unsigned char *bytes = (unsigned char *)&sin->sin_addr;
            if (host && hostlen > 0) {
                snprintf(host, hostlen, "%d.%d.%d.%d",
                         bytes[0], bytes[1], bytes[2], bytes[3]);
            }
        }
    } else {
        if (host && hostlen > 0) {
            strncpy(host, "unknown", hostlen - 1);
            host[hostlen - 1] = '\0';
        }
    }

    if (serv && servlen > 0)
        serv[0] = '\0';

    return 0;
}

/* --- getopt_long ---
 * Minimal getopt_long that falls back to getopt for short options.
 * Long options are matched by prefix.
 */

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    /* If current arg doesn't start with --, use regular getopt */
    if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] != '-') {
        return getopt(argc, argv, optstring);
    }

    /* Handle -- */
    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    /* Parse --longoption[=value] */
    const char *arg = argv[optind] + 2;
    const char *eq = strchr(arg, '=');
    size_t arglen = eq ? (size_t)(eq - arg) : strlen(arg);

    for (int i = 0; longopts && longopts[i].name; i++) {
        if (strlen(longopts[i].name) == arglen &&
            strncmp(longopts[i].name, arg, arglen) == 0) {
            if (longindex) *longindex = i;

            if (longopts[i].has_arg == required_argument) {
                if (eq) {
                    optarg = (char *)(eq + 1);
                } else {
                    optind++;
                    if (optind >= argc) return '?';
                    optarg = argv[optind];
                }
            } else if (longopts[i].has_arg == optional_argument) {
                if (eq)
                    optarg = (char *)(eq + 1);
                else
                    optarg = NULL;
            } else {
                optarg = NULL;
            }

            optind++;
            if (longopts[i].flag) {
                *longopts[i].flag = longopts[i].val;
                return 0;
            }
            return longopts[i].val;
        }
    }

    optind++;
    return '?';
}

/* --- mbrtowc / wcwidth --- */

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
    (void)ps;
    if (!s || n == 0) return 0;
    if (pwc) *pwc = (wchar_t)(unsigned char)*s;
    return 1;
}

int wcwidth(wchar_t wc) {
    if (wc == 0) return 0;
    if (wc < 32 || (wc >= 0x7f && wc < 0xa0)) return -1;
    if (wc >= 0x1100) return 2; /* CJK and other wide chars */
    return 1;
}

/* --- fileno --- */
int fileno(FILE *stream) {
    return stream->fd;
}

/* --- errno --- */


/* --- getopt / optind / optarg --- */
int optind = 1;
char *optarg = NULL;

int getopt(int argc, char *const argv[], const char *optstring) {
    static int sp = 1;
    int c;
    const char *cp;

    if (sp == 1) {
        if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0')
            return -1;
        if (strcmp(argv[optind], "--") == 0) {
            optind++;
            return -1;
        }
    }

    c = argv[optind][sp];
    cp = strchr(optstring, c);

    if (c == ':' || cp == NULL) {
        if (optstring[0] != ':')
            fprintf(stderr, "%s: unknown option '-%c'\n", argv[0], c);
        if (argv[optind][++sp] == '\0') {
            optind++;
            sp = 1;
        }
        return '?';
    }

    if (*(cp + 1) == ':') {
        if (argv[optind][sp + 1] != '\0') {
            optarg = &argv[optind++][sp + 1];
        } else if (++optind >= argc) {
            if (optstring[0] != ':')
                fprintf(stderr, "%s: option '-%c' requires an argument\n", argv[0], c);
            sp = 1;
            return '?';
        } else {
            optarg = argv[optind++];
        }
        sp = 1;
    } else {
        if (argv[optind][++sp] == '\0') {
            sp = 1;
            optind++;
        }
        optarg = NULL;
    }

    return c;
}

/* --- strerror --- */
static const char *__err_strings[] = {
    "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "Input/output error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Cannot allocate memory",
    "Permission denied",
    "Bad address",
    "Device or resource busy",
    "File exists",
    "Invalid cross-device link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate ioctl for device",
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Numerical argument out of domain",
    "Numerical result out of range",
    "Function not implemented",
};

char *strerror(int errnum) {
    static char buf[32];
    if (errnum >= 0 && errnum < (int)(sizeof(__err_strings) / sizeof(__err_strings[0])))
        return (char *)__err_strings[errnum];
    snprintf(buf, sizeof(buf), "Unknown error %d", errnum);
    return buf;
}

/* --- strdup --- */
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/* --- snprintf --- */
int snprintf(char *str, size_t size, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    size_t pos = 0;
    const char *f = format;
    va_start(ap, format);
    if (size == 0) { va_end(ap); return 0; }
    while (*f) {
        if (*f != '%') {
            if (pos + 1 < size) str[pos] = *f;
            pos++; f++; continue;
        }
        f++;
        int is_long = 0;
        if (*f == 'l') { is_long = 1; f++; if (*f == 'l') { is_long = 2; f++; } }
        switch (*f) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s) { if (pos + 1 < size) str[pos] = *s; pos++; s++; }
                break;
            }
            case 'c': { int c = va_arg(ap, int); if (pos + 1 < size) str[pos] = (char)c; pos++; break; }
            case 'd': case 'i': {
                long long v = is_long >= 2 ? va_arg(ap, long long) : is_long ? va_arg(ap, long) : va_arg(ap, int);
                char tmp[24]; int i = 0; int neg = 0;
                unsigned long long uv;
                if (v < 0) { neg = 1; uv = (unsigned long long)(-v); } else { uv = (unsigned long long)v; }
                if (uv == 0) tmp[i++] = '0'; else while (uv) { tmp[i++] = '0' + (int)(uv % 10); uv /= 10; }
                if (neg) { if (pos + 1 < size) str[pos] = '-'; pos++; }
                while (i > 0) { if (pos + 1 < size) str[pos] = tmp[--i]; pos++; }
                break;
            }
            case 'u': {
                unsigned long long v = is_long >= 2 ? va_arg(ap, unsigned long long) : is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[24]; int i = 0;
                if (v == 0) tmp[i++] = '0'; else while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
                while (i > 0) { if (pos + 1 < size) str[pos] = tmp[--i]; pos++; }
                break;
            }
            case 'x': case 'X': {
                unsigned long long v = is_long >= 2 ? va_arg(ap, unsigned long long) : is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[24]; int i = 0;
                const char *hex = (*f == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                if (v == 0) tmp[i++] = '0'; else while (v) { tmp[i++] = hex[v % 16]; v /= 16; }
                while (i > 0) { if (pos + 1 < size) str[pos] = tmp[--i]; pos++; }
                break;
            }
            case 'p': {
                unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                if (pos + 2 < size) { str[pos] = '0'; str[pos + 1] = 'x'; }
                pos += 2;
                char tmp[24]; int i = 0;
                if (v == 0) tmp[i++] = '0'; else while (v) { tmp[i++] = "0123456789abcdef"[v % 16]; v /= 16; }
                while (i > 0) { if (pos + 1 < size) str[pos] = tmp[--i]; pos++; }
                break;
            }
            case '%': if (pos + 1 < size) str[pos] = '%'; pos++; break;
            default:
                if (pos + 1 < size) str[pos] = '%'; pos++;
                if (*f) { if (pos + 1 < size) str[pos] = *f; pos++; }
                break;
        }
        f++;
    }
    str[pos < size ? pos : size - 1] = '\0';
    va_end(ap);
    return (int)pos;
}

/* --- fprintf --- */
int fprintf(FILE *stream, const char *format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    const char *f = format;
    size_t pos = 0;
    while (*f && pos < sizeof(buf) - 1) {
        if (*f != '%') { buf[pos++] = *f++; continue; }
        f++;
        int is_long = 0;
        if (*f == 'l') { is_long = 1; f++; if (*f == 'l') { is_long = 2; f++; } }
        switch (*f) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < sizeof(buf) - 1) buf[pos++] = *s++;
                break;
            }
            case 'c': { if (pos < sizeof(buf) - 1) buf[pos++] = (char)va_arg(ap, int); break; }
            case 'd': case 'i': {
                long long v = is_long >= 2 ? va_arg(ap, long long) : is_long ? va_arg(ap, long) : va_arg(ap, int);
                char tmp[24]; int i = 0; int neg = 0;
                unsigned long long uv;
                if (v < 0) { neg = 1; uv = (unsigned long long)(-v); } else { uv = (unsigned long long)v; }
                if (uv == 0) tmp[i++] = '0'; else while (uv) { tmp[i++] = '0' + (int)(uv % 10); uv /= 10; }
                if (neg && pos < sizeof(buf) - 1) buf[pos++] = '-';
                while (i > 0 && pos < sizeof(buf) - 1) buf[pos++] = tmp[--i];
                break;
            }
            case 'u': {
                unsigned long long v = is_long >= 2 ? va_arg(ap, unsigned long long) : is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[24]; int i = 0;
                if (v == 0) tmp[i++] = '0'; else while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
                while (i > 0 && pos < sizeof(buf) - 1) buf[pos++] = tmp[--i];
                break;
            }
            case 'x': case 'X': {
                unsigned long long v = is_long >= 2 ? va_arg(ap, unsigned long long) : is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[24]; int i = 0;
                const char *hex = (*f == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                if (v == 0) tmp[i++] = '0'; else while (v) { tmp[i++] = hex[v % 16]; v /= 16; }
                while (i > 0 && pos < sizeof(buf) - 1) buf[pos++] = tmp[--i];
                break;
            }
            case 'p': {
                unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                if (pos + 2 < sizeof(buf) - 1) { buf[pos] = '0'; buf[pos + 1] = 'x'; }
                pos += 2;
                char tmp[24]; int i = 0;
                if (v == 0) tmp[i++] = '0'; else while (v) { tmp[i++] = "0123456789abcdef"[v % 16]; v /= 16; }
                while (i > 0 && pos < sizeof(buf) - 1) buf[pos++] = tmp[--i];
                break;
            }
            case '%': if (pos < sizeof(buf) - 1) buf[pos++] = '%'; break;
            default:
                if (pos < sizeof(buf) - 1) buf[pos++] = '%';
                if (*f && pos < sizeof(buf) - 1) buf[pos++] = *f;
                break;
        }
        f++;
    }
    va_end(ap);
    buf[pos] = '\0';
    return write(fileno(stream), buf, pos);
}

/* ============================================================================
 * STDIO STUBS - sscanf, fscanf, getline, etc.
 * ============================================================================ */

int sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    const char *s = str;
    int count = 0;

    while (*format && *s) {
        while (*format == ' ' || *format == '\t') { format++; while (*s == ' ' || *s == '\t') s++; }
        if (*format == '\0') break;

        if (*format != '%') {
            if (*s != *format) break;
            s++; format++; continue;
        }
        format++;

        int suppress = 0;
        if (*format == '*') { suppress = 1; format++; }

        int width = 0;
        while (*format >= '0' && *format <= '9') { width = width * 10 + (*format - '0'); format++; }

        int is_long = 0;
        if (*format == 'l') { is_long = 1; format++; if (*format == 'l') { is_long = 2; format++; } }
        else if (*format == 'h') { format++; if (*format == 'h') format++; }
        else if (*format == 'z') { format++; }

        while (*s == ' ' || *s == '\t') s++;

        switch (*format) {
        case 'd': case 'i': {
            long val = 0; int neg = 0;
            if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
            if (!(*s >= '0' && *s <= '9')) goto done;
            while (*s >= '0' && *s <= '9' && (width == 0 || width-- > 0)) { val = val * 10 + (*s - '0'); s++; }
            if (!suppress) {
                if (is_long == 0) *va_arg(ap, int *) = neg ? -(int)val : (int)val;
                else if (is_long == 1) *va_arg(ap, long *) = neg ? -val : val;
                else *va_arg(ap, long long *) = neg ? -val : val;
                count++;
            }
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            int base = (*format == 'x' || *format == 'X') ? 16 : (*format == 'o') ? 8 : 10;
            unsigned long val = 0;
            if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            while (*s) {
                int d = -1;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                if (d < 0 || d >= base || (width != 0 && width-- == 0)) break;
                val = val * base + d; s++;
            }
            if (!suppress) {
                if (is_long == 0) *va_arg(ap, unsigned int *) = (unsigned int)val;
                else if (is_long == 1) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned long long *) = val;
                count++;
            }
            break;
        }
        case 's': {
            char *dest = suppress ? NULL : va_arg(ap, char *);
            while (*s == ' ' || *s == '\t') s++;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                if (dest) *dest++ = *s;
                s++;
            }
            if (dest) *dest = '\0';
            if (!suppress) count++;
            break;
        }
        case 'c': {
            char *dest = suppress ? NULL : va_arg(ap, char *);
            if (dest) *dest = *s;
            s++;
            if (!suppress) count++;
            break;
        }
        case 'n': {
            if (!suppress) {
                int *dest = va_arg(ap, int *);
                *dest = (int)(s - str);
            }
            break;
        }
        case '%':
            if (*s != '%') goto done;
            s++; break;
        default:
            goto done;
        }
        format++;
    }
done:
    va_end(ap);
    return count;
}

int fscanf(FILE *stream, const char *format, ...) {
    char buf[4096];
    int n = 0;
    int c;
    while (n < (int)sizeof(buf) - 1 && (c = fgetc(stream)) != EOF) {
        buf[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return EOF;
    buf[n] = '\0';

    va_list ap;
    va_start(ap, format);
    int count = 0;
    const char *s = buf;

    while (*format && *s) {
        while (*format == ' ' || *format == '\t') { format++; while (*s == ' ' || *s == '\t') s++; }
        if (*format == '\0') break;
        if (*format != '%') { if (*s != *format) break; s++; format++; continue; }
        format++;

        int suppress = 0;
        if (*format == '*') { suppress = 1; format++; }

        int width = 0;
        while (*format >= '0' && *format <= '9') { width = width * 10 + (*format - '0'); format++; }

        int is_long = 0;
        if (*format == 'l') { is_long = 1; format++; if (*format == 'l') { is_long = 2; format++; } }
        else if (*format == 'h') { format++; if (*format == 'h') format++; }
        else if (*format == 'z') { format++; }

        while (*s == ' ' || *s == '\t') s++;

        switch (*format) {
        case 'd': case 'i': {
            long val = 0; int neg = 0;
            if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
            if (!(*s >= '0' && *s <= '9')) goto fdone;
            while (*s >= '0' && *s <= '9' && (width == 0 || width-- > 0)) { val = val * 10 + (*s - '0'); s++; }
            if (!suppress) { if (is_long == 0) *va_arg(ap, int *) = neg ? -(int)val : (int)val; else if (is_long == 1) *va_arg(ap, long *) = neg ? -val : val; else *va_arg(ap, long long *) = neg ? -val : val; count++; }
            break;
        }
        case 'u': case 'x': case 'X': {
            int base = (*format == 'x' || *format == 'X') ? 16 : 10;
            unsigned long val = 0;
            if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            while (*s) { int d = -1; if (*s >= '0' && *s <= '9') d = *s - '0'; else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10; else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10; if (d < 0 || d >= base || (width != 0 && width-- == 0)) break; val = val * base + d; s++; }
            if (!suppress) { if (is_long == 0) *va_arg(ap, unsigned int *) = (unsigned int)val; else *va_arg(ap, unsigned long *) = val; count++; }
            break;
        }
        case 's': {
            char *dest = suppress ? NULL : va_arg(ap, char *);
            while (*s == ' ' || *s == '\t') s++;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') { if (dest) *dest++ = *s; s++; }
            if (dest) *dest = '\0'; if (!suppress) count++;
            break;
        }
        case 'c': { char *dest = suppress ? NULL : va_arg(ap, char *); if (dest) *dest = *s; s++; if (!suppress) count++; break; }
        case '%': if (*s != '%') goto fdone; s++; break;
        default: goto fdone;
        }
        format++;
    }
fdone:
    va_end(ap);
    return count;
}

int vfscanf(FILE *stream, const char *format, va_list ap) {
    char buf[4096];
    int n = 0;
    int c;
    while (n < (int)sizeof(buf) - 1 && (c = fgetc(stream)) != EOF) {
        buf[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return EOF;
    buf[n] = '\0';
    return vsscanf(buf, format, ap);
}

int vscanf(const char *format, va_list ap) {
    return vfscanf(stdin, format, ap);
}

int vsscanf(const char *str, const char *format, va_list ap) {
    const char *s = str;
    int count = 0;

    while (*format && *s) {
        while (*format == ' ' || *format == '\t') { format++; while (*s == ' ' || *s == '\t') s++; }
        if (*format == '\0') break;
        if (*format != '%') { if (*s != *format) break; s++; format++; continue; }
        format++;

        int suppress = 0;
        if (*format == '*') { suppress = 1; format++; }

        int width = 0;
        while (*format >= '0' && *format <= '9') { width = width * 10 + (*format - '0'); format++; }

        int is_long = 0;
        if (*format == 'l') { is_long = 1; format++; if (*format == 'l') { is_long = 2; format++; } }
        else if (*format == 'h') { format++; if (*format == 'h') format++; }
        else if (*format == 'z') { format++; }

        while (*s == ' ' || *s == '\t') s++;

        switch (*format) {
        case 'd': case 'i': {
            long val = 0; int neg = 0;
            if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
            if (!(*s >= '0' && *s <= '9')) goto done;
            while (*s >= '0' && *s <= '9' && (width == 0 || width-- > 0)) { val = val * 10 + (*s - '0'); s++; }
            if (!suppress) { if (is_long == 0) *va_arg(ap, int *) = neg ? -(int)val : (int)val; else if (is_long == 1) *va_arg(ap, long *) = neg ? -val : val; else *va_arg(ap, long long *) = neg ? -val : val; count++; }
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            int base = (*format == 'x' || *format == 'X') ? 16 : (*format == 'o') ? 8 : 10;
            unsigned long val = 0;
            if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            while (*s) { int d = -1; if (*s >= '0' && *s <= '9') d = *s - '0'; else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10; else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10; if (d < 0 || d >= base || (width != 0 && width-- == 0)) break; val = val * base + d; s++; }
            if (!suppress) { if (is_long == 0) *va_arg(ap, unsigned int *) = (unsigned int)val; else if (is_long == 1) *va_arg(ap, unsigned long *) = val; else *va_arg(ap, unsigned long long *) = val; count++; }
            break;
        }
        case 's': {
            char *dest = suppress ? NULL : va_arg(ap, char *);
            while (*s == ' ' || *s == '\t') s++;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') { if (dest) *dest++ = *s; s++; }
            if (dest) *dest = '\0'; if (!suppress) count++;
            break;
        }
        case 'c': { char *dest = suppress ? NULL : va_arg(ap, char *); if (dest) *dest = *s; s++; if (!suppress) count++; break; }
        case '%': if (*s != '%') goto done; s++; break;
        default: goto done;
        }
        format++;
    }
done:
    return count;
}

int asprintf(char **strp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vasprintf(strp, fmt, ap);
    va_end(ap);
    return ret;
}

int vasprintf(char **strp, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (len < 0) { *strp = NULL; return -1; }
    char *result = malloc(len + 1);
    if (!result) { *strp = NULL; return -1; }
    vsnprintf(result, len + 1, fmt, ap);
    *strp = result;
    return len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }

    if (!*lineptr) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t newsize = *n * 2;
            char *newbuf = realloc(*lineptr, newsize);
            if (!newbuf) return -1;
            *lineptr = newbuf;
            *n = newsize;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }

    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/* Temporary file stubs */
FILE *tmpfile(void) {
    char template[] = "/tmp/tmpfile.XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return NULL;
    unlink(template);
    return fdopen(fd, "w+");
}

char *tmpnam(char *s) {
    static char buf[L_tmpnam];
    if (!s) s = buf;
    snprintf(s, L_tmpnam, "/tmp/tmpnam.XXXXXX");
    return s;
}

/* ============================================================================
 * STDIO FUNCTION IMPLEMENTATIONS
 * ============================================================================ */

/* --- errno --- */
static int __errno_storage = 0;
int *__errno_location(void) { return &__errno_storage; }

/* --- vsnprintf (delegates to our snprintf) --- */
int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    if (size == 0) return 0;
    char tmp[4096];
    size_t n = (size < sizeof(tmp)) ? size : sizeof(tmp);
    int ret = snprintf(tmp, n, format, ap);
    if (ret >= 0 && (size_t)ret < n) {
        memcpy(str, tmp, ret + 1);
    } else if (n > 0) {
        memcpy(str, tmp, n - 1);
        str[n - 1] = '\0';
    }
    return ret;
}

/* --- clearerr / feof / ferror --- */
void clearerr(FILE *stream) {
    if (stream) { stream->eof = 0; stream->error = 0; }
}

int feof(FILE *stream) {
    return stream ? stream->eof : 1;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

/* --- fflush --- */
int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

/* --- fgetc --- */
int fgetc(FILE *stream) {
    if (!stream) return EOF;
    unsigned char c;
    ssize_t r = read(stream->fd, &c, 1);
    if (r <= 0) { stream->eof = 1; return EOF; }
    return c;
}

/* --- fgets --- */
char *fgets(char *s, int n, FILE *stream) {
    if (!s || n <= 0 || !stream) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(stream);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

/* --- fputc --- */
int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    ssize_t r = write(stream->fd, &ch, 1);
    return (r == 1) ? c : EOF;
}

/* --- fputs --- */
int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    ssize_t r = write(stream->fd, s, len);
    return (r == (ssize_t)len) ? 0 : EOF;
}

/* --- fread --- */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t r = read(stream->fd, ptr, total);
    if (r <= 0) return 0;
    return (size_t)r / size;
}

/* --- fwrite --- */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    ssize_t r = write(stream->fd, ptr, total);
    if (r < 0) return 0;
    return (size_t)r / size;
}

/* --- fseek --- */
int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    stream->eof = 0;
    return lseek(stream->fd, offset, whence) == (off_t)-1 ? -1 : 0;
}

/* --- ftell --- */
long ftell(FILE *stream) {
    if (!stream) return -1;
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

/* --- fclose --- */
int fclose(FILE *stream) {
    if (!stream) return EOF;
    return close(stream->fd);
}

/* --- fdopen --- */
FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) return NULL;
    static FILE f;
    f.fd = fd;
    f.flags = 0;
    f.buffer = NULL;
    f.buf_size = 0;
    f.buf_pos = 0;
    f.buf_len = 0;
    f.error = 0;
    f.eof = 0;
    f.unget = -1;
    return &f;
}

/* --- qsort (insertion sort) --- */
static void __qsort_swap(char *a, char *b, size_t size) {
    while (size--) { char tmp = *a; *a = *b; *b = tmp; a++; b++; }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (!base || nmemb < 2 || size == 0) return;
    char *arr = (char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            if (compar(arr + (j - 1) * size, arr + j * size) > 0) {
                __qsort_swap(arr + (j - 1) * size, arr + j * size, size);
            } else { break; }
        }
    }
}

/* --- mkstemp (stub) --- */
int mkstemp(char *template) {
    (void)template;
    errno = ENOSYS;
    return -1;
}

/* --- time --- */
time_t time(time_t *t) {
    if (t) *t = 0;
    return 0;
}

/* --- readlink --- */
ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

/* --- lstat --- */
int lstat(const char *path, struct stat *buf) {
    return stat(path, buf);
}

/* --- isatty --- */
int isatty(int fd) {
    (void)fd;
    return 0;
}

/* File locking stubs (single-threaded, no-op) */
void flockfile(FILE *stream) { (void)stream; }
int ftrylockfile(FILE *stream) { (void)stream; return 0; }
void funlockfile(FILE *stream) { (void)stream; }

/* Unlocked I/O stubs (delegate to locked versions) */
int getc_unlocked(FILE *stream) { return fgetc(stream); }
int getchar_unlocked(void) { return fgetc(stdin); }
int putc_unlocked(int c, FILE *stream) { return fputc(c, stream); }
int putchar_unlocked(int c) { return fputc(c, stdout); }
void clearerr_unlocked(FILE *stream) { clearerr(stream); }
int feof_unlocked(FILE *stream) { return feof(stream); }
int ferror_unlocked(FILE *stream) { return ferror(stream); }
int fileno_unlocked(FILE *stream) { return fileno(stream); }
int fflush_unlocked(FILE *stream) { return fflush(stream); }
int fgetc_unlocked(FILE *stream) { return fgetc(stream); }
int fputc_unlocked(int c, FILE *stream) { return fputc(c, stream); }
size_t fread_unlocked(void *ptr, size_t size, size_t n, FILE *stream) { return fread(ptr, size, n, stream); }
size_t fwrite_unlocked(const void *ptr, size_t size, size_t n, FILE *stream) { return fwrite(ptr, size, n, stream); }
char *fgets_unlocked(char *s, int n, FILE *stream) { return fgets(s, n, stream); }
int fputs_unlocked(const char *s, FILE *stream) { return fputs(s, stream); }

/* Large file support stubs */
int fseeko(FILE *stream, off_t offset, int whence) { return fseek(stream, offset, whence); }
off_t ftello(FILE *stream) { return ftell(stream); }

/* popen/pclose stubs */
FILE *popen(const char *command, const char *type) {
    (void)command; (void)type;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream) {
    if (stream) fclose(stream);
    return -1;
}

/* ============================================================================
 * TIME STUBS - strftime, localtime, mktime, etc.
 * ============================================================================ */

/* Days in each month (non-leap year) */
static const int __days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int __is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int __days_in_year(int year) {
    return __is_leap_year(year) ? 366 : 365;
}

/* Seconds since epoch for a given broken-down time (UTC) */
time_t mktime(struct tm *tm) {
    if (!tm) return -1;

    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon;
    int day = tm->tm_mday - 1;

    /* Days from 1970 */
    long days = 0;
    for (int y = 1970; y < year; y++) days += __days_in_year(y);
    for (int m = 0; m < mon; m++) days += __days_in_month[m];
    if (mon >= 2 && __is_leap_year(year)) days++;
    days += day;

    time_t t = days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;

    /* Compute tm_wday and tm_yday */
    int jdn = (int)days + 2440588; /* Julian day number for epoch */
    tm->tm_wday = (jdn + 1) % 7;

    int yday = 0;
    for (int m = 0; m < mon; m++) yday += __days_in_month[m];
    if (mon >= 2 && __is_leap_year(year)) yday++;
    yday += day;
    tm->tm_yday = yday;

    return t;
}

time_t timegm(struct tm *tm) {
    return mktime(tm);
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

clock_t clock(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
        return (clock_t)(ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / (1000000000L / CLOCKS_PER_SEC));
    return 0;
}

static struct tm __tm_buf;

static struct tm *__fill_tm(time_t t, struct tm *result) {
    long days = t / 86400;
    long rem = t % 86400;

    result->tm_hour = rem / 3600;
    rem %= 3600;
    result->tm_min = rem / 60;
    result->tm_sec = rem % 60;

    /* Day of week: Jan 1 1970 was Thursday (4) */
    int jdn = (int)days + 2440588;
    result->tm_wday = (jdn + 1) % 7;

    /* Year and day of year */
    int year = 1970;
    long remaining = days;
    while (remaining >= __days_in_year(year)) {
        remaining -= __days_in_year(year);
        year++;
    }
    result->tm_year = year - 1900;
    result->tm_yday = (int)remaining;

    /* Month and day of month */
    int mon = 0;
    while (mon < 12) {
        int dim = __days_in_month[mon];
        if (mon == 1 && __is_leap_year(year)) dim++;
        if (remaining < dim) break;
        remaining -= dim;
        mon++;
    }
    result->tm_mon = mon;
    result->tm_mday = (int)remaining + 1;
    result->tm_isdst = 0;
    result->tm_gmtoff = 0;
    result->tm_zone = "UTC";

    return result;
}

struct tm *gmtime(const time_t *timep) {
    if (!timep) return NULL;
    return __fill_tm(*timep, &__tm_buf);
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!timep || !result) return NULL;
    return __fill_tm(*timep, result);
}

struct tm *localtime(const time_t *timep) {
    /* Forest OS: treat local time as UTC for now */
    return gmtime(timep);
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    return gmtime_r(timep, result);
}

static const char *__day_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *__month_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

char *asctime(const struct tm *tm) {
    static char buf[64];
    if (!tm) return NULL;
    snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %d",
             __day_names[tm->tm_wday], __month_names[tm->tm_mon],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) return NULL;
    snprintf(buf, 64, "%s %s %2d %02d:%02d:%02d %d",
             __day_names[tm->tm_wday], __month_names[tm->tm_mon],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *timep) {
    struct tm *tm = localtime(timep);
    return tm ? asctime(tm) : NULL;
}

char *ctime_r(const time_t *timep, char *buf) {
    struct tm tm;
    struct tm *t = localtime_r(timep, &tm);
    return t ? asctime_r(t, buf) : NULL;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || !max || !format || !tm) return 0;

    char *out = s;
    char *end = s + max - 1;

    while (*format && out < end) {
        if (*format != '%') { *out++ = *format++; continue; }
        format++;

        /* Handle padding */
        char pad = ' ';
        if (*format == '0') { pad = '0'; format++; }

        /* Width */
        int width = 0;
        while (*format >= '0' && *format <= '9') { width = width * 10 + (*format - '0'); format++; }

        char tmp[32];
        switch (*format) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'p': snprintf(tmp, sizeof(tmp), "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'A': snprintf(tmp, sizeof(tmp), "%%s", __day_names[tm->tm_wday]); break;
        case 'a': snprintf(tmp, sizeof(tmp), "%s", __day_names[tm->tm_wday]); break;
        case 'B': snprintf(tmp, sizeof(tmp), "%s", __month_names[tm->tm_mon]); break;
        case 'b': snprintf(tmp, sizeof(tmp), "%s", __month_names[tm->tm_mon]); break;
        case 'n': snprintf(tmp, sizeof(tmp), "\n"); break;
        case 't': snprintf(tmp, sizeof(tmp), "\t"); break;
        case '%': snprintf(tmp, sizeof(tmp), "%%"); break;
        default: tmp[0] = '\0'; break;
        }

        for (char *p = tmp; *p && out < end; p++) *out++ = *p;
        if (*format) format++;
    }
    *out = '\0';
    return (size_t)(out - s);
}

char *strptime(const char *s, const char *format, struct tm *tm) {
    if (!s || !format || !tm) return NULL;

    while (*format && *s) {
        if (*format != '%') {
            if (*s != *format) return NULL;
            s++; format++; continue;
        }
        format++;

        /* Skip whitespace in input */
        while (*s == ' ' || *s == '\t') s++;

        int val = 0;
        const char *start = s;

        switch (*format) {
        case 'Y': {
            val = 0;
            for (int i = 0; i < 4 && *s >= '0' && *s <= '9'; i++) { val = val * 10 + (*s - '0'); s++; }
            tm->tm_year = val - 1900;
            break;
        }
        case 'y':
            val = 0;
            for (int i = 0; i < 2 && *s >= '0' && *s <= '9'; i++) { val = val * 10 + (*s - '0'); s++; }
            tm->tm_year = val + (val < 68 ? 100 : 0);
            break;
        case 'm':
            val = 0;
            for (int i = 0; i < 2 && *s >= '0' && *s <= '9'; i++) { val = val * 10 + (*s - '0'); s++; }
            tm->tm_mon = val - 1;
            break;
        case 'd': case 'e':
            val = 0;
            while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
            tm->tm_mday = val;
            break;
        case 'H':
            val = 0;
            for (int i = 0; i < 2 && *s >= '0' && *s <= '9'; i++) { val = val * 10 + (*s - '0'); s++; }
            tm->tm_hour = val;
            break;
        case 'M':
            val = 0;
            for (int i = 0; i < 2 && *s >= '0' && *s <= '9'; i++) { val = val * 10 + (*s - '0'); s++; }
            tm->tm_min = val;
            break;
        case 'S':
            val = 0;
            for (int i = 0; i < 2 && *s >= '0' && *s <= '9'; i++) { val = val * 10 + (*s - '0'); s++; }
            tm->tm_sec = val;
            break;
        case 'T':
            /* %H:%M:%S */
            {
                char *r = (char *)s;
                r = strptime(r, "%H:%M:%S", tm);
                if (!r) return NULL;
                s = r;
            }
            format++; /* skip the T in format (it's part of %T) */
            break;
        case 'p':
            if (s[0] == 'A' || s[0] == 'a') {
                s += 2; /* AM or am */
                if (tm->tm_hour == 12) tm->tm_hour = 0;
            } else if (s[0] == 'P' || s[0] == 'p') {
                s += 2; /* PM or pm */
                if (tm->tm_hour != 12) tm->tm_hour += 12;
            }
            break;
        case '%':
            if (*s != '%') return NULL;
            s++; break;
        default:
            break;
        }
        if (s == start && *format != 'p') return NULL;
        format++;
    }
    return (char *)s;
}

/* ============================================================================
 * PWD/GRP STUBS - hardcoded root user, everything else returns NULL
 * ============================================================================ */

static struct passwd __root_passwd = {
    .pw_name = "root",
    .pw_passwd = "x",
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = "root",
    .pw_dir = "/root",
    .pw_shell = "/bin/sh"
};

struct passwd *getpwnam(const char *name) {
    if (name && strcmp(name, "root") == 0) return &__root_passwd;
    return NULL;
}

struct passwd *getpwuid(uid_t uid) {
    if (uid == 0) return &__root_passwd;
    return NULL;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    (void)buf; (void)buflen;
    struct passwd *pw = getpwnam(name);
    if (pw) { *pwd = *pw; *result = pwd; }
    else *result = NULL;
    return *result ? 0 : ENOENT;
}

static struct group __root_group = {
    .gr_name = "root",
    .gr_passwd = "x",
    .gr_gid = 0,
    .gr_mem = NULL
};

struct group *getgrnam(const char *name) {
    if (name && strcmp(name, "root") == 0) return &__root_group;
    return NULL;
}

struct group *getgrgid(gid_t gid) {
    if (gid == 0) return &__root_group;
    return NULL;
}

/* ============================================================================
 * MATH STUBS - basic implementations using compiler builtins where possible
 * ============================================================================ */

double sin(double x) { return __builtin_sin(x); }
double cos(double x) { return __builtin_cos(x); }
double tan(double x) { return __builtin_tan(x); }
double asin(double x) { return __builtin_asin(x); }
double acos(double x) { return __builtin_acos(x); }
double atan(double x) { return __builtin_atan(x); }
double atan2(double y, double x) { return __builtin_atan2(y, x); }

float sinf(float x) { return __builtin_sinf(x); }
float cosf(float x) { return __builtin_cosf(x); }
float tanf(float x) { return __builtin_tanf(x); }
float asinf(float x) { return __builtin_asinf(x); }
float acosf(float x) { return __builtin_acosf(x); }
float atanf(float x) { return __builtin_atanf(x); }
float atan2f(float y, float x) { return __builtin_atan2f(y, x); }

long double sinl(long double x) { return __builtin_sinl(x); }
long double cosl(long double x) { return __builtin_cosl(x); }
long double tanl(long double x) { return __builtin_tanl(x); }
long double asinl(long double x) { return __builtin_asinl(x); }
long double acosl(long double x) { return __builtin_acosl(x); }
long double atanl(long double x) { return __builtin_atanl(x); }
long double atan2l(long double y, long double x) { return __builtin_atan2l(y, x); }

double sinh(double x) { return __builtin_sinh(x); }
double cosh(double x) { return __builtin_cosh(x); }
double tanh(double x) { return __builtin_tanh(x); }
double asinh(double x) { return __builtin_asinh(x); }
double acosh(double x) { return __builtin_acosh(x); }
double atanh(double x) { return __builtin_atanh(x); }

float sinhf(float x) { return __builtin_sinhf(x); }
float coshf(float x) { return __builtin_coshf(x); }
float tanhf(float x) { return __builtin_tanhf(x); }
float asinhf(float x) { return __builtin_asinhf(x); }
float acoshf(float x) { return __builtin_acoshf(x); }
float atanhf(float x) { return __builtin_atanhf(x); }

long double sinhl(long double x) { return __builtin_sinhl(x); }
long double coshl(long double x) { return __builtin_coshl(x); }
long double tanhl(long double x) { return __builtin_tanhl(x); }
long double asinhl(long double x) { return __builtin_asinhl(x); }
long double acoshl(long double x) { return __builtin_acoshl(x); }
long double atanhl(long double x) { return __builtin_atanhl(x); }

double exp(double x) { return __builtin_exp(x); }
double exp2(double x) { return __builtin_exp2(x); }
double expm1(double x) { return __builtin_expm1(x); }
double log(double x) { return __builtin_log(x); }
double log10(double x) { return __builtin_log10(x); }
double log2(double x) { return __builtin_log2(x); }
double log1p(double x) { return __builtin_log1p(x); }

float expf(float x) { return __builtin_expf(x); }
float exp2f(float x) { return __builtin_exp2f(x); }
float expm1f(float x) { return __builtin_expm1f(x); }
float logf(float x) { return __builtin_logf(x); }
float log10f(float x) { return __builtin_log10f(x); }
float log2f(float x) { return __builtin_log2f(x); }
float log1pf(float x) { return __builtin_log1pf(x); }

long double expl(long double x) { return __builtin_expl(x); }
long double exp2l(long double x) { return __builtin_exp2l(x); }
long double expm1l(long double x) { return __builtin_expm1l(x); }
long double logl(long double x) { return __builtin_logl(x); }
long double log10l(long double x) { return __builtin_log10l(x); }
long double log2l(long double x) { return __builtin_log2l(x); }
long double log1pl(long double x) { return __builtin_log1pl(x); }

double pow(double x, double y) { return __builtin_pow(x, y); }
double sqrt(double x) { return __builtin_sqrt(x); }
double cbrt(double x) { return __builtin_cbrt(x); }
double hypot(double x, double y) { return __builtin_hypot(x, y); }

float powf(float x, float y) { return __builtin_powf(x, y); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
float cbrtf(float x) { return __builtin_cbrtf(x); }
float hypotf(float x, float y) { return __builtin_hypotf(x, y); }

long double powl(long double x, long double y) { return __builtin_powl(x, y); }
long double sqrtl(long double x) { return __builtin_sqrtl(x); }
long double cbrtl(long double x) { return __builtin_cbrtl(x); }
long double hypotl(long double x, long double y) { return __builtin_hypotl(x, y); }

double erf(double x) { return __builtin_erf(x); }
double erfc(double x) { return __builtin_erfc(x); }
double lgamma(double x) { return __builtin_lgamma(x); }
double tgamma(double x) { return __builtin_tgamma(x); }

float erff(float x) { return __builtin_erff(x); }
float erfcf(float x) { return __builtin_erfcf(x); }
float lgammaf(float x) { return __builtin_lgammaf(x); }
float tgammaf(float x) { return __builtin_tgammaf(x); }

long double erfl(long double x) { return __builtin_erfl(x); }
long double erfcl(long double x) { return __builtin_erfcl(x); }
long double lgammal(long double x) { return __builtin_lgammal(x); }
long double tgammal(long double x) { return __builtin_tgammal(x); }

double ceil(double x) { return __builtin_ceil(x); }
double floor(double x) { return __builtin_floor(x); }
double trunc(double x) { return __builtin_trunc(x); }
double round(double x) { return __builtin_round(x); }
long lround(double x) { return __builtin_lround(x); }
long long llround(double x) { return __builtin_llround(x); }
double nearbyint(double x) { return __builtin_nearbyint(x); }
double rint(double x) { return __builtin_rint(x); }
long lrint(double x) { return __builtin_lrint(x); }
long long llrint(double x) { return __builtin_llrint(x); }

float ceilf(float x) { return __builtin_ceilf(x); }
float floorf(float x) { return __builtin_floorf(x); }
float truncf(float x) { return __builtin_truncf(x); }
float roundf(float x) { return __builtin_roundf(x); }
long lroundf(float x) { return __builtin_lroundf(x); }
long long llroundf(float x) { return __builtin_llroundf(x); }
float nearbyintf(float x) { return __builtin_nearbyintf(x); }
float rintf(float x) { return __builtin_rintf(x); }
long lrintf(float x) { return __builtin_lrintf(x); }
long long llrintf(float x) { return __builtin_llrintf(x); }

long double ceill(long double x) { return __builtin_ceill(x); }
long double floorl(long double x) { return __builtin_floorl(x); }
long double truncl(long double x) { return __builtin_truncl(x); }
long double roundl(long double x) { return __builtin_roundl(x); }
long lroundl(long double x) { return __builtin_lroundl(x); }
long long llroundl(long double x) { return __builtin_llroundl(x); }
long double nearbyintl(long double x) { return __builtin_nearbyintl(x); }
long double rintl(long double x) { return __builtin_rintl(x); }
long lrintl(long double x) { return __builtin_lrintl(x); }
long long llrintl(long double x) { return __builtin_llrintl(x); }

double fmod(double x, double y) { return __builtin_fmod(x, y); }
double remainder(double x, double y) { return __builtin_remainder(x, y); }
double remquo(double x, double y, int *quo) { return __builtin_remquo(x, y, quo); }

float fmodf(float x, float y) { return __builtin_fmodf(x, y); }
float remainderf(float x, float y) { return __builtin_remainderf(x, y); }
float remquof(float x, float y, int *quo) { return __builtin_remquof(x, y, quo); }

long double fmodl(long double x, long double y) { return __builtin_fmodl(x, y); }
long double remainderl(long double x, long double y) { return __builtin_remainderl(x, y); }
long double remquol(long double x, long double y, int *quo) { return __builtin_remquol(x, y, quo); }

double copysign(double x, double y) { return __builtin_copysign(x, y); }
double nextafter(double x, double y) { return __builtin_nextafter(x, y); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
float nextafterf(float x, float y) { return __builtin_nextafterf(x, y); }
long double copysignl(long double x, long double y) { return __builtin_copysignl(x, y); }
long double nextafterl(long double x, long double y) { return __builtin_nextafterl(x, y); }

double fdim(double x, double y) { return __builtin_fdim(x, y); }
double fmax(double x, double y) { return __builtin_fmax(x, y); }
double fmin(double x, double y) { return __builtin_fmin(x, y); }
float fdimf(float x, float y) { return __builtin_fdimf(x, y); }
float fmaxf(float x, float y) { return __builtin_fmaxf(x, y); }
float fminf(float x, float y) { return __builtin_fminf(x, y); }
long double fdiml(long double x, long double y) { return __builtin_fdiml(x, y); }
long double fmaxl(long double x, long double y) { return __builtin_fmaxl(x, y); }
long double fminl(long double x, long double y) { return __builtin_fminl(x, y); }

double fma(double x, double y, double z) { return __builtin_fma(x, y, z); }
float fmaf(float x, float y, float z) { return __builtin_fmaf(x, y, z); }
long double fmal(long double x, long double y, long double z) { return __builtin_fmal(x, y, z); }

double fabs(double x) { return __builtin_fabs(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
long double fabsl(long double x) { return __builtin_fabsl(x); }

double frexp(double value, int *exp) { return __builtin_frexp(value, exp); }
double ldexp(double x, int exp) { return __builtin_ldexp(x, exp); }
double modf(double value, double *iptr) { return __builtin_modf(value, iptr); }
double scalbn(double x, int n) { return __builtin_scalbn(x, n); }
double scalbln(double x, long n) { return __builtin_scalbln(x, n); }
int ilogb(double x) { return __builtin_ilogb(x); }
double logb(double x) { return __builtin_logb(x); }

float frexpf(float value, int *exp) { return __builtin_frexpf(value, exp); }
float ldexpf(float x, int exp) { return __builtin_ldexpf(x, exp); }
float modff(float value, float *iptr) { return __builtin_modff(value, iptr); }
float scalbnf(float x, int n) { return __builtin_scalbnf(x, n); }
float scalblnf(float x, long n) { return __builtin_scalblnf(x, n); }
int ilogbf(float x) { return __builtin_ilogbf(x); }
float logbf(float x) { return __builtin_logbf(x); }

long double frexpl(long double value, int *exp) { return __builtin_frexpl(value, exp); }
long double ldexpl(long double x, int exp) { return __builtin_ldexpl(x, exp); }
long double modfl(long double value, long double *iptr) { return __builtin_modfl(value, iptr); }
long double scalbnl(long double x, int n) { return __builtin_scalbnl(x, n); }
long double scalblnl(long double x, long n) { return __builtin_scalblnl(x, n); }
int ilogbl(long double x) { return __builtin_ilogbl(x); }
long double logbl(long double x) { return __builtin_logbl(x); }

/* Bessel function stubs (return 0 - not critical for apps) */
double j0(double x) { (void)x; return 0.0; }
double j1(double x) { (void)x; return 0.0; }
double jn(int n, double x) { (void)n; (void)x; return 0.0; }
double y0(double x) { (void)x; return 0.0; }
double y1(double x) { (void)x; return 0.0; }
double yn(int n, double x) { (void)n; (void)x; return 0.0; }

int signgam = 0;

/* nan/nanf/nanl */
double nan(const char *tagp) { (void)tagp; return NAN; }
float nanf(const char *tagp) { (void)tagp; return NAN; }
long double nanl(const char *tagp) { (void)tagp; return NAN; }

double nexttoward(double x, long double y) { (void)y; return x; }
float nexttowardf(float x, long double y) { (void)y; return x; }
long double nexttowardl(long double x, long double y) { return (long double)x + (y - (long double)x); }

/* ============================================================================
 * DIRENT STUBS - scandir, alphasort
 * ============================================================================ */

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int alphasort64(const struct dirent64 **a, const struct dirent64 **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int versionsort(const struct dirent **a, const struct dirent **b) {
    return alphasort(a, b);
}

int versionsort64(const struct dirent64 **a, const struct dirent64 **b) {
    return alphasort64(a, b);
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*select)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    DIR *dir = opendir(dirp);
    if (!dir) return -1;

    int cap = 64;
    int count = 0;
    struct dirent **list = malloc(cap * sizeof(struct dirent *));
    if (!list) { closedir(dir); return -1; }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (select && !select(entry)) continue;

        if (count >= cap) {
            cap *= 2;
            struct dirent **newlist = realloc(list, cap * sizeof(struct dirent *));
            if (!newlist) { free(list); closedir(dir); return -1; }
            list = newlist;
        }

        struct dirent *copy = malloc(sizeof(struct dirent));
        if (!copy) { free(list); closedir(dir); return -1; }
        *copy = *entry;
        list[count++] = copy;
    }
    closedir(dir);

    if (compar) qsort(list, count, sizeof(struct dirent *), (int (*)(const void *, const void *))compar);

    *namelist = list;
    return count;
}

int dirfd(DIR *dirp) {
    (void)dirp;
    return -1;
}

/* ============================================================================
 * FENV STUBS - no-op implementations
 * ============================================================================ */

int feclearexcept(int excepts) { (void)excepts; return 0; }
int fegetexceptflag(fexcept_t *flagp, int excepts) { (void)flagp; (void)excepts; return 0; }
int feraiseexcept(int excepts) { (void)excepts; return 0; }
int fesetexceptflag(const fexcept_t *flagp, int excepts) { (void)flagp; (void)excepts; return 0; }
int fetestexcept(int excepts) { (void)excepts; return 0; }
int fegetround(void) { return 0; /* FE_TONEAREST */ }
int fesetround(int rounding_mode) { (void)rounding_mode; return 0; }
/* fegetenv and fesetenv provided by fenv.h (static inline) */
int feholdexcept(fenv_t *envp) { (void)envp; return 0; }
int feupdateenv(const fenv_t *envp) { (void)envp; return 0; }

/* ============================================================================
 * SETJMP STUBS (require platform-specific assembly, stub for now)
 * ============================================================================ */

/* These require architecture-specific assembly and cannot be stubbed.
 * Apps that use setjmp/longjmp need proper implementations.
 * For now, provide a placeholder that will fail safely. */
int __setjmp_stub(void) {
    return 0;
}

/* ============================================================================
 * TERMIOS STUBS - basic ioctl passthrough
 * ============================================================================ */

#ifndef TCGETS
#define TCGETS 0x5401
#endif
#ifndef TCSETS
#define TCSETS 0x5402
#endif

int tcgetattr(int fd, struct termios *termios_p) {
    return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    (void)optional_actions;
    return ioctl(fd, TCSETS, termios_p);
}

int tcdrain(int fd) { return fsync(fd); }
int tcflow(int fd, int action) { (void)fd; (void)action; return 0; }
int tcflush(int fd, int queue_selector) { (void)fd; (void)queue_selector; return 0; }
int tcsendbreak(int fd, int duration) { (void)fd; (void)duration; return 0; }

speed_t cfgetispeed(const struct termios *termios_p) { (void)termios_p; return 0; }
speed_t cfgetospeed(const struct termios *termios_p) { (void)termios_p; return 0; }
int cfsetispeed(struct termios *termios_p, speed_t speed) { (void)termios_p; (void)speed; return 0; }
int cfsetospeed(struct termios *termios_p, speed_t speed) { (void)termios_p; (void)speed; return 0; }

/* ============================================================================
 * DLFCN STUBS
 * ============================================================================ */

void *dlopen(const char *filename, int flags) { (void)filename; (void)flags; errno = ENOSYS; return NULL; }
void *dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; errno = ENOSYS; return NULL; }
int dlclose(void *handle) { (void)handle; errno = ENOSYS; return -1; }
char *dlerror(void) { return "dynamic linking not supported"; }

/* ============================================================================
 * SYSCALL STUBS - needed by libforest.a
 * ============================================================================ */

int brk(void *addr) { (void)addr; return -1; }

void _exit(int status) {
    (void)status;
    while(1) { asm volatile("hlt"); }
}

off_t lseek(int fd, off_t offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    return -1;
}

int mkdir(const char *pathname, mode_t mode) {
    (void)pathname; (void)mode;
    errno = ENOSYS;
    return -1;
}

int rmdir(const char *pathname) {
    (void)pathname;
    errno = ENOSYS;
    return -1;
}

int unlink(const char *pathname) {
    (void)pathname;
    errno = ENOSYS;
    return -1;
}

int kill(int pid, int sig) {
    (void)pid; (void)sig;
    errno = ENOSYS;
    return -1;
}

int fsync(int fd) { (void)fd; return 0; }

/* ============================================================================
 * GLOBAL STUBS
 * ============================================================================ */

/* stderr is already defined in libc.a */

/* --- clock_gettime (stub) --- */
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id; (void)tp;
    return -1;
}

/* --- opendir / readdir / closedir (stubs) --- */
DIR *opendir(const char *name) { (void)name; return NULL; }
struct dirent *readdir(DIR *dir) { (void)dir; return NULL; }
int closedir(DIR *dir) { (void)dir; return -1; }

/* --- stat (via i386 syscall 18) --- */
#ifndef SYS_STAT_I386
#define SYS_STAT_I386 18
#endif
int stat(const char *pathname, struct stat *statbuf) {
    int ret;
    __asm__ __volatile__("int $0x80"
        : "=a"(ret)
        : "a"(SYS_STAT_I386), "b"(pathname), "c"(statbuf)
        : "memory");
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}
int fstat(int fd, struct stat *statbuf) { (void)fd; (void)statbuf; errno = ENOSYS; return -1; }

/* --- ioctl (stub, provided by sys/ioctl.h declaration) --- */
#include <stdarg.h>
int ioctl(int fd, unsigned long request, ...) {
    (void)fd; (void)request;
    errno = ENOTTY;
    return -1;
}

/* --- get_tasks (stub for /proc-equivalent) --- */
#include <sys/types.h>
ssize_t get_tasks(void *buf, size_t max_entries) {
    (void)buf; (void)max_entries;
    errno = ENOSYS;
    return -1;
}
