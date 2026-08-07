#ifndef AHCI_H
#define AHCI_H

#include <stdbool.h>
#include <stdint.h>
#include "types.h"
#include "spinlock.h"

#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMD_SLOTS 32
#define AHCI_MAX_SG_ENTRIES 65535
#define AHCI_SECTOR_SIZE 512
#define AHCI_FIS_SIZE 256

typedef enum {
    AHCI_DEV_TYPE_NONE = 0,
    AHCI_DEV_TYPE_SATA,
    AHCI_DEV_TYPE_SEMB,
    AHCI_DEV_TYPE_PM,
    AHCI_DEV_TYPE_SATAPI
} ahci_device_type_t;

typedef enum {
    AHCI_PHY_NONE = 0,
    AHCI_PHY_SATA,
    AHCI_PHY_SATAPI,
    AHCI_PHY_SEMB,
    AHCI_PHY_PM
} ahci_phy_type_t;

typedef enum {
    AHCI_CMD_LIST_NONE = 0,
    AHCI_CMD_LIST_READ,
    AHCI_CMD_LIST_WRITE
} ahci_command_type_t;

typedef struct {
    uint32_t desc_info;
    uint32_t reserved;
    uint64_t dt_base;
    uint32_t dt_base_upper;
    uint32_t reserved2;
} __attribute__((packed)) ahci_sg_entry_t;

typedef struct {
    uint32_t opts;
    uint32_t reserved;
    uint64_t cmd_table_base;
    uint32_t cmd_table_base_upper;
    uint32_t reserved2;
} __attribute__((packed)) ahci_cmd_slot_t;

typedef struct {
    uint8_t fis_type;
    uint8_t opts;
    uint8_t command;
    uint8_t feature_low;
    uint8_t sector_count_low;
    uint8_t lba_low;
    uint8_t lba_mid;
    uint8_t lba_high;
    uint8_t device;
    uint8_t lba_exp;
    uint8_t sector_count_exp;
    uint8_t feature_high;
    uint8_t reserved;
    uint16_t control;
} __attribute__((packed)) ahci_fis_reg_h2d_t;

typedef struct {
    uint8_t fis_type;
    uint8_t opts;
    uint8_t status;
    uint8_t error;
    uint8_t lba_low;
    uint8_t lba_mid;
    uint8_t lba_high;
    uint8_t device;
    uint8_t lba_exp;
    uint8_t sector_count_exp;
    uint8_t reserved;
    uint8_t reserved2;
    uint16_t sector_count;
} __attribute__((packed)) ahci_fis_reg_d2h_t;

typedef struct {
    uint8_t fis_type;
    uint8_t opts;
    uint8_t interrupt_bit;
    uint8_t reserved;
    uint32_t buffer[12];
} __attribute__((packed)) ahci_fis_data_t;

typedef struct {
    uint8_t fis_type;
    uint8_t opts;
    uint8_t pm_port;
    uint8_t reserved;
} __attribute__((packed)) ahci_fis_pio_setup_t;

typedef struct {
    uint8_t fis_type;
    uint8_t opts;
    uint8_t interrupt_bit;
    uint8_t error;
    uint32_t status;
} __attribute__((packed)) ahci_fis_d2h_t;

typedef struct {
    uint8_t fis_type;
    uint8_t opts;
    uint8_t sm_port;
    uint8_t reserved;
    uint32_t sector_count;
    uint32_t lba_low;
    uint32_t lba_mid;
    uint32_t lba_high;
    uint32_t lba_exp;
    uint32_t sector_count_exp;
    uint8_t reserved2[8];
} __attribute__((packed)) ahci_fis_set_device_bits_t;

typedef struct ahci_port ahci_port_t;

typedef struct {
    ahci_fis_reg_h2d_t command_fis;
    ahci_fis_reg_d2h_t status_fis;
    ahci_fis_data_t data_fis;
    ahci_fis_pio_setup_t pio_fis;
    ahci_fis_d2h_t d2h_fis;
    ahci_fis_set_device_bits_t device_bits_fis;
    uint8_t reserved[0x100 - sizeof(ahci_fis_set_device_bits_t) - sizeof(ahci_fis_data_t) * 2 - sizeof(ahci_fis_pio_setup_t)];
} __attribute__((packed)) ahci_cmd_table_t;

typedef struct {
    uint32_t cl_base;
    uint32_t cl_base_upper;
    uint32_t fb_base;
    uint32_t fb_base_upper;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t task_file_data;
    uint32_t signature;
    uint32_t sstatus;
    uint32_t scontrol;
    uint32_t serr;
    uint32_t sactive;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t reserved1[11];
    uint32_t vendor[4];
} __attribute__((packed)) ahci_hba_port_regs_t;

typedef struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_ports;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint32_t reserved[4];
    ahci_hba_port_regs_t ports[];
} __attribute__((packed)) ahci_hba_t;

