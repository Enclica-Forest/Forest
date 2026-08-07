/**
 * USB Core Subsystem for Fern
 *
 * Implements USB 1.0/2.0/3.0 support via UHCI, OHCI, EHCI, and xHCI controllers.
 * Based on USB specifications and OSDev documentation.
 */

#ifndef USB_H
#define USB_H

#include "../types.h"
#include "../pci.h"
#include <stdbool.h>
#include <stddef.h>

// USB Class codes
#define USB_CLASS_SERIAL_BUS        0x0C
#define USB_SUBCLASS_USB            0x03

// USB Controller Interface types (prog_if)
#define USB_PROGIF_UHCI             0x00    // Universal Host Controller Interface
#define USB_PROGIF_OHCI             0x10    // Open Host Controller Interface
#define USB_PROGIF_EHCI             0x20    // Enhanced Host Controller Interface (USB 2.0)
#define USB_PROGIF_XHCI             0x30    // eXtensible Host Controller Interface (USB 3.0)

// USB speeds
typedef enum {
    USB_SPEED_LOW       = 0,    // 1.5 Mbps (USB 1.0)
    USB_SPEED_FULL      = 1,    // 12 Mbps (USB 1.1)
    USB_SPEED_HIGH      = 2,    // 480 Mbps (USB 2.0)
    USB_SPEED_SUPER     = 3,    // 5 Gbps (USB 3.0)
    USB_SPEED_SUPER_PLUS = 4    // 10 Gbps (USB 3.1)
} usb_speed_t;

// USB transfer types
typedef enum {
    USB_TRANSFER_CONTROL    = 0,
    USB_TRANSFER_ISOCHRONOUS = 1,
    USB_TRANSFER_BULK       = 2,
    USB_TRANSFER_INTERRUPT  = 3
} usb_transfer_type_t;

// USB direction
typedef enum {
    USB_DIR_OUT = 0,    // Host to device
    USB_DIR_IN  = 1     // Device to host
} usb_direction_t;

// USB request types (bmRequestType)
#define USB_REQTYPE_DIR_OUT         0x00
#define USB_REQTYPE_DIR_IN          0x80
#define USB_REQTYPE_TYPE_STANDARD   0x00
#define USB_REQTYPE_TYPE_CLASS      0x20
#define USB_REQTYPE_TYPE_VENDOR     0x40
#define USB_REQTYPE_RECIP_DEVICE    0x00
#define USB_REQTYPE_RECIP_INTERFACE 0x01
#define USB_REQTYPE_RECIP_ENDPOINT  0x02
#define USB_REQTYPE_RECIP_OTHER     0x03

// Standard USB requests (bRequest)
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B
#define USB_REQ_SYNCH_FRAME         0x0C

// Descriptor types
#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIGURATION      0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_INTERFACE          0x04
#define USB_DESC_ENDPOINT           0x05
#define USB_DESC_DEVICE_QUALIFIER   0x06
#define USB_DESC_OTHER_SPEED_CONFIG 0x07
#define USB_DESC_INTERFACE_POWER    0x08
#define USB_DESC_HID                0x21
#define USB_DESC_HID_REPORT         0x22
#define USB_DESC_HID_PHYSICAL       0x23
#define USB_DESC_HUB                0x29

// USB Device classes
#define USB_CLASS_PER_INTERFACE     0x00
#define USB_CLASS_AUDIO             0x01
#define USB_CLASS_COMM              0x02
#define USB_CLASS_HID               0x03
#define USB_CLASS_PHYSICAL          0x05
#define USB_CLASS_IMAGE             0x06
#define USB_CLASS_PRINTER           0x07
#define USB_CLASS_MASS_STORAGE      0x08
#define USB_CLASS_HUB               0x09
#define USB_CLASS_CDC_DATA          0x0A
#define USB_CLASS_SMART_CARD        0x0B
#define USB_CLASS_CONTENT_SECURITY  0x0D
#define USB_CLASS_VIDEO             0x0E
#define USB_CLASS_HEALTHCARE        0x0F
#define USB_CLASS_DIAGNOSTIC        0xDC
#define USB_CLASS_WIRELESS          0xE0
#define USB_CLASS_MISC              0xEF
#define USB_CLASS_APP_SPECIFIC      0xFE
#define USB_CLASS_VENDOR_SPECIFIC   0xFF

