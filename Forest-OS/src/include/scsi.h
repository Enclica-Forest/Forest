#ifndef SCSI_H
#define SCSI_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define SCSI_MAX_DEVICES 16
#define SCSI_MAX_LUNS 8

#define SCSI_MAX_CMD_LEN 16
#define SCSI_MAX_SENSE_LEN 18

#define SCSI_TIMEOUT_MS 5000

typedef enum {
    SCSI_OP_TEST_UNIT_READY = 0x00,
    SCSI_OP_REQUEST_SENSE = 0x03,
    SCSI_OP_READ_6 = 0x08,
    SCSI_OP_WRITE_6 = 0x0A,
    SCSI_OP_INQUIRY = 0x12,
    SCSI_OP_READ_CAPACITY_10 = 0x25,
    SCSI_OP_READ_10 = 0x28,
    SCSI_OP_WRITE_10 = 0x2A,
    SCSI_OP_SYNCHRONIZE_CACHE = 0x35
} scsi_opcode_t;

typedef enum {
    SCSI_TYPE_DISK = 0x00,
    SCSI_TYPE_TAPE = 0x01,
    SCSI_TYPE_PRINTER = 0x02,
    SCSI_TYPE_PROCESSOR = 0x03,
    SCSI_TYPE_WORM = 0x04,
    SCSI_TYPE_CDROM = 0x05,
    SCSI_TYPE_SCANNER = 0x06,
    SCSI_TYPE_OPTICAL = 0x07,
    SCSI_TYPE_MEDIUM_CHANGER = 0x08,
    SCSI_TYPE_COMM = 0x09,
    SCSI_TYPE_UNKNOWN = 0x1F
} scsi_device_type_t;

typedef enum {
    SCSI_STATUS_GOOD = 0x00,
    SCSI_STATUS_CHECK_CONDITION = 0x02,
    SCSI_STATUS_CONDITION_MET = 0x04,
    SCSI_STATUS_BUSY = 0x08,
    SCSI_STATUS_INTERMEDIATE = 0x10,
    SCSI_STATUS_INTERMEDIATE_CONDITION_MET = 0x14,
    SCSI_STATUS_RESERVATION_CONFLICT = 0x18,
    SCSI_STATUS_COMMAND_TERMINATED = 0x22,
    SCSI_STATUS_QUEUE_FULL = 0x28,
    SCSI_STATUS_ACA_ACTIVE = 0x30,
    SCSI_STATUS_TASK_ABORTED = 0x40
} scsi_status_t;

typedef struct {
    uint8 vendor_id[8];
    uint8 product_id[16];
    uint8 revision[4];
    uint8 peripheral_type;
    uint8 removable;
    uint8 version;
    uint8 response_data_format;
    uint8 additional_length;
    uint8 flags[3];
    uint8 vendor_specific[20];
} __attribute__((packed)) scsi_inquiry_data_t;

typedef struct {
    uint8 sense_key;
    uint8 asc;
    uint8 ascq;
    uint8 fruc;
    uint8 sense_key_specific[3];
} __attribute__((packed)) scsi_sense_data_t;

typedef struct {
    uint32 last_lba;
    uint32 block_size;
} __attribute__((packed)) scsi_read_capacity_data_t;

typedef struct {
    uint8 opcode;
    uint8 lun_flags;
    uint8 lba_msb;
    uint8 lba_mid;
    uint8 lba_lsb;
    uint8 transfer_length;
    uint8 control;
} __attribute__((packed)) scsi_cdb6_t;

typedef struct {
    uint8 opcode;
    uint8 flags;
    uint32 lba;
    uint16 group_number;
    uint8 transfer_length_msb;
    uint8 transfer_length_lsb;
    uint8 control;
} __attribute__((packed)) scsi_cdb10_t;

typedef struct {
    uint8 opcode;
    uint8 allocation_length;
    uint8 control;
} __attribute__((packed)) scsi_cdb_inquiry_t;

typedef struct scsi_device scsi_device_t;

typedef bool (*scsi_execute_cmd_func)(scsi_device_t* dev, const void* cdb, uint8 cdb_len,
                                       void* data, uint32 data_len, uint32* transferred);
typedef bool (*scsi_reset_func)(scsi_device_t* dev);

struct scsi_device {
    uint8 target_id;
    uint8 lun;
    uint16 vendor_id;
    uint16 device_id;
    scsi_device_type_t device_type;
    scsi_inquiry_data_t inquiry_data;
    uint32 block_size;
    uint64 total_blocks;
    void* private_data;
    
    scsi_execute_cmd_func execute_cmd;
    scsi_reset_func reset;
    
    bool present;
    bool initialized;
};

bool scsi_init(void);
void scsi_shutdown(void);
scsi_device_t* scsi_allocate_device(uint8 target_id, uint8 lun);
void scsi_free_device(scsi_device_t* dev);

bool scsi_execute_cmd(scsi_device_t* dev, const void* cdb, uint8 cdb_len,
                      void* data, uint32 data_len, uint32* transferred);
bool scsi_test_unit_ready(scsi_device_t* dev);
bool scsi_request_sense(scsi_device_t* dev, scsi_sense_data_t* sense_data);
bool scsi_inquiry(scsi_device_t* dev, scsi_inquiry_data_t* inquiry);
bool scsi_read_capacity(scsi_device_t* dev, uint32* block_count, uint32* block_size);
bool scsi_read_blocks(scsi_device_t* dev, uint64 lba, uint32 blocks, void* buffer);
bool scsi_write_blocks(scsi_device_t* dev, uint64 lba, uint32 blocks, const void* buffer);
bool scsi_synchronize_cache(scsi_device_t* dev);

#endif
