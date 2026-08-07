/**
 * USB Hub Driver for Fern
 *
 * Implements USB Hub Class support for connecting multiple devices.
 * Based on USB 2.0/3.0 Hub Class specification.
 */

#ifndef USB_HUB_H
#define USB_HUB_H

#include "usb.h"
#include "../types.h"
#include <stdbool.h>

// Hub Class Request Codes
#define USB_HUB_GET_STATUS          0x00
#define USB_HUB_CLEAR_FEATURE       0x01
#define USB_HUB_SET_FEATURE         0x03
#define USB_HUB_GET_DESCRIPTOR      0x06
#define USB_HUB_SET_DESCRIPTOR      0x07
#define USB_HUB_CLEAR_TT_BUFFER     0x08
#define USB_HUB_RESET_TT            0x09
#define USB_HUB_GET_TT_STATE        0x0A
#define USB_HUB_STOP_TT             0x0B
#define USB_HUB_SET_HUB_DEPTH       0x0C    // USB 3.0 only
#define USB_HUB_GET_PORT_ERR_COUNT  0x0D    // USB 3.0 only

// Hub Feature Selectors
#define USB_HUB_FEATURE_C_HUB_LOCAL_POWER   0
#define USB_HUB_FEATURE_C_HUB_OVER_CURRENT  1

// Port Feature Selectors
#define USB_HUB_FEATURE_PORT_CONNECTION     0
#define USB_HUB_FEATURE_PORT_ENABLE         1
#define USB_HUB_FEATURE_PORT_SUSPEND        2
#define USB_HUB_FEATURE_PORT_OVER_CURRENT   3
#define USB_HUB_FEATURE_PORT_RESET          4
#define USB_HUB_FEATURE_PORT_LINK_STATE     5   // USB 3.0
#define USB_HUB_FEATURE_PORT_POWER          8
#define USB_HUB_FEATURE_PORT_LOW_SPEED      9
#define USB_HUB_FEATURE_C_PORT_CONNECTION   16
#define USB_HUB_FEATURE_C_PORT_ENABLE       17
#define USB_HUB_FEATURE_C_PORT_SUSPEND      18
#define USB_HUB_FEATURE_C_PORT_OVER_CURRENT 19
#define USB_HUB_FEATURE_C_PORT_RESET        20
#define USB_HUB_FEATURE_PORT_TEST           21
#define USB_HUB_FEATURE_PORT_INDICATOR      22
#define USB_HUB_FEATURE_C_PORT_LINK_STATE   25  // USB 3.0
#define USB_HUB_FEATURE_C_PORT_CONFIG_ERROR 26  // USB 3.0
#define USB_HUB_FEATURE_PORT_REMOTE_WAKE_MASK 27 // USB 3.0
#define USB_HUB_FEATURE_BH_PORT_RESET       28  // USB 3.0
#define USB_HUB_FEATURE_C_BH_PORT_RESET     29  // USB 3.0
#define USB_HUB_FEATURE_FORCE_LINKPM_ACCEPT 30  // USB 3.0

// Hub Descriptor Type
#define USB_HUB_DESC_TYPE_HUB       0x29    // USB 2.0
#define USB_HUB_DESC_TYPE_SS_HUB    0x2A    // USB 3.0

// Hub Characteristics bits
#define USB_HUB_CHAR_LPSM_MASK          0x0003  // Logical Power Switching Mode
#define USB_HUB_CHAR_LPSM_GANGED        0x0000  // All ports powered at once
#define USB_HUB_CHAR_LPSM_INDIVIDUAL    0x0001  // Individual port power control
#define USB_HUB_CHAR_COMPOUND           0x0004  // Compound device
#define USB_HUB_CHAR_OCPM_MASK          0x0018  // Over-Current Protection Mode
#define USB_HUB_CHAR_OCPM_GLOBAL        0x0000  // Global over-current
#define USB_HUB_CHAR_OCPM_INDIVIDUAL    0x0008  // Individual over-current
#define USB_HUB_CHAR_TT_THINK_MASK      0x0060  // TT Think Time
#define USB_HUB_CHAR_PORT_INDICATORS    0x0080  // Port Indicators

// Hub Status (2 bytes)
typedef struct __attribute__((packed)) {
    uint16 status;
    uint16 change;
} usb_hub_status_t;

