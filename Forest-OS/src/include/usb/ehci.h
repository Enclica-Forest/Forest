/**
 * EHCI (Enhanced Host Controller Interface) Driver for Fern
 *
 * Implements USB 2.0 (High-Speed) support via the EHCI controller specification.
 * Based on Intel EHCI specification and OSDev documentation.
 */

#ifndef EHCI_H
#define EHCI_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// EHCI Capability Registers (offsets from BAR0)
#define EHCI_CAP_CAPLENGTH          0x00    // Capability Register Length (1 byte)
#define EHCI_CAP_HCIVERSION         0x02    // Interface Version Number (2 bytes)
#define EHCI_CAP_HCSPARAMS          0x04    // Structural Parameters (4 bytes)
#define EHCI_CAP_HCCPARAMS          0x08    // Capability Parameters (4 bytes)
#define EHCI_CAP_HCSP_PORTROUTE     0x0C    // Companion Port Route Description

// EHCI Operational Registers (offsets from BAR0 + CAPLENGTH)
#define EHCI_OP_USBCMD              0x00    // USB Command (4 bytes)
#define EHCI_OP_USBSTS              0x04    // USB Status (4 bytes)
#define EHCI_OP_USBINTR             0x08    // USB Interrupt Enable (4 bytes)
#define EHCI_OP_FRINDEX             0x0C    // USB Frame Index (4 bytes)
#define EHCI_OP_CTRLDSSEGMENT       0x10    // 4G Segment Selector (4 bytes)
#define EHCI_OP_PERIODICLISTBASE    0x14    // Frame List Base Address (4 bytes)
#define EHCI_OP_ASYNCLISTADDR       0x18    // Next Asynchronous List Address (4 bytes)
#define EHCI_OP_CONFIGFLAG          0x40    // Configured Flag Register (4 bytes)
#define EHCI_OP_PORTSC              0x44    // Port Status/Control (4 bytes each)

// HCSPARAMS bits
#define EHCI_HCS_N_PORTS_MASK       0x0F        // Number of Ports
#define EHCI_HCS_PPC                (1 << 4)    // Port Power Control
#define EHCI_HCS_PRR                (1 << 7)    // Port Routing Rules
#define EHCI_HCS_N_PCC_SHIFT        8           // Ports Per Companion Controller
#define EHCI_HCS_N_PCC_MASK         (0x0F << 8)
#define EHCI_HCS_N_CC_SHIFT         12          // Number of Companion Controllers
#define EHCI_HCS_N_CC_MASK          (0x0F << 12)
#define EHCI_HCS_P_INDICATOR        (1 << 16)   // Port Indicators
#define EHCI_HCS_DEBUG_PORT_SHIFT   20          // Debug Port Number
#define EHCI_HCS_DEBUG_PORT_MASK    (0x0F << 20)

// HCCPARAMS bits
#define EHCI_HCC_64BIT              (1 << 0)    // 64-bit Addressing Capable
#define EHCI_HCC_PFLF               (1 << 1)    // Programmable Frame List Flag
#define EHCI_HCC_ASPC               (1 << 2)    // Asynchronous Schedule Park Capability
#define EHCI_HCC_IST_SHIFT          4           // Isochronous Scheduling Threshold
#define EHCI_HCC_EECP_SHIFT         8           // EHCI Extended Capabilities Pointer
#define EHCI_HCC_EECP_MASK          (0xFF << 8)

// USB Command Register bits
#define EHCI_CMD_RS                 (1 << 0)    // Run/Stop
#define EHCI_CMD_HCRESET            (1 << 1)    // Host Controller Reset
#define EHCI_CMD_FLS_MASK           (3 << 2)    // Frame List Size (1024/512/256)
#define EHCI_CMD_PSE                (1 << 4)    // Periodic Schedule Enable
#define EHCI_CMD_ASE                (1 << 5)    // Asynchronous Schedule Enable
#define EHCI_CMD_IAAD               (1 << 6)    // Interrupt on Async Advance Doorbell
#define EHCI_CMD_LHCR               (1 << 7)    // Light Host Controller Reset
#define EHCI_CMD_ASPMC_SHIFT        8           // Async Schedule Park Mode Count
#define EHCI_CMD_ASPMC_MASK         (3 << 8)
#define EHCI_CMD_ASPME              (1 << 11)   // Async Schedule Park Mode Enable
#define EHCI_CMD_ITC_SHIFT          16          // Interrupt Threshold Control
#define EHCI_CMD_ITC_MASK           (0xFF << 16)

