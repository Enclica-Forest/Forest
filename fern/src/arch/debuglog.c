/*
 * Fern - Cross-Architecture Debug Logging Implementation
 * debuglog.c
 *
 * Implements the unified debug logging API.  All output is routed through
 * the cross-architecture UART layer (uart.h), so this code runs unchanged
 * on x86, ARM32, AArch64, and RISC-V.
 *
 * Features:
 *   - Millisecond timestamps from timer_get_ticks()
 *   - CPU ID prefix from smp_get_cpu_id()
 *   - Log-level filtering (messages below the threshold are discarded
 *     from the UART output but still recorded in the ring buffer)
 *   - Persistent ring buffer for crash dumps and userspace readers
 *   - Printf-style formatting with width/zero-padding support
 */

#include "arch/debuglog.h"
#include "arch/uart.h"
#include "arch/timer.h"
#include "arch/smp.h"
#include "include/types.h"
#include "include/string.h"
#include <stdarg.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------- */

static bool debuglog_ready = false;
static debuglog_level_t current_level = DBGLOG_DEBUG;
static bool debuglog_uart_output = true;

/* -----------------------------------------------------------------------
 * Ring buffer
 *
 * Records every byte that passes through debuglog_write_char(), including
 * timestamps and level prefixes.  No locking: matches the existing code's
 * unlocked approach, and debuglog is called from panic/early-boot contexts
 * where a lock could itself deadlock.
 * --------------------------------------------------------------------- */

static char klog_ring[DEBUGLOG_RING_SIZE];
static uint32_t klog_write_total = 0;

static void klog_append(char c) {
    klog_ring[klog_write_total % DEBUGLOG_RING_SIZE] = c;
    klog_write_total++;
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

__attribute__((unused)) static void debuglog_write_hex64_val(uint64_t value) {
    char buffer[19];
    char* ptr = buffer;
    static const char hex_chars[] = "0123456789ABCDEF";

    *ptr++ = '0';
    *ptr++ = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        *ptr++ = hex_chars[(value >> i) & 0xF];
    }
    *ptr = '\0';
    debuglog_write_string(buffer);
}

static void debuglog_write_dec_val(uint64_t value) {
    char buffer[21];
    int pos = 0;

    if (value == 0) {
        debuglog_write_string("0");
        return;
    }

    while (value > 0 && pos < (int)(sizeof(buffer) - 1)) {
        buffer[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        debuglog_write_char(buffer[--pos]);
    }
}

static void debuglog_write_uint_base_width(uint32_t value, uint32_t base,
                                           bool uppercase, bool prefix,
                                           int width, char pad_char) {
    char buffer[34];
    static const char hex_lower[] = "0123456789abcdef";
    static const char hex_upper[] = "0123456789ABCDEF";
    const char* digits = uppercase ? hex_upper : hex_lower;

    int pos = sizeof(buffer) - 1;
    buffer[pos--] = '\0';
    int num_digits = 0;
    if (value == 0) {
        buffer[pos--] = '0';
        num_digits = 1;
    } else {
        while (value > 0 && pos >= 0) {
            buffer[pos--] = digits[value % base];
            value /= base;
            num_digits++;
        }
    }

    while (num_digits < width && pos >= 0) {
        buffer[pos--] = pad_char;
        num_digits++;
    }

    if (prefix && base == 16) {
        buffer[pos--] = 'x';
        buffer[pos--] = '0';
    }

    debuglog_write_string(&buffer[pos + 1]);
}

/* -----------------------------------------------------------------------
 * Format string engine
 * --------------------------------------------------------------------- */

static void debuglog_vformat(const char* format, va_list args) {
    while (format && *format) {
        if (*format != '%') {
            debuglog_write_char(*format++);
            continue;
        }

        format++;

        /* Parse width specifier (e.g., %02x, %8d) */
        char pad_char = ' ';
        int width = 0;

        if (*format == '0') {
            pad_char = '0';
            format++;
        }

        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        /* Parse precision specifier (e.g., %.32s) */
        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }

        /* Parse length modifier: 'l' (long), 'll' (long long) */
        bool is_long = false;
        bool is_longlong = false;
        if (*format == 'l') {
            format++;
            is_long = true;
            if (*format == 'l') {
                format++;
                is_longlong = true;
            }
        }
        (void)is_longlong;

        switch (*format) {
            case '%':
                debuglog_write_char('%');
                break;
            case 's': {
                const char* str = va_arg(args, const char*);
                if (!str) {
                    debuglog_write_string("(null)");
                    break;
                }
                if (precision > 0) {
                    char buf[64];
                    int len = 0;
                    while (str[len] && len < precision && len < (int)(sizeof(buf) - 1)) {
                        buf[len] = str[len];
                        len++;
                    }
                    buf[len] = '\0';
                    debuglog_write_string(buf);
                } else {
                    debuglog_write_string(str);
                }
                break;
            }
            case 'c': {
                int ch = va_arg(args, int);
                debuglog_write_char((char)ch);
                break;
            }
            case 'd':
            case 'i': {
                int32_t value = is_long ? (int32_t)va_arg(args, long) : va_arg(args, int32_t);
                if (value < 0) {
                    debuglog_write_char('-');
                    value = -value;
                }
                debuglog_write_dec_val((uint64_t)value);
                break;
            }
            case 'u': {
                uint32_t value = is_long ? (uint32_t)va_arg(args, unsigned long) : va_arg(args, uint32_t);
                debuglog_write_dec_val((uint64_t)value);
                break;
            }
            case 'x':
            case 'X': {
                uint32_t value = is_long ? (uint32_t)va_arg(args, unsigned long) : va_arg(args, uint32_t);
                bool uppercase = (*format == 'X');
                debuglog_write_uint_base_width(value, 16, uppercase, false, width, pad_char);
                break;
            }
            case 'p': {
                uintptr_t value = (uintptr_t)va_arg(args, void*);
                debuglog_write_uint_base_width((uint32_t)value, 16, false, true, 0, ' ');
                break;
            }
            case '\0':
                return;
            default:
                debuglog_write_char('%');
                debuglog_write_char(*format);
                break;
        }
        format++;
    }
}

