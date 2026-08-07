#ifndef UEFI_MEMORY_H
#define UEFI_MEMORY_H

#include "uefi_boot_services.h"
#include "../include/memory.h"

/* Get memory map from UEFI and convert to kernel format
 * Returns 0 on success, non-zero on failure
 */
int uefi_get_memory_map(EFI_SYSTEM_TABLE* SystemTable);

/* Get memory regions for PMM initialization
 * Returns pointer to regions array and fills count
 */
memory_region_t* uefi_get_regions(uint32_t* count);

/* Get total physical memory in bytes */
uint64_t uefi_get_total_memory(void);

/* Get usable physical memory in bytes */
uint64_t uefi_get_usable_memory(void);

/* Initialize PMM with UEFI memory map
 * This should be called after uefi_get_memory_map()
 */
int uefi_init_pmm(EFI_SYSTEM_TABLE* SystemTable);

/* Initialize PMM from UEFI memory map (convenience wrapper)
 * Calls uefi_get_memory_map() then pmm_init_from_memory_map() with the
 * raw EFI descriptors.  Should be called before kernel_main().
 * Returns 0 on success, negative on error.
 */
int uefi_pmm_init(EFI_SYSTEM_TABLE* SystemTable);

/* Print memory map for debugging */
void uefi_print_memory_map(void);

#endif /* UEFI_MEMORY_H */
