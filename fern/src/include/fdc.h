#ifndef FDC_H
#define FDC_H

#include <stdbool.h>
#include <stdint.h>
#include "types.h"
#include "spinlock.h"

#define FDC_MAX_DRIVES 4
#define FDC_MAX_TRACKS 82
#define FDC_MAX_SECTORS_PER_TRACK 36
#define FDC_SECTOR_SIZE 512
#define FDC_DEFAULT_SECTORS_PER_TRACK 18
#define FDC_DEFAULT_TRACKS 80
#define FDC_DEFAULT_HEADS 2

typedef enum {
    FDC_DRIVE_NONE = 0,
    FDC_DRIVE_360K_5_25,
    FDC_DRIVE_720K_5_25,
    FDC_DRIVE_1_2M_5_25,
    FDC_DRIVE_720K_3_5,
    FDC_DRIVE_1_44M_3_5,
    FDC_DRIVE_2_88M_3_5,
    FDC_DRIVE_UNKNOWN
} fdc_drive_type_t;

typedef enum {
    FDC_MOTOR_OFF = 0,
    FDC_MOTOR_ON = 1
} fdc_motor_state_t;

typedef enum {
    FDC_DENSITY_LOW = 0,
    FDC_DENSITY_HIGH = 1
} fdc_density_t;

typedef enum {
    FDC_RATE_500Kbps = 0,
    FDC_RATE_300Kbps = 1,
    FDC_RATE_250Kbps = 2,
    FDC_RATE_1Mbps = 3
} fdc_data_rate_t;

typedef enum {
    FDC_CMD_READ_TRACK = 0x02,
    FDC_CMD_SPECIFY = 0x03,
    FDC_CMD_SENSE_DRIVE_STATUS = 0x04,
    FDC_CMD_WRITE_DATA = 0x05,
    FDC_CMD_READ_DATA = 0x06,
    FDC_CMD_RECALIBRATE = 0x07,
    FDC_CMD_SENSE_INTERRUPT = 0x08,
    FDC_CMD_WRITE_DELETED_DATA = 0x09,
    FDC_CMD_READ_ID = 0x0A,
    FDC_CMD_READ_DELETED_DATA = 0x0C,
    FDC_CMD_FORMAT_TRACK = 0x0D,
    FDC_CMD_DUMP_REGISTERS = 0x0E,
    FDC_CMD_SEEK = 0x0F,
    FDC_CMD_VERSION = 0x10,
    FDC_CMD_PERPENDICULAR_MODE = 0x12,
    FDC_CMD_CONFIGURE = 0x13,
    FDC_CMD_LOCK = 0x14,
    FDC_CMD_VERIFY = 0x16,
    FDC_CMD_SCAN_EQUAL = 0x11,
    FDC_CMD_SCAN_LOW_OR_EQUAL = 0x19,
    FDC_CMD_SCAN_HIGH_OR_EQUAL = 0x1D
} fdc_command_t;

typedef enum {
    FDC_STATUS_MT = 0x80,
    FDC_STATUS_MF = 0x40,
    FDC_STATUS_RT = 0x20,
    FDC_STATUS_EC = 0x10,
    FDC_STATUS_SE = 0x08,
    FDC_STATUS_IC = 0x04,
    FDC_STATUS_NT = 0x01,
    FDC_STATUS_INV = 0x80
} fdc_status_flags_t;

typedef enum {
    FDC_INT_NONE = 0,
    FDC_INT_TIMEOUT,
    FDC_INT_READY,
    FDC_INT_ERROR
} fdc_interrupt_type_t;

typedef struct {
    fdc_drive_type_t type;
    uint8_t tracks;
    uint8_t heads;
    uint8_t sectors_per_track;
    uint16_t bytes_per_sector;
    uint16_t total_sectors;
    uint32_t total_capacity;
    bool present;
    bool motor_on;
    uint8_t current_track;
    uint8_t current_head;
    uint8_t current_sector;
    fdc_data_rate_t data_rate;
    fdc_density_t density;
    bool perpendicular;
} fdc_drive_info_t;

typedef struct {
    uint8_t step_rate;
    uint8_t head_unload_time;
    uint8_t head_load_time;
    bool ndma;
    bool fifo;
    bool precompensation;
    uint8_t rate;
} fdc_specify_params_t;

