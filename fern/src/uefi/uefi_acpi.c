#include "uefi_acpi.h"
#include "../include/debuglog.h"
#include "../include/string.h"

static uefi_acpi_rsdp_t *g_uefi_rsdp = NULL;
static const uefi_acpi_xsdt_t *g_uefi_xsdt = NULL;

static bool uefi_acpi_checksum_valid(const void *data, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static bool uefi_guid_compare(const EFI_GUID *a, const EFI_GUID *b) {
    return a->data1 == b->data1 &&
           a->data2 == b->data2 &&
           a->data3 == b->data3 &&
           __builtin_memcmp(a->data4, b->data4, 8) == 0;
}

bool uefi_find_acpi_rsdp(EFI_SYSTEM_TABLE *system_table, uefi_acpi_rsdp_t **out_rsdp) {
    if (!system_table || !system_table->configuration_table) {
        debuglog(DEBUG_WARN, "[UEFI-ACPI] No configuration tables\n");
        return false;
    }

    EFI_GUID acpi20_guid = ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = ACPI_TABLE_GUID;

    uint64_t num_tables = system_table->number_of_table_entries;
    void *config_tables = system_table->configuration_table;

    typedef struct {
        EFI_GUID vendor_guid;
        void *vendor_table;
    } EFI_CONFIGURATION_TABLE;

    EFI_CONFIGURATION_TABLE *tables = (EFI_CONFIGURATION_TABLE *)config_tables;

    for (uint64_t i = 0; i < num_tables; i++) {
        if (uefi_guid_compare(&tables[i].vendor_guid, &acpi20_guid)) {
            uefi_acpi_rsdp_t *rsdp = (uefi_acpi_rsdp_t *)tables[i].vendor_table;
            if (uefi_acpi_checksum_valid(rsdp, sizeof(uefi_acpi_rsdp_t))) {
                debuglog(DEBUG_INFO, "[UEFI-ACPI] Found ACPI 2.0 RSDP at 0x%x\n", (uint32_t)(uint64_t)rsdp);
                *out_rsdp = rsdp;
                return true;
            }
            debuglog(DEBUG_WARN, "[UEFI-ACPI] ACPI 2.0 RSDP checksum invalid\n");
        }

        if (uefi_guid_compare(&tables[i].vendor_guid, &acpi10_guid)) {
            uefi_acpi_rsdp_t *rsdp = (uefi_acpi_rsdp_t *)tables[i].vendor_table;
            if (uefi_acpi_checksum_valid(rsdp, 20)) {
                debuglog(DEBUG_INFO, "[UEFI-ACPI] Found ACPI 1.0 RSDP at 0x%x\n", (uint32_t)(uint64_t)rsdp);
                *out_rsdp = rsdp;
                return true;
            }
        }
    }

    debuglog(DEBUG_WARN, "[UEFI-ACPI] No RSDP found in UEFI configuration tables\n");
    return false;
}

const uefi_acpi_xsdt_t *uefi_find_acpi_xsdt(const uefi_acpi_rsdp_t *rsdp) {
    if (!rsdp) {
        return NULL;
    }

    if (rsdp->v1.revision >= 2 && rsdp->xsdt_address != 0) {
        const uefi_acpi_xsdt_t *xsdt = (const uefi_acpi_xsdt_t *)rsdp->xsdt_address;
        if (uefi_acpi_checksum_valid(xsdt, xsdt->header.length)) {
            debuglog(DEBUG_INFO, "[UEFI-ACPI] Found XSDT at 0x%x\n", (uint32_t)rsdp->xsdt_address);
            return xsdt;
        }
        debuglog(DEBUG_WARN, "[UEFI-ACPI] XSDT checksum invalid, falling back to RSDT\n");
    }

    if (rsdp->v1.rsdt_address != 0) {
        debuglog(DEBUG_INFO, "[UEFI-ACPI] Using RSDT at 0x%x\n", rsdp->v1.rsdt_address);
        return (const uefi_acpi_xsdt_t *)rsdp->v1.rsdt_address;
    }

    return NULL;
}

static uint32_t uefi_acpi_xsdt_entry_count(const uefi_acpi_xsdt_t *xsdt) {
    if (!xsdt || xsdt->header.length <= sizeof(uefi_acpi_table_header_t)) {
        return 0;
    }
    return (xsdt->header.length - sizeof(uefi_acpi_table_header_t)) / sizeof(uint32_t);
}

const uefi_acpi_table_header_t *uefi_find_acpi_table(const uefi_acpi_xsdt_t *xsdt, const char *signature) {
    if (!xsdt || !signature) {
        return NULL;
    }

    uint32_t count = uefi_acpi_xsdt_entry_count(xsdt);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys_addr = xsdt->entries[i];
        const uefi_acpi_table_header_t *table = (const uefi_acpi_table_header_t *)(uint64_t)phys_addr;

        if (!__builtin_memcmp(table->signature, signature, 4)) {
            if (uefi_acpi_checksum_valid(table, table->length)) {
                debuglog(DEBUG_INFO, "[UEFI-ACPI] Found table %.4s at 0x%x (len=%u)\n",
                         signature, phys_addr, table->length);
                return table;
            }
            debuglog(DEBUG_WARN, "[UEFI-ACPI] Table %.4s checksum invalid\n", signature);
        }
    }

    return NULL;
}

