#include <stdarg.h>
#include <stdbool.h>
#include "include/libc/stdio.h"
#include "include/libc/string.h"

static void buffer_append(char** buf, size_t* remaining, char c) {
    if (*remaining == 0) {
        return;
    }
    **buf = c;
    (*buf)++;
    (*remaining)--;
}

static void format_string(char** buf, size_t* remaining, const char* str) {
    if (!str) {
        str = "(null)";
    }
    while (*str) {
        buffer_append(buf, remaining, *str++);
    }
}

__attribute__((unused)) static void format_uint(char** buf, size_t* remaining, unsigned long value,
                        unsigned int base, bool uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    unsigned int i = 0;

    if (value == 0) {
        buffer_append(buf, remaining, '0');
        return;
    }

    while (value > 0 && i < sizeof(tmp)) {
        unsigned int digit = (unsigned int)(value % base);
        tmp[i++] = digits[digit];
        value /= base;
    }

    while (i > 0) {
        buffer_append(buf, remaining, tmp[--i]);
    }
}

static void format_uint_padded(char** buf, size_t* remaining, unsigned long value,
                               unsigned int base, bool uppercase, int width, char pad_char) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int i = 0;

    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0 && i < (int)sizeof(tmp)) {
            unsigned int digit = (unsigned int)(value % base);
            tmp[i++] = digits[digit];
            value /= base;
        }
    }

    // Add padding if width is specified
    while (i < width && i < (int)sizeof(tmp)) {
        tmp[i++] = pad_char;
    }

    // Output in reverse order
    while (i > 0) {
        buffer_append(buf, remaining, tmp[--i]);
    }
}

static void format_string_padded(char** buf, size_t* remaining, const char* str,
                                  int width, bool left_justify) {
    if (!str) {
        str = "(null)";
    }

    int len = 0;
    const char* p = str;
    while (*p++) len++;

    // Calculate padding needed
    int pad = (width > len) ? (width - len) : 0;

    // Right padding (left justify): output string first, then spaces
    // Left padding (right justify): output spaces first, then string
    if (!left_justify) {
        while (pad-- > 0) {
            buffer_append(buf, remaining, ' ');
        }
    }

    // Output the string
    while (*str) {
        buffer_append(buf, remaining, *str++);
    }

    if (left_justify) {
        while (pad-- > 0) {
            buffer_append(buf, remaining, ' ');
        }
    }
}

static int vsnprintf_simple(char* buffer, size_t size, const char* format, va_list args) {
    if (!buffer || !format) {
        return 0;
    }

    char* out = buffer;
    size_t remaining = size ? size - 1 : 0;

    while (*format) {
        if (*format != '%') {
            buffer_append(&out, &remaining, *format++);
            continue;
        }

        format++;

        // Parse flags
        bool zero_pad = false;
        bool left_justify = false;

        while (*format == '-' || *format == '0' || *format == '+' || *format == ' ') {
            if (*format == '-') left_justify = true;
            else if (*format == '0') zero_pad = true;
            format++;
        }

        // Parse width
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        // Parse length modifier
        bool long_flag = false;
        if (*format == 'l') {
            long_flag = true;
            format++;
        }

        char pad_char = zero_pad ? '0' : ' ';

        switch (*format) {
            case 's': {
                const char* str = va_arg(args, const char*);
                if (width > 0) {
                    format_string_padded(&out, &remaining, str, width, left_justify);
                } else {
                    format_string(&out, &remaining, str);
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                buffer_append(&out, &remaining, c);
                break;
            }
            case 'd':
            case 'i': {
                long val = long_flag ? va_arg(args, long) : va_arg(args, int);
                if (val < 0) {
                    buffer_append(&out, &remaining, '-');
                    val = -val;
                    if (width > 0) width--;
                }
                format_uint_padded(&out, &remaining, (unsigned long)val, 10, false, width, pad_char);
                break;
            }
            case 'u': {
                unsigned long val = long_flag ? va_arg(args, unsigned long)
                                              : va_arg(args, unsigned int);
                format_uint_padded(&out, &remaining, val, 10, false, width, pad_char);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long val = long_flag ? va_arg(args, unsigned long)
                                              : va_arg(args, unsigned int);
                format_uint_padded(&out, &remaining, val, 16, *format == 'X', width, pad_char);
                break;
            }
            case 'p': {
                // Pointer format - always use full width for the architecture
                void* ptr = va_arg(args, void*);
                buffer_append(&out, &remaining, '0');
                buffer_append(&out, &remaining, 'x');
                format_uint_padded(&out, &remaining, (unsigned long)ptr, 16, false, sizeof(void*) * 2, '0');
                break;
            }
            case '%':
                buffer_append(&out, &remaining, '%');
                break;
            case '\0':
                // Premature end of format string
                goto done;
            default:
                buffer_append(&out, &remaining, '%');
                buffer_append(&out, &remaining, *format);
                break;
        }
        format++;
    }

done:
    if (size) {
        *out = '\0';
    }

    return (int)(out - buffer);
}

int vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
    return vsnprintf_simple(buffer, size, format, args);
}

int snprintf(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf_simple(buffer, size, format, args);
    va_end(args);
    return written;
}

