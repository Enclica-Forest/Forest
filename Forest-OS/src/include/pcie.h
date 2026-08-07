#ifndef PCIE_H
#define PCIE_H

#include "pci.h"
#include <stdbool.h>

#ifndef ENABLE_PCIE_ECAM
#  define ENABLE_PCIE_ECAM 1
#endif

void pcie_print_device_info(const pci_device_t* device);

uint32 pcie_get_device_capabilities(const pci_device_t* device);
uint16 pcie_get_device_status(const pci_device_t* device);
uint32 pcie_get_link_capabilities(const pci_device_t* device);
uint16 pcie_get_link_status(const pci_device_t* device);

bool pcie_is_endpoint(const pci_device_t* device);
bool pcie_is_root_port(const pci_device_t* device);
bool pcie_is_bridge(const pci_device_t* device);

bool pcie_has_capability(const pci_device_t* device, uint8 capability_id);
uint8 pcie_get_msix_capability(const pci_device_t* device);
uint8 pcie_get_msi_capability(const pci_device_t* device);

/* ECAM (Enhanced Configuration Access Mechanism) direct MMIO access. The
 * base is taken from the ACPI MCFG table; if missing, these fall back to
 * the Type1 CF8/CFC pair. Returns 0xFFFFFFFF on no-MCFG reads. */
uint32 pcie_ecam_read (uint16 segment, uint8 bus, uint8 dev, uint8 fn, uint16 off);
void   pcie_ecam_write(uint16 segment, uint8 bus, uint8 dev, uint8 fn, uint16 off, uint32 value);

/* Capability enumeration including extended (PCIe 2.0+) caps at 0x100+:
 *   MSI(0x05) MSI-X(0x11) PCIe(0x10) PM(0x01) ACS(0x0D) ARI(0x0E) */
typedef struct {
    uint8  cap_id;
    uint16 offset;
    uint8  version;
} pcie_cap_entry_t;

#define PCIE_CAP_LIST_MAX 32
typedef struct {
    uint16            count;
    pcie_cap_entry_t  entries[PCIE_CAP_LIST_MAX];
    uint16            ext_count;
    pcie_cap_entry_t  ext_entries[PCIE_CAP_LIST_MAX];
} pcie_cap_list_t;

int  pcie_enumerate_capabilities(const pci_device_t* device, pcie_cap_list_t* out);

/* Standard capability IDs we know about (lives alongside PCIE_CAP_ID_*). */
#define PCIE_CAP_ID_POWER_MGMT   0x01
#define PCIE_CAP_ID_ACS          0x0D
#define PCIE_CAP_ID_ARI          0x0E   /* Alternate Routing ID */

bool pcie_run_tests(void);

#endif