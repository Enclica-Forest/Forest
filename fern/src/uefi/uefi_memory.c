#include "uefi_boot_services.h"
#include "../include/memory.h"
#include "../arch/pmm.h"

/* Maximum number of memory regions we can handle */
#define MAX_MEMORY_REGIONS 128

/* Kernel memory regions extracted from UEFI memory map */
static memory_region_t g_memory_regions[MAX_MEMORY_REGIONS];
static uint32_t g_region_count = 0;

/* Raw EFI memory descriptors (kept for pmm_init_from_memory_map) */
static uint8_t g_raw_descriptors[8192];
static uint32_t g_raw_desc_size = 0;

/* Total memory statistics */
static uint64_t g_total_memory = 0;
static uint64_t g_usable_memory = 0;

/*
 * Convert UEFI memory type to kernel memory region type
 */
static memory_region_type_t uefi_type_to_kernel_type(uint32_t uefi_type)
{
    switch (uefi_type) {
        case EfiConventionalMemory:
            return MEMORY_REGION_AVAILABLE;
        case EfiReservedMemoryType:
            return MEMORY_REGION_RESERVED;
        case EfiACPIReclaimMemory:
            return MEMORY_REGION_ACPI_RECLAIM;
        case EfiACPIMemoryNVS:
            return MEMORY_REGION_ACPI_NVS;
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
            return MEMORY_REGION_KERNEL;
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
            return MEMORY_REGION_RESERVED;
        case EfiUnusableMemory:
            return MEMORY_REGION_BADRAM;
        default:
            return MEMORY_REGION_RESERVED;
    }
}

/*
 * Get memory map from UEFI and convert to kernel format
 * Returns 0 on success, non-zero on failure
 */
int uefi_get_memory_map(EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_STATUS status;
    UINTN memory_map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    uint32_t descriptor_version = 0;
    EFI_MEMORY_DESCRIPTOR* memory_map = NULL;

    if (!SystemTable || !SystemTable->BootServices) {
        return -1;
    }

    EFI_BOOT_SERVICES* bs = SystemTable->BootServices;

    /* First call to get the required buffer size */
    status = bs->GetMemoryMap(
        &memory_map_size,
        NULL,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );

    if (EFI_ERROR(status) && status != EFI_BUFFER_TOO_SMALL) {
        return -2;
    }

    /* Allocate buffer for memory map (add extra space for alignment) */
    memory_map_size += descriptor_size * 8;
    status = bs->AllocatePool(
        EfiBootServicesData,
        memory_map_size,
        (void**)&memory_map
    );

    if (EFI_ERROR(status) || !memory_map) {
        return -3;
    }

    /* Get the actual memory map */
    status = bs->GetMemoryMap(
        &memory_map_size,
        memory_map,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );

    if (EFI_ERROR(status)) {
        bs->FreePool(memory_map);
        return -4;
    }

    /* Reset counters */
    g_region_count = 0;
    g_total_memory = 0;
    g_usable_memory = 0;

    /* Save raw descriptors for pmm_init_from_memory_map() */
    g_raw_desc_size = 0;
    if (memory_map_size <= sizeof(g_raw_descriptors)) {
        /* Copy before freeing the UEFI-allocated buffer */
        for (uint32_t i = 0; i < memory_map_size; i++)
            g_raw_descriptors[i] = ((uint8_t*)memory_map)[i];
        g_raw_desc_size = memory_map_size;
    }

    /* Convert UEFI memory descriptors to kernel format */
    uint32_t num_descriptors = memory_map_size / descriptor_size;
    EFI_MEMORY_DESCRIPTOR* desc = memory_map;

    for (uint32_t i = 0; i < num_descriptors && g_region_count < MAX_MEMORY_REGIONS; i++) {
        memory_region_type_t type = uefi_type_to_kernel_type(desc->Type);

        /* Track total memory */
        g_total_memory += desc->NumberOfPages * 4096;

        /* Track usable memory (conventional memory only) */
        if (desc->Type == EfiConventionalMemory) {
            g_usable_memory += desc->NumberOfPages * 4096;
        }

        /* Store region info */
        memory_region_t* region = &g_memory_regions[g_region_count];
        region->base_address = desc->PhysicalStart;
        region->length = desc->NumberOfPages * 4096;
        region->type = type;
        region->validated = 1;
        region->usable = (type == MEMORY_REGION_AVAILABLE);

        /* Add description based on type */
        switch (desc->Type) {
            case EfiConventionalMemory:
                region->description = "Available RAM";
                break;
            case EfiReservedMemoryType:
                region->description = "Reserved";
                break;
            case EfiACPIReclaimMemory:
                region->description = "ACPI Reclaimable";
                break;
            case EfiACPIMemoryNVS:
                region->description = "ACPI NVS";
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
                region->description = "Loader";
                break;
            case EfiBootServicesCode:
            case EfiBootServicesData:
                region->description = "Boot Services";
                break;
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
                region->description = "Runtime Services";
                break;
            case EfiMemoryMappedIO:
                region->description = "Memory Mapped I/O";
                break;
            case EfiMemoryMappedIOPortSpace:
                region->description = "I/O Port Space";
                break;
            case EfiUnusableMemory:
                region->description = "Bad Memory";
                break;
            default:
                region->description = "Unknown";
                break;
        }

        g_region_count++;
        desc = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)desc + descriptor_size);
    }

    /* Free the memory map buffer */
    bs->FreePool(memory_map);

    return 0;
}

