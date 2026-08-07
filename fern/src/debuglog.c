#include "include/debuglog.h"
#include "include/io_ports.h"
#include "include/types.h"
#include "include/util.h"
#include "include/string.h"
#include <stdarg.h>
#include <stdint.h>

#define DEBUGLOG_SERIAL_PORT 0x3F8
#define DEBUGLOG_DEBUGCON_PORT 0xE9
#define DEBUGLOG_SERIAL_TIMEOUT 0xFFFF
#define DEBUGLOG_BUFFER_SIZE 1024

static bool debuglog_initialized = false;
static char debuglog_buffer[DEBUGLOG_BUFFER_SIZE];
static uint32_t debuglog_buffer_pos = 0;

/* Kernel log ring buffer backing dmesg/syslog/journalctl (see SYS_SYSLOG in
 * syscall.c). Every character logged anywhere in the kernel lands here via
 * debuglog_write_char(), the single choke point all debuglog()/print()/etc
 * output already passes through. klog_write_total is a monotonic count of
 * every byte ever appended (not wrapped), so readers can tell how much
 * history has been overwritten. No locking: matches this file's existing
 * unlocked debuglog_buffer_pos, and debuglog is called from panic/early-boot
 * contexts where a lock could itself deadlock. */
#define KLOG_RING_SIZE 32768
static char klog_ring[KLOG_RING_SIZE];
static uint32_t klog_write_total = 0;

static void klog_append(char c) {
    klog_ring[klog_write_total % KLOG_RING_SIZE] = c;
    klog_write_total++;
}

uint32 debuglog_klog_buffer_size(void) {
    return KLOG_RING_SIZE;
}

uint32 debuglog_klog_unread_size(void) {
    return (klog_write_total < KLOG_RING_SIZE) ? klog_write_total : KLOG_RING_SIZE;
}

/* Copies out the oldest-to-newest bytes still in the ring (i.e. full
 * available history, oldest first) -- the same ordering `dmesg` shows. */
uint32 debuglog_klog_read(char* out, uint32 max_len) {
    if (!out || max_len == 0) {
        return 0;
    }

    uint32_t available = debuglog_klog_unread_size();
    uint32_t to_copy = (available < max_len) ? available : max_len;
    uint32_t start = klog_write_total - available;

    for (uint32_t i = 0; i < to_copy; i++) {
        out[i] = klog_ring[(start + i) % KLOG_RING_SIZE];
    }
    return to_copy;
}

void debuglog_klog_clear(void) {
    klog_write_total = 0;
}

static inline void debuglog_serial_wait_tx(void) {
    uint32 timeout = DEBUGLOG_SERIAL_TIMEOUT;
    while (timeout--) {
        if (inportb(DEBUGLOG_SERIAL_PORT + 5) & 0x20) {
            break;
        }
    }
}

static void debuglog_flush(void) {
    for (uint32_t i = 0; i < debuglog_buffer_pos; i++) {
        debuglog_serial_wait_tx();
        outportb(DEBUGLOG_SERIAL_PORT, (uint8)debuglog_buffer[i]);
    }
    debuglog_buffer_pos = 0;
}

static void debuglog_serial_write_char(char c) {
    debuglog_buffer[debuglog_buffer_pos++] = c;
    if (debuglog_buffer_pos >= DEBUGLOG_BUFFER_SIZE || c == '\n') {
        debuglog_flush();
    }
}

static void debuglog_serial_init(void) {
    outportb(DEBUGLOG_SERIAL_PORT + 1, 0x01);      // Enable receive interrupts (IER = 0x01)
    outportb(DEBUGLOG_SERIAL_PORT + 3, 0x80);      // Enable DLAB
    outportb(DEBUGLOG_SERIAL_PORT + 0, 0x03);      // 38400 baud (divisor = 3)
    outportb(DEBUGLOG_SERIAL_PORT + 1, 0x00);
    outportb(DEBUGLOG_SERIAL_PORT + 3, 0x03);      // 8 bits, no parity, one stop bit
    outportb(DEBUGLOG_SERIAL_PORT + 2, 0xC7);      // Enable FIFO, clear them
    outportb(DEBUGLOG_SERIAL_PORT + 4, 0x0B);      // IRQs enabled, RTS/DSR set
}

void debuglog_init(void) {
    if (debuglog_initialized) {
        return;
    }

    debuglog_serial_init();
    debuglog_initialized = true;
}

bool debuglog_is_ready(void) {
    return debuglog_initialized;
}

void debuglog_write_char(char c) {
    klog_append(c);

    if (!debuglog_initialized) {
        return;
    }

    if (c == '\n') {
        debuglog_write_char('\r');
    }

    debuglog_serial_write_char(c);
    outportb(DEBUGLOG_DEBUGCON_PORT, (uint8)c);
}

