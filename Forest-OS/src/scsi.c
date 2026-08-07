#include "include/scsi.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"

#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_STORAGE_SCSI 0x00

static scsi_device_t* g_scsi_devices[SCSI_MAX_DEVICES];
static uint32 g_scsi_count = 0;

static bool scsi_stub_execute_cmd(scsi_device_t* dev, const void* cdb, uint8 cdb_len,
                                     void* data, uint32 data_len, uint32* transferred) {
    (void)dev;
    (void)cdb;
    (void)cdb_len;
    (void)data;
    (void)data_len;
    (void)transferred;
    return false;
}

static bool scsi_stub_reset(scsi_device_t* dev) {
    (void)dev;
    return false;
}

scsi_device_t* scsi_allocate_device(uint8 target_id, uint8 lun) {
    if (target_id >= SCSI_MAX_DEVICES || lun >= SCSI_MAX_LUNS) {
        return 0;
    }

    for (uint32 i = 0; i < SCSI_MAX_DEVICES; i++) {
        if (g_scsi_devices[i] && 
            g_scsi_devices[i]->target_id == target_id && 
            g_scsi_devices[i]->lun == lun) {
            return 0;
        }
    }

    scsi_device_t* dev = (scsi_device_t*)kmalloc(sizeof(scsi_device_t));
    if (!dev) {
        return 0;
    }

    memory_set((uint8*)dev, 0, sizeof(scsi_device_t));

    dev->target_id = target_id;
    dev->lun = lun;
    dev->execute_cmd = scsi_stub_execute_cmd;
    dev->reset = scsi_stub_reset;
    dev->present = false;
    dev->initialized = false;

    return dev;
}

void scsi_free_device(scsi_device_t* dev) {
    if (!dev) {
        return;
    }

    for (uint32 i = 0; i < SCSI_MAX_DEVICES; i++) {
        if (g_scsi_devices[i] == dev) {
            g_scsi_devices[i] = 0;
            break;
        }
    }

    kfree(dev);
}

bool scsi_execute_cmd(scsi_device_t* dev, const void* cdb, uint8 cdb_len,
                      void* data, uint32 data_len, uint32* transferred) {
    if (!dev || !dev->execute_cmd) {
        return false;
    }

    return dev->execute_cmd(dev, cdb, cdb_len, data, data_len, transferred);
}

bool scsi_test_unit_ready(scsi_device_t* dev) {
    if (!dev) {
        return false;
    }

    scsi_cdb6_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_TEST_UNIT_READY;
    cdb.lun_flags = dev->lun << 5;

    uint32 transferred = 0;
    uint8 status = 0;

    if (!scsi_execute_cmd(dev, &cdb, sizeof(cdb), &status, 1, &transferred)) {
        return false;
    }

    return status == SCSI_STATUS_GOOD;
}

bool scsi_request_sense(scsi_device_t* dev, scsi_sense_data_t* sense_data) {
    if (!dev || !sense_data) {
        return false;
    }

    scsi_cdb6_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_REQUEST_SENSE;
    cdb.transfer_length = SCSI_MAX_SENSE_LEN;

    uint32 transferred = 0;
    if (!scsi_execute_cmd(dev, &cdb, sizeof(cdb), sense_data, SCSI_MAX_SENSE_LEN, &transferred)) {
        return false;
    }

    return transferred > 0;
}

bool scsi_inquiry(scsi_device_t* dev, scsi_inquiry_data_t* inquiry) {
    if (!dev || !inquiry) {
        return false;
    }

    scsi_cdb_inquiry_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_INQUIRY;
    cdb.allocation_length = sizeof(scsi_inquiry_data_t);

    uint32 transferred = 0;
    if (!scsi_execute_cmd(dev, &cdb, sizeof(cdb), inquiry, sizeof(scsi_inquiry_data_t), &transferred)) {
        return false;
    }

    if (transferred > 0) {
        dev->device_type = inquiry->peripheral_type;
        memory_copy(inquiry->vendor_id, dev->inquiry_data.vendor_id, 8);
        memory_copy(inquiry->product_id, dev->inquiry_data.product_id, 16);
    }

    return transferred > 0;
}

bool scsi_read_capacity(scsi_device_t* dev, uint32* block_count, uint32* block_size) {
    if (!dev || !block_count || !block_size) {
        return false;
    }

    scsi_cdb10_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_READ_CAPACITY_10;
    cdb.flags = dev->lun << 5;

    scsi_read_capacity_data_t capacity;
    uint32 transferred = 0;
    if (!scsi_execute_cmd(dev, &cdb, sizeof(cdb), &capacity, sizeof(capacity), &transferred)) {
        return false;
    }

    if (transferred == sizeof(capacity)) {
        *block_count = capacity.last_lba;
        *block_size = capacity.block_size;
        dev->block_size = capacity.block_size;
        dev->total_blocks = (uint64)capacity.last_lba + 1;
        return true;
    }

    return false;
}

