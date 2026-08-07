#include "include/pcie.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/pci.h"

static const char* pcie_port_type_strings[] = {
    "PCIe Endpoint",
    "Legacy Endpoint",
    "Unknown (2)",
    "Unknown (3)",
    "Root Port",
    "Upstream Port",
    "Downstream Port",
    "PCI-to-PCIe Bridge",
    "PCIe-to-PCI Bridge",
    "Root Complex Integrated Endpoint",
    "Root Complex Event Collector"
};

static const char* pcie_speed_strings[] = {
    "Unknown",
    "2.5 GT/s (Gen1)",
    "5.0 GT/s (Gen2)",
    "8.0 GT/s (Gen3)",
    "16.0 GT/s (Gen4)",
    "32.0 GT/s (Gen5)"
};

static const char* pcie_get_port_type_string(uint8 port_type) {
    if (port_type < sizeof(pcie_port_type_strings) / sizeof(pcie_port_type_strings[0])) {
        return pcie_port_type_strings[port_type];
    }
    return "Unknown";
}

static const char* pcie_get_speed_string(uint8 speed) {
    if (speed < sizeof(pcie_speed_strings) / sizeof(pcie_speed_strings[0])) {
        return pcie_speed_strings[speed];
    }
    return "Unknown";
}

void pcie_print_device_info(const pci_device_t* device) {
    debuglog_printf("PCIe Device: %04x:%02x:%02x.%x\n",
                   device->segment, device->bus, device->device, device->function);
    debuglog_printf("  Vendor:Device: %04x:%04x\n", device->vendor_id, device->device_id);
    debuglog_printf("  Class: %02x:%02x:%02x\n", device->class_code, device->subclass, device->prog_if);
    debuglog_printf("  Revision: %d\n", device->revision_id);
    debuglog_printf("  Header Type: 0x%02x\n", device->header_type);
    
    if (device->is_pcie) {
        debuglog_write("  PCIe Device: YES\n");
        debuglog_printf("  PCIe Port Type: %s\n", pcie_get_port_type_string(device->pcie_device_port_type));
        debuglog_printf("  PCIe Speed: %s\n", pcie_get_speed_string(device->pcie_link_speed));
        debuglog_printf("  PCIe Width: x%d\n", device->pcie_link_width);
    } else {
        debuglog_write("  PCIe Device: NO (Conventional PCI)\n");
    }
    
    for (uint8 i = 0; i < PCI_BAR_COUNT; i++) {
        if (device->bar[i] != 0) {
            uint32 bar = device->bar[i];
            uint8 bar_type = 0;
            
            if ((bar & 0x1) == 0) {
                bar_type = (bar & 0x6) >> 1;
                uint64 address = bar & ~0xF;
                if (bar_type == 2) {
                    address |= ((uint64)device->bar[i + 1]) << 32;
                }
                debuglog_printf("  BAR[%d]: Memory - Type %d, Address: 0x%llx\n", 
                               i, bar_type, address);
                if (bar_type == 2) i++;
            } else {
                debuglog_printf("  BAR[%d]: I/O - Address: 0x%x\n", i, bar & ~0x3);
            }
        }
    }
}

uint32 pcie_get_device_capabilities(const pci_device_t* device) {
    if (!device->is_pcie || device->pcie_cap_offset == 0) {
        return 0;
    }
    
    return pcie_config_read32(device->segment, device->bus, device->device, 
                              device->function, device->pcie_cap_offset + PCIE_DEVICE_CAP_OFFSET);
}

uint16 pcie_get_device_status(const pci_device_t* device) {
    if (!device->is_pcie || device->pcie_cap_offset == 0) {
        return 0;
    }
    
    return pcie_config_read16(device->segment, device->bus, device->device, 
                              device->function, device->pcie_cap_offset + PCIE_DEVICE_STATUS_OFFSET);
}

uint32 pcie_get_link_capabilities(const pci_device_t* device) {
    if (!device->is_pcie || device->pcie_cap_offset == 0) {
        return 0;
    }
    
    return pcie_config_read32(device->segment, device->bus, device->device, 
                              device->function, device->pcie_cap_offset + PCIE_LINK_CAP_OFFSET);
}

uint16 pcie_get_link_status(const pci_device_t* device) {
    if (!device->is_pcie || device->pcie_cap_offset == 0) {
        return 0;
    }
    
    return pcie_config_read16(device->segment, device->bus, device->device, 
                              device->function, device->pcie_cap_offset + PCIE_LINK_STATUS_OFFSET);
}

bool pcie_is_endpoint(const pci_device_t* device) {
    return device->is_pcie && 
           (device->pcie_device_port_type == PCIE_PORT_TYPE_ENDPOINT ||
            device->pcie_device_port_type == PCIE_PORT_TYPE_LEGACY_ENDPOINT);
}

bool pcie_is_root_port(const pci_device_t* device) {
    return device->is_pcie && device->pcie_device_port_type == PCIE_PORT_TYPE_ROOT_PORT;
}

