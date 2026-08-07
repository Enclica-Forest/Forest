/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/sysmon.h - System Monitor tool (live RAM / GOP / firmware / uptime).
 * =============================================================================
 * A "template B" windowed tool: tool_sysmon_open() calls wm_open() and returns;
 * the bootx64.c menu loop drives compositing + input. The draw callback
 * re-gathers a small snapshot every N frames and paints a live dashboard of:
 *   - RAM totals + conventional/reserved/ACPI/MMIO breakdown (GetMemoryMap),
 *   - the current GOP mode (resolution / pixel format / pitch),
 *   - firmware vendor + revision, UEFI spec revision, SecureBoot state,
 *   - an approximate uptime (frame counter, RTC-corrected when available),
 *   - the block-device count (diskio_enumerate).
 *
 * Freestanding C11, no libc, no heap beyond BootServices AllocatePool (a scratch
 * memory-map buffer freed immediately after each poll). Every protocol lookup is
 * guarded; missing sources render as "N/A".
 * ========================================================================== */
#ifndef FOREB_UEFI_SYSMON_H
#define FOREB_UEFI_SYSMON_H

#include "efi.h"

/* One-time init: caches gST/gBS/gRT and (idempotently) inits diskio. Call once
 * during startup alongside tools_init() / tool_clone_init(). NULL-safe. */
void tool_sysmon_init(EFI_SYSTEM_TABLE *st);

/* Open the System Monitor window (no-op if it is already open). */
void tool_sysmon_open(void);

#endif /* FOREB_UEFI_SYSMON_H */
