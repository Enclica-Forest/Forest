/**
 * xHCI (eXtensible Host Controller Interface) Driver for Fern
 *
 * Implements USB 3.0/3.1 support via the xHCI controller specification.
 * Based on Intel xHCI 1.2 specification and OSDev documentation.
 */

#ifndef XHCI_H
#define XHCI_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// xHCI Capability Registers (offsets from BAR0)
#define XHCI_CAP_CAPLENGTH          0x00    // Capability Register Length (1 byte)
#define XHCI_CAP_HCIVERSION         0x02    // Interface Version Number (2 bytes)
#define XHCI_CAP_HCSPARAMS1         0x04    // Structural Parameters 1 (4 bytes)
#define XHCI_CAP_HCSPARAMS2         0x08    // Structural Parameters 2 (4 bytes)
#define XHCI_CAP_HCSPARAMS3         0x0C    // Structural Parameters 3 (4 bytes)
#define XHCI_CAP_HCCPARAMS1         0x10    // Capability Parameters 1 (4 bytes)
#define XHCI_CAP_DBOFF              0x14    // Doorbell Offset (4 bytes)
#define XHCI_CAP_RTSOFF             0x18    // Runtime Register Space Offset (4 bytes)
#define XHCI_CAP_HCCPARAMS2         0x1C    // Capability Parameters 2 (4 bytes)

// xHCI Operational Registers (offsets from BAR0 + CAPLENGTH)
#define XHCI_OP_USBCMD              0x00    // USB Command (4 bytes)
#define XHCI_OP_USBSTS              0x04    // USB Status (4 bytes)
#define XHCI_OP_PAGESIZE            0x08    // Page Size (4 bytes)
#define XHCI_OP_DNCTRL              0x14    // Device Notification Control (4 bytes)
#define XHCI_OP_CRCR                0x18    // Command Ring Control (8 bytes)
#define XHCI_OP_DCBAAP              0x30    // Device Context Base Address Array Pointer (8 bytes)
#define XHCI_OP_CONFIG              0x38    // Configure (4 bytes)
#define XHCI_OP_PORTSC              0x400   // Port Status/Control (4 bytes each)

// HCSPARAMS1 bits
#define XHCI_HCS1_MAXSLOTS_MASK     0xFF        // Max Device Slots
#define XHCI_HCS1_MAXINTRS_SHIFT    8           // Max Interrupters
#define XHCI_HCS1_MAXINTRS_MASK     (0x7FF << 8)
#define XHCI_HCS1_MAXPORTS_SHIFT    24          // Max Ports
#define XHCI_HCS1_MAXPORTS_MASK     (0xFF << 24)

// HCSPARAMS2 bits
#define XHCI_HCS2_IST_MASK          0x0F        // Isochronous Scheduling Threshold
#define XHCI_HCS2_ERST_MAX_SHIFT    4           // Event Ring Segment Table Max
#define XHCI_HCS2_ERST_MAX_MASK     (0x0F << 4)
#define XHCI_HCS2_SPR               (1 << 26)   // Scratchpad Restore
#define XHCI_HCS2_SPB_MAX_SHIFT     27          // Max Scratchpad Buffers
#define XHCI_HCS2_SPB_MAX_MASK      (0x1F << 27)

// HCCPARAMS1 bits
#define XHCI_HCC1_AC64              (1 << 0)    // 64-bit Addressing Capable
#define XHCI_HCC1_BNC               (1 << 1)    // BW Negotiation Capable
#define XHCI_HCC1_CSZ               (1 << 2)    // Context Size (0=32, 1=64)
#define XHCI_HCC1_PPC               (1 << 3)    // Port Power Control
#define XHCI_HCC1_PIND              (1 << 4)    // Port Indicators
#define XHCI_HCC1_LHRC              (1 << 5)    // Light HC Reset Capable
#define XHCI_HCC1_LTC               (1 << 6)    // Latency Tolerance Messaging Capable
#define XHCI_HCC1_NSS               (1 << 7)    // No Secondary SID Support
#define XHCI_HCC1_PAE               (1 << 8)    // Parse All Event Data
#define XHCI_HCC1_SPC               (1 << 9)    // Stopped - Short Packet Capable
#define XHCI_HCC1_SEC               (1 << 10)   // Stopped EDTLA Capable
#define XHCI_HCC1_CFC               (1 << 11)   // Contiguous Frame ID Capable
#define XHCI_HCC1_MAXPSASIZE_SHIFT  12          // Max Primary Stream Array Size
#define XHCI_HCC1_MAXPSASIZE_MASK   (0x0F << 12)
#define XHCI_HCC1_XECP_SHIFT        16          // xHCI Extended Capabilities Pointer
#define XHCI_HCC1_XECP_MASK         (0xFFFF << 16)