// Hub status bits
#define USB_HUB_STATUS_LOCAL_POWER      (1 << 0)
#define USB_HUB_STATUS_OVER_CURRENT     (1 << 1)

// Port Status (USB 2.0, 4 bytes)
typedef struct __attribute__((packed)) {
    uint16 status;
    uint16 change;
} usb_hub_port_status_t;

// Port status bits (USB 2.0)
#define USB_HUB_PORT_STATUS_CONNECTION      (1 << 0)
#define USB_HUB_PORT_STATUS_ENABLE          (1 << 1)
#define USB_HUB_PORT_STATUS_SUSPEND         (1 << 2)
#define USB_HUB_PORT_STATUS_OVER_CURRENT    (1 << 3)
#define USB_HUB_PORT_STATUS_RESET           (1 << 4)
#define USB_HUB_PORT_STATUS_POWER           (1 << 8)
#define USB_HUB_PORT_STATUS_LOW_SPEED       (1 << 9)
#define USB_HUB_PORT_STATUS_HIGH_SPEED      (1 << 10)
#define USB_HUB_PORT_STATUS_TEST            (1 << 11)
#define USB_HUB_PORT_STATUS_INDICATOR       (1 << 12)

// Port change bits
#define USB_HUB_PORT_CHANGE_CONNECTION      (1 << 0)
#define USB_HUB_PORT_CHANGE_ENABLE          (1 << 1)
#define USB_HUB_PORT_CHANGE_SUSPEND         (1 << 2)
#define USB_HUB_PORT_CHANGE_OVER_CURRENT    (1 << 3)
#define USB_HUB_PORT_CHANGE_RESET           (1 << 4)

// USB 3.0 Port Status Extension bits
#define USB_HUB_PORT_STATUS_LINK_STATE_MASK (0x0F << 5)
#define USB_HUB_PORT_STATUS_SPEED_MASK      (0x07 << 10)

// USB Hub Device
typedef struct usb_hub_device {
    usb_device_t* usb_device;
    usb_interface_t* interface;
    usb_endpoint_t* status_endpoint;
    uint8  num_ports;
    uint16 characteristics;
    uint8  power_on_delay;          // Power-on to power-good time (in 2ms units)
    uint8  max_current;             // Max current (in mA)
    bool   is_usb3;                 // USB 3.0 hub
    bool   has_power_switching;     // Supports individual port power
    bool   has_over_current;        // Has over-current protection
    // Port state
    struct {
        bool connected;
        bool enabled;
        bool suspended;
        bool reset;
        bool powered;
        usb_speed_t speed;
        usb_device_t* device;       // Connected device
    } ports[16];
    struct usb_hub_device* next;
} usb_hub_device_t;

// USB Hub Functions
bool usb_hub_init(void);
void usb_hub_shutdown(void);
void usb_hub_poll(void);

// Device management
bool usb_hub_probe(usb_device_t* device, usb_interface_t* interface);
void usb_hub_disconnect(usb_device_t* device, usb_interface_t* interface);

// Hub operations
bool usb_hub_get_status(usb_hub_device_t* hub, usb_hub_status_t* status);
bool usb_hub_get_port_status(usb_hub_device_t* hub, uint8 port,
                              usb_hub_port_status_t* status);
bool usb_hub_set_port_feature(usb_hub_device_t* hub, uint8 port, uint8 feature);
bool usb_hub_clear_port_feature(usb_hub_device_t* hub, uint8 port, uint8 feature);

// Port operations
bool usb_hub_reset_port(usb_hub_device_t* hub, uint8 port);
bool usb_hub_enable_port(usb_hub_device_t* hub, uint8 port);
bool usb_hub_disable_port(usb_hub_device_t* hub, uint8 port);
bool usb_hub_power_on_port(usb_hub_device_t* hub, uint8 port);
bool usb_hub_power_off_port(usb_hub_device_t* hub, uint8 port);
usb_speed_t usb_hub_get_port_speed(usb_hub_device_t* hub, uint8 port);

// Device enumeration
usb_hub_device_t* usb_hub_get_first(void);
usb_hub_device_t* usb_hub_get_next(usb_hub_device_t* hub);

// USB Hub class driver
extern usb_class_driver_t usb_hub_driver;

#endif // USB_HUB_H
