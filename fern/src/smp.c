#include "include/smp.h"
#include "include/acpi.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "include/string.h"

#define LAPIC_REG_ID 0x20
#define MADT_ENTRY_TYPE_LAPIC      0
#define MADT_ENTRY_TYPE_X2APIC     9

/* Sentinel value returned when LAPIC ID cannot be read safely */
#define SMP_APIC_ID_UNKNOWN  0xFFFFFFFFu

static smp_state_t g_smp_state = {
    .cpu_count = 1,
    .online_cpus = 1,
    .bsp_index = 0,
    .bsp_apic_id = 0,
    .lapic_base = 0xFEE00000,
    .initialized = false
};

/*
 * Read the BSP's APIC ID from the memory-mapped LAPIC registers.
 *
 * Returns SMP_APIC_ID_UNKNOWN (0xFFFFFFFF) on any error so callers can
 * distinguish "unknown" from a genuine APIC ID of 0.
 *
 * Safety: maps the LAPIC page via identity mapping before any MMIO read
 * and validates the register value before returning.
 */
static uint32 smp_read_bsp_apic_id(uint32 lapic_base) {
    if (lapic_base == 0) {
        return SMP_APIC_ID_UNKNOWN;
    }

    /* Map the LAPIC registers so the MMIO read is safe */
    page_directory_t* dir = vmm_get_current_page_directory();
    if (dir) {
        uint32 start = memory_align_down(lapic_base, MEMORY_PAGE_SIZE);
        uint32 end = memory_align_up(lapic_base + 0x1000, MEMORY_PAGE_SIZE);
        vmm_identity_map_range(dir, start, end, PAGE_PRESENT | PAGE_WRITABLE);
    }

    volatile uint32* id_reg = (volatile uint32*)(lapic_base + LAPIC_REG_ID);
    uint32 raw = *id_reg;

    /* 0xFFFFFFFF means the MMIO read returned all-ones — APIC not present */
    if (raw == 0xFFFFFFFFu) {
        debuglog(DEBUG_WARN, "SMP: LAPIC ID register read all-ones at 0x%08x, APIC not responding\n",
                 lapic_base + LAPIC_REG_ID);
        return SMP_APIC_ID_UNKNOWN;
    }

    return (raw >> 24) & 0xFF;
}

static void smp_register_cpu(uint32 acpi_id, uint32 apic_id, uint32 flags) {
    if (!(flags & ACPI_MADT_LAPIC_FLAG_ENABLED)) {
        return;
    }

    if (g_smp_state.cpu_count >= SMP_MAX_CPUS) {
        debuglog(DEBUG_WARN, "SMP: CPU limit reached (%u)\n", SMP_MAX_CPUS);
        return;
    }

    smp_cpu_info_t* info = &g_smp_state.cpus[g_smp_state.cpu_count];
    info->acpi_id = acpi_id;
    info->apic_id = apic_id;
    info->enabled = true;
    info->online = false;
    info->bsp = false;

    if (apic_id == g_smp_state.bsp_apic_id || g_smp_state.cpu_count == 0) {
        info->bsp = true;
        info->online = true;
        g_smp_state.bsp_index = g_smp_state.cpu_count;
        g_smp_state.online_cpus++;
    }

    g_smp_state.cpu_count++;
    debuglog(DEBUG_INFO,
             "SMP: discovered CPU ACPI=%u APIC=%u (%s)\n",
             acpi_id, apic_id, info->bsp ? "BSP" : "AP");
}

/*
 * smp_init() - Detect and enumerate CPUs from the ACPI MADT table.
 *
 * Design contract:
 *  - NEVER blocks or spins waiting for APs to come online.
 *  - NEVER sends INIT/SIPI IPIs to Application Processors.
 *  - Always leaves the BSP in a valid, usable state.
 *  - Safe to call with a single-CPU QEMU instance (no -smp flag).
 *
 * When MADT is absent, or no CPUs are listed, or only the BSP is present,
 * the function logs "Single CPU mode" and returns immediately.
 *
 * Returns true if at least one CPU was registered (always true on success).
 */