typedef struct {
    ahci_phy_type_t device_type;
    bool present;
    bool atapi;
    bool supports_native_cmd_queuing;
    bool supports_aggressive_link_power;
    bool supports_activity_led;
    bool supports_aggressive_slumber;
    bool supports_only_initiator;
    bool supports_explicit_buffer_size;
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t max_cmd_slots;
    uint32_t port_multiplier;
    uint8_t signature[6];
    uint8_t revision;
    uint16_t interface_speed_support;
    char model[41];
    char serial[21];
    char firmware[9];
} ahci_port_info_t;

struct ahci_port {
    uint8_t port_number;
    ahci_port_info_t info;
    ahci_hba_port_regs_t *regs;
    ahci_cmd_slot_t *cmd_list;
    ahci_cmd_table_t *cmd_tables;
    ahci_fis_reg_d2h_t *received_fis;
    uint8_t *cmd_table_memory;
    uint8_t *cmd_list_memory;
    uint8_t *fis_memory;
    spinlock_t lock;
    bool initialized;
    uint32_t sactive_mask;
    uint32_t outstanding_commands;
};

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t bar5;
    ahci_hba_t *hba;
    uint32_t abar;
    uint32_t caps;
    uint32_t caps2;
    uint32_t ports_implemented;
    uint32_t port_count;
    ahci_port_t ports[AHCI_MAX_PORTS];
    spinlock_t lock;
    bool initialized;
} ahci_controller_t;

typedef struct {
    ahci_port_t *port;
    uint8_t slot;
    ahci_command_type_t type;
    uint64_t lba;
    uint32_t sector_count;
    void *buffer;
    bool is_write;
    bool use_ncq;
    bool completed;
    uint32_t bytes_transferred;
} ahci_request_t;

extern ahci_controller_t g_ahci_controller;

bool ahci_init(void);
void ahci_shutdown(void);
bool ahci_detect_controller(void);
bool ahci_detect_ports(void);
ahci_port_t *ahci_get_port(uint8_t port_number);
int ahci_read_sectors(ahci_port_t *port, uint64_t lba, uint32_t count, void *buffer);
int ahci_write_sectors(ahci_port_t *port, uint64_t lba, uint32_t count, const void *buffer);
bool ahci_port_start(ahci_port_t *port);
bool ahci_port_stop(ahci_port_t *port);
void ahci_dump_controller(void);
void ahci_dump_port(ahci_port_t *port);

#define AHCI_CAP_S64A (1U << 31)
#define AHCI_CAP_SNCQ (1U << 30)
#define AHCI_CAP_S64A_BIT 31
#define AHCI_CAP_SNCQ_BIT 30

#define AHCI_CMD_ST  (1U << 0)
#define AHCI_CMD_SUD (1U << 1)
#define AHCI_CMD_POD (1U << 2)
#define AHCI_CMD_CLO (1U << 3)
#define AHCI_CMD_FRE (1U << 4)
#define AHCI_CMD_RESERVED (1U << 5)
#define AHCI_CMD_CR (1U << 0)
#define AHCI_CMD_FR (1U << 1)
#define AHCI_CMD_MPSS (1U << 3)
#define AHCI_CMD_FBSCP (1U << 2)

#define AHCI_PxIS_TFES (1U << 30)
#define AHCI_PxIS_HBFS (1U << 29)
#define AHCI_PxIS_HBDS (1U << 28)
#define AHCI_PxIS_IFS  (1U << 27)
#define AHCI_PxIS_PRCS (1U << 22)
#define AHCI_PxIS_DMPS (1U << 7)
#define AHCI_PxIS_PCS  (1U << 6)
#define AHCI_PxIS_DPS  (1U << 5)
#define AHCI_PxIS_SDBS (1U << 4)
#define AHCI_PxIS_DS   (1U << 3)
#define AHCI_PxIS_SS   (1U << 2)
#define AHCI_PxIS_PS   (1U << 1)

#define AHCI_SSTS_DET 0x0F
#define AHCI_SSTS_DET_MASK 0x0F
#define AHCI_SSTS_DET_NO_DEVICE 0x00
#define AHCI_SSTS_DET_PHY_RESET 0x01
#define AHCI_SSTS_DET_COMINIT 0x03
#define AHCI_SSTS_DET_ACTIVE 0x0F

#define AHCI_SSTS_SPD_MASK 0x7800
#define AHCI_SSTS_SPD_GEN1 0x1000
#define AHCI_SSTS_SPD_GEN2 0x2000
#define AHCI_SSTS_SPD_GEN3 0x3000

#define AHCI_SSTS_IPM_MASK 0x0600
#define AHCI_SSTS_IPM_ACTIVE 0x0100
#define AHCI_SSTS_IPM_PARTIAL 0x0200
#define AHCI_SSTS_IPM_SLUMBER 0x0400

#define ATA_FIS_TYPE_REG_H2D 0x27
#define ATA_FIS_TYPE_REG_D2H 0x34
#define ATA_FIS_TYPE_DATA    0x46
#define ATA_FIS_TYPE_PIO_SETUP 0x5F
#define ATA_FIS_TYPE_D2H_REG 0x58
#define ATA_FIS_TYPE_SDB 0xA1

#endif