bool scsi_read_blocks(scsi_device_t* dev, uint64 lba, uint32 blocks, void* buffer) {
    if (!dev || !buffer || blocks == 0) {
        return false;
    }

    scsi_cdb10_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_READ_10;
    cdb.flags = dev->lun << 5;
    cdb.lba = (uint32)lba;
    cdb.transfer_length_msb = (blocks >> 8) & 0xFF;
    cdb.transfer_length_lsb = blocks & 0xFF;

    uint32 transferred = 0;
    if (!scsi_execute_cmd(dev, &cdb, sizeof(cdb), buffer, blocks * dev->block_size, &transferred)) {
        return false;
    }

    return transferred > 0;
}

bool scsi_write_blocks(scsi_device_t* dev, uint64 lba, uint32 blocks, const void* buffer) {
    if (!dev || !buffer || blocks == 0) {
        return false;
    }

    scsi_cdb10_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_WRITE_10;
    cdb.flags = dev->lun << 5;
    cdb.lba = (uint32)lba;
    cdb.transfer_length_msb = (blocks >> 8) & 0xFF;
    cdb.transfer_length_lsb = blocks & 0xFF;

    uint32 transferred = 0;
    if (!scsi_execute_cmd(dev, &cdb, sizeof(cdb), (void*)buffer, blocks * dev->block_size, &transferred)) {
        return false;
    }

    return transferred > 0;
}

bool scsi_synchronize_cache(scsi_device_t* dev) {
    if (!dev) {
        return false;
    }

    scsi_cdb10_t cdb;
    memory_set((uint8*)&cdb, 0, sizeof(cdb));
    cdb.opcode = SCSI_OP_SYNCHRONIZE_CACHE;
    cdb.flags = dev->lun << 5;

    uint32 transferred = 0;
    return scsi_execute_cmd(dev, &cdb, sizeof(cdb), 0, 0, &transferred);
}

static bool scsi_probe_target(uint8 target_id) {
    bool found = false;

    for (uint8 lun = 0; lun < SCSI_MAX_LUNS; lun++) {
        scsi_device_t* dev = scsi_allocate_device(target_id, lun);
        if (!dev) {
            continue;
        }

        if (scsi_test_unit_ready(dev)) {
            dev->present = true;

            if (scsi_inquiry(dev, &dev->inquiry_data)) {
                found = true;

                print("[SCSI] Target ");
                print_dec(target_id);
                print(" LUN ");
                print_dec(lun);
                print(": ");

                char vendor_str[9];
                memory_copy(dev->inquiry_data.vendor_id, (uint8*)vendor_str, 8);
                vendor_str[8] = 0;
                print(vendor_str);

                print(" ");

                char product_str[17];
                memory_copy(dev->inquiry_data.product_id, (uint8*)product_str, 16);
                product_str[16] = 0;
                print(product_str);

                uint32 block_count, block_size;
                if (scsi_read_capacity(dev, &block_count, &block_size)) {
                    print(" - ");
                    print_dec(block_count);
                    print(" blocks of ");
                    print_dec(block_size);
                    print(" bytes\n");

                    dev->initialized = true;
                }
            } else {
                print("[SCSI] Target ");
                print_dec(target_id);
                print(" LUN ");
                print_dec(lun);
                print(": Inquiry failed\n");
            }
        } else {
            scsi_free_device(dev);
        }
    }

    return found;
}

static bool scsi_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    if (device->class_code != PCI_CLASS_STORAGE) {
        return false;
    }

    if (device->subclass != PCI_SUBCLASS_STORAGE_SCSI) {
        return false;
    }

    print("[SCSI] Found SCSI controller: VID=");
    print_hex(device->vendor_id);
    print(" PID=");
    print_hex(device->device_id);
    print("\n");

    return false;
}

static int scsi_drv_probe(drv_device_t* dev, const drv_id_t* id) {
    (void)dev; (void)id;
    return 0;
}

static void scsi_drv_remove(drv_device_t* dev) {
    (void)dev;
}

static const drv_id_t g_scsi_drv_ids[] = {
    { DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_BUS_PCI,
      PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_SCSI, 0xFF, DRV_MATCH_CLASS, 0 },
    DRV_ID_TABLE_END
};

static drv_driver_t g_scsi_drv = {
    .name = "scsi",
    .version = "1.0",
    .bus = DRV_BUS_PCI,
    .id_table = g_scsi_drv_ids,
    .probe = scsi_drv_probe,
    .remove = scsi_drv_remove,
    .flags = DRV_FLAG_PM,
};

bool scsi_init(void) {
    print("[SCSI] Initializing SCSI driver...\n");

    memory_set((uint8*)g_scsi_devices, 0, sizeof(g_scsi_devices));
    g_scsi_count = 0;

    pci_enumerate(scsi_pci_callback, 0);

    for (uint8 target_id = 0; target_id < SCSI_MAX_DEVICES; target_id++) {
        scsi_probe_target(target_id);
    }

    print("[SCSI] SCSI driver initialized\n");

    drv_register(&g_scsi_drv);

    return true;
}

void scsi_shutdown(void) {
    print("[SCSI] Shutting down SCSI driver...\n");

    for (uint32 i = 0; i < SCSI_MAX_DEVICES; i++) {
        if (g_scsi_devices[i]) {
            scsi_free_device(g_scsi_devices[i]);
        }
    }

    g_scsi_count = 0;
}