// USB Command Register bits
#define XHCI_CMD_RS                 (1 << 0)    // Run/Stop
#define XHCI_CMD_HCRST              (1 << 1)    // Host Controller Reset
#define XHCI_CMD_INTE               (1 << 2)    // Interrupter Enable
#define XHCI_CMD_HSEE               (1 << 3)    // Host System Error Enable
#define XHCI_CMD_LHCRST             (1 << 7)    // Light Host Controller Reset
#define XHCI_CMD_CSS                (1 << 8)    // Controller Save State
#define XHCI_CMD_CRS                (1 << 9)    // Controller Restore State
#define XHCI_CMD_EWE                (1 << 10)   // Enable Wrap Event
#define XHCI_CMD_EU3S               (1 << 11)   // Enable U3 MFINDEX Stop
#define XHCI_CMD_CME                (1 << 13)   // CEM Enable
#define XHCI_CMD_ETE                (1 << 14)   // Extended TBC Enable
#define XHCI_CMD_TSC_EN             (1 << 15)   // Extended TBC TRB Status Enable
#define XHCI_CMD_VTIOE              (1 << 16)   // VTIO Enable

// USB Status Register bits
#define XHCI_STS_HCH                (1 << 0)    // HC Halted
#define XHCI_STS_HSE                (1 << 2)    // Host System Error
#define XHCI_STS_EINT               (1 << 3)    // Event Interrupt
#define XHCI_STS_PCD                (1 << 4)    // Port Change Detect
#define XHCI_STS_SSS                (1 << 8)    // Save State Status
#define XHCI_STS_RSS                (1 << 9)    // Restore State Status
#define XHCI_STS_SRE                (1 << 10)   // Save/Restore Error
#define XHCI_STS_CNR                (1 << 11)   // Controller Not Ready
#define XHCI_STS_HCE                (1 << 12)   // Host Controller Error

// Port Status/Control Register bits
#define XHCI_PORT_CCS               (1 << 0)    // Current Connect Status
#define XHCI_PORT_PED               (1 << 1)    // Port Enabled/Disabled
#define XHCI_PORT_OCA               (1 << 3)    // Over-current Active
#define XHCI_PORT_PR                (1 << 4)    // Port Reset
#define XHCI_PORT_PLS_SHIFT         5           // Port Link State
#define XHCI_PORT_PLS_MASK          (0x0F << 5)
#define XHCI_PORT_PP                (1 << 9)    // Port Power
#define XHCI_PORT_SPEED_SHIFT       10          // Port Speed
#define XHCI_PORT_SPEED_MASK        (0x0F << 10)
#define XHCI_PORT_PIC_SHIFT         14          // Port Indicator Control
#define XHCI_PORT_PIC_MASK          (3 << 14)
#define XHCI_PORT_LWS               (1 << 16)   // Port Link State Write Strobe
#define XHCI_PORT_CSC               (1 << 17)   // Connect Status Change
#define XHCI_PORT_PEC               (1 << 18)   // Port Enabled/Disabled Change
#define XHCI_PORT_WRC               (1 << 19)   // Warm Port Reset Change
#define XHCI_PORT_OCC               (1 << 20)   // Over-current Change
#define XHCI_PORT_PRC               (1 << 21)   // Port Reset Change
#define XHCI_PORT_PLC               (1 << 22)   // Port Link State Change
#define XHCI_PORT_CEC               (1 << 23)   // Port Config Error Change
#define XHCI_PORT_CAS               (1 << 24)   // Cold Attach Status
#define XHCI_PORT_WCE               (1 << 25)   // Wake on Connect Enable
#define XHCI_PORT_WDE               (1 << 26)   // Wake on Disconnect Enable
#define XHCI_PORT_WOE               (1 << 27)   // Wake on Over-current Enable
#define XHCI_PORT_DR                (1 << 30)   // Device Removable
#define XHCI_PORT_WPR               (1 << 31)   // Warm Port Reset

