#ifndef ATA_H
#define ATA_H

#include <stdbool.h>
#include <stdint.h>
#include "types.h"
#include "spinlock.h"

#define ATA_MAX_CHANNELS 2
#define ATA_MAX_DEVICES_PER_CHANNEL 2
#define ATA_SECTOR_SIZE 512

typedef enum {
    ATA_DEV_TYPE_NONE = 0,
    ATA_DEV_TYPE_ATA,
    ATA_DEV_TYPE_ATAPI
} ata_device_type_t;

typedef enum {
    ATA_MODE_PIO = 0,
    ATA_MODE_DMA = 1
} ata_transfer_mode_t;

typedef enum {
    ATA_CHANNEL_PRIMARY = 0,
    ATA_CHANNEL_SECONDARY = 1
} ata_channel_t;

typedef enum {
    ATA_DEV_MASTER = 0,
    ATA_DEV_SLAVE = 1
} ata_device_select_t;

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint16_t bm_base;
    uint8_t irq;
    bool present;
    bool atapi_mode;
} ata_channel_info_t;

typedef struct {
    ata_device_type_t type;
    bool exists;
    bool supports_lba48;
    uint64_t sectors;
    uint16_t sector_size;
    uint8_t model[41];
    uint8_t serial[21];
    uint8_t firmware[9];
    ata_transfer_mode_t preferred_mode;
    bool dma_capable;
    bool atapi_removable;
} ata_device_info_t;

typedef struct {
    ata_channel_t channel;
    ata_device_select_t select;
    ata_device_info_t info;
    void *private_data;
} ata_device_t;

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint16_t bm_base;
    uint8_t irq;
    ata_channel_info_t channel;
    // Indexed [channel][select] - one real drive per (channel, select)
    // combination, matching the four legacy IDE positions (primary/
    // secondary x master/slave). Do NOT flatten this back to a single
    // [ATA_MAX_DEVICES_PER_CHANNEL] array: that previously made secondary
    // channel detection silently overwrite primary channel results (and
    // vice versa), since both channels' master/slave shared the same 2
    // slots keyed only by select.
    ata_device_t devices[ATA_MAX_CHANNELS][ATA_MAX_DEVICES_PER_CHANNEL];
    spinlock_t lock;
    bool initialized;
} ata_controller_t;

typedef struct {
    uint8_t features;
    uint8_t sector_count;
    uint8_t lba_low;
    uint8_t lba_mid;
    uint8_t lba_high;
    uint8_t device;
    uint8_t command;
    uint8_t control;
} ata_registers_t;

typedef enum {
    ATA_CMD_READ_PIO = 0x20,
    ATA_CMD_READ_PIO_EXT = 0x24,
    ATA_CMD_READ_DMA = 0xC8,
    ATA_CMD_READ_DMA_EXT = 0x25,
    ATA_CMD_WRITE_PIO = 0x30,
    ATA_CMD_WRITE_PIO_EXT = 0x34,
    ATA_CMD_WRITE_DMA = 0xCA,
    ATA_CMD_WRITE_DMA_EXT = 0x35,
    ATA_CMD_CACHE_FLUSH = 0xE7,
    ATA_CMD_CACHE_FLUSH_EXT = 0xEA,
    ATA_CMD_IDENTIFY = 0xEC,
    ATA_CMD_ATAPI_IDENTIFY = 0xA1,
    ATA_CMD_PACKET = 0xA0,
    ATA_CMD_NOP = 0x00
} ata_command_t;

typedef enum {
    ATA_STATUS_ERR = 0x01,
    ATA_STATUS_IDX = 0x02,
    ATA_STATUS_CORR = 0x04,
    ATA_STATUS_DRQ = 0x08,
    ATA_STATUS_SRV = 0x10,
    ATA_STATUS_DF = 0x20,
    ATA_STATUS_RDY = 0x40,
    ATA_STATUS_BSY = 0x80
} ata_status_t;

typedef enum {
    ATA_ERROR_NONE = 0,
    ATA_ERROR_BAD_SECTOR,
    ATA_ERROR_COMMAND_ABORTED,
    ATA_ERROR_MEDIA_CHANGE,
    ATA_ERROR_ID_NOT_FOUND,
    ATA_ERROR_MEDIA_LOCKED,
    ATA_ERROR_UNCORRECTABLE,
    ATA_ERROR_UNKNOWN
} ata_error_t;

typedef struct {
    ata_device_t *device;
    uint64_t lba;
    uint32_t sector_count;
    void *buffer;
    bool is_write;
    bool use_dma;
    ata_error_t error;
    bool completed;
} ata_request_t;

extern ata_controller_t g_ata_controller;

bool ata_init(void);
void ata_shutdown(void);
bool ata_detect_devices(void);
ata_device_t *ata_get_device(ata_channel_t channel, ata_device_select_t select);
int ata_read_sectors(ata_device_t *device, uint64_t lba, uint32_t count, void *buffer);
int ata_write_sectors(ata_device_t *device, uint64_t lba, uint32_t count, const void *buffer);
int ata_read_sectors_dma(ata_device_t *device, uint64_t lba, uint32_t count, void *buffer);
int ata_write_sectors_dma(ata_device_t *device, uint64_t lba, uint32_t count, const void *buffer);
bool ata_atapi_packet(ata_device_t *device, const uint8_t *packet, uint16_t packet_size, void *data, uint32_t *data_transferred);
void ata_dump_devices(void);

#define ATA_IO_BASE_PRIMARY 0x1F0
#define ATA_IO_BASE_SECONDARY 0x170
#define ATA_CTRL_BASE_PRIMARY 0x3F6
#define ATA_CTRL_BASE_SECONDARY 0x376
#define ATA_BM_BASE_PRIMARY 0x0
#define ATA_BM_BASE_SECONDARY 0x0

#define ATA_IRQ_PRIMARY 14
#define ATA_IRQ_SECONDARY 15

#endif
