#include "include/usb.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/interrupt.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"

#define PCI_CLASS_SERIAL_USB 0x0C
#define PCI_SUBCLASS_USB_XHCI 0x30
#define PCI_INTERFACE_XHCI 0x30

#define XHCI_CAPLENGTH_REG 0x00
#define XHCI_HCIVERSION_REG 0x02
#define XHCI_HCSPARAMS1_REG 0x04
#define XHCI_HCSPARAMS2_REG 0x08
#define XHCI_HCSPARAMS3_REG 0x0C
#define XHCI_HCCPARAMS1_REG 0x10
#define XHCI_DBOFF_REG 0x14
#define XHCI_RTSOFF_REG 0x18

#define XHCI_USBCMD_REG 0x00
#define XHCI_USBSTS_REG 0x04
#define XHCI_PAGESIZE_REG 0x08
#define XHCI_DNCTRL_REG 0x14
#define XHCI_CRCR_REG 0x18
#define XHCI_DCBAAP_REG 0x30
#define XHCI_CONFIG_REG 0x38

#define XHCI_PORTSC_REG 0x400

#define XHCI_CMD_RUN 0x00000001
#define XHCI_CMD_HCRST 0x00000002
#define XHCI_CMD_INTE 0x00000004
#define XHCI_CMD_HSEE 0x00000008

#define XHCI_STS_HCH 0x00000001
#define XHCI_STS_HSE 0x00000002
#define XHCI_STS_EINT 0x00000004
#define XHCI_STS_PCD 0x00000008
#define XHCI_STS_SSS 0x00000010
#define XHCI_STS_RSS 0x00000020
#define XHCI_STS_SRE 0x00000040
#define XHCI_STS_CNR 0x00000080
#define XHCI_STS_HCE 0x00000100

#define XHCI_CONFIG_MAX_SLOTS_EN 0x000000FF

#define XHCI_TRB_TYPE_NORMAL 1
#define XHCI_TRB_TYPE_SETUP_STAGE 2
#define XHCI_TRB_TYPE_DATA_STAGE 3
#define XHCI_TRB_TYPE_STATUS_STAGE 4
#define XHCI_TRB_TYPE_ISoch 1
#define XHCI_TRB_TYPE_LINK 6
#define XHCI_TRB_TYPE_EVENT_DATA 7
#define XHCI_TRB_TYPE_NOOP 8
#define XHCI_TRB_TYPE_EN_SLOT_ENTRY 9
#define XHCI_TRB_TYPE_DIS_SLOT_ENTRY 10
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11
#define XHCI_TRB_TYPE_CONFIG_ENDPOINT 12
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT 13

typedef struct {
    uint64 param;
    uint32 status;
    uint32 control;
} __attribute__((packed)) xhci_trb_t;

typedef struct {
    uint32 route_string;
    uint32 speed;
    uint8 reserved1[8];
    uint32 port_num;
    uint8 reserved2[4];
    uint32 tt_hub_slot_id;
    uint32 tt_port_num;
    uint8 reserved3[16];
    uint32 device_address;
    uint32 reserved4;
    uint32 reserved5;
    uint32 reserved6;
    uint32 reserved7;
} __attribute__((packed)) xhci_slot_context_t;

typedef struct {
    uint32 ep_state;
    uint32 reserved1;
    uint32 max_packet_size;
    uint32 max_esit_payload_hi;
    uint32 max_esit_payload_lo;
    uint32 tr_dequeue_ptr;
    uint16 average_trb_length;
    uint16 max_endpoint_service_time_interval;
    uint8 max_burst_size;
    uint8 max_pstreams;
    uint16 mult;
    uint8 reserved2;
    uint8 cerr;
    uint8 ep_type;
    uint8 host_initiate_disable;
    uint32 interval;
    uint8 reserved3[4];
} __attribute__((packed)) xhci_endpoint_context_t;

typedef struct {
    xhci_slot_context_t slot;
    xhci_endpoint_context_t ep[31];
} __attribute__((packed)) xhci_device_context_t;

typedef struct {
    xhci_device_context_t* device_contexts;
    uintptr_t* dcbaap;
    xhci_trb_t* command_ring;
    xhci_trb_t* event_ring;
    uintptr_t cap_base;
    uintptr_t op_base;
    uintptr_t rt_base;
    uintptr_t db_base;
    uint32 max_slots;
    uint32 max_interrupters;
    uint32 max_ports;
    uint8 irq;
} xhci_hc_t;

