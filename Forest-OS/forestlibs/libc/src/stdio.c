/*
 * stdio.c - Standard I/O functions for Fern libc
 * 
 * This file implements buffered I/O operations on top of the
 * raw file descriptor syscalls.
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ============================================================================
 * STANDARD STREAMS
 * ============================================================================ */

static FILE __stdin = { .fd = STDIN_FILENO, .flags = 0, .buffer = NULL };
static FILE __stdout = { .fd = STDOUT_FILENO, .flags = 0, .buffer = NULL };
static FILE __stderr = { .fd = STDERR_FILENO, .flags = 0, .buffer = NULL };

FILE *stdin = &__stdin;
FILE *stdout = &__stdout;
FILE *stderr = &__stderr;

/* ============================================================================
 * FILE OPERATIONS
 * ============================================================================ */

FILE *fopen(const char *pathname, const char *mode) {
    if (!pathname || !mode) {
        errno = EINVAL;
        return NULL;
    }
    
    int flags = 0;
    
    /* Parse mode string */
    switch (mode[0]) {
        case 'r':
            flags = O_RDONLY;
            if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) {
                flags = O_RDWR;
            }
            break;
        case 'w':
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) {
                flags = O_RDWR | O_CREAT | O_TRUNC;
            }
            break;
        case 'a':
            flags = O_WRONLY | O_CREAT | O_APPEND;
            if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) {
                flags = O_RDWR | O_CREAT | O_APPEND;
            }
            break;
        default:
            errno = EINVAL;
            return NULL;
    }
    
    int fd = open(pathname, flags, 0666);
    if (fd < 0) {
        return NULL;
    }
    
    FILE *fp = malloc(sizeof(FILE));
    if (!fp) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    
    fp->fd = fd;
    fp->flags = flags;
    fp->buffer = NULL;
    fp->buf_size = 0;
    fp->buf_pos = 0;
    fp->buf_len = 0;
    fp->error = 0;
    fp->eof = 0;
    fp->unget = -1;
    
    return fp;
}

FILE *fdopen(int fd, const char *mode) {
    if (fd < 0 || !mode) {
        errno = EINVAL;
        return NULL;
    }
    
    FILE *fp = malloc(sizeof(FILE));
    if (!fp) {
        errno = ENOMEM;
        return NULL;
    }
    
    fp->fd = fd;
    fp->flags = 0;
    fp->buffer = NULL;
    fp->buf_size = 0;
    fp->buf_pos = 0;
    fp->buf_len = 0;
    fp->error = 0;
    fp->eof = 0;
    fp->unget = -1;
    
    return fp;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return NULL;
    }
    
    /* Close old file */
    if (stream->fd >= 0 && stream != stdin && stream != stdout && stream != stderr) {
        close(stream->fd);
    }
    
    /* Open new file */
    FILE *new_fp = fopen(pathname, mode);
    if (!new_fp) {
        return NULL;
    }
    
    /* Copy to existing stream */
    stream->fd = new_fp->fd;
    stream->flags = new_fp->flags;
    stream->error = 0;
    stream->eof = 0;
    stream->unget = -1;
    
    free(new_fp);
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return EOF;
    }
    
    int result = 0;
    
    /* Flush any buffered data */
    if (stream->buffer) {
        fflush(stream);
        free(stream->buffer);
    }
    
    /* Close the file descriptor */
    if (stream->fd >= 0) {
        if (close(stream->fd) < 0) {
            result = EOF;
        }
    }
    
    /* Don't free the standard streams */
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    
    return result;
}

int fflush(FILE *stream) {
    if (!stream) {
        /* Flush all streams - not implemented */
        return 0;
    }
    
    /* For now, just sync the file descriptor */
    if (stream->fd >= 0) {
        fsync(stream->fd);
    }
    
    return 0;
}

int fileno(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    return stream->fd;
}

/* ============================================================================
 * CHARACTER I/O
 * ============================================================================ */

int fgetc(FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return EOF;
    }
    
    /* Check for pushed-back character */
    if (stream->unget >= 0) {
        int c = stream->unget;
        stream->unget = -1;
        return c;
    }
    
    unsigned char c;
    ssize_t n = read(stream->fd, &c, 1);
    
    if (n < 0) {
        stream->error = 1;
        return EOF;
    }
    if (n == 0) {
        stream->eof = 1;
        return EOF;
    }
    
    return c;
}

