#ifndef USB_H
#define USB_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define USB_MAX_DEVICES 128
#define USB_MAX_ENDPOINTS 32

typedef enum {
    USB_SPEED_LOW = 0,
    USB_SPEED_FULL = 1,
    USB_SPEED_HIGH = 2,
    USB_SPEED_SUPER = 3
} usb_speed_t;

typedef enum {
    USB_DIRECTION_OUT = 0,
    USB_DIRECTION_IN = 1
} usb_direction_t;

typedef enum {
    USB_CONTROL_ENDPOINT = 0,
    USB_ISOCHRONOUS_ENDPOINT = 1,
    USB_BULK_ENDPOINT = 2,
    USB_INTERRUPT_ENDPOINT = 3
} usb_transfer_type_t;

typedef enum {
    USB_SETUP_REQUEST_GET_STATUS = 0,
    USB_SETUP_REQUEST_CLEAR_FEATURE = 1,
    USB_SETUP_REQUEST_SET_FEATURE = 3,
    USB_SETUP_REQUEST_SET_ADDRESS = 5,
    USB_SETUP_REQUEST_GET_DESCRIPTOR = 6,
    USB_SETUP_REQUEST_SET_DESCRIPTOR = 7,
    USB_SETUP_REQUEST_GET_CONFIGURATION = 8,
    USB_SETUP_REQUEST_SET_CONFIGURATION = 9,
    USB_SETUP_REQUEST_GET_INTERFACE = 10,
    USB_SETUP_REQUEST_SET_INTERFACE = 11,
    USB_SETUP_REQUEST_SYNCH_FRAME = 12
} usb_setup_request_t;

typedef enum {
    USB_DESCRIPTOR_DEVICE = 1,
    USB_DESCRIPTOR_CONFIGURATION = 2,
    USB_DESCRIPTOR_STRING = 3,
    USB_DESCRIPTOR_INTERFACE = 4,
    USB_DESCRIPTOR_ENDPOINT = 5,
    USB_DESCRIPTOR_DEVICE_QUALIFIER = 6,
    USB_DESCRIPTOR_OTHER_SPEED_CONFIGURATION = 7,
    USB_DESCRIPTOR_INTERFACE_POWER = 8,
    USB_DESCRIPTOR_HID = 0x21,
    USB_DESCRIPTOR_REPORT = 0x22
} usb_descriptor_type_t;

typedef enum {
    USB_REQUEST_TYPE_STANDARD = (0 << 5),
    USB_REQUEST_TYPE_CLASS = (1 << 5),
    USB_REQUEST_TYPE_VENDOR = (2 << 5),
    USB_REQUEST_TYPE_RESERVED = (3 << 5)
} usb_request_type_t;

typedef enum {
    USB_RECIPIENT_DEVICE = 0,
    USB_RECIPIENT_INTERFACE = 1,
    USB_RECIPIENT_ENDPOINT = 2,
    USB_RECIPIENT_OTHER = 3
} usb_recipient_t;

typedef struct {
    uint8 bmRequestType;
    uint8 bRequest;
    uint16 wValue;
    uint16 wIndex;
    uint16 wLength;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    uint8 bLength;
    uint8 bDescriptorType;
    uint16 bcdUSB;
    uint8 bDeviceClass;
    uint8 bDeviceSubClass;
    uint8 bDeviceProtocol;
    uint8 bMaxPacketSize0;
    uint16 idVendor;
    uint16 idProduct;
    uint16 bcdDevice;
    uint8 iManufacturer;
    uint8 iProduct;
    uint8 iSerialNumber;
    uint8 bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8 bLength;
    uint8 bDescriptorType;
    uint16 wTotalLength;
    uint8 bNumInterfaces;
    uint8 bConfigurationValue;
    uint8 iConfiguration;
    uint8 bmAttributes;
    uint8 MaxPower;
} __attribute__((packed)) usb_configuration_descriptor_t;

typedef struct {
    uint8 bLength;
    uint8 bDescriptorType;
    uint8 bInterfaceNumber;
    uint8 bAlternateSetting;
    uint8 bNumEndpoints;
    uint8 bInterfaceClass;
    uint8 bInterfaceSubClass;
    uint8 bInterfaceProtocol;
    uint8 iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8 bLength;
    uint8 bDescriptorType;
    uint8 bEndpointAddress;
    uint8 bmAttributes;
    uint16 wMaxPacketSize;
    uint8 bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

typedef struct {
    uint8 bLength;
    uint8 bDescriptorType;
    uint16 bcdHID;
    uint8 bCountryCode;
    uint8 bNumDescriptors;
    uint8 bDescriptorType1;
    uint16 wDescriptorLength;
} __attribute__((packed)) usb_hid_descriptor_t;

typedef struct usb_endpoint usb_endpoint_t;
typedef struct usb_device usb_device_t;

struct usb_endpoint {
    uint8 number;
    usb_direction_t direction;
    usb_transfer_type_t type;
    uint16 max_packet_size;
    uint8 interval;
    usb_device_t* device;
};

struct usb_device {
    uint8 address;
    uint8 port;
    usb_speed_t speed;
    uint16 vendor_id;
    uint16 product_id;
    uint8 num_configurations;
    usb_device_descriptor_t* descriptor;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    struct usb_device* parent;
    bool initialized;
};

typedef enum {
    USB_HC_TYPE_UHCI,
    USB_HC_TYPE_OHCI,
    USB_HC_TYPE_EHCI,
    USB_HC_TYPE_XHCI
} usb_hc_type_t;

typedef struct usb_host_controller usb_host_controller_t;

struct usb_host_controller {
    usb_hc_type_t type;
    uint16 vendor_id;
    uint16 device_id;
    uintptr_t base_address;
    uint8 irq;
    void* private_data;
    
    bool (*init)(usb_host_controller_t* hc);
    void (*shutdown)(usb_host_controller_t* hc);
    int (*control_transfer)(usb_host_controller_t* hc, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint32 length);
    int (*bulk_transfer)(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                        void* data, uint32 length, usb_direction_t direction);
    int (*interrupt_transfer)(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                            void* data, uint32 length, usb_direction_t direction);
};

bool usb_init(void);
void usb_shutdown(void);
bool usb_register_host_controller(usb_host_controller_t* hc);
usb_device_t* usb_allocate_device(uint8 port, usb_speed_t speed);
void usb_free_device(usb_device_t* device);
int usb_control_transfer(usb_device_t* device, usb_setup_packet_t* setup,
                        void* data, uint32 length);
int usb_get_descriptor(usb_device_t* device, uint8 type, uint8 index,
                     void* buffer, uint32 length);
int usb_set_address(usb_device_t* device, uint8 address);
int usb_set_configuration(usb_device_t* device, uint8 config);

/* Accessors used by the unified driver model to enumerate registered host
   controllers without exposing the internal arrays. */
uint32_t usb_get_hc_count(void);
usb_host_controller_t* usb_get_hc(uint32_t index);

#endif