typedef struct {
    bool motor[FDC_MAX_DRIVES];
    fdc_drive_info_t drives[FDC_MAX_DRIVES];
    uint8_t current_drive;
    uint8_t current_head;
    uint8_t current_track;
    uint8_t current_sector;
    uint32_t sector_size;
    bool irq_enabled;
    bool dma_enabled;
    uint8_t version;
    uint8_t extended_status[8];
    spinlock_t lock;
    bool initialized;
} fdc_controller_t;

typedef struct {
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    uint8_t bytes_per_sector;
} fdc_sector_id_t;

typedef struct {
    fdc_sector_id_t id;
    uint8_t status1;
    uint8_t status2;
    uint8_t reserved[6];
} fdc_format_sector_t;

typedef struct {
    uint8_t st0;
    uint8_t st1;
    uint8_t st2;
    uint8_t c;
    uint8_t h;
    uint8_t r;
    uint8_t n;
    uint8_t eot;
    uint8_t gap;
    uint8_t dtl;
} fdc_result_t;

typedef struct {
    fdc_controller_t *controller;
    fdc_drive_info_t *drive;
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    uint8_t sector_count;
    void *buffer;
    bool is_read;
    bool completed;
    fdc_result_t result;
    uint32_t bytes_transferred;
} fdc_request_t;

extern fdc_controller_t g_fdc_controller;

bool fdc_init(void);
void fdc_shutdown(void);
bool fdc_detect_drives(void);
int fdc_read_sector(fdc_drive_info_t *drive, uint16_t sector, void *buffer);
int fdc_write_sector(fdc_drive_info_t *drive, uint16_t sector, const void *buffer);
int fdc_read_sectors(fdc_drive_info_t *drive, uint16_t start_sector, uint16_t count, void *buffer);
int fdc_write_sectors(fdc_drive_info_t *drive, uint16_t start_sector, uint16_t count, const void *buffer);
void fdc_motor_on(fdc_drive_info_t *drive);
void fdc_motor_off(fdc_drive_info_t *drive);
bool fdc_calibrate(fdc_drive_info_t *drive);
bool fdc_seek(fdc_drive_info_t *drive, uint8_t cylinder, uint8_t head);
void fdc_dump_controller(void);
void fdc_dump_drive(fdc_drive_info_t *drive);

#define FDC_IO_BASE 0x3F0
#define FDC_IRQ 6
#define FDC_DMA_CHANNEL 2

#define FDC_DOR 0x3F2
#define FDC_MSR 0x3F4
#define FDC_DTR 0x3F4
#define FDC_DATA 0x3F5
#define FDC_DIR 0x3F7
#define FDC_CCR 0x3F7

#define DOR_MOTOR_D0 (1U << 4)
#define DOR_MOTOR_D1 (1U << 5)
#define DOR_MOTOR_D2 (1U << 6)
#define DOR_MOTOR_D3 (1U << 7)
#define DOR_IRQ_ENABLE (1U << 3)
#define DOR_RESET (1U << 2)
#define DOR_DRIVE_SELECT_0 0
#define DOR_DRIVE_SELECT_1 1
#define DOR_DRIVE_SELECT_2 2
#define DOR_DRIVE_SELECT_3 3

#define MSR_RQM 0x80
#define MSR_DIO 0x40
#define MSR_NDMA 0x20
#define MSR_BUSY 0x10

#define ST0_IC_MASK 0xC0
#define ST0_IC_NORMAL 0x00
#define ST0_IC_ABNORMAL 0x40
#define ST0_IC_INVALID 0x80
#define ST0_SE 0x20
#define ST0_EC 0x10
#define ST0_H 0x04
#define ST0_DRIVE 0x03

#define ST1_MAM 0x01
#define ST1_NW 0x02
#define ST1_ND 0x04
#define ST1_WP 0x08
#define ST1_SE 0x10
#define ST1_EN 0x80

#define ST2_CM 0x01
#define ST2_CRC 0x02
#define ST2_WC 0x04
#define ST2_BC 0x08
#define ST2_MD 0x10
#define ST2_DB 0x20
#define ST2_ND 0x40

#define FDC_CONFIGURE_EIS 0x40
#define FDC_CONFIGURE_EFIFO 0x20
#define FDC_CONFIGURE_POLL 0x10
#define FDC_CONFIGURE_FIFOTHRESHOLD 0x0F

#endif
