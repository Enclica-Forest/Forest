/*
 * Fern - Cross-Architecture PCI/PCIe Abstraction Layer
 * src/arch/pci.h
 *
 * Provides a unified PCI enumeration interface that works with:
 *   - x86 / x86_64: Port I/O (0xCF8/0xCFC) or ECAM (memory-mapped)
 *   - AArch64: ECAM via DTB /pci node (memory-mapped config space)
 *   - RISC-V: PCIe via DTB /soc/pci node (memory-mapped)
 *   - ARM32: Typically no PCI, but could have PCIe via DTB
 *
 * The implementation delegates to arch-specific backends.  On x86, ACPI
 * MCFG is preferred for ECAM; on ARM/RISC-V, the Device Tree provides
 * the ECAM base address and segment topology.
 */

#ifndef FOREST_ARCH_PCI_H
#define FOREST_ARCH_PCI_H

#include "../include/pci.h"
#include "../include/types.h"
#include <stdbool.h>

/* =========================================================================
 * 1. PCI Subsystem Initialization
 * ========================================================================= */

/**
 * arch_pci_init - Initialize the PCI subsystem for the current architecture.
 *
 * On x86: probes ACPI MCFG for ECAM segments, falls back to Type1 I/O.
 * On ARM64/RISC-V: parses DTB for PCIe ECAM node.
 *
 * Must be called once during boot before any pci_enumerate().
 * Returns true on success.
 */
bool arch_pci_init(void);

/* =========================================================================
 * 2. Low-Level Config Space Access
 * ========================================================================= */

/**
 * arch_pci_read_config - Read a 32-bit value from PCI config space.
 *
 * @bus:     PCI bus number (0-255)
 * @device:  PCI device number (0-31)
 * @function: PCI function number (0-7)
 * @offset:  Register offset within config space (0-4095 for PCIe ECAM)
 * @size:    Access width in bytes: 1, 2, or 4
 *
 * Returns the value read, zero-extended.  Returns 0xFFFFFFFF on error.
 */
uint32 arch_pci_read_config(uint8 bus, uint8 device, uint8 function,
                            uint16 offset, uint8 size);

/**
 * arch_pci_write_config - Write a value to PCI config space.
 *
 * @bus:     PCI bus number (0-255)
 * @device:  PCI device number (0-31)
 * @function: PCI function number (0-7)
 * @offset:  Register offset within config space
 * @value:   Value to write (only the low @size bytes are written)
 * @size:    Access width in bytes: 1, 2, or 4
 */
void arch_pci_write_config(uint8 bus, uint8 device, uint8 function,
                           uint16 offset, uint32 value, uint8 size);

/* Convenience wrappers for specific widths */
static inline uint8 arch_pci_read_config_byte(uint8 bus, uint8 dev, uint8 fn,
                                              uint16 offset)
{
    return (uint8)arch_pci_read_config(bus, dev, fn, offset, 1);
}

static inline uint16 arch_pci_read_config_word(uint8 bus, uint8 dev, uint8 fn,
                                               uint16 offset)
{
    return (uint16)arch_pci_read_config(bus, dev, fn, offset, 2);
}

static inline uint32 arch_pci_read_config_dword(uint8 bus, uint8 dev, uint8 fn,
                                                uint16 offset)
{
    return arch_pci_read_config(bus, dev, fn, offset, 4);
}

static inline void arch_pci_write_config_byte(uint8 bus, uint8 dev, uint8 fn,
                                              uint16 offset, uint8 val)
{
    arch_pci_write_config(bus, dev, fn, offset, val, 1);
}

static inline void arch_pci_write_config_word(uint8 bus, uint8 dev, uint8 fn,
                                              uint16 offset, uint16 val)
{
    arch_pci_write_config(bus, dev, fn, offset, val, 2);
}

static inline void arch_pci_write_config_dword(uint8 bus, uint8 dev, uint8 fn,
                                               uint16 offset, uint32 val)
{
    arch_pci_write_config(bus, dev, fn, offset, val, 4);
}

/* =========================================================================
 * 3. Device Enumeration
 * ========================================================================= */

/**
 * arch_pci_find_device - Find a PCI device by vendor/device ID.
 *
 * @vendor_id:  PCI vendor ID to match
 * @device_id:  PCI device ID to match
 * @out_device: Receives the found device info
 *
 * Returns true if a matching device was found.
 */
bool arch_pci_find_device(uint16 vendor_id, uint16 device_id,
                          pci_device_t *out_device);

/**
 * arch_pci_find_class - Find a PCI device by class/subclass code.
 *
 * @class_code: PCI class code to match (e.g., 0x01 for mass storage)
 * @subclass:   PCI subclass to match (e.g., 0x06 for SATA)
 * @out_device: Receives the found device info
 *
 * Returns true if a matching device was found.
 */
bool arch_pci_find_class(uint8 class_code, uint8 subclass,
                         pci_device_t *out_device);

/**
 * arch_pci_enumerate - Enumerate all PCI devices, calling a callback.
 *
 * @callback: Called for each device found.  Return false to stop.
 * @context:  Opaque pointer passed to the callback.
 */
typedef bool (*arch_pci_enum_callback_t)(const pci_device_t *device,
                                         void *context);

void arch_pci_enumerate(arch_pci_enum_callback_t callback, void *context);

/* =========================================================================
 * 4. ECAM Segment Information (for platforms with multiple segments)
 * ========================================================================= */

#define PCI_ARCH_MAX_SEGMENTS 16

/**
 * struct pci_ecam_segment - Describes one ECAM (Enhanced Config) segment.
 *
 * @base:       Physical base address of the ECAM MMIO region
 * @start_bus:  First bus number in this segment
 * @end_bus:    Last bus number in this segment (inclusive)
 * @segment:    PCI segment group number
 */
typedef struct {
    uint64 base;
    uint8  start_bus;
    uint8  end_bus;
    uint16 segment;
    bool   present;
} pci_ecam_segment_t;

/**
 * arch_pci_get_ecam_segments - Query discovered ECAM segments.
 *
 * @segments:   Array to fill (at most PCI_ARCH_MAX_SEGMENTS entries)
 * @max_count:  Size of the output array
 *
 * Returns the number of segments discovered.
 */
uint32 arch_pci_get_ecam_segments(pci_ecam_segment_t *segments,
                                  uint32 max_count);

/**
 * arch_pci_has_ecam - Check if ECAM (memory-mapped config) is available.
 *
 * Returns true if at least one valid ECAM segment was discovered.
 * If false, port I/O (Type1 CF8/CFC) is the only access method.
 */
bool arch_pci_has_ecam(void);

/* =========================================================================
 * 5. Architecture-Specific Access Mode Indication
 * ========================================================================= */

typedef enum {
    PCI_ACCESS_PORT_IO = 0,  /* x86 Type1 CF8/CFC */
    PCI_ACCESS_ECAM    = 1,  /* Memory-mapped ECAM */
    PCI_ACCESS_DT_ECAM = 2,  /* DTB-described ECAM (ARM/RISC-V) */
} pci_access_mode_t;

/**
 * arch_pci_get_access_mode - Return the active PCI config access method.
 */
pci_access_mode_t arch_pci_get_access_mode(void);

#endif /* FOREST_ARCH_PCI_H */
