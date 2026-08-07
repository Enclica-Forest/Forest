#ifndef PCI_QUIRKS_H
#define PCI_QUIRKS_H

#include "types.h"
#include "pci.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apply known PCI device quirks (resets, BAR sizing workarounds, broken MSI,
 * device-specific config tweaks). Idempotent. Returns number of quirks
 * applied. */
int pci_quirks_apply(const pci_device_t* dev);

/* Whole-bus enumeration helper used at boot to apply quirks to every
 * enumerated device. */
int pci_quirks_apply_all(void);

/* Query if the given PCI device is known to need a function-level reset
 * before BAR probing (some NVMe controllers / broken GPUs). */
bool pci_quirks_needs_flr(const pci_device_t* dev);

#ifdef __cplusplus
}
#endif

#endif /* PCI_QUIRKS_H */