/*
 * Get memory regions for PMM initialization
 * Returns pointer to regions array and fills count
 */
memory_region_t* uefi_get_regions(uint32_t* count)
{
    if (count) {
        *count = g_region_count;
    }
    return g_memory_regions;
}

/*
 * Get total physical memory in bytes
 */
uint64_t uefi_get_total_memory(void)
{
    return g_total_memory;
}

/*
 * Get usable physical memory in bytes
 */
uint64_t uefi_get_usable_memory(void)
{
    return g_usable_memory;
}

/*
 * Initialize PMM with UEFI memory map
 * This should be called after uefi_get_memory_map()
 */
int uefi_init_pmm(EFI_SYSTEM_TABLE* SystemTable)
{
    if (!SystemTable) {
        return -1;
    }

    /* Get the memory map first */
    int result = uefi_get_memory_map(SystemTable);
    if (result != 0) {
        return result;
    }

    /* Initialize PMM with the detected regions */
    /* The kernel's pmm_init expects memory_region_t array */
    /* We already have g_memory_regions populated */

    return 0;
}

/*
 * Print memory map for debugging
 */
void uefi_print_memory_map(void)
{
    static const char* type_names[] = {
        "Reserved",
        "Available",
        "ACPI Reclaim",
        "ACPI NVS",
        "Kernel",
        "Bad RAM"
    };

    for (uint32_t i = 0; i < g_region_count; i++) {
        memory_region_t* r = &g_memory_regions[i];
        const char* type_str = "Unknown";
        if (r->type >= 0 && r->type <= MEMORY_REGION_BADRAM) {
            type_str = type_names[r->type];
        }
        /* Note: In actual UEFI environment, we'd use ConOut to print */
        /* This is a stub for kernel-side debugging */
        (void)type_str;
        (void)r;
    }
}

/*
 * Initialize PMM from UEFI memory map (convenience wrapper).
 * Calls uefi_get_memory_map() to populate both the kernel regions and
 * the raw descriptor buffer, then passes the raw descriptors to
 * pmm_init_from_memory_map() which handles architecture-specific parsing.
 */
int uefi_pmm_init(EFI_SYSTEM_TABLE* SystemTable)
{
    if (!SystemTable)
        return -1;

    /* Get the memory map (populates g_raw_descriptors + g_memory_regions) */
    int result = uefi_get_memory_map(SystemTable);
    if (result != 0)
        return result;

    if (g_raw_desc_size == 0)
        return -1;

    /* Pass raw EFI descriptors to the arch-specific PMM init */
    return pmm_init_from_memory_map(g_raw_descriptors, g_raw_desc_size);
}
