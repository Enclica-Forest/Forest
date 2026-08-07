#include "include/acpi.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/memory.h"
#include "include/mm.h"
#include "include/util.h"
#include "include/io.h"
#include "include/timer.h"

// APIC structure types
#define APIC_TYPE_LOCAL_APIC 0
#define APIC_TYPE_IO_APIC 1
#define APIC_TYPE_INTERRUPT_OVERRIDE 2

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) apic_header_t;

typedef struct {
    apic_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) apic_local_apic_t;

typedef struct {
    apic_header_t header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_addr;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) apic_io_apic_t;

typedef struct {
    apic_header_t header;
    uint8_t bus;
    uint8_t source;
    uint32_t interrupt;
    uint16_t flags;
} __attribute__((packed)) apic_interrupt_override_t;

static const acpi_madt_header_t* g_madt = NULL;
static const acpi_fadt_t* g_fadt = NULL;
static bool g_acpi_initialized = false;

// Helper functions
static uint8_t acpi_checksum(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum;
}

static void acpi_parse_fadt(const acpi_fadt_t* fadt) {
    debuglog(DEBUG_INFO, "[ACPI] Found FADT at 0x%x\n", (uint32_t)fadt);
    g_fadt = fadt;

    // Enable ACPI mode if needed
    if (fadt->smi_command != 0 && fadt->acpi_enable != 0) {
        debuglog(DEBUG_INFO, "[ACPI] Enabling ACPI mode\n");

        // Write ACPI_ENABLE to SMI command port
        outb(fadt->smi_command, fadt->acpi_enable);

        // Wait for ACPI to be enabled (bit 0 of PM1a control)
        uint32_t pm1a_cnt = fadt->pm1a_control_block;
        if (pm1a_cnt != 0) {
            uint32_t timeout = 3000; // 3 seconds
            uint32_t start = timer_get_ticks();

            while ((inw(pm1a_cnt) & 1) == 0) {
                if (timer_get_ticks() - start > timeout) {
                    debuglog(DEBUG_WARN, "[ACPI] Timeout waiting for ACPI enable\n");
                    break;
                }
                timer_sleep_ms(10);
            }

            if (inw(pm1a_cnt) & 1) {
                debuglog(DEBUG_INFO, "[ACPI] ACPI mode enabled successfully\n");
            }
        }
    } else {
        debuglog(DEBUG_INFO, "[ACPI] ACPI already enabled\n");
    }
}

// Parse MADT
static void acpi_parse_madt(const acpi_madt_header_t* madt) {
    debuglog(DEBUG_INFO, "[ACPI] Found MADT at 0x%x\n", (uint32_t)madt);
    g_madt = madt;

    debuglog(DEBUG_INFO, "[ACPI] Local APIC Address = 0x%x\n", madt->local_apic_addr);

    uint8_t* p = (uint8_t*)(madt + 1);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (p < end) {
        acpi_madt_entry_header_t* header = (acpi_madt_entry_header_t*)p;
        uint8_t type = header->type;
        uint8_t length = header->length;

        if (type == 0) { // Local APIC
            acpi_madt_lapic_t* s = (acpi_madt_lapic_t*)p;
            debuglog(DEBUG_INFO, "[ACPI] Found CPU: acpi_id=%d apic_id=%d flags=0x%x\n",
                     s->acpi_processor_id, s->apic_id, s->flags);
        } else {
            debuglog(DEBUG_INFO, "[ACPI] Unknown MADT entry type %d\n", type);
        }

        p += length;
    }
}

// Parse a table
static void acpi_parse_table(const acpi_sdt_header_t* header) {
    char sig[5];
    memcpy(sig, header->signature, 4);
    sig[4] = '\0';

    debuglog(DEBUG_INFO, "[ACPI] Parsing table %s (0x%x bytes)\n",
             sig, header->length);

    uint32_t signature = *(uint32_t*)header->signature;
    if (signature == 0x50434146) { // FADT
        acpi_parse_fadt((const acpi_fadt_t*)header);
    } else if (signature == 0x43495041) { // MADT
        acpi_parse_madt((const acpi_madt_header_t*)header);
    } else {
        // Other tables can be parsed here as needed
        debuglog(DEBUG_INFO, "[ACPI] Skipping table %s\n", sig);
    }
}

// Map ACPI table to virtual memory
const acpi_sdt_header_t* acpi_map_table(uint64 phys_addr) {
    uint32_t phys_32 = (uint32_t)phys_addr;
    if (phys_32 < 0x100000) {
        // Identity mapped
        return (const acpi_sdt_header_t*)(uintptr_t)phys_32;
    }

    // Map the physical page to kernel virtual address
    uint32_t page_addr = phys_32 & ~0xFFF;
    uint32_t offset = phys_32 & 0xFFF;

    // Find a free virtual address in kernel space
    uint32_t virt_base = 0xD0000000; // Use 3GB + 256MB area for ACPI mappings
    uint32_t virt_addr = virt_base + (page_addr >> 12) * 0x1000;

    // Map the page
    page_directory_t* dir = vmm_get_current_page_directory();
    if (vmm_map_page(dir, virt_addr, page_addr, PAGE_PRESENT | PAGE_WRITABLE) == MEMORY_OK) {
        return (const acpi_sdt_header_t*)(virt_addr + offset);
    }

    debuglog(DEBUG_WARN, "[ACPI] Failed to map table at 0x%x\n", phys_32);
    return NULL;
}

