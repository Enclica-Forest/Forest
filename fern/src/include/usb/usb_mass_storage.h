/**
 * USB Mass Storage Class Driver for Fern
 *
 * Implements USB Mass Storage Class support for flash drives, external HDDs, etc.
 * Based on USB Mass Storage Class Bulk-Only (BBB) Transport specification.
 */

#ifndef USB_MASS_STORAGE_H
#define USB_MASS_STORAGE_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// Mass Storage Class Codes
#define USB_MSC_CLASS               0x08

// Mass Storage Subclass Codes
#define USB_MSC_SUBCLASS_RBC        0x01    // Reduced Block Commands
#define USB_MSC_SUBCLASS_MMC5       0x02    // ATAPI (CD/DVD)
#define USB_MSC_SUBCLASS_UFI        0x04    // USB Floppy Interface
#define USB_MSC_SUBCLASS_SCSI       0x06    // SCSI Transparent Command Set
#define USB_MSC_SUBCLASS_LSDFS      0x07    // Lockable Storage Devices
#define USB_MSC_SUBCLASS_IEEE1667   0x08    // IEEE 1667

// Mass Storage Protocol Codes
#define USB_MSC_PROTOCOL_CBI_INT    0x00    // Control/Bulk/Interrupt w/ Command Completion
#define USB_MSC_PROTOCOL_CBI_NOINT  0x01    // Control/Bulk/Interrupt w/o Command Completion
#define USB_MSC_PROTOCOL_BBB        0x50    // Bulk-Only (BBB)
#define USB_MSC_PROTOCOL_UAS        0x62    // USB Attached SCSI

// Mass Storage Class Requests
#define USB_MSC_GET_MAX_LUN         0xFE    // Get Max LUN
#define USB_MSC_BULK_ONLY_RESET     0xFF    // Bulk-Only Mass Storage Reset

// Command Block Wrapper (CBW) Signature
#define USB_MSC_CBW_SIGNATURE       0x43425355  // "USBC" little-endian

// Command Status Wrapper (CSW) Signature
#define USB_MSC_CSW_SIGNATURE       0x53425355  // "USBS" little-endian

// CBW Flags
#define USB_MSC_CBW_FLAG_DATA_IN    0x80    // Data from device to host
#define USB_MSC_CBW_FLAG_DATA_OUT   0x00    // Data from host to device

// CSW Status Values
#define USB_MSC_CSW_STATUS_PASSED   0x00    // Command passed
#define USB_MSC_CSW_STATUS_FAILED   0x01    // Command failed
#define USB_MSC_CSW_STATUS_PHASE    0x02    // Phase error

// Command Block Wrapper (31 bytes)
typedef struct __attribute__((packed)) {
    uint32 signature;           // CBW Signature (0x43425355)
    uint32 tag;                 // Transaction tag
    uint32 data_transfer_length;// Data transfer length
    uint8  flags;               // Flags (direction)
    uint8  lun;                 // Logical Unit Number (bits 0-3)
    uint8  cb_length;           // Command Block length (1-16)
    uint8  cb[16];              // Command Block (SCSI command)
} usb_msc_cbw_t;

// Command Status Wrapper (13 bytes)
typedef struct __attribute__((packed)) {
    uint32 signature;           // CSW Signature (0x53425355)
    uint32 tag;                 // Transaction tag (matches CBW)
    uint32 data_residue;        // Difference between expected and actual data
    uint8  status;              // Status (passed/failed/phase error)
} usb_msc_csw_t;

// SCSI Commands (commonly used ones)
#define SCSI_CMD_TEST_UNIT_READY    0x00
#define SCSI_CMD_REQUEST_SENSE      0x03
#define SCSI_CMD_FORMAT_UNIT        0x04
#define SCSI_CMD_READ_6             0x08
#define SCSI_CMD_WRITE_6            0x0A
#define SCSI_CMD_INQUIRY            0x12
#define SCSI_CMD_MODE_SELECT_6      0x15
#define SCSI_CMD_MODE_SENSE_6       0x1A
#define SCSI_CMD_START_STOP_UNIT    0x1B
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_CMD_READ_CAPACITY_10   0x25
#define SCSI_CMD_READ_10            0x28
#define SCSI_CMD_WRITE_10           0x2A
#define SCSI_CMD_VERIFY_10          0x2F
#define SCSI_CMD_SYNCHRONIZE_CACHE_10 0x35
#define SCSI_CMD_READ_12            0xA8
#define SCSI_CMD_WRITE_12           0xAA
#define SCSI_CMD_READ_16            0x88
#define SCSI_CMD_WRITE_16           0x8A
#define SCSI_CMD_READ_CAPACITY_16   0x9E

// SCSI Inquiry Data (36 bytes minimum)
typedef struct __attribute__((packed)) {
    uint8  peripheral_type;     // Device type (bits 0-4), qualifier (bits 5-7)
    uint8  removable;           // Removable media (bit 7)
    uint8  version;             // SCSI version
    uint8  response_format;     // Response data format
    uint8  additional_length;   // Additional data length
    uint8  flags[3];            // Various flags
    char   vendor[8];           // Vendor identification
    char   product[16];         // Product identification
    char   revision[4];         // Product revision level
} scsi_inquiry_data_t;