/* Write a level prefix string, routed through debuglog_write_string */
static void debuglog_write_level_prefix(debuglog_level_t level) {
    static const char* prefix[] = {
        "[DEBUG] ",  /* DBGLOG_DEBUG = 0 */
        "[INFO]  ",  /* DBGLOG_INFO  = 1 */
        "[WARN]  ",  /* DBGLOG_WARN  = 2 */
        "[ERROR] ",  /* DBGLOG_ERROR = 3 */
        "[FATAL] "   /* DBGLOG_FATAL = 4 */
    };

    int idx = (int)level;
    if (idx < 0 || idx >= DBGLOG_LEVEL_COUNT) {
        idx = 1; /* default to INFO */
    }
    debuglog_write_string(prefix[idx]);
}

/* Write "[ticks] [CPUn] " timestamp+cpu prefix */
static void debuglog_write_header(void) {
    uint64_t ticks = 0;
    uint32_t cpu_id = 0;

    extern uint64_t timer_get_ticks(void);
    ticks = timer_get_ticks();

    extern uint32_t smp_get_cpu_id(void);
    cpu_id = smp_get_cpu_id();

    debuglog_write_char('[');
    debuglog_write_dec_val(ticks);
    debuglog_write_string("] [CPU");
    debuglog_write_dec_val((uint64_t)cpu_id);
    debuglog_write_string("] ");
}

/* -----------------------------------------------------------------------
 * Public API – initialization
 * --------------------------------------------------------------------- */

void debuglog_init(void) {
    if (debuglog_ready) {
        return;
    }

    /* UART should already be initialized by arch early boot code.
     * We don't call uart_init() here because the arch code sets up
     * the correct driver with platform-specific base addresses. */

    debuglog_ready = true;

    /* Log the subsystem startup */
    debuglog(DBGLOG_INFO, "debuglog: ring buffer %d bytes, level=%d\n",
             DEBUGLOG_RING_SIZE, (int)current_level);
}

bool debuglog_is_ready(void) {
    return debuglog_ready;
}

void debuglog_set_level(debuglog_level_t level) {
    if (level >= DBGLOG_LEVEL_COUNT) {
        level = DBGLOG_FATAL;
    }
    current_level = level;
}