int fputc(int c, FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return EOF;
    }
    
    unsigned char ch = (unsigned char)c;
    ssize_t n = write(stream->fd, &ch, 1);
    
    if (n < 0) {
        stream->error = 1;
        return EOF;
    }
    
    return c;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int getchar(void) {
    return fgetc(stdin);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF) {
        return EOF;
    }
    
    if (stream->unget >= 0) {
        return EOF;  /* Can only push back one character */
    }
    
    stream->unget = c;
    stream->eof = 0;
    return c;
}

/* ============================================================================
 * STRING I/O
 * ============================================================================ */

char *fgets(char *s, int n, FILE *stream) {
    if (!s || n <= 0 || !stream) {
        errno = EINVAL;
        return NULL;
    }
    
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) {
        errno = EINVAL;
        return EOF;
    }
    
    size_t len = strlen(s);
    ssize_t n = write(stream->fd, s, len);
    
    if (n < 0) {
        stream->error = 1;
        return EOF;
    }
    
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) {
        return EOF;
    }
    return fputc('\n', stdout);
}

char *gets(char *s) {
    /* DEPRECATED: Use fgets instead */
    if (!s) return NULL;
    
    int i = 0;
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        s[i++] = (char)c;
    }
    s[i] = '\0';
    return (c == EOF && i == 0) ? NULL : s;
}

/* ============================================================================
 * BLOCK I/O
 * ============================================================================ */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }
    
    size_t total = size * nmemb;
    ssize_t n = read(stream->fd, ptr, total);
    
    if (n < 0) {
        stream->error = 1;
        return 0;
    }
    if (n == 0) {
        stream->eof = 1;
    }
    
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }
    
    size_t total = size * nmemb;
    ssize_t n = write(stream->fd, ptr, total);
    
    if (n < 0) {
        stream->error = 1;
        return 0;
    }
    
    return (size_t)n / size;
}

/* ============================================================================
 * FILE POSITIONING
 * ============================================================================ */

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    
    off_t result = lseek(stream->fd, offset, whence);
    if (result < 0) {
        return -1;
    }
    
    stream->eof = 0;
    stream->unget = -1;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

void rewind(FILE *stream) {
    if (stream) {
        fseek(stream, 0, SEEK_SET);
        stream->error = 0;
    }
}

int fgetpos(FILE *stream, fpos_t *pos) {
    if (!stream || !pos) {
        errno = EINVAL;
        return -1;
    }
    
    *pos = ftell(stream);
    return (*pos < 0) ? -1 : 0;
}

int fsetpos(FILE *stream, const fpos_t *pos) {
    if (!stream || !pos) {
        errno = EINVAL;
        return -1;
    }
    
    return fseek(stream, *pos, SEEK_SET);
}

/* ============================================================================
 * ERROR HANDLING
 * ============================================================================ */