// Mass Storage subclasses
#define USB_MSC_SUBCLASS_SCSI       0x06
#define USB_MSC_PROTOCOL_BBB        0x50    // Bulk-Only (BBB)

// HID subclasses
#define USB_HID_SUBCLASS_NONE       0x00
#define USB_HID_SUBCLASS_BOOT       0x01

// HID protocols
#define USB_HID_PROTOCOL_NONE       0x00
#define USB_HID_PROTOCOL_KEYBOARD   0x01
#define USB_HID_PROTOCOL_MOUSE      0x02

// USB Setup Packet
typedef struct __attribute__((packed)) {
    uint8  bmRequestType;
    uint8  bRequest;
    uint16 wValue;
    uint16 wIndex;
    uint16 wLength;
} usb_setup_packet_t;

// USB Device Descriptor
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint16 bcdUSB;
    uint8  bDeviceClass;
    uint8  bDeviceSubClass;
    uint8  bDeviceProtocol;
    uint8  bMaxPacketSize0;
    uint16 idVendor;
    uint16 idProduct;
    uint16 bcdDevice;
    uint8  iManufacturer;
    uint8  iProduct;
    uint8  iSerialNumber;
    uint8  bNumConfigurations;
} usb_device_descriptor_t;

// USB Configuration Descriptor
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint16 wTotalLength;
    uint8  bNumInterfaces;
    uint8  bConfigurationValue;
    uint8  iConfiguration;
    uint8  bmAttributes;
    uint8  bMaxPower;
} usb_config_descriptor_t;

// USB Interface Descriptor
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint8  bInterfaceNumber;
    uint8  bAlternateSetting;
    uint8  bNumEndpoints;
    uint8  bInterfaceClass;
    uint8  bInterfaceSubClass;
    uint8  bInterfaceProtocol;
    uint8  iInterface;
} usb_interface_descriptor_t;

// USB Endpoint Descriptor
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint8  bEndpointAddress;
    uint8  bmAttributes;
    uint16 wMaxPacketSize;
    uint8  bInterval;
} usb_endpoint_descriptor_t;

// USB String Descriptor (header)
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint16 wString[1];  // Variable length UTF-16 string
} usb_string_descriptor_t;

// USB HID Descriptor
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint16 bcdHID;
    uint8  bCountryCode;
    uint8  bNumDescriptors;
    uint8  bReportDescriptorType;
    uint16 wReportDescriptorLength;
} usb_hid_descriptor_t;

// USB Hub Descriptor
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;
    uint8  bNbrPorts;
    uint16 wHubCharacteristics;
    uint8  bPwrOn2PwrGood;
    uint8  bHubContrCurrent;
    uint8  DeviceRemovable[1];  // Variable length bitmap
} usb_hub_descriptor_t;

// Forward declarations
struct usb_device;
struct usb_controller;
struct usb_endpoint;

// USB Endpoint
typedef struct usb_endpoint {
    uint8  address;
    uint8  attributes;
    uint16 max_packet_size;
    uint8  interval;
    usb_transfer_type_t type;
    usb_direction_t direction;
    void*  hcd_data;    // Host controller specific data
} usb_endpoint_t;

// USB Interface
typedef struct usb_interface {
    uint8  number;
    uint8  class_code;
    uint8  subclass;
    uint8  protocol;
    uint8  num_endpoints;
    usb_endpoint_t* endpoints;
    void*  driver_data;     // Class driver data
} usb_interface_t;

// USB Configuration
typedef struct usb_configuration {
    uint8  value;
    uint8  num_interfaces;
    uint8  attributes;
    uint8  max_power;
    usb_interface_t* interfaces;
} usb_configuration_t;

// USB Device
typedef struct usb_device {
    uint8  address;
    usb_speed_t speed;
    uint16 vendor_id;
    uint16 product_id;
    uint8  device_class;
    uint8  device_subclass;
    uint8  device_protocol;
    uint8  max_packet_size0;    // Control endpoint max packet
    uint8  num_configurations;
    usb_configuration_t* configurations;
    usb_configuration_t* active_config;
    struct usb_controller* controller;
    struct usb_device* parent;  // Parent hub
    uint8  port;                // Port on parent hub
    void*  hcd_data;            // Host controller specific data
    bool   configured;
    char   manufacturer[64];
    char   product[64];
    char   serial[64];
} usb_device_t;

