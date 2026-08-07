/**
 * UHCI (Universal Host Controller Interface) Driver for Fern
 *
 * Implements USB 1.0 support via the UHCI controller specification.
 * Based on Intel UHCI specification and OSDev documentation.
 */

#ifndef UHCI_H
#define UHCI_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// UHCI I/O Registers
#define UHCI_REG_USBCMD         0x00    // USB Command (2 bytes)
#define UHCI_REG_USBSTS         0x02    // USB Status (2 bytes)
#define UHCI_REG_USBINTR        0x04    // USB Interrupt Enable (2 bytes)
#define UHCI_REG_FRNUM          0x06    // Frame Number (2 bytes)
#define UHCI_REG_FRBASEADD      0x08    // Frame List Base Address (4 bytes)
#define UHCI_REG_SOFMOD         0x0C    // Start of Frame Modify (1 byte)
#define UHCI_REG_PORTSC1        0x10    // Port 1 Status/Control (2 bytes)
#define UHCI_REG_PORTSC2        0x12    // Port 2 Status/Control (2 bytes)

// USB Command Register bits
#define UHCI_CMD_RS             (1 << 0)    // Run/Stop
#define UHCI_CMD_HCRESET        (1 << 1)    // Host Controller Reset
#define UHCI_CMD_GRESET         (1 << 2)    // Global Reset
#define UHCI_CMD_EGSM           (1 << 3)    // Enter Global Suspend Mode
#define UHCI_CMD_FGR            (1 << 4)    // Force Global Resume
#define UHCI_CMD_SWDBG          (1 << 5)    // Software Debug
#define UHCI_CMD_CF             (1 << 6)    // Configure Flag
#define UHCI_CMD_MAXP           (1 << 7)    // Max Packet (0=32, 1=64)

// USB Status Register bits
#define UHCI_STS_USBINT         (1 << 0)    // USB Interrupt
#define UHCI_STS_ERROR          (1 << 1)    // USB Error Interrupt
#define UHCI_STS_RD             (1 << 2)    // Resume Detect
#define UHCI_STS_HSE            (1 << 3)    // Host System Error
#define UHCI_STS_HCPE           (1 << 4)    // Host Controller Process Error
#define UHCI_STS_HCH            (1 << 5)    // Host Controller Halted

// USB Interrupt Enable Register bits
#define UHCI_INTR_TIMEOUT_CRC   (1 << 0)    // Timeout/CRC Interrupt Enable
#define UHCI_INTR_RESUME        (1 << 1)    // Resume Interrupt Enable
#define UHCI_INTR_IOC           (1 << 2)    // Interrupt on Complete Enable
#define UHCI_INTR_SP            (1 << 3)    // Short Packet Interrupt Enable

// Port Status/Control Register bits
#define UHCI_PORTSC_CCS         (1 << 0)    // Current Connect Status
#define UHCI_PORTSC_CSC         (1 << 1)    // Connect Status Change (W1C)
#define UHCI_PORTSC_PED         (1 << 2)    // Port Enabled/Disabled
#define UHCI_PORTSC_PEDC        (1 << 3)    // Port Enable/Disable Change (W1C)
#define UHCI_PORTSC_LS          (3 << 4)    // Line Status (bits 4-5)
#define UHCI_PORTSC_RD          (1 << 6)    // Resume Detect
#define UHCI_PORTSC_LSDA        (1 << 8)    // Low Speed Device Attached
#define UHCI_PORTSC_PR          (1 << 9)    // Port Reset
#define UHCI_PORTSC_SUSP        (1 << 12)   // Suspend

// Frame list size
#define UHCI_FRAME_LIST_SIZE    1024

// TD and QH pointer bits
#define UHCI_PTR_TERMINATE      (1 << 0)    // Pointer terminates
#define UHCI_PTR_QH             (1 << 1)    // Points to QH (vs TD)
#define UHCI_PTR_DEPTH          (1 << 2)    // Depth first (TD only)

// Transfer Descriptor Status bits
#define UHCI_TD_STATUS_ACTLEN_MASK  0x7FF   // Actual length
#define UHCI_TD_STATUS_BITSTUFF     (1 << 17)   // Bitstuff Error
#define UHCI_TD_STATUS_CRC_TIMEOUT  (1 << 18)   // CRC/Timeout Error
#define UHCI_TD_STATUS_NAK          (1 << 19)   // NAK Received
#define UHCI_TD_STATUS_BABBLE       (1 << 20)   // Babble Detected
#define UHCI_TD_STATUS_DBUFFER      (1 << 21)   // Data Buffer Error
#define UHCI_TD_STATUS_STALLED      (1 << 22)   // Stalled
#define UHCI_TD_STATUS_ACTIVE       (1 << 23)   // Active
#define UHCI_TD_STATUS_IOC          (1 << 24)   // Interrupt on Complete
#define UHCI_TD_STATUS_IOS          (1 << 25)   // Isochronous Select
#define UHCI_TD_STATUS_LS           (1 << 26)   // Low Speed Device
#define UHCI_TD_STATUS_ERRCNT_MASK  (3 << 27)   // Error Counter
#define UHCI_TD_STATUS_SPD          (1 << 29)   // Short Packet Detect

// Transfer Descriptor Token bits
#define UHCI_TD_TOKEN_PID_IN        0x69
#define UHCI_TD_TOKEN_PID_OUT       0xE1
#define UHCI_TD_TOKEN_PID_SETUP     0x2D

// UHCI Transfer Descriptor (16 bytes, 16-byte aligned)
typedef struct __attribute__((packed, aligned(16))) {
    uint32 link;            // Link Pointer
    uint32 status;          // Status/Control
    uint32 token;           // Token
    uint32 buffer;          // Buffer Pointer
    // Software use (not read by hardware)
    uint32 sw_buffer;       // Original buffer address
    uint32 sw_length;       // Transfer length
    uint32 sw_reserved[2];  // Padding to 32 bytes
} uhci_td_t;

// UHCI Queue Head (16 bytes, 16-byte aligned)
typedef struct __attribute__((packed, aligned(16))) {
    uint32 head_link;       // Queue Head Horizontal Link
    uint32 element_link;    // Queue Element Link
    // Software use
    uint32 sw_reserved[6];  // Padding to 32 bytes
} uhci_qh_t;

// UHCI Controller Data
typedef struct {
    uint16  io_base;                // I/O base address
    uint32* frame_list;             // Frame list (1024 entries)
    uhci_qh_t* qh_control;          // Control queue head
    uhci_qh_t* qh_bulk;             // Bulk queue head
    uhci_qh_t* qh_interrupt[11];    // Interrupt queue heads (1ms, 2ms, 4ms, etc.)
    uhci_td_t* td_pool;             // Transfer descriptor pool
    uint32  td_pool_bitmap[32];     // TD allocation bitmap
    bool    legacy_support;         // BIOS legacy support present
} uhci_data_t;

// UHCI Functions
bool uhci_init(usb_controller_t* controller);
void uhci_shutdown(usb_controller_t* controller);
bool uhci_reset_port(usb_controller_t* controller, uint8 port);
bool uhci_enable_port(usb_controller_t* controller, uint8 port);
bool uhci_disable_port(usb_controller_t* controller, uint8 port);
usb_speed_t uhci_get_port_speed(usb_controller_t* controller, uint8 port);
bool uhci_port_connected(usb_controller_t* controller, uint8 port);
int uhci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length);
int uhci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length);
int uhci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length);
void uhci_poll(usb_controller_t* controller);

// UHCI controller operations
extern usb_controller_ops_t uhci_ops;

#endif // UHCI_H
