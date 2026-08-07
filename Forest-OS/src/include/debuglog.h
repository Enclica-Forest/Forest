#ifndef DEBUGLOG_H
#define DEBUGLOG_H

#include "types.h"
#include <stdbool.h>

void debuglog_init(void);
bool debuglog_is_ready(void);
void debuglog_write_char(char c);
void debuglog_write(const char* text);
void debuglog_write_hex(uint32 value);
void debuglog_write_hex64(uint64_t value);
void debuglog_write_dec(uint32 value);

typedef enum {
    DEBUG_DETAIL = 0,
    DEBUG_INFO,
    DEBUG_WARN,
    DEBUG_ERROR,
    DEBUG_FATAL
} debug_log_level_t;

void debuglog(debug_log_level_t level, const char* format, ...) __attribute__((format(printf, 2, 3)));

/* Printf-style debug output without level prefix (for advanced subsystems) */
void debuglog_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/* Persistent kernel log ring buffer, fed by every debuglog_write_char() call
 * (i.e. everything logged anywhere in the kernel via debuglog()/print()/etc)
 * so userspace dmesg/syslog/journalctl can read real boot+runtime log
 * history back out via SYS_SYSLOG, instead of having nothing to show. */
uint32 debuglog_klog_read(char* out, uint32 max_len);
void debuglog_klog_clear(void);
uint32 debuglog_klog_unread_size(void);
uint32 debuglog_klog_buffer_size(void);

#endif
