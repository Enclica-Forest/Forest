/*
 * settings_nv.h - Durable persistence for the ForeB Settings/Theme tool.
 *
 * The windowed Settings tool (uefi/tools.c, tool_settings_open) edits the live
 * struct forebo_config theme block in place, but those edits only last for the
 * current boot: nothing writes them back to durable storage. This module
 * stashes the user-tunable UI settings into a UEFI Non-Volatile variable
 * (L"ForeBSettings", a private ForeB vendor GUID) so they survive a reboot.
 *
 * The blob is small, fixed-size, packed and VERSIONED (magic + version +
 * sizeof guard); a stale/foreign blob is ignored rather than applied. Both
 * calls are fully NULL-/absent-safe: they no-op when cfg is NULL, when
 * RuntimeServices (or GetVariable/SetVariable) is unavailable, or when the
 * variable is simply not set yet. Nothing here allocates or faults.
 *
 * Integration (done by the caller, not this module):
 *   - efi_main: call settings_nv_load(&g_cfg) right AFTER forebo_config_load
 *     so saved values override the parsed config for this boot.
 *   - Settings tool (tools.c): call settings_nv_save(&g_cfg) on the tool's
 *     apply/close path so the current edits are persisted for next boot.
 */
#ifndef FOREB_SETTINGS_NV_H
#define FOREB_SETTINGS_NV_H

struct forebo_config;   /* forward decl (see include/forebo_cfg.h) */

/*
 * Cache the firmware system table so the save/load entry points can reach
 * RuntimeServices (this codebase has no ambient gST global; every module caches
 * it once, e.g. diskio_init/img_init). `st` is the EFI_SYSTEM_TABLE* passed to
 * efi_main (typed void* here to keep this header free of the UEFI headers). The
 * integrator calls this once early in efi_main, right after gST/gBS are set and
 * before settings_nv_load. If it is never called (or passed NULL) save/load
 * simply no-op — they never fault.
 */
void settings_nv_init(void *st);

/*
 * Persist the user-tunable UI settings from *cfg into the L"ForeBSettings"
 * NV variable (NV|BS|RT). No-op (never faults) if cfg is NULL or the firmware
 * RuntimeServices / SetVariable are unavailable. Errors are silently ignored.
 */
void settings_nv_save(const struct forebo_config *cfg);

/*
 * Read the L"ForeBSettings" NV variable and, when present and version/size
 * valid, apply the saved UI settings onto *cfg (theme colours + toggles).
 * No-op (leaving cfg untouched) if cfg is NULL, the variable is unset, the
 * firmware RuntimeServices / GetVariable are unavailable, or the stored blob
 * is stale (wrong magic/version/size).
 */
void settings_nv_load(struct forebo_config *cfg);

#endif /* FOREB_SETTINGS_NV_H */