// Port Link States
#define XHCI_PLS_U0                 0           // USB 3.0 Link State: On
#define XHCI_PLS_U1                 1           // USB 3.0 Link State: Standby
#define XHCI_PLS_U2                 2           // USB 3.0 Link State: Sleep
#define XHCI_PLS_U3                 3           // USB 3.0 Link State: Suspend
#define XHCI_PLS_DISABLED           4           // Disabled
#define XHCI_PLS_RXDETECT           5           // RxDetect
#define XHCI_PLS_INACTIVE           6           // Inactive
#define XHCI_PLS_POLLING            7           // Polling
#define XHCI_PLS_RECOVERY           8           // Recovery
#define XHCI_PLS_HOTRESET           9           // Hot Reset
#define XHCI_PLS_COMPLIANCE         10          // Compliance Mode
#define XHCI_PLS_TESTMODE           11          // Test Mode
#define XHCI_PLS_RESUME             15          // Resume

// Port Speeds
#define XHCI_SPEED_FULL             1           // Full Speed (12 Mbps)
#define XHCI_SPEED_LOW              2           // Low Speed (1.5 Mbps)
#define XHCI_SPEED_HIGH             3           // High Speed (480 Mbps)
#define XHCI_SPEED_SUPER            4           // SuperSpeed (5 Gbps)
#define XHCI_SPEED_SUPER_PLUS       5           // SuperSpeed+ (10 Gbps)

// Runtime Registers (offsets from BAR0 + RTSOFF)
#define XHCI_RT_MFINDEX             0x00        // Microframe Index
#define XHCI_RT_IR0                 0x20        // Interrupter Register Set 0

// Interrupter Register Set (32 bytes)
#define XHCI_IR_IMAN                0x00        // Interrupter Management
#define XHCI_IR_IMOD                0x04        // Interrupter Moderation
#define XHCI_IR_ERSTSZ              0x08        // Event Ring Segment Table Size
#define XHCI_IR_ERSTBA              0x10        // Event Ring Segment Table Base Address
#define XHCI_IR_ERDP                0x18        // Event Ring Dequeue Pointer

// Interrupter Management bits
#define XHCI_IMAN_IP                (1 << 0)    // Interrupt Pending
#define XHCI_IMAN_IE                (1 << 1)    // Interrupt Enable

// Doorbell Register (offset DBOFF from BAR0)
// Doorbell 0 = Host Controller
// Doorbell 1-MaxSlots = Device Slots

// TRB Types
#define XHCI_TRB_NORMAL             1
#define XHCI_TRB_SETUP              2
#define XHCI_TRB_DATA               3
#define XHCI_TRB_STATUS             4
#define XHCI_TRB_ISOCH              5
#define XHCI_TRB_LINK               6
#define XHCI_TRB_EVENT_DATA         7
#define XHCI_TRB_NOOP               8
#define XHCI_TRB_ENABLE_SLOT        9
#define XHCI_TRB_DISABLE_SLOT       10
#define XHCI_TRB_ADDRESS_DEVICE     11
#define XHCI_TRB_CONFIGURE_EP       12
#define XHCI_TRB_EVALUATE_CTX       13
#define XHCI_TRB_RESET_EP           14
#define XHCI_TRB_STOP_EP            15
#define XHCI_TRB_SET_TR_DEQUEUE     16
#define XHCI_TRB_RESET_DEVICE       17
#define XHCI_TRB_FORCE_EVENT        18
#define XHCI_TRB_NEG_BANDWIDTH      19
#define XHCI_TRB_SET_LAT_TOL        20
#define XHCI_TRB_GET_PORT_BW        21
#define XHCI_TRB_FORCE_HEADER       22
#define XHCI_TRB_NOOP_CMD           23
// Event TRB Types
#define XHCI_TRB_TRANSFER_EVENT     32
#define XHCI_TRB_CMD_COMPLETION     33
#define XHCI_TRB_PORT_STATUS_CHANGE 34
#define XHCI_TRB_BANDWIDTH_REQ      35
#define XHCI_TRB_DOORBELL           36
#define XHCI_TRB_HOST_CONTROLLER    37
#define XHCI_TRB_DEVICE_NOTIFICATION 38
#define XHCI_TRB_MFINDEX_WRAP       39

