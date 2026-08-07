/**
 * OHCI (Open Host Controller Interface) Driver for Fern
 *
 * Implements USB 1.0 support via the OHCI controller specification.
 * Based on OHCI 1.0a specification and OSDev documentation.
 */

#ifndef OHCI_H
#define OHCI_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// OHCI Memory Mapped Registers (offsets)
#define OHCI_REG_REVISION           0x00    // HcRevision
#define OHCI_REG_CONTROL            0x04    // HcControl
#define OHCI_REG_CMDSTATUS          0x08    // HcCommandStatus
#define OHCI_REG_INTSTATUS          0x0C    // HcInterruptStatus
#define OHCI_REG_INTENABLE          0x10    // HcInterruptEnable
#define OHCI_REG_INTDISABLE         0x14    // HcInterruptDisable
#define OHCI_REG_HCCA               0x18    // HcHCCA
#define OHCI_REG_PERIODCURRENTED    0x1C    // HcPeriodCurrentED
#define OHCI_REG_CONTROLHEADED      0x20    // HcControlHeadED
#define OHCI_REG_CONTROLCURRENTED   0x24    // HcControlCurrentED
#define OHCI_REG_BULKHEADED         0x28    // HcBulkHeadED
#define OHCI_REG_BULKCURRENTED      0x2C    // HcBulkCurrentED
#define OHCI_REG_DONEHEAD           0x30    // HcDoneHead
#define OHCI_REG_FMINTERVAL         0x34    // HcFmInterval
#define OHCI_REG_FMREMAINING        0x38    // HcFmRemaining
#define OHCI_REG_FMNUMBER           0x3C    // HcFmNumber
#define OHCI_REG_PERIODICSTART      0x40    // HcPeriodicStart
#define OHCI_REG_LSTHRESHOLD        0x44    // HcLSThreshold
#define OHCI_REG_RHDESCRIPTORA      0x48    // HcRhDescriptorA
#define OHCI_REG_RHDESCRIPTORB      0x4C    // HcRhDescriptorB
#define OHCI_REG_RHSTATUS           0x50    // HcRhStatus
#define OHCI_REG_RHPORTSTATUS       0x54    // HcRhPortStatus[0..N]

// HcControl register bits
#define OHCI_CTRL_CBSR_MASK         0x03        // Control/Bulk Service Ratio
#define OHCI_CTRL_PLE               (1 << 2)    // Periodic List Enable
#define OHCI_CTRL_IE                (1 << 3)    // Isochronous Enable
#define OHCI_CTRL_CLE               (1 << 4)    // Control List Enable
#define OHCI_CTRL_BLE               (1 << 5)    // Bulk List Enable
#define OHCI_CTRL_HCFS_MASK         (3 << 6)    // Host Controller Functional State
#define OHCI_CTRL_HCFS_RESET        (0 << 6)
#define OHCI_CTRL_HCFS_RESUME       (1 << 6)
#define OHCI_CTRL_HCFS_OPERATIONAL  (2 << 6)
#define OHCI_CTRL_HCFS_SUSPEND      (3 << 6)
#define OHCI_CTRL_IR                (1 << 8)    // Interrupt Routing
#define OHCI_CTRL_RWC               (1 << 9)    // Remote Wakeup Connected
#define OHCI_CTRL_RWE               (1 << 10)   // Remote Wakeup Enable

// HcCommandStatus register bits
#define OHCI_CMDSTS_HCR             (1 << 0)    // Host Controller Reset
#define OHCI_CMDSTS_CLF             (1 << 1)    // Control List Filled
#define OHCI_CMDSTS_BLF             (1 << 2)    // Bulk List Filled
#define OHCI_CMDSTS_OCR             (1 << 3)    // Ownership Change Request
#define OHCI_CMDSTS_SOC_MASK        (3 << 16)   // Scheduling Overrun Count