debuglog_level_t debuglog_get_level(void) {
    return current_level;
}

/* -----------------------------------------------------------------------
 * Public API – character / string output
 * --------------------------------------------------------------------- */

void debuglog_write_char(char c) {
    if (!debuglog_ready) {
        /* Before init, just record raw characters in the ring buffer */
        klog_append(c);
        return;
    }

    /* Always record in ring buffer (for crash dumps / dmesg).
     * Only output to UART when debuglog_uart_output is true. */
    if (c == '\n') {
        klog_append('\r');
        if (debuglog_uart_output) {
            uart_putc('\r');
        }
    }
    klog_append(c);
    if (debuglog_uart_output) {
        uart_putc(c);
    }
}

void debuglog_write_string(const char* str) {
    if (!str) {
        return;
    }

    while (*str) {
        if (!debuglog_ready) {
            klog_append(*str);
            str++;
            continue;
        }

        if (*str == '\n') {
            klog_append('\r');
            if (debuglog_uart_output) {
                uart_putc('\r');
            }
        }
        klog_append(*str);
        if (debuglog_uart_output) {
            uart_putc(*str);
        }
        str++;
    }
}

/* -----------------------------------------------------------------------
 * Public API – formatted value output
 * --------------------------------------------------------------------- */

void debuglog_write_hex(uint64_t value) {
    char buffer[19];
    char* ptr = buffer;
    static const char hex_chars[] = "0123456789ABCDEF";

    *ptr++ = '0';
    *ptr++ = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        *ptr++ = hex_chars[(value >> i) & 0xF];
    }
    *ptr = '\0';
    debuglog_write_string(buffer);
}

void debuglog_write_dec(uint64_t value) {
    char buffer[21];
    int pos = 0;

    if (value == 0) {
        debuglog_write_string("0");
        return;
    }

    while (value > 0 && pos < (int)(sizeof(buffer) - 1)) {
        buffer[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        debuglog_write_char(buffer[--pos]);
    }
}

/* -----------------------------------------------------------------------
 * Public API – printf-style logging
 * --------------------------------------------------------------------- */

void debuglog_printf(const char* fmt, ...) {
    if (!debuglog_ready || !fmt) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    debuglog_vprintf(fmt, args);
    va_end(args);
}

void debuglog_vprintf(const char* fmt, va_list ap) {
    if (!debuglog_ready || !fmt) {
        return;
    }

    debuglog_vformat(fmt, ap);
}

void debuglog(debuglog_level_t level, const char* format, ...) {
    if (!debuglog_ready || !format) {
        return;
    }

    /* Messages below the threshold are still recorded in the ring buffer
     * (for crash dumps / dmesg) but not sent to the UART. */
    bool should_output = ((int)level >= (int)current_level);

    /* Save and set the UART output flag so debuglog_write_char() and
     * debuglog_write_string() know whether to emit to the serial port. */
    bool prev_uart_output = debuglog_uart_output;
    debuglog_uart_output = should_output;

    debuglog_write_header();
    debuglog_write_level_prefix(level);

    va_list args;
    va_start(args, format);
    debuglog_vformat(format, args);
    va_end(args);

    debuglog_uart_output = prev_uart_output;
}

/* -----------------------------------------------------------------------
 * Public API – ring buffer queries
 * --------------------------------------------------------------------- */

uint32_t debuglog_klog_read(char* out, uint32_t max_len) {
    if (!out || max_len == 0) {
        return 0;
    }

    uint32_t available = debuglog_klog_unread_size();
    uint32_t to_copy = (available < max_len) ? available : max_len;
    uint32_t start = klog_write_total - available;

    for (uint32_t i = 0; i < to_copy; i++) {
        out[i] = klog_ring[(start + i) % DEBUGLOG_RING_SIZE];
    }
    return to_copy;
}

void debuglog_klog_clear(void) {
    klog_write_total = 0;
}

uint32_t debuglog_klog_unread_size(void) {
    return (klog_write_total < DEBUGLOG_RING_SIZE) ? klog_write_total : DEBUGLOG_RING_SIZE;
}

uint32_t debuglog_klog_buffer_size(void) {
    return DEBUGLOG_RING_SIZE;
}
