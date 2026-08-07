/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_hw.h - "Hardware & Diag" tool category (KEY = hw).
 * =============================================================================
 * CPU / PCI / ACPI / SMBIOS / memory diagnostics, each a template-B wm.c window.
 * Needs firmware services (GetMemoryMap, GOP, Stall, EFI config tables) so it
 * exposes cat_hw_init() which stores gST/gBS/gRT (clock.c idiom). Tools that do
 * not need firmware services (CPUID, MSR, PCI, PIT via port I/O) still work when
 * cat_hw_init() was never called / services are NULL.
 *
 * Freestanding C11, no libc, no heap (except bounded BootServices AllocatePool
 * in the memory tester), integer math only (-mno-sse), all pre-ExitBootServices.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_HW_H
#define FOREB_UEFI_TOOLS_HW_H

#include "tools.h"

/* Per-tool open() callbacks (template B: open a wm window, return). */
void tool_hw_cpuid_open(void);    /* CPUID: vendor/brand, family/model, features */
void tool_hw_msr_open(void);      /* MSR reader: curated safe MSRs + typed index  */
void tool_hw_pci_open(void);      /* PCI lister: 0xCF8/0xCFC config-space scan     */
void tool_hw_acpi_open(void);     /* ACPI tables: RSDP -> RSDT/XSDT signatures     */
void tool_hw_smbios_open(void);   /* SMBIOS/DMI: entry point + structure walk      */
void tool_hw_memtest_open(void);  /* Memory pattern tester (bounded LoaderData)    */
void tool_hw_gop_open(void);      /* GOP mode lister: every QueryMode mode         */
void tool_hw_tsc_open(void);      /* TSC frequency estimate (rdtsc + Stall)        */
void tool_hw_pit_open(void);      /* Timer / PIT test (8254 channel 0 + speaker)   */

/* Store gST/gBS/gRT for the firmware-dependent tools. NULL-safe. */
void cat_hw_init(EFI_SYSTEM_TABLE *st);

/* Category exports (consumed by uefi/tools_registry.c). */
extern const struct forebo_tool cat_hw_tools[];
extern const int                cat_hw_count;

#endif /* FOREB_UEFI_TOOLS_HW_H */