// HcInterruptStatus/Enable/Disable register bits
#define OHCI_INT_SO                 (1 << 0)    // Scheduling Overrun
#define OHCI_INT_WDH                (1 << 1)    // Writeback Done Head
#define OHCI_INT_SF                 (1 << 2)    // Start of Frame
#define OHCI_INT_RD                 (1 << 3)    // Resume Detect
#define OHCI_INT_UE                 (1 << 4)    // Unrecoverable Error
#define OHCI_INT_FNO                (1 << 5)    // Frame Number Overflow
#define OHCI_INT_RHSC               (1 << 6)    // Root Hub Status Change
#define OHCI_INT_OC                 (1 << 30)   // Ownership Change
#define OHCI_INT_MIE                (1 << 31)   // Master Interrupt Enable

// HcRhDescriptorA register bits
#define OHCI_RHA_NDP_MASK           0xFF        // Number of Downstream Ports
#define OHCI_RHA_PSM                (1 << 8)    // Power Switching Mode
#define OHCI_RHA_NPS                (1 << 9)    // No Power Switching
#define OHCI_RHA_DT                 (1 << 10)   // Device Type
#define OHCI_RHA_OCPM               (1 << 11)   // Over Current Protection Mode
#define OHCI_RHA_NOCP               (1 << 12)   // No Over Current Protection
#define OHCI_RHA_POTPGT_SHIFT       24          // Power On To Power Good Time

// HcRhStatus register bits
#define OHCI_RHS_LPS                (1 << 0)    // Local Power Status
#define OHCI_RHS_OCI                (1 << 1)    // Over Current Indicator
#define OHCI_RHS_DRWE               (1 << 15)   // Device Remote Wakeup Enable
#define OHCI_RHS_LPSC               (1 << 16)   // Local Power Status Change
#define OHCI_RHS_OCIC               (1 << 17)   // Over Current Indicator Change
#define OHCI_RHS_CRWE               (1 << 31)   // Clear Remote Wakeup Enable

// HcRhPortStatus register bits
#define OHCI_PORT_CCS               (1 << 0)    // Current Connect Status
#define OHCI_PORT_PES               (1 << 1)    // Port Enable Status
#define OHCI_PORT_PSS               (1 << 2)    // Port Suspend Status
#define OHCI_PORT_POCI              (1 << 3)    // Port Over Current Indicator
#define OHCI_PORT_PRS               (1 << 4)    // Port Reset Status
#define OHCI_PORT_PPS               (1 << 8)    // Port Power Status
#define OHCI_PORT_LSDA              (1 << 9)    // Low Speed Device Attached
#define OHCI_PORT_CSC               (1 << 16)   // Connect Status Change
#define OHCI_PORT_PESC              (1 << 17)   // Port Enable Status Change
#define OHCI_PORT_PSSC              (1 << 18)   // Port Suspend Status Change
#define OHCI_PORT_OCIC              (1 << 19)   // Port Over Current Indicator Change
#define OHCI_PORT_PRSC              (1 << 20)   // Port Reset Status Change

// OHCI Endpoint Descriptor Flags
#define OHCI_ED_FA_MASK             0x7F            // Function Address
#define OHCI_ED_EN_SHIFT            7               // Endpoint Number shift
#define OHCI_ED_EN_MASK             (0xF << 7)      // Endpoint Number
#define OHCI_ED_D_SHIFT             11              // Direction shift
#define OHCI_ED_D_TD                (0 << 11)       // Get direction from TD
#define OHCI_ED_D_OUT               (1 << 11)       // OUT
#define OHCI_ED_D_IN                (2 << 11)       // IN
#define OHCI_ED_S                   (1 << 13)       // Speed (0=full, 1=low)
#define OHCI_ED_K                   (1 << 14)       // Skip
#define OHCI_ED_F                   (1 << 15)       // Format (0=general, 1=isochronous)
#define OHCI_ED_MPS_SHIFT           16              // Max Packet Size shift
#define OHCI_ED_MPS_MASK            (0x7FF << 16)   // Max Packet Size

// OHCI Endpoint Descriptor (16 bytes, 16-byte aligned)
typedef struct __attribute__((packed, aligned(16))) {
    uint32 flags;           // FA, EN, D, S, K, F, MPS
    uint32 tail_td;         // Tail TD pointer (physical)
    uint32 head_td;         // Head TD pointer (physical) + Halt/Toggle bits
    uint32 next_ed;         // Next ED pointer (physical)
} ohci_ed_t;