// USB Transfer Request
typedef struct usb_transfer {
    usb_device_t*   device;
    usb_endpoint_t* endpoint;
    usb_setup_packet_t* setup;  // For control transfers
    void*           buffer;
    uint32          length;
    uint32          actual_length;
    int             status;
    bool            complete;
    void            (*callback)(struct usb_transfer* transfer);
    void*           context;
} usb_transfer_t;

// USB Controller operations
typedef struct usb_controller_ops {
    bool (*init)(struct usb_controller* controller);
    void (*shutdown)(struct usb_controller* controller);
    bool (*reset_port)(struct usb_controller* controller, uint8 port);
    bool (*enable_port)(struct usb_controller* controller, uint8 port);
    bool (*disable_port)(struct usb_controller* controller, uint8 port);
    usb_speed_t (*get_port_speed)(struct usb_controller* controller, uint8 port);
    bool (*port_connected)(struct usb_controller* controller, uint8 port);
    int  (*control_transfer)(struct usb_controller* controller, usb_device_t* device,
                             usb_setup_packet_t* setup, void* data, uint16 length);
    int  (*bulk_transfer)(struct usb_controller* controller, usb_device_t* device,
                          usb_endpoint_t* endpoint, void* data, uint32 length);
    int  (*interrupt_transfer)(struct usb_controller* controller, usb_device_t* device,
                               usb_endpoint_t* endpoint, void* data, uint32 length);
    void (*poll)(struct usb_controller* controller);
} usb_controller_ops_t;

// USB Controller types
typedef enum {
    USB_CONTROLLER_UHCI,
    USB_CONTROLLER_OHCI,
    USB_CONTROLLER_EHCI,
    USB_CONTROLLER_XHCI
} usb_controller_type_t;

// USB Controller
typedef struct usb_controller {
    usb_controller_type_t type;
    pci_device_t pci_device;
    uint32 base_address;        // BAR address (I/O or MMIO)
    bool   is_mmio;             // True if MMIO, false if I/O
    uint8  num_ports;
    usb_device_t* devices[128]; // Up to 127 devices + address 0
    usb_controller_ops_t* ops;
    void*  hcd_data;            // Host controller specific data
    bool   initialized;
    struct usb_controller* next;
} usb_controller_t;

// USB Class Driver
typedef struct usb_class_driver {
    const char* name;
    uint8 class_code;
    uint8 subclass;
    uint8 protocol;
    bool (*probe)(usb_device_t* device, usb_interface_t* interface);
    void (*disconnect)(usb_device_t* device, usb_interface_t* interface);
    struct usb_class_driver* next;
} usb_class_driver_t;

// USB Core Functions
bool usb_init(void);
void usb_shutdown(void);
void usb_poll(void);

// Controller management
bool usb_register_controller(usb_controller_t* controller);
void usb_unregister_controller(usb_controller_t* controller);
usb_controller_t* usb_find_controller(usb_controller_type_t type);

// Device management
usb_device_t* usb_alloc_device(usb_controller_t* controller);
void usb_free_device(usb_device_t* device);
bool usb_enumerate_device(usb_device_t* device);
bool usb_configure_device(usb_device_t* device, uint8 config);
uint8 usb_alloc_address(usb_controller_t* controller);

// Transfers
int usb_control_msg(usb_device_t* device, uint8 request_type, uint8 request,
                    uint16 value, uint16 index, void* data, uint16 length);
int usb_get_descriptor(usb_device_t* device, uint8 type, uint8 index,
                       void* buffer, uint16 length);
int usb_set_address(usb_device_t* device, uint8 address);
int usb_set_configuration(usb_device_t* device, uint8 config);

// Class driver management
bool usb_register_class_driver(usb_class_driver_t* driver);
void usb_unregister_class_driver(usb_class_driver_t* driver);

// Utility functions
const char* usb_speed_string(usb_speed_t speed);
const char* usb_class_string(uint8 class_code);

#endif // USB_H