bool smp_init(void) {
    if (g_smp_state.initialized) {
        return g_smp_state.cpu_count > 0;
    }

    /* Reset state before enumeration */
    memset(&g_smp_state, 0, sizeof(g_smp_state));
    g_smp_state.lapic_base = 0xFEE00000;
    g_smp_state.cpu_count = 0;
    g_smp_state.online_cpus = 0;

    /* Without MADT we cannot enumerate CPUs — run in single-CPU mode */
    const acpi_madt_header_t* madt = acpi_get_madt();
    if (!madt) {
        debuglog(DEBUG_WARN, "SMP: MADT not present, single CPU mode\n");
        goto single_cpu_fallback;
    }

    /* Prefer the MADT's LAPIC address over the architectural default */
    if (madt->local_apic_addr != 0) {
        g_smp_state.lapic_base = madt->local_apic_addr;
    }

    /* Read the BSP's APIC ID — non-fatal if it fails */
    g_smp_state.bsp_apic_id = smp_read_bsp_apic_id(g_smp_state.lapic_base);
    if (g_smp_state.bsp_apic_id == SMP_APIC_ID_UNKNOWN) {
        debuglog(DEBUG_WARN, "SMP: Cannot read BSP APIC ID, assuming ID 0\n");
        g_smp_state.bsp_apic_id = 0;
    }

    /* Walk MADT entries — this is purely passive, no IPI is sent */
    const uint8* ptr = (const uint8*)(madt + 1);
    const uint8* end = ((const uint8*)madt) + madt->header.length;

    while (ptr + sizeof(acpi_madt_entry_header_t) <= end) {
        const acpi_madt_entry_header_t* hdr = (const acpi_madt_entry_header_t*)ptr;

        /* Guard against malformed MADT entries */
        if (hdr->length < sizeof(acpi_madt_entry_header_t)) {
            debuglog(DEBUG_WARN, "SMP: Malformed MADT entry (length=%u), stopping parse\n",
                     (unsigned)hdr->length);
            break;
        }

        switch (hdr->type) {
            case MADT_ENTRY_TYPE_LAPIC: {
                if (hdr->length >= sizeof(acpi_madt_lapic_t)) {
                    const acpi_madt_lapic_t* lapic = (const acpi_madt_lapic_t*)ptr;
                    smp_register_cpu(lapic->acpi_processor_id, lapic->apic_id, lapic->flags);
                }
                break;
            }
            case MADT_ENTRY_TYPE_X2APIC: {
                if (hdr->length >= sizeof(acpi_madt_x2apic_t)) {
                    const acpi_madt_x2apic_t* x2apic = (const acpi_madt_x2apic_t*)ptr;
                    smp_register_cpu(x2apic->acpi_processor_uid, x2apic->x2apic_id, x2apic->flags);
                }
                break;
            }
            default:
                break;
        }

        ptr += hdr->length;
    }

    /* If MADT listed no enabled CPUs, fall back to single-CPU mode */
    if (g_smp_state.cpu_count == 0) {
        debuglog(DEBUG_WARN, "SMP: MADT has no enabled CPUs, single CPU mode\n");
        goto single_cpu_fallback;
    }

    g_smp_state.initialized = true;

    if (g_smp_state.cpu_count == 1) {
        debuglog(DEBUG_INFO, "SMP: Single CPU mode (BSP APIC ID=%u)\n",
                 g_smp_state.bsp_apic_id);
    } else {
        debuglog(DEBUG_INFO, "SMP: %u CPU(s) found in MADT (%u online) — "
                 "APs will be started separately\n",
                 g_smp_state.cpu_count, g_smp_state.online_cpus);
    }

    return true;

single_cpu_fallback:
    /*
     * Minimal single-CPU state: one BSP, no APs.
     * The rest of the kernel can always call smp_get_cpu_count() safely.
     */
    g_smp_state.cpu_count = 1;
    g_smp_state.online_cpus = 1;
    g_smp_state.bsp_index = 0;
    {
        smp_cpu_info_t* bsp = &g_smp_state.cpus[0];
        bsp->acpi_id = 0;
        bsp->apic_id = g_smp_state.bsp_apic_id;
        bsp->enabled = true;
        bsp->bsp = true;
        bsp->online = true;
    }
    g_smp_state.initialized = true;
    debuglog(DEBUG_INFO, "SMP: Running in single CPU mode\n");
    return true;
}

uint32 smp_get_cpu_count(void) {
    return g_smp_state.online_cpus > 0 ? g_smp_state.online_cpus : 1;
}

bool smp_has_smp(void) {
    return smp_get_cpu_count() > 1;
}

const smp_cpu_info_t* smp_get_cpu(uint32 index) {
    if (index >= g_smp_state.cpu_count) {
        return NULL;
    }
    return &g_smp_state.cpus[index];
}

const smp_state_t* smp_get_state(void) {
    return &g_smp_state;
}

uint32 smp_get_lapic_base(void) {
    return g_smp_state.lapic_base;
}

uint32 smp_get_bsp_index(void) {
    return g_smp_state.bsp_index;
}

void smp_mark_cpu_online(uint32 apic_id) {
    for (uint32 i = 0; i < g_smp_state.cpu_count; i++) {
        smp_cpu_info_t* cpu = &g_smp_state.cpus[i];
        if (cpu->apic_id == apic_id) {
            if (!cpu->online) {
                cpu->online = true;
                g_smp_state.online_cpus++;
            }
            break;
        }
    }
}