// OHCI TD Flags
#define OHCI_TD_R                   (1 << 18)       // Buffer Rounding
#define OHCI_TD_DP_SETUP            (0 << 19)       // Direction/PID: SETUP
#define OHCI_TD_DP_OUT              (1 << 19)       // Direction/PID: OUT
#define OHCI_TD_DP_IN               (2 << 19)       // Direction/PID: IN
#define OHCI_TD_DI_MASK             (7 << 21)       // Delay Interrupt
#define OHCI_TD_DI_NONE             (7 << 21)       // No interrupt
#define OHCI_TD_T_DATA0             (2 << 24)       // Data toggle DATA0
#define OHCI_TD_T_DATA1             (3 << 24)       // Data toggle DATA1
#define OHCI_TD_EC_MASK             (3 << 26)       // Error Count
#define OHCI_TD_CC_MASK             (0xF << 28)     // Condition Code
#define OHCI_TD_CC_SHIFT            28

// OHCI General Transfer Descriptor (16 bytes, 16-byte aligned)
typedef struct __attribute__((packed, aligned(16))) {
    uint32 flags;           // R, DP, DI, T, EC, CC
    uint32 cbp;             // Current Buffer Pointer
    uint32 next_td;         // Next TD pointer (physical)
    uint32 be;              // Buffer End
} ohci_td_t;

// OHCI Host Controller Communication Area (256 bytes, 256-byte aligned)
typedef struct __attribute__((packed, aligned(256))) {
    uint32 interrupt_table[32]; // Interrupt ED pointers
    uint16 frame_number;        // Current frame number
    uint16 pad1;
    uint32 done_head;           // Done queue head
    uint8  reserved[116];       // Reserved for HC use
    uint8  unused[4];           // Unused
} ohci_hcca_t;

// Condition Codes
#define OHCI_CC_NOERROR             0
#define OHCI_CC_CRC                 1
#define OHCI_CC_BITSTUFFING         2
#define OHCI_CC_DATATOGGLEMISMATCH  3
#define OHCI_CC_STALL               4
#define OHCI_CC_DEVICENOTRESPONDING 5
#define OHCI_CC_PIDCHECKFAILURE     6
#define OHCI_CC_UNEXPECTEDPID       7
#define OHCI_CC_DATAOVERRUN         8
#define OHCI_CC_DATAUNDERRUN        9
#define OHCI_CC_BUFFEROVERRUN       12
#define OHCI_CC_BUFFERUNDERRUN      13
#define OHCI_CC_NOTACCESSED         14

// OHCI Controller Data
typedef struct {
    volatile uint32* regs;      // Memory mapped registers
    ohci_hcca_t* hcca;          // Host Controller Communication Area
    ohci_ed_t* ed_control;      // Control ED list head
    ohci_ed_t* ed_bulk;         // Bulk ED list head
    ohci_ed_t* ed_interrupt[32];// Interrupt ED heads
    ohci_ed_t* ed_pool;         // ED pool
    ohci_td_t* td_pool;         // TD pool
    uint32 ed_bitmap[8];        // ED allocation bitmap
    uint32 td_bitmap[32];       // TD allocation bitmap
    uint8  num_ports;           // Number of root hub ports
} ohci_data_t;

// OHCI Functions
bool ohci_init(usb_controller_t* controller);
void ohci_shutdown(usb_controller_t* controller);
bool ohci_reset_port(usb_controller_t* controller, uint8 port);
bool ohci_enable_port(usb_controller_t* controller, uint8 port);
bool ohci_disable_port(usb_controller_t* controller, uint8 port);
usb_speed_t ohci_get_port_speed(usb_controller_t* controller, uint8 port);
bool ohci_port_connected(usb_controller_t* controller, uint8 port);
int ohci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length);
int ohci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length);
int ohci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length);
void ohci_poll(usb_controller_t* controller);

// OHCI controller operations
extern usb_controller_ops_t ohci_ops;

#endif // OHCI_H
