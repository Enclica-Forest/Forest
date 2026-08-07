/**
 * USB Mass Storage Class Driver for Fern
 *
 * Implements USB Mass Storage Class support for flash drives, external HDDs, etc.
 * Based on USB Mass Storage Class Bulk-Only (BBB) Transport specification.
 */

#include "../include/usb/usb_mass_storage.h"
#include "../include/usb/usb.h"
#include "../include/devfs.h"
#include "../include/system.h"
#include "../include/memory.h"
#include "../include/debuglog.h"
#include <string.h>

// MSC driver state
static usb_msc_device_t* msc_devices = NULL;
static uint32 msc_device_count = 0;
static bool msc_initialized = false;

// Forward declarations
static usb_msc_device_t* usb_msc_alloc_device(void);
static void usb_msc_free_device(usb_msc_device_t* device);
static bool usb_msc_configure_device(usb_msc_device_t* device);
static int usb_msc_bulk_reset_recovery(usb_msc_device_t* device);

// Byte swap helpers for big-endian SCSI data
static inline uint32 bswap32(uint32 x) {
    return ((x >> 24) & 0xFF) |
           ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) |
           ((x << 24) & 0xFF000000);
}

static inline uint64 bswap64(uint64 x) {
    return ((x >> 56) & 0xFF) |
           ((x >> 40) & 0xFF00) |
           ((x >> 24) & 0xFF0000) |
           ((x >> 8) & 0xFF000000) |
           ((x << 8) & 0xFF00000000ULL) |
           ((x << 24) & 0xFF0000000000ULL) |
           ((x << 40) & 0xFF000000000000ULL) |
           ((x << 56) & 0xFF00000000000000ULL);
}

// USB MSC class driver
usb_class_driver_t usb_msc_driver = {
    .name = "usb_msc",
    .class_code = USB_MSC_CLASS,
    .subclass = USB_MSC_SUBCLASS_SCSI,
    .protocol = USB_MSC_PROTOCOL_BBB,
    .probe = usb_msc_probe,
    .disconnect = usb_msc_disconnect,
    .next = NULL
};

/**
 * Initialize USB Mass Storage subsystem
 */
bool usb_msc_init(void) {
    if (msc_initialized) {
        return true;
    }

    debug_print("USB MSC: Initializing\n");

    msc_devices = NULL;
    msc_device_count = 0;

    // Register with USB core
    if (!usb_register_class_driver(&usb_msc_driver)) {
        debug_print("USB MSC: Failed to register class driver\n");
        return false;
    }

    msc_initialized = true;
    debug_print("USB MSC: Initialized\n");

    return true;
}

/**
 * Shutdown USB Mass Storage subsystem
 */
void usb_msc_shutdown(void) {
    if (!msc_initialized) {
        return;
    }

    debug_print("USB MSC: Shutting down\n");

    // Free all MSC devices
    usb_msc_device_t* device = msc_devices;
    while (device) {
        usb_msc_device_t* next = device->next;
        usb_msc_free_device(device);
        device = next;
    }

    msc_devices = NULL;
    msc_device_count = 0;

    usb_unregister_class_driver(&usb_msc_driver);

    msc_initialized = false;
    debug_print("USB MSC: Shutdown complete\n");
}

/**
 * Allocate an MSC device structure
 */
static usb_msc_device_t* usb_msc_alloc_device(void) {
    usb_msc_device_t* device = (usb_msc_device_t*)kmalloc(sizeof(usb_msc_device_t));
    if (!device) {
        return NULL;
    }

    memset(device, 0, sizeof(usb_msc_device_t));
    return device;
}

/**
 * Free an MSC device structure
 */
static void usb_msc_free_device(usb_msc_device_t* device) {
    if (!device) return;
    kfree(device);
}

/**
 * Probe a USB device for Mass Storage support
 */