bool pcie_is_bridge(const pci_device_t* device) {
    if (!device->is_pcie) {
        return (device->header_type & 0x7F) == 0x01;
    }
    
    return (device->pcie_device_port_type == PCIE_PORT_TYPE_UPSTREAM_PORT ||
            device->pcie_device_port_type == PCIE_PORT_TYPE_DOWNSTREAM_PORT ||
            device->pcie_device_port_type == PCIE_PORT_TYPE_PCI_TO_PCIE_BRIDGE ||
            device->pcie_device_port_type == PCIE_PORT_TYPE_PCIE_TO_PCI_BRIDGE);
}

bool pcie_has_capability(const pci_device_t* device, uint8 capability_id) {
    if (!device->is_pcie) {
        return false;
    }
    
    return pcie_find_capability_offset(device->segment, device->bus, device->device, 
                                       device->function, capability_id) != 0;
}

uint8 pcie_get_msix_capability(const pci_device_t* device) {
    if (!pcie_has_capability(device, PCIE_CAP_ID_MSIX)) {
        return 0;
    }
    
    uint8 offset = pcie_find_capability_offset(device->segment, device->bus, device->device, 
                                                device->function, PCIE_CAP_ID_MSIX);
    return pcie_config_read16(device->segment, device->bus, device->device, 
                              device->function, offset + 2);
}

uint8 pcie_get_msi_capability(const pci_device_t* device) {
    if (!pcie_has_capability(device, PCIE_CAP_ID_MSI)) {
        return 0;
    }
    
    uint8 offset = pcie_find_capability_offset(device->segment, device->bus, device->device,
                                                device->function, PCIE_CAP_ID_MSI);
    uint16 control = pcie_config_read16(device->segment, device->bus, device->function,
                                        0, offset + 2);
    return control & 0xFF;
}

/* -------------------------------------------------------------------------- *
 * PCIe ECAM direct access + capability enumeration.
 *
 * Configuration access is delegated to pci_config_read32/write32 which already
 * select ECAM when MCFG is present and fall back to Type1 CF8/CFC otherwise.
 * The wrappers below give a stable name for the documented `pcie_ecam_*`
 * API. The capability walker covers standard (0x34) and extended (0x100)
 * capability lists.
 * -------------------------------------------------------------------------- */

uint32 pcie_ecam_read(uint16 segment, uint8 bus, uint8 dev, uint8 fn, uint16 off) {
#if ENABLE_PCIE_ECAM
    return pcie_config_read32(segment, bus, dev, fn, off);
#else
    (void)segment; (void)bus; (void)dev; (void)fn; (void)off;
    return 0xFFFFFFFFu;
#endif
}

void pcie_ecam_write(uint16 segment, uint8 bus, uint8 dev, uint8 fn,
                     uint16 off, uint32 value) {
#if ENABLE_PCIE_ECAM
    pci_config_write32(segment, bus, dev, fn, off, value);
#else
    (void)segment; (void)bus; (void)dev; (void)fn; (void)off; (void)value;
#endif
}

int pcie_enumerate_capabilities(const pci_device_t* device, pcie_cap_list_t* out) {
    if (!device || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Standard capability list. Devices with header type 0 use the list at
     * offset 0x34; bridges use 0x14. */
    uint16 cap_off = (device->header_type & 0x7F) == 0x01
                     ? 0x14 : 0x34;
    uint8 next = (uint8)(pcie_ecam_read(device->segment, device->bus,
                                       device->device, device->function,
                                       cap_off) & 0xFC);
    while (next != 0 && out->count < PCIE_CAP_LIST_MAX) {
        uint32 w = pcie_ecam_read(device->segment, device->bus,
                                  device->device, device->function, next);
        out->entries[out->count].cap_id  = (uint8)(w & 0xFF);
        out->entries[out->count].offset   = next;
        out->entries[out->count].version  = (uint8)((w >> 16) & 0xFF);
        out->count++;
        uint16 nx = (uint16)((w >> 8) & 0xFF);
        next = (uint8)((nx & 0xFF) & 0xFC);
    }

    /* Extended (PCIe 2.0+) capability list starts at offset 0x100. */
    uint16 ext = 0x100;
    while (ext != 0 && ext < 0x1000 && out->ext_count < PCIE_CAP_LIST_MAX) {
        uint32 header = pcie_ecam_read(device->segment, device->bus,
                                        device->device, device->function, ext);
        if (header == 0xFFFFFFFFu) break;
        uint16 cap = (uint16)(header & 0xFFFF);
        uint16 next_off = (uint16)((header >> 20) & 0xFFF);
        out->ext_entries[out->ext_count].cap_id  = (uint8)(cap & 0xFF);
        out->ext_entries[out->ext_count].offset  = ext;
        out->ext_entries[out->ext_count].version = (uint8)((cap >> 8) & 0xF);
        out->ext_count++;
        if (next_off <= ext) break;   /* guard against bad tables */
        ext = next_off;
    }
    return (int)out->count;
}