// USB Status Register bits
#define EHCI_STS_INT                (1 << 0)    // USB Interrupt
#define EHCI_STS_ERR                (1 << 1)    // USB Error Interrupt
#define EHCI_STS_PCD                (1 << 2)    // Port Change Detect
#define EHCI_STS_FLR                (1 << 3)    // Frame List Rollover
#define EHCI_STS_HSE                (1 << 4)    // Host System Error
#define EHCI_STS_IAA                (1 << 5)    // Interrupt on Async Advance
#define EHCI_STS_HALT               (1 << 12)   // HCHalted
#define EHCI_STS_RECLAMATION        (1 << 13)   // Reclamation
#define EHCI_STS_PSS                (1 << 14)   // Periodic Schedule Status
#define EHCI_STS_ASS                (1 << 15)   // Asynchronous Schedule Status

// USB Interrupt Enable Register bits
#define EHCI_INTR_INT               (1 << 0)    // USB Interrupt Enable
#define EHCI_INTR_ERR               (1 << 1)    // USB Error Interrupt Enable
#define EHCI_INTR_PCD               (1 << 2)    // Port Change Interrupt Enable
#define EHCI_INTR_FLR               (1 << 3)    // Frame List Rollover Interrupt Enable
#define EHCI_INTR_HSE               (1 << 4)    // Host System Error Interrupt Enable
#define EHCI_INTR_IAA               (1 << 5)    // Interrupt on Async Advance Enable

// Port Status/Control Register bits
#define EHCI_PORT_CCS               (1 << 0)    // Current Connect Status
#define EHCI_PORT_CSC               (1 << 1)    // Connect Status Change
#define EHCI_PORT_PED               (1 << 2)    // Port Enabled/Disabled
#define EHCI_PORT_PEDC              (1 << 3)    // Port Enable/Disable Change
#define EHCI_PORT_OCA               (1 << 4)    // Over-current Active
#define EHCI_PORT_OCC               (1 << 5)    // Over-current Change
#define EHCI_PORT_FPR               (1 << 6)    // Force Port Resume
#define EHCI_PORT_SUSPEND           (1 << 7)    // Suspend
#define EHCI_PORT_RESET             (1 << 8)    // Port Reset
#define EHCI_PORT_LS_MASK           (3 << 10)   // Line Status
#define EHCI_PORT_PP                (1 << 12)   // Port Power
#define EHCI_PORT_OWNER             (1 << 13)   // Port Owner (0=EHCI, 1=Companion)
#define EHCI_PORT_PIC_MASK          (3 << 14)   // Port Indicator Control
#define EHCI_PORT_PTC_MASK          (0x0F << 16)// Port Test Control
#define EHCI_PORT_WKOC              (1 << 22)   // Wake on Over-current Enable
#define EHCI_PORT_WKDC              (1 << 21)   // Wake on Disconnect Enable
#define EHCI_PORT_WKC               (1 << 20)   // Wake on Connect Enable

// Configure Flag Register
#define EHCI_CF_FLAG                (1 << 0)    // Configure Flag

// Extended Capability IDs
#define EHCI_EECP_LEGACY            0x01        // BIOS/OS Handoff
#define EHCI_EECP_DEBUG             0xA0        // Debug Port

// Legacy Support Registers (in PCI config space at EECP)
#define EHCI_LEGACY_USBLEGSUP       0x00        // USB Legacy Support
#define EHCI_LEGACY_USBLEGCTLSTS    0x04        // USB Legacy Support Control/Status

#define EHCI_LEGACY_BIOS_OWNED      (1 << 16)   // BIOS owns HC
#define EHCI_LEGACY_OS_OWNED        (1 << 24)   // OS owns HC

// Frame list size
#define EHCI_FRAME_LIST_SIZE        1024