bool usb_msc_probe(usb_device_t* device, usb_interface_t* interface) {
    if (!device || !interface) {
        return false;
    }

    // Check if this is a Mass Storage interface
    if (interface->class_code != USB_MSC_CLASS) {
        return false;
    }

    // We only support BBB protocol for now
    if (interface->protocol != USB_MSC_PROTOCOL_BBB) {
        debug_print("USB MSC: Unsupported protocol 0x");
        debug_print_hex(interface->protocol);
        debug_print("\n");
        return false;
    }

    debug_print("USB MSC: Probing device VID=0x");
    debug_print_hex(device->vendor_id);
    debug_print(" PID=0x");
    debug_print_hex(device->product_id);
    debug_print("\n");

    // Allocate MSC device
    usb_msc_device_t* msc_device = usb_msc_alloc_device();
    if (!msc_device) {
        debug_print("USB MSC: Failed to allocate device\n");
        return false;
    }

    msc_device->usb_device = device;
    msc_device->interface = interface;
    msc_device->tag = 1;

    // Find bulk IN and OUT endpoints
    for (uint8 i = 0; i < interface->num_endpoints; i++) {
        usb_endpoint_t* ep = &interface->endpoints[i];
        if (ep->type == USB_TRANSFER_BULK) {
            if (ep->direction == USB_DIR_IN) {
                msc_device->bulk_in = ep;
            } else {
                msc_device->bulk_out = ep;
            }
        }
    }

    if (!msc_device->bulk_in || !msc_device->bulk_out) {
        debug_print("USB MSC: Missing bulk endpoints\n");
        usb_msc_free_device(msc_device);
        return false;
    }

    // Get max LUN
    if (!usb_msc_get_max_lun(msc_device, &msc_device->max_lun)) {
        // Some devices don't support Get Max LUN, assume LUN 0 only
        msc_device->max_lun = 0;
    }

    debug_print("USB MSC: Max LUN = ");
    debug_print_dec(msc_device->max_lun);
    debug_print("\n");

    // Configure the device (probe each LUN)
    if (!usb_msc_configure_device(msc_device)) {
        debug_print("USB MSC: Failed to configure device\n");
        usb_msc_free_device(msc_device);
        return false;
    }

    // Link to device list
    msc_device->next = msc_devices;
    msc_devices = msc_device;
    msc_device_count++;

    // Store driver data in interface
    interface->driver_data = msc_device;

    // Register with devfs
    char bus_id[16];
    // Simple bus ID: address.interface
    strcpy(bus_id, "");
    // TODO: Use proper USB topology ID
    bus_id[0] = '0' + device->address;
    bus_id[1] = '.';
    bus_id[2] = '0' + interface->number;
    bus_id[3] = '\0';

    static dev_ops_t msc_ops = {
        .read = NULL,  // TODO: Implement block device read
        .write = NULL,
        .open = NULL,
        .close = NULL,
        .ioctl = NULL,
        .poll = NULL
    };

    devfs_register_usb_device(bus_id, DEV_TYPE_BLOCK, &msc_ops, msc_device);

    debug_print("USB MSC: Device probed successfully\n");
    return true;
}

/**
 * Configure MSC device - probe each LUN
 */