// TRB Completion Codes
#define XHCI_CC_INVALID             0
#define XHCI_CC_SUCCESS             1
#define XHCI_CC_DATA_BUFFER_ERROR   2
#define XHCI_CC_BABBLE_DETECTED     3
#define XHCI_CC_USB_TRANSACTION_ERROR 4
#define XHCI_CC_TRB_ERROR           5
#define XHCI_CC_STALL_ERROR         6
#define XHCI_CC_RESOURCE_ERROR      7
#define XHCI_CC_BANDWIDTH_ERROR     8
#define XHCI_CC_NO_SLOTS_AVAILABLE  9
#define XHCI_CC_INVALID_STREAM_TYPE 10
#define XHCI_CC_SLOT_NOT_ENABLED    11
#define XHCI_CC_EP_NOT_ENABLED      12
#define XHCI_CC_SHORT_PACKET        13
#define XHCI_CC_RING_UNDERRUN       14
#define XHCI_CC_RING_OVERRUN        15
#define XHCI_CC_VF_EVENT_RING_FULL  16
#define XHCI_CC_PARAMETER_ERROR     17
#define XHCI_CC_BANDWIDTH_OVERRUN   18
#define XHCI_CC_CONTEXT_STATE_ERROR 19
#define XHCI_CC_NO_PING_RESPONSE    20
#define XHCI_CC_EVENT_RING_FULL     21
#define XHCI_CC_INCOMPATIBLE_DEVICE 22
#define XHCI_CC_MISSED_SERVICE      23
#define XHCI_CC_COMMAND_RING_STOPPED 24
#define XHCI_CC_COMMAND_ABORTED     25
#define XHCI_CC_STOPPED             26
#define XHCI_CC_STOPPED_LENGTH_INVALID 27
#define XHCI_CC_STOPPED_SHORT_PACKET 28
#define XHCI_CC_MAX_EXIT_LATENCY_TOO_LARGE 29
#define XHCI_CC_ISOCH_BUFFER_OVERRUN 31
#define XHCI_CC_EVENT_LOST          32
#define XHCI_CC_UNDEFINED_ERROR     33
#define XHCI_CC_INVALID_STREAM_ID   34
#define XHCI_CC_SECONDARY_BANDWIDTH 35
#define XHCI_CC_SPLIT_TRANSACTION   36

// Transfer Request Block (16 bytes)
typedef struct __attribute__((packed, aligned(16))) {
    uint64 parameter;           // Parameter (varies by TRB type)
    uint32 status;              // Status (transfer length, etc.)
    uint32 control;             // Control (TRB type, flags, etc.)
} xhci_trb_t;

// TRB control field bits
#define XHCI_TRB_CTRL_C             (1 << 0)    // Cycle bit
#define XHCI_TRB_CTRL_ENT           (1 << 1)    // Evaluate Next TRB
#define XHCI_TRB_CTRL_ISP           (1 << 2)    // Interrupt on Short Packet
#define XHCI_TRB_CTRL_NS            (1 << 3)    // No Snoop
#define XHCI_TRB_CTRL_CH            (1 << 4)    // Chain bit
#define XHCI_TRB_CTRL_IOC           (1 << 5)    // Interrupt on Complete
#define XHCI_TRB_CTRL_IDT           (1 << 6)    // Immediate Data
#define XHCI_TRB_CTRL_BEI           (1 << 9)    // Block Event Interrupt
#define XHCI_TRB_CTRL_TYPE_SHIFT    10          // TRB Type
#define XHCI_TRB_CTRL_TYPE_MASK     (0x3F << 10)
#define XHCI_TRB_CTRL_TRT_SHIFT     16          // Transfer Type (Setup TRB)
#define XHCI_TRB_CTRL_TRT_NO_DATA   (0 << 16)
#define XHCI_TRB_CTRL_TRT_OUT_DATA  (2 << 16)
#define XHCI_TRB_CTRL_TRT_IN_DATA   (3 << 16)
#define XHCI_TRB_CTRL_DIR           (1 << 16)   // Direction (Data/Status TRB)
#define XHCI_TRB_CTRL_SLOT_SHIFT    24          // Slot ID
#define XHCI_TRB_CTRL_EP_SHIFT      16          // Endpoint ID