int vsprintf(char* buffer, const char* format, va_list args) {
    return vsnprintf(buffer, (size_t)-1, format, args);
}

int sprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf_simple(buffer, (size_t)-1, format, args);
    va_end(args);
    return written;
}

int printf(const char* format, ...) {
    (void)format;
    return 0;
}

int vfprintf(FILE* stream, const char* format, va_list args) {
    (void)stream;
    (void)format;
    (void)args;
    return 0;
}

int fprintf(FILE* stream, const char* format, ...) {
    (void)stream;
    (void)format;
    return 0;
}

int puts(const char* str) {
    (void)str;
    return 0;
}

int putchar(int c) {
    return c;
}

int getchar(void) {
    return -1;
}

char* gets(char* str) {
    if (str) {
        *str = '\0';
    }
    return str;
}

FILE* stdin = NULL;
FILE* stdout = NULL;
FILE* stderr = NULL;

FILE* fopen(const char* filename, const char* mode) {
    (void)filename;
    (void)mode;
    return NULL;
}

int fclose(FILE* file) {
    (void)file;
    return -1;
}

int fgetc(FILE* file) {
    (void)file;
    return -1;
}

int fputc(int c, FILE* file) {
    (void)file;
    return c;
}

char* fgets(char* str, int n, FILE* file) {
    (void)str;
    (void)n;
    (void)file;
    return NULL;
}

int fputs(const char* str, FILE* file) {
    (void)str;
    (void)file;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Minimal sscanf implementation.
 *
 * Supports the conversion specifiers used by the kernel/userspace:
 *   %u  - unsigned decimal int
 *   %d  - signed decimal int
 *   %x  - hexadecimal unsigned int
 *   %s  - string (no width), stops at whitespace
 *   %Ns - string with explicit max width N (e.g. %31s)
 *   %%  - literal '%'
 * Whitespace in the format matches any amount of whitespace in the input.
 * Returns the number of fields successfully assigned, or EOF on early
 * input failure.
 * ------------------------------------------------------------------------- */
static int sscan_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static unsigned long sscan_strtoul(const char** p, int base, int* ok) {
    unsigned long v = 0;
    int digits = 0;
    const char* s = *p;
    while (sscan_is_space(*s)) s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        s++;
        digits++;
    }
    *p = s;
    if (ok) *ok = (digits > 0);
    return v;
}

int sscanf(const char* str, const char* fmt, ...) {
    if (!str || !fmt) return EOF;
    va_list args;
    va_start(args, fmt);
    const char* in = str;
    const char* f = fmt;
    int matched = 0;

    while (*f) {
        if (sscan_is_space(*f)) {
            while (sscan_is_space(*in)) in++;
            f++;
            continue;
        }
        if (*f != '%') {
            if (*in != *f) { va_end(args); return matched; }
            in++; f++;
            continue;
        }
        f++;  /* skip '%' */
        if (*f == '%') {
            if (*in != '%') { va_end(args); return matched; }
            in++; f++;
            continue;
        }
        int width = 0;
        bool has_width = false;
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f - '0');
            has_width = true;
            f++;
        }
        char spec = *f;
        if (spec == '\0') { va_end(args); return matched; }
        f++;
        while (sscan_is_space(*in)) in++;
        if (*in == '\0' && (spec == 'd' || spec == 'u' || spec == 'x' || spec == 's')) {
            va_end(args); return matched;
        }
        switch (spec) {
            case 'd': {
                int neg = 0;
                if (*in == '+') in++;
                else if (*in == '-') { neg = 1; in++; }
                int ok = 0;
                const char* before = in;
                unsigned long uv = sscan_strtoul(&in, 10, &ok);
                if (!ok) { va_end(args); return matched; }
                (void)before;
                int* out = va_arg(args, int*);
                if (out) *out = neg ? -(int)uv : (int)uv;
                matched++;
                break;
            }
            case 'u': {
                int ok = 0;
                unsigned long uv = sscan_strtoul(&in, 10, &ok);
                if (!ok) { va_end(args); return matched; }
                unsigned int* out = va_arg(args, unsigned int*);
                if (out) *out = (unsigned int)uv;
                matched++;
                break;
            }
            case 'x': {
                int ok = 0;
                unsigned long uv = sscan_strtoul(&in, 16, &ok);
                if (!ok) { va_end(args); return matched; }
                unsigned int* out = va_arg(args, unsigned int*);
                if (out) *out = (unsigned int)uv;
                matched++;
                break;
            }
            case 's': {
                int maxw = has_width ? width : (int)~0u;
                char* out = va_arg(args, char*);
                if (!out) { va_end(args); return matched; }
                int n = 0;
                while (*in && !sscan_is_space(*in) && n < maxw - 1) {
                    out[n++] = *in++;
                }
                out[n] = '\0';
                if (n == 0 && !has_width) { va_end(args); return matched; }
                matched++;
                break;
            }
            case 'c': {
                char* out = va_arg(args, char*);
                if (!out || *in == '\0') { va_end(args); return matched; }
                *out = *in++;
                matched++;
                break;
            }
            default:
                /* Unsupported specifier; stop parsing. */
                va_end(args);
                return matched;
        }
    }
    va_end(args);
    return matched;
}