static xhci_hc_t* g_xhci_hcs[8] = {0};
static uint32 g_xhci_count = 0;

static uint32 xhci_readl(xhci_hc_t* hc, uint16 offset) {
    return mmio_read32((const volatile void*)(hc->cap_base + offset));
}

static void xhci_writel(xhci_hc_t* hc, uint16 offset, uint32 value) {
    mmio_write32((volatile void*)(hc->cap_base + offset), value);
}

static uint64 xhci_readq(xhci_hc_t* hc, uint16 offset) {
    uint64 lo = (uint64)xhci_readl(hc, offset);
    uint64 hi = (uint64)xhci_readl(hc, offset + 4);
    return (hi << 32) | lo;
}

static void xhci_writeq(xhci_hc_t* hc, uint16 offset, uint64 value) {
    xhci_writel(hc, offset, (uint32)(value & 0xFFFFFFFF));
    xhci_writel(hc, offset + 4, (uint32)(value >> 32));
}

static uint32 xhci_readl_op(xhci_hc_t* hc, uint16 offset) {
    return mmio_read32((const volatile void*)(hc->op_base + offset));
}

static void xhci_writel_op(xhci_hc_t* hc, uint16 offset, uint32 value) {
    mmio_write32((volatile void*)(hc->op_base + offset), value);
}

static bool xhci_hc_init(usb_host_controller_t* hc) {
    if (!hc) {
        return false;
    }

    xhci_hc_t* private_data = (xhci_hc_t*)kmalloc(sizeof(xhci_hc_t));
    if (!private_data) {
        return false;
    }

    memory_set((uint8*)private_data, 0, sizeof(xhci_hc_t));
    
    pci_device_t pci_device;
    if (!pci_find_by_vendor_device(hc->vendor_id, hc->device_id, &pci_device)) {
        kfree(private_data);
        return false;
    }

    private_data->cap_base = pci_device.bar[0] & ~0xF;
    uint32 cap_length = xhci_readl(private_data, XHCI_CAPLENGTH_REG) & 0xFF;
    private_data->op_base = private_data->cap_base + cap_length;
    
    private_data->irq = pci_device.device;

    uint32 hcsparams1 = xhci_readl(private_data, XHCI_HCSPARAMS1_REG);
    private_data->max_slots = (hcsparams1 >> 0) & 0xFF;
    private_data->max_interrupters = (hcsparams1 >> 8) & 0x7FF;
    private_data->max_ports = (hcsparams1 >> 24) & 0xFF;

    uint32 hccparams1 = xhci_readl(private_data, XHCI_HCCPARAMS1_REG);
    uint64 dboff = xhci_readl(private_data, XHCI_DBOFF_REG);
    uint64 rtsoff = xhci_readl(private_data, XHCI_RTSOFF_REG);
    private_data->db_base = private_data->cap_base + dboff;
    private_data->rt_base = private_data->cap_base + rtsoff;

    private_data->dcbaap = (uintptr_t*)kmalloc(sizeof(uintptr_t) * 256);
    if (!private_data->dcbaap) {
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->dcbaap, 0, sizeof(uintptr_t) * 256);

    private_data->device_contexts = (xhci_device_context_t*)kmalloc(sizeof(xhci_device_context_t) * 256);
    if (!private_data->device_contexts) {
        kfree(private_data->dcbaap);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->device_contexts, 0, sizeof(xhci_device_context_t) * 256);

    private_data->command_ring = (xhci_trb_t*)kmalloc(sizeof(xhci_trb_t) * 256);
    if (!private_data->command_ring) {
        kfree(private_data->device_contexts);
        kfree(private_data->dcbaap);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->command_ring, 0, sizeof(xhci_trb_t) * 256);

    private_data->event_ring = (xhci_trb_t*)kmalloc(sizeof(xhci_trb_t) * 256);
    if (!private_data->event_ring) {
        kfree(private_data->command_ring);
        kfree(private_data->device_contexts);
        kfree(private_data->dcbaap);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->event_ring, 0, sizeof(xhci_trb_t) * 256);

    xhci_writel_op(private_data, XHCI_USBCMD_REG, 0);
    timer_sleep_ms(5);

    xhci_writel_op(private_data, XHCI_USBCMD_REG, XHCI_CMD_HCRST);
    while (xhci_readl_op(private_data, XHCI_USBCMD_REG) & XHCI_CMD_HCRST) {
        timer_sleep_ms(1);
    }

    xhci_writeq(private_data, XHCI_DCBAAP_REG, (uint64)(uintptr_t)private_data->dcbaap);

    uint32 config = private_data->max_slots;
    xhci_writel_op(private_data, XHCI_CONFIG_REG, config);

    xhci_writel_op(private_data, XHCI_USBCMD_REG, XHCI_CMD_RUN | XHCI_CMD_INTE);

    for (uint32 i = 1; i <= private_data->max_ports; i++) {
        uint32 portsc = xhci_readl_op(private_data, XHCI_PORTSC_REG + ((i - 1) * 16));
        
        if (portsc & 0x00000001) {
            print("[XHCI] Port ");
            print_dec(i);
            print(": Device connected\n");
        }
    }

    hc->private_data = private_data;

    if (g_xhci_count < 8) {
        g_xhci_hcs[g_xhci_count++] = private_data;
    }

    print("[XHCI] Host controller initialized at 0x");
    print_hex(private_data->cap_base);
    print(" (");
    print_dec(private_data->max_slots);
    print(" slots, ");
    print_dec(private_data->max_ports);
    print(" ports)\n");

    return true;
}

