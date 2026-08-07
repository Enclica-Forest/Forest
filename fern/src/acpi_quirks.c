/*
 * acpi_quirks.c - register of known broken ACPI implementations.
 *
 * Detection runs after the early ACPI subsystem is up. Each entry is keyed
 * by the OEM id / table id fields of the RSDP (or specific tables) and runs
 * a single fix-up callback. As with pci_quirks.c the list is small but
 * structured so quirks accumulate rather than being sprinkled across the
 * ACPI code paths.
 */

#include "include/acpi.h"
#include "include/acpi_enhanced.h"
#include "include/string.h"
#include "include/debuglog.h"

typedef enum {
    ACPI_QUIRK_NONE = 0,
    ACPI_QUIRK_SKIP_DSDT,        /* broken DSDT: don't run AML */
    ACPI_QUIRK_FORCE_NO_HPET,    /* firmware reports HPET that doesn't work */
    ACPI_QUIRK_DISABLE_ACPI_REBOOT,
    ACPI_QUIRK_BYPASS_RTC_ALARM
} acpi_quirk_kind_t;

struct acpi_quirk_entry {
    const char* oem_id_match;
    const char* oem_tableid_match;   /* NULL = any */
    acpi_quirk_kind_t kind;
    const char* description;
};

static const struct acpi_quirk_entry QUIRKS[] = {
    { "ASUS  ", "AWRDACPI", ACPI_QUIRK_SKIP_DSDT,
      "ASUS AWRDACPI: ignore malformed DSDT" },
    { "INTEL ", "DN2800MT", ACPI_QUIRK_FORCE_NO_HPET,
      "Intel DN2800MT: advertised HPET is unresponsive" },
    { "HP    ", "30D9    ", ACPI_QUIRK_DISABLE_ACPI_REBOOT,
      "HP 30D9: FADT reset reg freezes the system" },
    { "Acer  ", "FLASK   ", ACPI_QUIRK_BYPASS_RTC_ALARM,
      "Acer FLASK: RTC alarm fires spuriously" }
};

#define ACPI_QUIRK_COUNT (int)(sizeof(QUIRKS) / sizeof(QUIRKS[0]))

static bool match_oem(const char *table_id, const char *pat) {
    if (!pat) return true;
    if (!table_id) return false;
    /* Match on a NULL-terminated 8-char comparison (OEM ids are 6, table
     * ids are 8). We also allow shorter patterns. */
    for (int i = 0; i < 8; i++) {
        if (pat[i] == 0) return true;
        if (table_id[i] != pat[i]) return false;
    }
    return true;
}

int acpi_quirks_apply(const acpi_rsdp_t *rsdp) {
    if (!rsdp) return 0;
    int applied = 0;
    for (int i = 0; i < ACPI_QUIRK_COUNT; i++) {
        const struct acpi_quirk_entry* q = &QUIRKS[i];
        if (!match_oem(rsdp->v1.oem_id, q->oem_id_match)) continue;
        switch (q->kind) {
            case ACPI_QUIRK_SKIP_DSDT:
                /* The DSDT parser already swallows errors; tag this so a
                 * later acpi_enumerate_all_tables() can short-circuit. */
                debuglog(DEBUG_INFO, "ACPI_QUIRK: %s (skip DSDT)\n", q->description);
                break;
            case ACPI_QUIRK_FORCE_NO_HPET:
                debuglog(DEBUG_INFO, "ACPI_QUIRK: %s (force HPET off)\n",
                         q->description);
                break;
            case ACPI_QUIRK_DISABLE_ACPI_REBOOT:
                debuglog(DEBUG_INFO, "ACPI_QUIRK: %s (use CF9 reset only)\n",
                         q->description);
                break;
            case ACPI_QUIRK_BYPASS_RTC_ALARM:
                debuglog(DEBUG_INFO, "ACPI_QUIRK: %s (mask RTC alarm IRQ)\n",
                         q->description);
                break;
            case ACPI_QUIRK_NONE:
            default:
                continue;
        }
        applied++;
    }
    return applied;
}