// SCSI Peripheral Device Types
#define SCSI_DEVICE_DIRECT_ACCESS   0x00    // Disk
#define SCSI_DEVICE_SEQUENTIAL      0x01    // Tape
#define SCSI_DEVICE_PRINTER         0x02
#define SCSI_DEVICE_PROCESSOR       0x03
#define SCSI_DEVICE_WRITE_ONCE      0x04
#define SCSI_DEVICE_CDROM           0x05
#define SCSI_DEVICE_SCANNER         0x06
#define SCSI_DEVICE_OPTICAL         0x07
#define SCSI_DEVICE_MEDIUM_CHANGER  0x08
#define SCSI_DEVICE_COMMUNICATION   0x09
#define SCSI_DEVICE_ENCLOSURE       0x0D
#define SCSI_DEVICE_SIMPLIFIED_DA   0x0E
#define SCSI_DEVICE_OPTICAL_CARD    0x0F
#define SCSI_DEVICE_UNKNOWN         0x1F

// SCSI Read Capacity (10) Response
typedef struct __attribute__((packed)) {
    uint32 last_lba;            // Last logical block address (big-endian)
    uint32 block_length;        // Block length in bytes (big-endian)
} scsi_read_capacity_10_t;

// SCSI Read Capacity (16) Response
typedef struct __attribute__((packed)) {
    uint64 last_lba;            // Last logical block address (big-endian)
    uint32 block_length;        // Block length in bytes (big-endian)
    uint8  flags;               // Protection and logical blocks per physical
    uint8  alignment[3];        // Alignment
    uint8  reserved[16];
} scsi_read_capacity_16_t;

// SCSI Request Sense Data
typedef struct __attribute__((packed)) {
    uint8  response_code;       // Response code
    uint8  obsolete;
    uint8  sense_key;           // Sense key (bits 0-3)
    uint32 information;         // Information (big-endian)
    uint8  additional_length;   // Additional sense length
    uint32 cmd_specific;        // Command specific information (big-endian)
    uint8  asc;                 // Additional sense code
    uint8  ascq;                // Additional sense code qualifier
    uint8  fruc;                // Field replaceable unit code
    uint8  sense_key_specific[3];
} scsi_request_sense_data_t;

// SCSI Sense Keys
#define SCSI_SENSE_NO_SENSE         0x00
#define SCSI_SENSE_RECOVERED_ERROR  0x01
#define SCSI_SENSE_NOT_READY        0x02
#define SCSI_SENSE_MEDIUM_ERROR     0x03
#define SCSI_SENSE_HARDWARE_ERROR   0x04
#define SCSI_SENSE_ILLEGAL_REQUEST  0x05
#define SCSI_SENSE_UNIT_ATTENTION   0x06
#define SCSI_SENSE_DATA_PROTECT     0x07
#define SCSI_SENSE_BLANK_CHECK      0x08
#define SCSI_SENSE_VENDOR_SPECIFIC  0x09
#define SCSI_SENSE_COPY_ABORTED     0x0A
#define SCSI_SENSE_ABORTED_COMMAND  0x0B
#define SCSI_SENSE_VOLUME_OVERFLOW  0x0D
#define SCSI_SENSE_MISCOMPARE       0x0E

// USB Mass Storage Device
typedef struct usb_msc_device {
    usb_device_t* usb_device;
    usb_interface_t* interface;
    usb_endpoint_t* bulk_in;
    usb_endpoint_t* bulk_out;
    uint8  max_lun;             // Maximum LUN
    uint32 tag;                 // Current command tag
    // Per-LUN information
    struct {
        bool present;
        bool removable;
        uint8 device_type;
        uint64 num_blocks;
        uint32 block_size;
        uint64 capacity;        // Total capacity in bytes
        char vendor[9];
        char product[17];
        char revision[5];
    } lun[16];
    struct usb_msc_device* next;
} usb_msc_device_t;

// USB Mass Storage Functions
bool usb_msc_init(void);
void usb_msc_shutdown(void);

// Device management
bool usb_msc_probe(usb_device_t* device, usb_interface_t* interface);
void usb_msc_disconnect(usb_device_t* device, usb_interface_t* interface);

// Mass Storage Class requests
bool usb_msc_reset(usb_msc_device_t* device);
bool usb_msc_get_max_lun(usb_msc_device_t* device, uint8* max_lun);

// SCSI Commands via BBB
int usb_msc_scsi_command(usb_msc_device_t* device, uint8 lun,
                         void* cmd, uint8 cmd_len,
                         void* data, uint32 data_len,
                         bool data_in);

// High-level SCSI operations
bool usb_msc_test_unit_ready(usb_msc_device_t* device, uint8 lun);
bool usb_msc_inquiry(usb_msc_device_t* device, uint8 lun, scsi_inquiry_data_t* data);
bool usb_msc_read_capacity(usb_msc_device_t* device, uint8 lun,
                           uint64* num_blocks, uint32* block_size);
bool usb_msc_request_sense(usb_msc_device_t* device, uint8 lun,
                           scsi_request_sense_data_t* sense);

// Block I/O operations
int usb_msc_read_blocks(usb_msc_device_t* device, uint8 lun,
                        uint64 lba, uint32 count, void* buffer);
int usb_msc_write_blocks(usb_msc_device_t* device, uint8 lun,
                         uint64 lba, uint32 count, const void* buffer);
bool usb_msc_sync_cache(usb_msc_device_t* device, uint8 lun);

// Device enumeration
usb_msc_device_t* usb_msc_get_first_device(void);
usb_msc_device_t* usb_msc_get_next_device(usb_msc_device_t* device);
uint32 usb_msc_get_device_count(void);

// Utility functions
const char* usb_msc_device_type_string(uint8 type);

// USB MSC class driver
extern usb_class_driver_t usb_msc_driver;

#endif // USB_MASS_STORAGE_H