void clearerr(FILE *stream) {
    if (stream) {
        stream->error = 0;
        stream->eof = 0;
    }
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

/* ============================================================================
 * FORMATTED OUTPUT - printf family
 * ============================================================================ */

/* Internal printf implementation */
static void __format_string(char **buf, size_t *remaining, const char *str) {
    while (*str && *remaining > 0) {
        **buf = *str++;
        (*buf)++;
        (*remaining)--;
    }
}

static void __format_uint(char **buf, size_t *remaining, unsigned long long value, 
                         int base, int uppercase, int width, char pad) {
    char digits[32];
    int i = 0;
    
    if (value == 0) {
        digits[i++] = '0';
    } else {
        const char *hex = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
        while (value > 0) {
            digits[i++] = hex[value % base];
            value /= base;
        }
    }
    
    /* Padding */
    while (width > i && *remaining > 0) {
        **buf = pad;
        (*buf)++;
        (*remaining)--;
        width--;
    }
    
    /* Digits (in reverse) */
    while (i > 0 && *remaining > 0) {
        **buf = digits[--i];
        (*buf)++;
        (*remaining)--;
    }
}

static void __format_int(char **buf, size_t *remaining, long long value,
                        int width, char pad) {
    if (value < 0) {
        if (*remaining > 0) {
            **buf = '-';
            (*buf)++;
            (*remaining)--;
            width--;
        }
        value = -value;
    }
    __format_uint(buf, remaining, (unsigned long long)value, 10, 0, width, pad);
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    char *out = str;
    size_t remaining = size ? size - 1 : 0;
    int written = 0;
    
    while (*format) {
        if (*format != '%') {
            if (remaining > 0) {
                *out++ = *format;
                remaining--;
            }
            written++;
            format++;
            continue;
        }
        
        format++;  /* Skip '%' */
        
        /* Parse flags */
        char pad = ' ';
        if (*format == '0') {
            pad = '0';
            format++;
        }
        
        /* Parse width */
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }
        
        /* Parse length modifier */
        int long_flag = 0;
        int long_long_flag = 0;
        if (*format == 'l') {
            long_flag = 1;
            format++;
            if (*format == 'l') {
                long_long_flag = 1;
                format++;
            }
        } else if (*format == 'h') {
            format++;
            if (*format == 'h') {
                format++;
            }
        } else if (*format == 'z') {
            long_flag = (sizeof(size_t) == sizeof(long));
            format++;
        }
        
        /* Process conversion specifier */
        switch (*format) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t len = strlen(s);
                written += len;
                while (*s && remaining > 0) {
                    *out++ = *s++;
                    remaining--;
                }
                break;
            }
            
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (remaining > 0) {
                    *out++ = c;
                    remaining--;
                }
                written++;
                break;
            }
            
            case 'd':
            case 'i': {
                long long val;
                if (long_long_flag) {
                    val = va_arg(ap, long long);
                } else if (long_flag) {
                    val = va_arg(ap, long);
                } else {
                    val = va_arg(ap, int);
                }
                size_t before = remaining;
                __format_int(&out, &remaining, val, width, pad);
                written += before - remaining;
                break;
            }
            
            case 'u': {
                unsigned long long val;
                if (long_long_flag) {
                    val = va_arg(ap, unsigned long long);
                } else if (long_flag) {
                    val = va_arg(ap, unsigned long);
                } else {
                    val = va_arg(ap, unsigned int);
                }
                size_t before = remaining;
                __format_uint(&out, &remaining, val, 10, 0, width, pad);
                written += before - remaining;
                break;
            }
            
            case 'x': {
                unsigned long long val;
                if (long_long_flag) {
                    val = va_arg(ap, unsigned long long);
                } else if (long_flag) {
                    val = va_arg(ap, unsigned long);
                } else {
                    val = va_arg(ap, unsigned int);
                }
                size_t before = remaining;
                __format_uint(&out, &remaining, val, 16, 0, width, pad);
                written += before - remaining;
                break;
            }
            
            case 'X': {
                unsigned long long val;
                if (long_long_flag) {
                    val = va_arg(ap, unsigned long long);
                } else if (long_flag) {
                    val = va_arg(ap, unsigned long);
                } else {
                    val = va_arg(ap, unsigned int);
                }
                size_t before = remaining;
                __format_uint(&out, &remaining, val, 16, 1, width, pad);
                written += before - remaining;
                break;
            }
            
            case 'p': {
                void *ptr = va_arg(ap, void *);
                if (remaining >= 2) {
                    *out++ = '0';
                    *out++ = 'x';
                    remaining -= 2;
                    written += 2;
                }
                size_t before = remaining;
                __format_uint(&out, &remaining, (unsigned long long)(uintptr_t)ptr, 16, 0, 0, '0');
                written += before - remaining;
                break;
            }
            
            case '%':
                if (remaining > 0) {
                    *out++ = '%';
                    remaining--;
                }
                written++;
                break;
            
            default:
                if (remaining > 0) {
                    *out++ = '%';
                    remaining--;
                }
                if (remaining > 0 && *format) {
                    *out++ = *format;
                    remaining--;
                }
                written += 2;
                break;
        }
        
        if (*format) format++;
    }
    
    if (size > 0) {
        *out = '\0';
    }
    
    return written;
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, (size_t)-1, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vsnprintf(str, size, format, ap);
    va_end(ap);
    return result;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vsprintf(str, format, ap);
    va_end(ap);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    
    if (len > 0) {
        size_t to_write = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        ssize_t n = write(stream->fd, buf, to_write);
        if (n < 0) {
            stream->error = 1;
            return -1;
        }
    }
    
    return len;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vfprintf(stream, format, ap);
    va_end(ap);
    return result;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vprintf(format, ap);
    va_end(ap);
    return result;
}

int dprintf(int fd, const char *format, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    
    if (len > 0) {
        write(fd, buf, len);
    }
    
    return len;
}

int vdprintf(int fd, const char *format, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    
    if (len > 0) {
        write(fd, buf, len);
    }
    
    return len;
}

/* ============================================================================
 * FILE MANAGEMENT
 * ============================================================================ */

int remove(const char *pathname) {
    /* Try unlink first (for regular files) */
    if (unlink(pathname) == 0) {
        return 0;
    }
    
    /* If that fails with EISDIR, try rmdir */
    if (errno == EISDIR) {
        return rmdir(pathname);
    }
    
    return -1;
}

/* rename is implemented in syscalls.c */
