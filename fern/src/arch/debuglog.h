/*
 * Fern - Cross-Architecture Debug Logging Interface
 * debuglog.h
 *
 * Unified debug log API that works across all supported architectures:
 *   - x86:     16550 COM1 (port I/O at 0x3F8)
 *   - ARM32:   PL011 (MMIO)
 *   - AArch64: PL011 (MMIO)
 *   - RISC-V:  8250-compatible (MMIO)
 *
 * Output is routed through the cross-architecture UART layer (uart.h),
 * so the same debuglog code runs on every platform.  Each log line
 * carries a millisecond timestamp (from timer_get_ticks()) and a CPU
 * ID prefix (from smp_get_cpu_id()), making multi-core debugging
 * straightforward.
 *
 * A persistent ring buffer records all output for crash dumps and
 * userspace dmesg/syslog readers.
 */

#ifndef FOREST_ARCH_DEBUGLOG_H
#define FOREST_ARCH_DEBUGLOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * Log levels
 * --------------------------------------------------------------------- */

typedef enum {
    DBGLOG_DEBUG = 0,
    DBGLOG_INFO,
    DBGLOG_WARN,
    DBGLOG_ERROR,
    DBGLOG_FATAL,
    DBGLOG_LEVEL_COUNT  /* sentinel – number of levels */
} debuglog_level_t;

/* -----------------------------------------------------------------------
 * Ring buffer configuration
 * --------------------------------------------------------------------- */

#define DEBUGLOG_RING_SIZE  32768

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/**
 * debuglog_init() - Initialise the debug logging subsystem.
 *
 * Must be called once during early boot, after uart_init().  Sets up
 * the ring buffer and marks the subsystem as ready.
 */
void debuglog_init(void);

/**
 * debuglog_is_ready() - Return true if debuglog_init() has completed.
 */
bool debuglog_is_ready(void);

/**
 * debuglog_set_level() - Set the minimum log level for output.
 *
 * Messages below this level are silently discarded (but still recorded
 * in the ring buffer for crash dumps).
 *
 * @level: Minimum level to display (DBGLOG_DEBUG .. DBGLOG_FATAL).
 */
void debuglog_set_level(debuglog_level_t level);

/**
 * debuglog_get_level() - Return the current minimum log level.
 */
debuglog_level_t debuglog_get_level(void);

/**
 * debuglog_write_char() - Write one character to the log.
 *
 * Routes through uart_putc() and appends to the ring buffer.
 * '\n' is translated to "\r\n" for terminal compatibility.
 */
void debuglog_write_char(char c);

/**
 * debuglog_write_string() - Write a NUL-terminated string to the log.
 *
 * Routes through uart_puts() for efficient output.
 */
void debuglog_write_string(const char* str);

/**
 * debuglog_write_hex() - Write a 64-bit value in hexadecimal format.
 *
 * Output format: "0x" followed by 16 uppercase hex digits.
 *
 * @value: The value to write.
 */
void debuglog_write_hex(uint64_t value);

/**
 * debuglog_write_dec() - Write a 64-bit value in decimal format.
 *
 * @value: The value to write.
 */
void debuglog_write_dec(uint64_t value);

/**
 * debuglog_printf() - Printf-style formatted logging (no level prefix).
 *
 * Supports: %c %s %d %u %x %X %p %% and width/zero-padding.
 * Output is routed through the UART layer.
 *
 * @fmt:  printf-style format string.
 * @...:  Variable arguments matching the format string.
 */
void debuglog_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * debuglog_vprintf() - va_list variant of debuglog_printf().
 */
void debuglog_vprintf(const char* fmt, va_list ap);

/**
 * debuglog() - Leveled logging with automatic prefix.
 *
 * Writes a timestamp, CPU ID, and level prefix (e.g. "[12345.678] [CPU0] [WARN] ")
 * followed by the formatted message.
 *
 * @level:   Log level for this message.
 * @format:  printf-style format string.
 * @...:     Variable arguments matching the format string.
 */
void debuglog(debuglog_level_t level, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

/* -----------------------------------------------------------------------
 * Ring buffer API (for crash dumps and userspace dmesg/syslog)
 * --------------------------------------------------------------------- */

/**
 * debuglog_klog_read() - Read from the ring buffer.
 *
 * Copies the oldest-to-newest bytes still in the ring into @out.
 *
 * @out:     Destination buffer.
 * @max_len: Maximum number of bytes to copy.
 *
 * Returns the number of bytes actually copied.
 */
uint32_t debuglog_klog_read(char* out, uint32_t max_len);

/**
 * debuglog_klog_clear() - Reset the ring buffer (discard all history).
 */
void debuglog_klog_clear(void);

/**
 * debuglog_klog_unread_size() - Return bytes of unread history in the ring.
 */
uint32_t debuglog_klog_unread_size(void);

/**
 * debuglog_klog_buffer_size() - Return total ring buffer capacity.
 */
uint32_t debuglog_klog_buffer_size(void);

#endif /* FOREST_ARCH_DEBUGLOG_H */