bool uefi_acpi_init(EFI_SYSTEM_TABLE *system_table) {
    debuglog(DEBUG_INFO, "[UEFI-ACPI] Initializing ACPI discovery from UEFI\n");

    uefi_acpi_rsdp_t *rsdp = NULL;
    if (!uefi_find_acpi_rsdp(system_table, &rsdp)) {
        debuglog(DEBUG_WARN, "[UEFI-ACPI] Failed to find RSDP\n");
        return false;
    }
    g_uefi_rsdp = rsdp;

    const uefi_acpi_xsdt_t *xsdt = uefi_find_acpi_xsdt(rsdp);
    if (!xsdt) {
        debuglog(DEBUG_WARN, "[UEFI-ACPI] Failed to find XSDT/RSDT\n");
        return false;
    }
    g_uefi_xsdt = xsdt;

    const uefi_acpi_table_header_t *madt = uefi_find_acpi_table(xsdt, ACPI_MADT_SIGNATURE);
    if (madt) {
        debuglog(DEBUG_INFO, "[UEFI-ACPI] MADT found\n");
    }

    const uefi_acpi_table_header_t *hpet = uefi_find_acpi_table(xsdt, ACPI_HPET_SIGNATURE);
    if (hpet) {
        debuglog(DEBUG_INFO, "[UEFI-ACPI] HPET found\n");
    }

    const uefi_acpi_table_header_t *fadt = uefi_find_acpi_table(xsdt, ACPI_FADT_SIGNATURE);
    if (fadt) {
        debuglog(DEBUG_INFO, "[UEFI-ACPI] FADT found\n");
    }

    debuglog(DEBUG_INFO, "[UEFI-ACPI] ACPI discovery complete\n");
    return true;
}

const uefi_acpi_rsdp_t *uefi_acpi_get_rsdp(void) {
    return g_uefi_rsdp;
}

const uefi_acpi_xsdt_t *uefi_acpi_get_xsdt(void) {
    return g_uefi_xsdt;
}

const uefi_acpi_table_header_t *uefi_acpi_get_madt(void) {
    if (!g_uefi_xsdt) return NULL;
    return uefi_find_acpi_table(g_uefi_xsdt, ACPI_MADT_SIGNATURE);
}

const uefi_acpi_table_header_t *uefi_acpi_get_hpet(void) {
    if (!g_uefi_xsdt) return NULL;
    return uefi_find_acpi_table(g_uefi_xsdt, ACPI_HPET_SIGNATURE);
}

const uefi_acpi_table_header_t *uefi_acpi_get_fadt(void) {
    if (!g_uefi_xsdt) return NULL;
    return uefi_find_acpi_table(g_uefi_xsdt, ACPI_FADT_SIGNATURE);
}

const uefi_acpi_table_header_t *uefi_acpi_get_mcfg(void) {
    if (!g_uefi_xsdt) return NULL;
    return uefi_find_acpi_table(g_uefi_xsdt, ACPI_MCFG_SIGNATURE);
}