// Queue Head Types
#define EHCI_QH_TYPE_ITD            0           // Isochronous Transfer Descriptor
#define EHCI_QH_TYPE_QH             1           // Queue Head
#define EHCI_QH_TYPE_SITD           2           // Split Transaction Isochronous TD
#define EHCI_QH_TYPE_FSTN           3           // Frame Span Traversal Node

// Queue Head Link Pointer bits
#define EHCI_QH_LP_TERMINATE        (1 << 0)    // Terminate
#define EHCI_QH_LP_TYPE_SHIFT       1           // Type
#define EHCI_QH_LP_TYPE_MASK        (3 << 1)

// Queue Head Endpoint Characteristics
#define EHCI_QH_EC_ADDR_MASK        0x7F            // Device Address
#define EHCI_QH_EC_INACTIVATE       (1 << 7)        // Inactivate on Next Transaction
#define EHCI_QH_EC_ENDPOINT_SHIFT   8               // Endpoint Number
#define EHCI_QH_EC_ENDPOINT_MASK    (0x0F << 8)
#define EHCI_QH_EC_EPS_SHIFT        12              // Endpoint Speed
#define EHCI_QH_EC_EPS_FULL         (0 << 12)
#define EHCI_QH_EC_EPS_LOW          (1 << 12)
#define EHCI_QH_EC_EPS_HIGH         (2 << 12)
#define EHCI_QH_EC_DTC              (1 << 14)       // Data Toggle Control
#define EHCI_QH_EC_H                (1 << 15)       // Head of Reclamation List
#define EHCI_QH_EC_MPL_SHIFT        16              // Maximum Packet Length
#define EHCI_QH_EC_MPL_MASK         (0x7FF << 16)
#define EHCI_QH_EC_C                (1 << 27)       // Control Endpoint Flag
#define EHCI_QH_EC_RL_SHIFT         28              // NAK Count Reload
#define EHCI_QH_EC_RL_MASK          (0x0F << 28)

// Queue Head Endpoint Capabilities
#define EHCI_QH_CAP_SSMASK_MASK     0xFF            // Interrupt Schedule Mask
#define EHCI_QH_CAP_SCMASK_SHIFT    8               // Split Completion Mask
#define EHCI_QH_CAP_SCMASK_MASK     (0xFF << 8)
#define EHCI_QH_CAP_HUB_ADDR_SHIFT  16              // Hub Address
#define EHCI_QH_CAP_HUB_ADDR_MASK   (0x7F << 16)
#define EHCI_QH_CAP_PORT_SHIFT      23              // Port Number
#define EHCI_QH_CAP_PORT_MASK       (0x7F << 23)
#define EHCI_QH_CAP_MULT_SHIFT      30              // High-Bandwidth Pipe Multiplier
#define EHCI_QH_CAP_MULT_MASK       (3 << 30)

// Transfer Descriptor Token
#define EHCI_TD_TOKEN_STATUS_MASK   0xFF            // Status
#define EHCI_TD_TOKEN_STATUS_ACTIVE (1 << 7)        // Active
#define EHCI_TD_TOKEN_STATUS_HALTED (1 << 6)        // Halted
#define EHCI_TD_TOKEN_STATUS_DBUFFER (1 << 5)       // Data Buffer Error
#define EHCI_TD_TOKEN_STATUS_BABBLE (1 << 4)        // Babble Detected
#define EHCI_TD_TOKEN_STATUS_XACT   (1 << 3)        // Transaction Error
#define EHCI_TD_TOKEN_STATUS_MMF    (1 << 2)        // Missed Micro-Frame
#define EHCI_TD_TOKEN_STATUS_SPLIT  (1 << 1)        // Split Transaction State
#define EHCI_TD_TOKEN_STATUS_PING   (1 << 0)        // Ping State/ERR
#define EHCI_TD_TOKEN_PID_SHIFT     8               // PID Code
#define EHCI_TD_TOKEN_PID_OUT       (0 << 8)
#define EHCI_TD_TOKEN_PID_IN        (1 << 8)
#define EHCI_TD_TOKEN_PID_SETUP     (2 << 8)
#define EHCI_TD_TOKEN_CERR_SHIFT    10              // Error Counter
#define EHCI_TD_TOKEN_CERR_MASK     (3 << 10)
#define EHCI_TD_TOKEN_CPAGE_SHIFT   12              // Current Page
#define EHCI_TD_TOKEN_CPAGE_MASK    (7 << 12)
#define EHCI_TD_TOKEN_IOC           (1 << 15)       // Interrupt on Complete
#define EHCI_TD_TOKEN_BYTES_SHIFT   16              // Total Bytes to Transfer
#define EHCI_TD_TOKEN_BYTES_MASK    (0x7FFF << 16)
#define EHCI_TD_TOKEN_DT            (1 << 31)       // Data Toggle

