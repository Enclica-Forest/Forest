#ifndef DEBUGLOG_H
#define DEBUGLOG_H

/*
 * Unified debug logging header.
 *
 * Pulls in the cross-architecture implementation from arch/debuglog.h
 * and provides backward-compatible aliases for callers that still use
 * the old type/function names.
 */

#include "arch/debuglog.h"
#include "types.h"
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Backward-compatible type aliases
 * --------------------------------------------------------------------- */

typedef debuglog_level_t debug_log_level_t;

#define DEBUG_DETAIL DBGLOG_DEBUG
#define DEBUG_INFO   DBGLOG_INFO
#define DEBUG_WARN   DBGLOG_WARN
#define DEBUG_ERROR  DBGLOG_ERROR
#define DEBUG_FATAL  DBGLOG_FATAL

/* -----------------------------------------------------------------------
 * Backward-compatible function aliases
 *
 * debuglog_write()        → debuglog_write_string()
 * debuglog_write_hex64()  → debuglog_write_hex()
 *
 * Old callers that pass uint32 to debuglog_write_hex()/debuglog_write_dec()
 * get an implicit widening conversion to uint64_t, so no wrapper is needed.
 * --------------------------------------------------------------------- */

static inline void debuglog_write(const char* text) {
    debuglog_write_string(text);
}

void debuglog_write_hex64(uint64_t value);

#endif