// Event Ring Segment Table Entry
typedef struct __attribute__((packed)) {
    uint64 ring_segment_base;   // Ring Segment Base Address
    uint32 ring_segment_size;   // Ring Segment Size (entries)
    uint32 reserved;
} xhci_erst_entry_t;

// Slot Context (32 or 64 bytes depending on CSZ)
typedef struct __attribute__((packed)) {
    uint32 route_string_and_speed;  // Route String, Speed, etc.
    uint32 latency_and_slots;       // Max Exit Latency, Hub Slot ID, etc.
    uint32 port_and_state;          // Root Hub Port Number, Context State, etc.
    uint32 device_address;          // Device Address
    uint32 reserved[4];
} xhci_slot_context_t;

// Endpoint Context (32 or 64 bytes depending on CSZ)
typedef struct __attribute__((packed)) {
    uint32 ep_info1;                // EP State, Mult, MaxPStreams, etc.
    uint32 ep_info2;                // Max Packet Size, Max Burst Size, etc.
    uint64 tr_dequeue_ptr;          // TR Dequeue Pointer
    uint32 average_trb_length;      // Average TRB Length
    uint32 max_esit_payload;        // Max ESIT Payload (Lo/Hi)
    uint32 reserved[2];
} xhci_endpoint_context_t;

// Device Context (contains Slot Context + 31 Endpoint Contexts)
typedef struct __attribute__((packed)) {
    xhci_slot_context_t slot;
    xhci_endpoint_context_t endpoints[31];
} xhci_device_context_t;

// Input Control Context
typedef struct __attribute__((packed)) {
    uint32 drop_flags;
    uint32 add_flags;
    uint32 reserved[6];
} xhci_input_control_context_t;

// xHCI Controller Data
typedef struct {
    volatile uint8* cap_regs;       // Capability registers
    volatile uint32* op_regs;       // Operational registers
    volatile uint32* doorbell_regs; // Doorbell registers
    volatile uint32* runtime_regs;  // Runtime registers
    uint8 cap_length;               // Capability registers length
    uint8 max_slots;                // Max device slots
    uint16 max_interrupters;        // Max interrupters
    uint8 max_ports;                // Max ports
    uint8 context_size;             // Context size (32 or 64)
    bool has_64bit;                 // 64-bit addressing capable
    uint32 page_size;               // Page size
    uint16 xecp;                    // Extended capabilities pointer
    // Memory structures
    uint64* dcbaa;                  // Device Context Base Address Array
    xhci_trb_t* cmd_ring;           // Command ring
    uint32 cmd_ring_enqueue;        // Command ring enqueue index
    bool cmd_ring_cycle;            // Command ring cycle state
    xhci_erst_entry_t* erst;        // Event Ring Segment Table
    xhci_trb_t* event_ring;         // Event ring
    uint32 event_ring_dequeue;      // Event ring dequeue index
    bool event_ring_cycle;          // Event ring cycle state
    xhci_device_context_t** device_contexts;  // Device contexts array
    uint64* scratchpad_buffers;     // Scratchpad buffer array
    // Allocation pools
    xhci_trb_t* transfer_ring_pool; // Transfer ring pool
    uint32 transfer_ring_bitmap[8]; // Allocation bitmap
} xhci_data_t;

// xHCI Functions
bool xhci_init(usb_controller_t* controller);
void xhci_shutdown(usb_controller_t* controller);
bool xhci_reset_port(usb_controller_t* controller, uint8 port);
bool xhci_enable_port(usb_controller_t* controller, uint8 port);
bool xhci_disable_port(usb_controller_t* controller, uint8 port);
usb_speed_t xhci_get_port_speed(usb_controller_t* controller, uint8 port);
bool xhci_port_connected(usb_controller_t* controller, uint8 port);
int xhci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length);
int xhci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length);
int xhci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length);
void xhci_poll(usb_controller_t* controller);

// xHCI controller operations
extern usb_controller_ops_t xhci_ops;

#endif // XHCI_H
