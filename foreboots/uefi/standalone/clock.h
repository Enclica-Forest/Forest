/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/clock.h - Clock tool: live firmware RTC date + time.
 * =============================================================================
 * A TEMPLATE B window (see wm.h) that re-reads the firmware Real-Time Clock
 * (gST->RuntimeServices->GetTime, EFI_TIME) every frame and shows a large
 * digital HH:MM:SS readout, the date (YYYY-MM-DD + weekday), the UTC offset /
 * daylight flag when the firmware reports one, and a small integer-trig analog
 * face. If GetTime is unavailable it shows "RTC unavailable" and never crashes.
 *
 * Freestanding (no libc, no heap, no float): the analog hands use a fixed-point
 * sine lookup table, all math is 64-bit integer.
 *
 * Wiring:
 *   1. clock_init(SystemTable) once at startup (alongside tools_init/diskio_init)
 *      so the tool can reach RuntimeServices->GetTime.
 *   2. Add to the forebo_tools[] registry in tools.c:
 *        { "Clock", "Firmware RTC date + time", "gear", tool_clock_open },
 * ========================================================================== */
#ifndef FOREB_UEFI_CLOCK_H
#define FOREB_UEFI_CLOCK_H

#include "../efi.h"

/* Cache the system table (for RuntimeServices->GetTime). NULL-safe; if never
 * called (or passed NULL) the tool degrades gracefully to "RTC unavailable". */
void clock_init(EFI_SYSTEM_TABLE *st);

/* Open the Clock tool window (TEMPLATE B: returns immediately, the bootx64.c
 * menu loop drives it). Idempotent: a second call while open is a no-op. */
void tool_clock_open(void);

#endif /* FOREB_UEFI_CLOCK_H */