// Queue Head (48 bytes, 32-byte aligned)
typedef struct __attribute__((packed, aligned(32))) {
    uint32 horizontal_link;     // Horizontal Link Pointer
    uint32 endpoint_ch;         // Endpoint Characteristics
    uint32 endpoint_cap;        // Endpoint Capabilities
    uint32 current_td;          // Current TD Address
    // Transfer Overlay (copy of current TD)
    uint32 next_td;             // Next TD Pointer
    uint32 alt_td;              // Alternate Next TD Pointer
    uint32 token;               // Token
    uint32 buffer[5];           // Buffer Pointers
    uint32 ext_buffer[5];       // Extended Buffer Pointers (64-bit addressing)
    // Software fields
    uint32 sw_td_head;          // Software: head of TD list
    uint32 sw_reserved[2];      // Padding
} ehci_qh_t;

// Queue Transfer Descriptor (32 bytes, 32-byte aligned)
typedef struct __attribute__((packed, aligned(32))) {
    uint32 next_td;             // Next TD Pointer
    uint32 alt_td;              // Alternate Next TD Pointer
    uint32 token;               // Token
    uint32 buffer[5];           // Buffer Pointers
    // Extended buffer pointers for 64-bit addressing
    uint32 ext_buffer[5];       // Extended Buffer Pointers
    // Software fields
    uint32 sw_buffer;           // Original buffer address
    uint32 sw_length;           // Transfer length
} ehci_qtd_t;

// EHCI Controller Data
typedef struct {
    volatile uint8* cap_regs;   // Capability registers
    volatile uint32* op_regs;   // Operational registers
    uint8 cap_length;           // Capability registers length
    uint8 num_ports;            // Number of ports
    uint8 num_companions;       // Number of companion controllers
    uint8 ports_per_companion;  // Ports per companion
    bool has_64bit;             // 64-bit addressing capable
    bool has_debug_port;        // Has debug port
    uint8 debug_port;           // Debug port number
    uint8 eecp;                 // Extended capabilities pointer
    void* frame_list_raw;       // Raw allocation for aligned frame list
    uint32* frame_list;         // Periodic frame list
    ehci_qh_t* async_head;      // Asynchronous schedule head
    void* qh_pool_raw;          // Raw allocation for aligned QH pool
    ehci_qh_t* qh_pool;         // Queue head pool
    void* qtd_pool_raw;         // Raw allocation for aligned QTD pool
    ehci_qtd_t* qtd_pool;       // QTD pool
    uint32 qh_bitmap[8];        // QH allocation bitmap
    uint32 qtd_bitmap[32];      // QTD allocation bitmap
} ehci_data_t;

// EHCI Functions
bool ehci_init(usb_controller_t* controller);
void ehci_shutdown(usb_controller_t* controller);
bool ehci_reset_port(usb_controller_t* controller, uint8 port);
bool ehci_enable_port(usb_controller_t* controller, uint8 port);
bool ehci_disable_port(usb_controller_t* controller, uint8 port);
usb_speed_t ehci_get_port_speed(usb_controller_t* controller, uint8 port);
bool ehci_port_connected(usb_controller_t* controller, uint8 port);
int ehci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length);
int ehci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length);
int ehci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length);
void ehci_poll(usb_controller_t* controller);

// EHCI controller operations
extern usb_controller_ops_t ehci_ops;

#endif // EHCI_H