void debuglog_write(const char* text) {
    if (!debuglog_initialized || !text) {
        return;
    }

    while (*text) {
        debuglog_write_char(*text++);
    }
}

void debuglog_write_hex(uint32 value) {
    char buffer[11];
    char* ptr = buffer;
    static const char hex_chars[] = "0123456789ABCDEF";

    *ptr++ = '0';
    *ptr++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        *ptr++ = hex_chars[(value >> i) & 0xF];
    }
    *ptr = '\0';
    debuglog_write(buffer);
}

void debuglog_write_hex64(uint64_t value) {
    char buffer[19];
    char* ptr = buffer;
    static const char hex_chars[] = "0123456789ABCDEF";

    *ptr++ = '0';
    *ptr++ = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        *ptr++ = hex_chars[(value >> i) & 0xF];
    }
    *ptr = '\0';
    debuglog_write(buffer);
}

void debuglog_write_dec(uint32 value) {
    char buffer[16];
    int pos = 0;

    if (value == 0) {
        debuglog_write("0");
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

static void debuglog_write_uint_base_width(uint32 value, uint32 base, bool uppercase, bool prefix, int width, char pad_char) {
    char buffer[34];
    static const char* hex_lower = "0123456789abcdef";
    static const char* hex_upper = "0123456789ABCDEF";
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

    // Add padding if needed
    while (num_digits < width && pos >= 0) {
        buffer[pos--] = pad_char;
        num_digits++;
    }

    if (prefix && base == 16) {
        buffer[pos--] = 'x';
        buffer[pos--] = '0';
    }

    debuglog_write(&buffer[pos + 1]);
}

static void debuglog_write_uint_base(uint32 value, uint32 base, bool uppercase, bool prefix) {
    debuglog_write_uint_base_width(value, base, uppercase, prefix, 0, ' ');
}

static void debuglog_vformat(const char* format, va_list args) {
    while (format && *format) {
        if (*format != '%') {
            debuglog_write_char(*format++);
            continue;
        }

        format++;

        // Parse width specifier (e.g., %02x, %8d)
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

        // Parse precision specifier (e.g., %.32s)
        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }

        // Parse length modifier: 'l' (long), 'll' (long long)
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
        (void)is_longlong; // treat ll same as l on 32-bit target

        switch (*format) {
            case '%':
                debuglog_write_char('%');
                break;
            case 's': {
                const char* str = va_arg(args, const char*);
                if (!str) {
                    debuglog_write("(null)");
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
                    debuglog_write(buf);
                } else {
                    debuglog_write(str);
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
                int32 value = is_long ? (int32)va_arg(args, long) : va_arg(args, int32);
                if (value < 0) {
                    debuglog_write_char('-');
                    value = -value;
                }
                debuglog_write_dec((uint32)value);
                break;
            }
            case 'u': {
                uint32 value = is_long ? (uint32)va_arg(args, unsigned long) : va_arg(args, uint32);
                debuglog_write_dec(value);
                break;
            }
            case 'x':
            case 'X': {
                uint32 value = is_long ? (uint32)va_arg(args, unsigned long) : va_arg(args, uint32);
                bool uppercase = (*format == 'X');
                debuglog_write_uint_base_width(value, 16, uppercase, false, width, pad_char);
                break;
            }
            case 'p': {
                uintptr_t value = (uintptr_t)va_arg(args, void*);
                debuglog_write_uint_base((uint32)value, 16, false, true);
                break;
            }
            case '\0':
                // End of format string after %
                return;
            default:
                debuglog_write_char('%');
                debuglog_write_char(*format);
                break;
        }
        format++;
    }
}

void debuglog(debug_log_level_t level, const char* format, ...) {
    if (!debuglog_initialized || !format) {
        return;
    }

    static const char* level_prefix[] = {
        "[DETAIL]",   // DEBUG_DETAIL = 0
        "[INFO]  ",   // DEBUG_INFO   = 1
        "[WARN]  ",   // DEBUG_WARN   = 2
        "[ERROR] ",   // DEBUG_ERROR  = 3
        "[FATAL] "    // DEBUG_FATAL  = 4
    };

    int index = (int)level;
    if (index < 0 || index >= (int)(sizeof(level_prefix) / sizeof(level_prefix[0]))) {
        index = 1; // Default to INFO
    }

    debuglog_write(level_prefix[index]);
    va_list args;
    va_start(args, format);
    debuglog_vformat(format, args);
    va_end(args);
}

void debuglog_printf(const char* format, ...) {
    if (!debuglog_initialized || !format) {
        return;
    }

    va_list args;
    va_start(args, format);
    debuglog_vformat(format, args);
    va_end(args);
}
