#include "uefi_runtime.h"
#include "../include/debuglog.h"

static EFI_RUNTIME_SERVICES *g_runtime_services = NULL;
static EFI_SYSTEM_TABLE *g_system_table = NULL;
static bool g_uefi_runtime_initialized = false;

void uefi_runtime_init(EFI_SYSTEM_TABLE *system_table) {
    if (!system_table) {
        debuglog(DEBUG_WARN, "[UEFI] No system table provided\n");
        return;
    }

    g_system_table = system_table;

    if (system_table->runtime_services) {
        g_runtime_services = (EFI_RUNTIME_SERVICES *)system_table->runtime_services;
        debuglog(DEBUG_INFO, "[UEFI] Runtime services found at 0x%x\n",
                 (uint32_t)(uint64_t)g_runtime_services);

        if (g_runtime_services->GetTime) {
            debuglog(DEBUG_INFO, "[UEFI] GetTime: available\n");
        }
        if (g_runtime_services->SetTime) {
            debuglog(DEBUG_INFO, "[UEFI] SetTime: available\n");
        }
        if (g_runtime_services->GetVariable) {
            debuglog(DEBUG_INFO, "[UEFI] GetVariable: available\n");
        }
        if (g_runtime_services->SetVariable) {
            debuglog(DEBUG_INFO, "[UEFI] SetVariable: available\n");
        }
        if (g_runtime_services->ResetSystem) {
            debuglog(DEBUG_INFO, "[UEFI] ResetSystem: available\n");
        }
        if (g_runtime_services->SetVirtualAddressMap) {
            debuglog(DEBUG_INFO, "[UEFI] SetVirtualAddressMap: available\n");
        }
        if (g_runtime_services->ConvertPointer) {
            debuglog(DEBUG_INFO, "[UEFI] ConvertPointer: available\n");
        }
    } else {
        debuglog(DEBUG_WARN, "[UEFI] No runtime services in system table\n");
    }

    g_uefi_runtime_initialized = true;
    debuglog(DEBUG_INFO, "[UEFI] Runtime services initialized\n");
}

void uefi_runtime_set_virtual_address(EFI_MEMORY_DESCRIPTOR *map, UINTN count) {
    if (!g_runtime_services || !g_runtime_services->SetVirtualAddressMap) {
        debuglog(DEBUG_WARN, "[UEFI] SetVirtualAddressMap not available\n");
        return;
    }

    if (!map || count == 0) {
        debuglog(DEBUG_WARN, "[UEFI] Invalid map for SetVirtualAddressMap\n");
        return;
    }

    debuglog(DEBUG_INFO, "[UEFI] SetVirtualAddressMap: %u descriptors\n", (uint32_t)count);

    for (UINTN i = 0; i < count; i++) {
        debuglog(DEBUG_DETAIL, "[UEFI]   Descriptor %u: type=%u phys=0x%x virt=0x%x pages=%u attr=0x%x\n",
                 (uint32_t)i, map[i].type,
                 (uint32_t)map[i].phys_addr, (uint32_t)map[i].virt_addr,
                 (uint32_t)map[i].num_pages, (uint32_t)map[i].attribute);
    }

    EFI_STATUS status = g_runtime_services->SetVirtualAddressMap(
        count * sizeof(EFI_MEMORY_DESCRIPTOR),
        sizeof(EFI_MEMORY_DESCRIPTOR),
        1,
        map
    );

    if (status == 0) {
        debuglog(DEBUG_INFO, "[UEFI] SetVirtualAddressMap succeeded\n");
    } else {
        debuglog(DEBUG_WARN, "[UEFI] SetVirtualAddressMap failed: 0x%x\n", (uint32_t)status);
    }
}

const EFI_RUNTIME_SERVICES *uefi_runtime_get_services(void) {
    return g_runtime_services;
}

bool uefi_runtime_is_initialized(void) {
    return g_uefi_runtime_initialized;
}
