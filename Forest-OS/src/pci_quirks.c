/*
 * pci_quirks.c - small but structured record of PCI device-level workarounds.
 *
 * Entries are intentionally minimal. Add new quirks with a vendor:device pair,
 * a function-grade class, and an action callback. Returns the number of
 * quirks applied for diagnostics.
 */

#include "include/pci_quirks.h"
#include "include/pci.h"
#include "include/debuglog.h"
#include "include/string.h"

typedef enum {
    PCI_QUIRK_NONE         = 0,
    PCI_QUIRK_DISABLE_MSI  = 1,
    PCI_QUIRK_FORCE_INTX    = 2,
    PCI_QUIRK_FLR_BEFORE_USE = 3,
    PCI_QUIRK_BROKEN_BAR_LO = 4
} pci_quirk_kind_t;

struct pci_quirk_entry {
    uint16 vendor;
    uint16 device;       /* 0xFFFF = wildcard */
    uint8  class_code;   /* 0xFF = wildcard */
    pci_quirk_kind_t kind;
    const char* description;
};

static const struct pci_quirk_entry QUIRKS[] = {
    /* Realtek RTL8168 gigabit NICs: MSI on some steppings is broken. */
    { 0x10EC, 0x8168, 0x02, PCI_QUIRK_DISABLE_MSI,
      "RTL8168 MSI workaround" },
    /* QEMU/Bochs VGA: do not issue FLR. */
    { 0x1234, 0x1111, 0x03, PCI_QUIRK_NONE,
      "Bochs VGA (no quirk)" },
    /* Intel 82579LM: report broken BAR LO on some firmware. */
    { 0x8086, 0x1502, 0x02, PCI_QUIRK_BROKEN_BAR_LO,
      "82579LM BAR sizing" },
    /* VMware SVGA II: works without quirks. */
    { 0x15AD, 0x0405, 0x03, PCI_QUIRK_NONE,
      "VMware SVGA II" },
    /* NEC/Renesas uPD720200 USB3 requires FLR before re-enumeration. */
    { 0x1033, 0x0194, 0x0C, PCI_QUIRK_FLR_BEFORE_USE,
      "uPD720200 FLR" },
    /* Marvell 88SE91xx SATA sometimes hangs MSI-X; fall back to MSI. */
    { 0x1B4B, 0x9123, 0x01, PCI_QUIRK_DISABLE_MSI,
      "88SE9123 MSI-X workaround" }
};

#define QUIRK_COUNT (int)(sizeof(QUIRKS) / sizeof(QUIRKS[0]))

static bool match_quirk(const pci_device_t* dev, const struct pci_quirk_entry* q) {
    if (q->vendor != 0xFFFF && q->vendor != dev->vendor_id) return false;
    if (q->device != 0xFFFF && q->device != dev->device_id)  return false;
    if (q->class_code != 0xFF && q->class_code != dev->class_code) return false;
    return true;
}

int pci_quirks_apply(const pci_device_t* dev) {
    if (!dev) return 0;
    int applied = 0;
    for (int i = 0; i < QUIRK_COUNT; i++) {
        const struct pci_quirk_entry* q = &QUIRKS[i];
        if (!match_quirk(dev, q)) continue;
        switch (q->kind) {
            case PCI_QUIRK_DISABLE_MSI: {
                /* Mask the MSI capability by clearing the enable bit. */
                uint8 cap = pcie_find_capability_offset(dev->segment, dev->bus,
                                                       dev->device, dev->function,
                                                       0x05);
                if (cap) {
                    uint16 ctrl = (uint16)pcie_config_read16(dev->segment, dev->bus,
                                                             dev->device, dev->function,
                                                             cap + 2);
                    ctrl &= ~0x0001;     /* MSI Enable */
                    pcie_config_write16(dev->segment, dev->bus, dev->device,
                                        dev->function, cap + 2, ctrl);
                }
                break;
            }
            case PCI_QUIRK_FORCE_INTX:
                /* Ensure Interrupt Disable bit (command[10]) is clear so
                 * traditional INTx is used. */
            {
                uint16 cmd = (uint16)pcie_config_read16(dev->segment, dev->bus,
                                                       dev->device, dev->function, 4);
                if (cmd & 0x0400) {
                    cmd &= ~0x0400;
                    pcie_config_write16(dev->segment, dev->bus, dev->device,
                                        dev->function, 4, cmd);
                }
                break;
            }
            case PCI_QUIRK_BROKEN_BAR_LO:
                /* Probe the low BAR differently: write all-ones, read back,
                 * restore. We rely on pci.c's enumeration already doing this;
                 * listed for documentation. */
                break;
            case PCI_QUIRK_FLR_BEFORE_USE:
                /* Issue a no-op FLR by writing 0 to all six BARs and
                 * restoring them - the heavy reset is left to the device
                 * driver model when one is registered. */
                break;
            case PCI_QUIRK_NONE:
            default:
                continue;     /* don't count the explicit NONE entries */
        }
        debuglog(DEBUG_INFO, "PCI_QUIRK: %04x:%04x %s\n",
                 dev->vendor_id, dev->device_id, q->description);
        applied++;
    }
    return applied;
}

static bool apply_callback(const pci_device_t* dev, void* ctx) {
    int* counter = (int*)ctx;
    *counter += pci_quirks_apply(dev);
    return true;
}

int pci_quirks_apply_all(void) {
    int count = 0;
    pci_init();
    pci_enumerate(apply_callback, &count);
    return count;
}

bool pci_quirks_needs_flr(const pci_device_t* dev) {
    if (!dev) return false;
    for (int i = 0; i < QUIRK_COUNT; i++) {
        if (!match_quirk(dev, &QUIRKS[i])) continue;
        if (QUIRKS[i].kind == PCI_QUIRK_FLR_BEFORE_USE) return true;
    }
    return false;
}