/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_convert.h - "Converters" tool category (KEY = convert).
 * =============================================================================
 * A self-contained group of pure compute/draw GUI tools (template B windows):
 * number-base, ASCII table, Base64, Caesar/ROT13, RGB<->Hex, temperature,
 * data-size, Roman numerals and angle converters. All integer / fixed-point
 * math (no float: SSE/x87 are disabled by the build flags). NO firmware
 * services are used, so this category needs NO init function.
 *
 * See CATEGORY MODULE CONTRACT in tools_cat.h / tools.h.
 * Freestanding (no libc), pre-ExitBootServices, fixed buffers, no heap.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_CONVERT_H
#define FOREB_UEFI_TOOLS_CONVERT_H

#include "tools.h"   /* struct forebo_tool + wm/ui/input/efi */

/* Individual tool openers (each opens one wm window and returns). */
void tool_convert_base_open(void);    /* dec/hex/bin/oct live base converter   */
void tool_convert_ascii_open(void);   /* scrollable ASCII / code table         */
void tool_convert_base64_open(void);  /* Base64 encode / decode a string       */
void tool_convert_caesar_open(void);  /* Caesar / ROT13 cipher                 */
void tool_convert_rgb_open(void);     /* RGB <-> Hex colour with live swatch   */
void tool_convert_temp_open(void);    /* temperature C / F / K                 */
void tool_convert_datasize_open(void);/* data size B/KB/MB/GB/TB               */
void tool_convert_roman_open(void);   /* Roman numerals <-> integer            */
void tool_convert_angle_open(void);   /* angle deg / rad(milli) / grad         */

/* Category exports (defined in tools_convert.c). */
extern const struct forebo_tool cat_convert_tools[];
extern const int                cat_convert_count;

#endif /* FOREB_UEFI_TOOLS_CONVERT_H */