static bool usb_msc_configure_device(usb_msc_device_t* device) {
    if (!device) return false;

    bool any_lun_present = false;

    for (uint8 lun = 0; lun <= device->max_lun; lun++) {
        debug_print("USB MSC: Probing LUN ");
        debug_print_dec(lun);
        debug_print("\n");

        // Test Unit Ready (may need multiple attempts)
        for (int retry = 0; retry < 3; retry++) {
            if (usb_msc_test_unit_ready(device, lun)) {
                break;
            }

            // Request sense to clear any pending errors
            scsi_request_sense_data_t sense;
            usb_msc_request_sense(device, lun, &sense);

            // Short delay
            for (volatile int i = 0; i < 100000; i++);
        }

        // Get inquiry data
        scsi_inquiry_data_t inquiry;
        if (!usb_msc_inquiry(device, lun, &inquiry)) {
            debug_print("USB MSC: LUN ");
            debug_print_dec(lun);
            debug_print(" - INQUIRY failed\n");
            continue;
        }

        // Copy device info
        device->lun[lun].device_type = inquiry.peripheral_type & 0x1F;
        device->lun[lun].removable = (inquiry.removable & 0x80) != 0;

        // Copy strings (remove trailing spaces)
        memcpy(device->lun[lun].vendor, inquiry.vendor, 8);
        device->lun[lun].vendor[8] = '\0';
        for (int i = 7; i >= 0 && device->lun[lun].vendor[i] == ' '; i--) {
            device->lun[lun].vendor[i] = '\0';
        }

        memcpy(device->lun[lun].product, inquiry.product, 16);
        device->lun[lun].product[16] = '\0';
        for (int i = 15; i >= 0 && device->lun[lun].product[i] == ' '; i--) {
            device->lun[lun].product[i] = '\0';
        }

        memcpy(device->lun[lun].revision, inquiry.revision, 4);
        device->lun[lun].revision[4] = '\0';
        for (int i = 3; i >= 0 && device->lun[lun].revision[i] == ' '; i--) {
            device->lun[lun].revision[i] = '\0';
        }

        debug_print("USB MSC: LUN ");
        debug_print_dec(lun);
        debug_print(" - Type=");
        debug_print(usb_msc_device_type_string(device->lun[lun].device_type));
        debug_print(" Vendor=");
        debug_print(device->lun[lun].vendor);
        debug_print(" Product=");
        debug_print(device->lun[lun].product);
        debug_print("\n");

        // Skip if not a direct access device
        if (device->lun[lun].device_type != SCSI_DEVICE_DIRECT_ACCESS &&
            device->lun[lun].device_type != SCSI_DEVICE_CDROM) {
            continue;
        }

        // Wait for device to become ready
        for (int retry = 0; retry < 10; retry++) {
            if (usb_msc_test_unit_ready(device, lun)) {
                device->lun[lun].present = true;
                break;
            }

            scsi_request_sense_data_t sense;
            usb_msc_request_sense(device, lun, &sense);

            // Short delay
            for (volatile int i = 0; i < 100000; i++);
        }

        if (!device->lun[lun].present) {
            debug_print("USB MSC: LUN ");
            debug_print_dec(lun);
            debug_print(" - Not ready\n");
            continue;
        }

        // Get capacity
        if (!usb_msc_read_capacity(device, lun,
                                   &device->lun[lun].num_blocks,
                                   &device->lun[lun].block_size)) {
            debug_print("USB MSC: LUN ");
            debug_print_dec(lun);
            debug_print(" - READ CAPACITY failed\n");
            device->lun[lun].present = false;
            continue;
        }

        device->lun[lun].capacity = device->lun[lun].num_blocks * device->lun[lun].block_size;

        debug_print("USB MSC: LUN ");
        debug_print_dec(lun);
        debug_print(" - ");
        debug_print_dec((uint32)(device->lun[lun].capacity / (1024 * 1024)));
        debug_print(" MB (");
        debug_print_dec((uint32)device->lun[lun].num_blocks);
        debug_print(" blocks, ");
        debug_print_dec(device->lun[lun].block_size);
        debug_print(" bytes/block)\n");

        any_lun_present = true;
    }

    return any_lun_present;
}

/**
 * Disconnect an MSC device
 */
void usb_msc_disconnect(usb_device_t* device, usb_interface_t* interface) {
    if (!device || !interface) return;

    usb_msc_device_t* msc_device = (usb_msc_device_t*)interface->driver_data;
    if (!msc_device) return;

    debug_print("USB MSC: Disconnecting device\n");

    // Remove from device list
    if (msc_devices == msc_device) {
        msc_devices = msc_device->next;
    } else {
        usb_msc_device_t* prev = msc_devices;
        while (prev && prev->next != msc_device) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = msc_device->next;
        }
    }

    // Unregister from devfs
    char bus_id[16];
    bus_id[0] = '0' + device->address;
    bus_id[1] = '.';
    bus_id[2] = '0' + interface->number;
    bus_id[3] = '\0';
    devfs_unregister_usb_device(bus_id);

    msc_device_count--;
    interface->driver_data = NULL;
    usb_msc_free_device(msc_device);

    debug_print("USB MSC: Device disconnected\n");
}

/**
 * Mass Storage Reset
 */
bool usb_msc_reset(usb_msc_device_t* device) {
    if (!device || !device->usb_device) return false;

    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_MSC_BULK_ONLY_RESET,
        0,
        device->interface->number,
        NULL, 0);

    return result >= 0;
}

/**
 * Get Maximum LUN
 */
bool usb_msc_get_max_lun(usb_msc_device_t* device, uint8* max_lun) {
    if (!device || !device->usb_device || !max_lun) return false;

    uint8 lun;
    int result = usb_control_msg(device->usb_device,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        USB_MSC_GET_MAX_LUN,
        0,
        device->interface->number,
        &lun, 1);

    if (result >= 0) {
        *max_lun = lun;
        return true;
    }

    return false;
}

/**
 * Execute a SCSI command via BBB protocol
 */