static void xhci_hc_shutdown(usb_host_controller_t* hc) {
    if (!hc || !hc->private_data) {
        return;
    }

    xhci_hc_t* private_data = (xhci_hc_t*)hc->private_data;

    xhci_writel_op(private_data, XHCI_USBCMD_REG, 0);
    timer_sleep_ms(10);

    if (private_data->device_contexts) {
        kfree(private_data->device_contexts);
    }
    if (private_data->dcbaap) {
        kfree(private_data->dcbaap);
    }
    if (private_data->command_ring) {
        kfree(private_data->command_ring);
    }
    if (private_data->event_ring) {
        kfree(private_data->event_ring);
    }

    for (uint32 i = 0; i < g_xhci_count; i++) {
        if (g_xhci_hcs[i] == private_data) {
            g_xhci_hcs[i] = 0;
            break;
        }
    }

    kfree(private_data);
    hc->private_data = 0;
}

static int xhci_control_transfer(usb_host_controller_t* hc, usb_device_t* device,
                                usb_setup_packet_t* setup, void* data, uint32 length) {
    (void)hc;
    (void)device;
    (void)setup;
    (void)data;
    (void)length;
    print("[XHCI] Control transfer not implemented\n");
    return -1;
}

static int xhci_bulk_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                               void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[XHCI] Bulk transfer not implemented\n");
    return -1;
}

static int xhci_interrupt_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                                    void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[XHCI] Interrupt transfer not implemented\n");
    return -1;
}

static usb_host_controller_t g_xhci_hc_template = {
    .type = USB_HC_TYPE_XHCI,
    .init = xhci_hc_init,
    .shutdown = xhci_hc_shutdown,
    .control_transfer = xhci_control_transfer,
    .bulk_transfer = xhci_bulk_transfer,
    .interrupt_transfer = xhci_interrupt_transfer
};

static bool xhci_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    if (device->class_code != PCI_CLASS_SERIAL_USB) {
        return false;
    }

    if (device->subclass != PCI_SUBCLASS_USB_XHCI || device->prog_if != PCI_INTERFACE_XHCI) {
        return false;
    }

    usb_host_controller_t* hc = (usb_host_controller_t*)kmalloc(sizeof(usb_host_controller_t));
    if (!hc) {
        return false;
    }

    memory_copy((uint8*)&g_xhci_hc_template, (uint8*)hc, sizeof(usb_host_controller_t));

    hc->vendor_id = device->vendor_id;
    hc->device_id = device->device_id;
    hc->base_address = device->bar[0];

    if (hc->init(hc)) {
        return usb_register_host_controller(hc);
    }

    kfree(hc);
    return false;
}

bool xhci_init(void) {
    print("[XHCI] Initializing XHCI host controllers...\n");

    pci_enumerate(xhci_pci_callback, 0);

    print("[XHCI] Found ");
    print_dec(g_xhci_count);
    print(" XHCI host controller(s)\n");

    return g_xhci_count > 0;
}