// Parse RSDT
static void acpi_parse_rsdt(const acpi_sdt_header_t* rsdt) {
    uint32_t* p = (uint32_t*)(rsdt + 1);
    uint32_t* end = (uint32_t*)((uint8_t*)rsdt + rsdt->length);

    while (p < end) {
        uint32_t address = *p++;
        const acpi_sdt_header_t* table = acpi_map_table(address);

        if (table && acpi_checksum((const void*)table, table->length) == 0) {
            acpi_parse_table(table);
        } else {
            debuglog(DEBUG_WARN, "[ACPI] Table at 0x%x invalid or unmappable\n", address);
        }
    }
}

// Parse XSDT
static void acpi_parse_xsdt(const acpi_sdt_header_t* xsdt) {
    uint64_t* p = (uint64_t*)(xsdt + 1);
    uint64_t* end = (uint64_t*)((uint8_t*)xsdt + xsdt->length);

    while (p < end) {
        uint64_t address = *p++;
        const acpi_sdt_header_t* table = acpi_map_table(address);

        if (table && acpi_checksum((const void*)table, table->length) == 0) {
            acpi_parse_table(table);
        } else {
            debuglog(DEBUG_WARN, "[ACPI] Table at 0x%x invalid or unmappable\n", (uint32_t)address);
        }
    }
}


// Main ACPI init function
bool acpi_init_with_multiboot(uint32 multiboot_magic, uint32 mbi_addr) {
    (void)multiboot_magic;
    (void)mbi_addr;

    if (g_acpi_initialized) {
        return true;
    }
    g_acpi_initialized = true;

    debuglog(DEBUG_INFO, "[ACPI] Starting ACPI initialization\n");

    // Search for RSDP in BIOS area
    uint8_t* p = (uint8_t*)0x000E0000;
    uint8_t* end = (uint8_t*)0x00100000;
    const char* rsdp_sig = "RSD PTR ";

    const acpi_sdt_header_t* rsdt = NULL;
    const acpi_sdt_header_t* xsdt = NULL;

    while (p < end) {
        if (memcmp(p, rsdp_sig, 8) == 0) {
            // Found RSDP
            debuglog(DEBUG_INFO, "[ACPI] RSDP found at 0x%x\n", (uint32_t)p);

            // Verify checksum
            if (acpi_checksum(p, 20) != 0) {
                debuglog(DEBUG_WARN, "[ACPI] RSDP checksum failed\n");
                p += 16;
                continue;
            }

            uint8_t revision = p[15];
            uint32_t rsdt_addr = *(uint32_t*)(p + 16);

            rsdt = acpi_map_table(rsdt_addr);
            if (!rsdt || acpi_checksum(rsdt, rsdt->length) != 0) {
                debuglog(DEBUG_WARN, "[ACPI] RSDT invalid\n");
                rsdt = NULL;
            }

            if (revision >= 2) {
                uint64_t xsdt_addr = *(uint64_t*)(p + 24);
                if (xsdt_addr != 0) {
                    xsdt = acpi_map_table(xsdt_addr);
                    if (!xsdt || acpi_checksum(xsdt, xsdt->length) != 0) {
                        debuglog(DEBUG_WARN, "[ACPI] XSDT invalid\n");
                        xsdt = NULL;
                    }
                }
            }

            break;
        }
        p += 16;
    }

    if (!rsdt && !xsdt) {
        debuglog(DEBUG_WARN, "[ACPI] No valid RSDT or XSDT found\n");
        return false;
    }

    // Parse tables
    if (xsdt) {
        acpi_parse_xsdt(xsdt);
    } else if (rsdt) {
        acpi_parse_rsdt(rsdt);
    }

    debuglog(DEBUG_INFO, "[ACPI] Initialization complete\n");
    return true;
}

// Stub functions for compatibility
bool acpi_shutdown(void) {
    debuglog(DEBUG_INFO, "[ACPI] Shutdown not implemented\n");
    return false;
}

bool acpi_reboot(void) {
    debuglog(DEBUG_INFO, "[ACPI] Reboot not implemented\n");
    return false;
}

const char* acpi_get_last_error(void) {
    return "ACPI error";
}

void acpi_set_last_error(const char* error) {
    (void)error;
}

const acpi_fadt_t* acpi_get_fadt(void) {
    return g_fadt;
}

const acpi_madt_header_t* acpi_get_madt(void) {
    return g_madt;
}

const acpi_mcfg_table_t* acpi_get_mcfg(void) {
    return NULL; // Not implemented
}

const acpi_rsdp_t* acpi_get_rsdp(void) {
    return NULL; // Not implemented
}

const acpi_rsdp_t* acpi_find_rsdp(void) {
    return NULL; // Not implemented
}

bool acpi_init(void) {
    return acpi_init_with_multiboot(0, 0);
}



const acpi_sdt_header_t* acpi_find_table(const char* signature) {
    // Not implemented
    (void)signature;
    return NULL;
}



uint32 acpi_remap_irq(uint32 irq) {
    if (!g_madt) {
        return irq;
    }

    uint8_t* p = (uint8_t*)(g_madt + 1);
    uint8_t* end = (uint8_t*)g_madt + g_madt->header.length;

    while (p < end) {
        apic_header_t* header = (apic_header_t*)p;
        uint8_t type = header->type;
        uint8_t length = header->length;

        if (type == APIC_TYPE_INTERRUPT_OVERRIDE) {
            apic_interrupt_override_t* s = (apic_interrupt_override_t*)p;
            if (s->source == irq) {
                return s->interrupt;
            }
        }

        p += length;
    }

    return irq;
}