int usb_msc_scsi_command(usb_msc_device_t* device, uint8 lun,
                         void* cmd, uint8 cmd_len,
                         void* data, uint32 data_len,
                         bool data_in) {
    if (!device || !device->usb_device || !cmd || cmd_len == 0 || cmd_len > 16) {
        return -1;
    }

    usb_controller_t* controller = device->usb_device->controller;
    if (!controller || !controller->ops) return -1;

    // Build Command Block Wrapper
    usb_msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = device->tag++;
    cbw.data_transfer_length = data_len;
    cbw.flags = data_in ? USB_MSC_CBW_FLAG_DATA_IN : USB_MSC_CBW_FLAG_DATA_OUT;
    cbw.lun = lun & 0x0F;
    cbw.cb_length = cmd_len;
    memcpy(cbw.cb, cmd, cmd_len);

    // Send CBW
    int result = controller->ops->bulk_transfer(
        controller, device->usb_device, device->bulk_out,
        &cbw, sizeof(cbw));

    if (result != sizeof(cbw)) {
        debug_print("USB MSC: CBW transfer failed\n");
        usb_msc_bulk_reset_recovery(device);
        return -1;
    }

    // Data phase (if any)
    int data_transferred = 0;
    if (data_len > 0 && data) {
        usb_endpoint_t* ep = data_in ? device->bulk_in : device->bulk_out;
        result = controller->ops->bulk_transfer(
            controller, device->usb_device, ep,
            data, data_len);

        if (result < 0) {
            debug_print("USB MSC: Data transfer failed\n");
            // Continue to read CSW
        } else {
            data_transferred = result;
        }
    }

    // Read Command Status Wrapper
    usb_msc_csw_t csw;
    memset(&csw, 0, sizeof(csw));

    result = controller->ops->bulk_transfer(
        controller, device->usb_device, device->bulk_in,
        &csw, sizeof(csw));

    if (result != sizeof(csw)) {
        debug_print("USB MSC: CSW transfer failed\n");
        usb_msc_bulk_reset_recovery(device);
        return -1;
    }

    // Validate CSW
    if (csw.signature != USB_MSC_CSW_SIGNATURE) {
        debug_print("USB MSC: Invalid CSW signature\n");
        usb_msc_bulk_reset_recovery(device);
        return -1;
    }

    if (csw.tag != cbw.tag) {
        debug_print("USB MSC: CSW tag mismatch\n");
        usb_msc_bulk_reset_recovery(device);
        return -1;
    }

    // Check status
    switch (csw.status) {
        case USB_MSC_CSW_STATUS_PASSED:
            return data_len - csw.data_residue;

        case USB_MSC_CSW_STATUS_FAILED:
            // Command failed - caller should request sense
            return -2;

        case USB_MSC_CSW_STATUS_PHASE:
            debug_print("USB MSC: Phase error\n");
            usb_msc_bulk_reset_recovery(device);
            return -1;

        default:
            debug_print("USB MSC: Unknown CSW status\n");
            return -1;
    }
}

/**
 * Bulk-Only Mass Storage Reset Recovery
 */
static int usb_msc_bulk_reset_recovery(usb_msc_device_t* device) {
    if (!device) return -1;

    // 1. Mass Storage Reset
    usb_msc_reset(device);

    // 2. Clear HALT on bulk-in endpoint
    usb_control_msg(device->usb_device,
        USB_REQTYPE_RECIP_ENDPOINT,
        USB_REQ_CLEAR_FEATURE,
        0,  // ENDPOINT_HALT
        device->bulk_in->address,
        NULL, 0);

    // 3. Clear HALT on bulk-out endpoint
    usb_control_msg(device->usb_device,
        USB_REQTYPE_RECIP_ENDPOINT,
        USB_REQ_CLEAR_FEATURE,
        0,  // ENDPOINT_HALT
        device->bulk_out->address,
        NULL, 0);

    return 0;
}

/**
 * Test Unit Ready
 */
bool usb_msc_test_unit_ready(usb_msc_device_t* device, uint8 lun) {
    uint8 cmd[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};

    int result = usb_msc_scsi_command(device, lun, cmd, 6, NULL, 0, false);
    return result >= 0;
}

/**
 * SCSI Inquiry
 */
bool usb_msc_inquiry(usb_msc_device_t* device, uint8 lun, scsi_inquiry_data_t* data) {
    if (!data) return false;

    uint8 cmd[6] = {SCSI_CMD_INQUIRY, 0, 0, 0, 36, 0};

    int result = usb_msc_scsi_command(device, lun, cmd, 6, data, 36, true);
    return result >= 0;
}

