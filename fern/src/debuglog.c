/*
 * Fern - Debug Logging (backward-compatible wrapper)
 * debuglog.c
 *
 * This file wraps the cross-architecture debug logging implementation
 * in arch/debuglog.c, providing backward-compatible entry points for
 * callers that use the old function names.
 *
 * New code should include "arch/debuglog.h" directly and use the
 * unified uint64 API.
 */

#include "include/debuglog.h"
#include "include/types.h"
#include <stdint.h>

/*
 * debuglog_write_hex64() is a backward-compatible alias.
 * The new unified API uses debuglog_write_hex(uint64_t) for both.
 * Old callers that pass uint32 to debuglog_write_hex() get an implicit
 * widening conversion, so no wrapper is needed for that function.
 */
void debuglog_write_hex64(uint64_t value) {
    debuglog_write_hex(value);
}
