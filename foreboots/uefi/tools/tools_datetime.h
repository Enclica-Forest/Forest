/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_datetime.h - "Time & Date" tool category (KEY = datetime).
 * =============================================================================
 * Clocks, timers and calendars driven by the firmware RTC
 * (gST->RuntimeServices->GetTime) and the compositor frame clock. Integer /
 * fixed-point math only (no libc, no heap, no float, pre-ExitBootServices).
 *
 * Each tool follows "template B": its open() calls wm_open() and returns; the
 * bootx64.c menu loop drives draw + events. See CATEGORY MODULE CONTRACT.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_DATETIME_H
#define FOREB_UEFI_TOOLS_DATETIME_H

#include "tools.h"

/* Store gST/gBS/gRT for the RTC-backed tools (clock.c idiom). Call once from
 * tools_init(); tools that need no firmware services just ignore the globals. */
void cat_datetime_init(EFI_SYSTEM_TABLE *st);

/* Individual tools (each opens one wm window). */
void tool_datetime_stopwatch_open(void);   /* Stopwatch: start/stop/lap        */
void tool_datetime_countdown_open(void);   /* Countdown timer + beep at zero   */
void tool_datetime_calendar_open(void);    /* Month calendar (prev/next month) */
void tool_datetime_worldclock_open(void);  /* World clocks (fixed UTC offsets) */
void tool_datetime_unixtime_open(void);    /* Unix-time <-> date converter      */
void tool_datetime_weekday_open(void);     /* Day-of-week for any date         */
void tool_datetime_uptime_open(void);      /* Uptime counter                   */
void tool_datetime_binclock_open(void);    /* Binary / word clock              */

/* Category registry exports. */
extern const struct forebo_tool cat_datetime_tools[];
extern const int                cat_datetime_count;

#endif /* FOREB_UEFI_TOOLS_DATETIME_H */