/**
 * Read Capacity
 */
bool usb_msc_read_capacity(usb_msc_device_t* device, uint8 lun,
                           uint64* num_blocks, uint32* block_size) {
    if (!num_blocks || !block_size) return false;

    // Try READ CAPACITY (10) first
    scsi_read_capacity_10_t cap10;
    uint8 cmd10[10] = {SCSI_CMD_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    int result = usb_msc_scsi_command(device, lun, cmd10, 10, &cap10, sizeof(cap10), true);
    if (result >= 0) {
        uint32 last_lba = bswap32(cap10.last_lba);
        *block_size = bswap32(cap10.block_length);

        // Check if we need READ CAPACITY (16) for large drives
        if (last_lba == 0xFFFFFFFF) {
            // Try READ CAPACITY (16)
            scsi_read_capacity_16_t cap16;
            uint8 cmd16[16] = {SCSI_CMD_READ_CAPACITY_16, 0x10, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, sizeof(cap16), 0, 0};

            result = usb_msc_scsi_command(device, lun, cmd16, 16, &cap16, sizeof(cap16), true);
            if (result >= 0) {
                *num_blocks = bswap64(cap16.last_lba) + 1;
                *block_size = bswap32(cap16.block_length);
                return true;
            }
        }

        *num_blocks = (uint64)last_lba + 1;
        return true;
    }

    return false;
}

/**
 * Request Sense
 */
bool usb_msc_request_sense(usb_msc_device_t* device, uint8 lun,
                           scsi_request_sense_data_t* sense) {
    if (!sense) return false;

    uint8 cmd[6] = {SCSI_CMD_REQUEST_SENSE, 0, 0, 0, 18, 0};

    int result = usb_msc_scsi_command(device, lun, cmd, 6, sense, 18, true);
    return result >= 0;
}

/**
 * Read blocks from device
 */
int usb_msc_read_blocks(usb_msc_device_t* device, uint8 lun,
                        uint64 lba, uint32 count, void* buffer) {
    if (!device || !buffer || count == 0) return -1;

    if (lun > device->max_lun || !device->lun[lun].present) {
        return -1;
    }

    uint32 block_size = device->lun[lun].block_size;
    uint32 transfer_size = count * block_size;

    // Use READ (10) for LBA < 2^32 and count < 65536
    if (lba < 0x100000000ULL && count <= 65535) {
        uint8 cmd[10];
        cmd[0] = SCSI_CMD_READ_10;
        cmd[1] = 0;
        cmd[2] = (lba >> 24) & 0xFF;
        cmd[3] = (lba >> 16) & 0xFF;
        cmd[4] = (lba >> 8) & 0xFF;
        cmd[5] = lba & 0xFF;
        cmd[6] = 0;
        cmd[7] = (count >> 8) & 0xFF;
        cmd[8] = count & 0xFF;
        cmd[9] = 0;

        int result = usb_msc_scsi_command(device, lun, cmd, 10, buffer, transfer_size, true);
        if (result < 0) {
            if (result == -2) {
                // Check sense data
                scsi_request_sense_data_t sense;
                usb_msc_request_sense(device, lun, &sense);
            }
            return result;
        }
        return result / block_size;
    }

    // Use READ (16) for large LBA
    uint8 cmd[16];
    cmd[0] = SCSI_CMD_READ_16;
    cmd[1] = 0;
    cmd[2] = (lba >> 56) & 0xFF;
    cmd[3] = (lba >> 48) & 0xFF;
    cmd[4] = (lba >> 40) & 0xFF;
    cmd[5] = (lba >> 32) & 0xFF;
    cmd[6] = (lba >> 24) & 0xFF;
    cmd[7] = (lba >> 16) & 0xFF;
    cmd[8] = (lba >> 8) & 0xFF;
    cmd[9] = lba & 0xFF;
    cmd[10] = (count >> 24) & 0xFF;
    cmd[11] = (count >> 16) & 0xFF;
    cmd[12] = (count >> 8) & 0xFF;
    cmd[13] = count & 0xFF;
    cmd[14] = 0;
    cmd[15] = 0;

    int result = usb_msc_scsi_command(device, lun, cmd, 16, buffer, transfer_size, true);
    if (result < 0) return result;
    return result / block_size;
}

/**
 * Write blocks to device
 */
int usb_msc_write_blocks(usb_msc_device_t* device, uint8 lun,
                         uint64 lba, uint32 count, const void* buffer) {
    if (!device || !buffer || count == 0) return -1;

    if (lun > device->max_lun || !device->lun[lun].present) {
        return -1;
    }

    uint32 block_size = device->lun[lun].block_size;
    uint32 transfer_size = count * block_size;

    // Use WRITE (10) for LBA < 2^32 and count < 65536
    if (lba < 0x100000000ULL && count <= 65535) {
        uint8 cmd[10];
        cmd[0] = SCSI_CMD_WRITE_10;
        cmd[1] = 0;
        cmd[2] = (lba >> 24) & 0xFF;
        cmd[3] = (lba >> 16) & 0xFF;
        cmd[4] = (lba >> 8) & 0xFF;
        cmd[5] = lba & 0xFF;
        cmd[6] = 0;
        cmd[7] = (count >> 8) & 0xFF;
        cmd[8] = count & 0xFF;
        cmd[9] = 0;

        int result = usb_msc_scsi_command(device, lun, cmd, 10, (void*)buffer, transfer_size, false);
        if (result < 0) {
            if (result == -2) {
                scsi_request_sense_data_t sense;
                usb_msc_request_sense(device, lun, &sense);
            }
            return result;
        }
        return result / block_size;
    }

    // Use WRITE (16) for large LBA
    uint8 cmd[16];
    cmd[0] = SCSI_CMD_WRITE_16;
    cmd[1] = 0;
    cmd[2] = (lba >> 56) & 0xFF;
    cmd[3] = (lba >> 48) & 0xFF;
    cmd[4] = (lba >> 40) & 0xFF;
    cmd[5] = (lba >> 32) & 0xFF;
    cmd[6] = (lba >> 24) & 0xFF;
    cmd[7] = (lba >> 16) & 0xFF;
    cmd[8] = (lba >> 8) & 0xFF;
    cmd[9] = lba & 0xFF;
    cmd[10] = (count >> 24) & 0xFF;
    cmd[11] = (count >> 16) & 0xFF;
    cmd[12] = (count >> 8) & 0xFF;
    cmd[13] = count & 0xFF;
    cmd[14] = 0;
    cmd[15] = 0;

    int result = usb_msc_scsi_command(device, lun, cmd, 16, (void*)buffer, transfer_size, false);
    if (result < 0) return result;
    return result / block_size;
}

/**
 * Synchronize cache (flush writes)
 */
bool usb_msc_sync_cache(usb_msc_device_t* device, uint8 lun) {
    if (!device) return false;

    if (lun > device->max_lun || !device->lun[lun].present) {
        return false;
    }

    uint8 cmd[10] = {SCSI_CMD_SYNCHRONIZE_CACHE_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    int result = usb_msc_scsi_command(device, lun, cmd, 10, NULL, 0, false);
    return result >= 0;
}

/**
 * Get first MSC device
 */
usb_msc_device_t* usb_msc_get_first_device(void) {
    return msc_devices;
}

/**
 * Get next MSC device
 */
usb_msc_device_t* usb_msc_get_next_device(usb_msc_device_t* device) {
    return device ? device->next : NULL;
}

/**
 * Get total device count
 */
uint32 usb_msc_get_device_count(void) {
    return msc_device_count;
}

/**
 * Get device type string
 */
const char* usb_msc_device_type_string(uint8 type) {
    switch (type) {
        case SCSI_DEVICE_DIRECT_ACCESS:  return "Disk";
        case SCSI_DEVICE_SEQUENTIAL:     return "Tape";
        case SCSI_DEVICE_PRINTER:        return "Printer";
        case SCSI_DEVICE_PROCESSOR:      return "Processor";
        case SCSI_DEVICE_WRITE_ONCE:     return "Write-Once";
        case SCSI_DEVICE_CDROM:          return "CD-ROM";
        case SCSI_DEVICE_SCANNER:        return "Scanner";
        case SCSI_DEVICE_OPTICAL:        return "Optical";
        case SCSI_DEVICE_MEDIUM_CHANGER: return "Changer";
        case SCSI_DEVICE_COMMUNICATION:  return "Communication";
        case SCSI_DEVICE_ENCLOSURE:      return "Enclosure";
        case SCSI_DEVICE_SIMPLIFIED_DA:  return "Simplified-DA";
        case SCSI_DEVICE_OPTICAL_CARD:   return "Optical-Card";
        default:                         return "Unknown";
    }
}
