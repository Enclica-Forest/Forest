#include "include/pcie.h"
#include "include/pci.h"
#include "include/debuglog.h"

struct pcie_test_counts {
    uint32 total;
    uint32 pcie;
    uint32 legacy;
};

bool pcie_test_callback(const pci_device_t* device, void* context) {
    struct pcie_test_counts* counts = (struct pcie_test_counts*)context;
    counts->total++;
    
    if (device->is_pcie) {
        counts->pcie++;
        debuglog_printf("PCIe Device found: %04x:%02x:%02x.%x\n",
                       device->segment, device->bus, device->device, device->function);
    } else {
        counts->legacy++;
    }
    
    return true;
}

bool pcie_test_enumeration(void) {
    debuglog_printf("=== PCIe Test: Device Enumeration ===\n");
    
    bool pci_ok = pci_init();
    if (!pci_ok) {
        debuglog_printf("FAIL: PCI initialization failed\n");
        return false;
    }
    
    struct pcie_test_counts counts = {0, 0, 0};
    pci_enumerate(pcie_test_callback, &counts);
    
    debuglog_printf("Total devices: %u\n", counts.total);
    debuglog_printf("PCIe devices: %u\n", counts.pcie);
    debuglog_printf("Legacy PCI devices: %u\n", counts.legacy);
    
    return true;
}

bool pcie_test_extended_config(void) {
    debuglog_printf("=== PCIe Test: Extended Configuration Space ===\n");
    
    pci_device_t test_device;
    if (!pci_find_by_class(0x01, 0x06, &test_device)) {
        debuglog_printf("INFO: No SATA controller found for extended config test\n");
        return true;
    }
    
    if (!test_device.is_pcie) {
        debuglog_printf("INFO: Device is not PCIe, skipping extended config test\n");
        return true;
    }
    
    uint8 cap_offset = pcie_find_capability_offset(test_device.segment, test_device.bus,
                                                   test_device.device, test_device.function,
                                                   0x10);
    if (cap_offset == 0) {
        debuglog_printf("FAIL: PCIe capability not found\n");
        return false;
    }
    
    debuglog_printf("PCIe capability found at offset 0x%02x\n", cap_offset);
    
    uint32 device_cap = pcie_get_device_capabilities(&test_device);
    uint16 device_status = pcie_get_device_status(&test_device);
    uint32 link_cap = pcie_get_link_capabilities(&test_device);
    uint16 link_status = pcie_get_link_status(&test_device);
    
    debuglog_printf("Device capabilities: 0x%08x\n", device_cap);
    debuglog_printf("Device status: 0x%04x\n", device_status);
    debuglog_printf("Link capabilities: 0x%08x\n", link_cap);
    debuglog_printf("Link status: 0x%04x\n", link_status);
    
    return true;
}

bool pcie_run_tests(void) {
    debuglog_printf("Starting PCIe test suite...\n");
    
    bool test1 = pcie_test_enumeration();
    bool test2 = pcie_test_extended_config();
    
    debuglog_printf("PCIe test suite results:\n");
    debuglog_printf("  Enumeration test: %s\n", test1 ? "PASS" : "FAIL");
    debuglog_printf("  Extended config test: %s\n", test2 ? "PASS" : "FAIL");
    
    return test1 && test2;
}