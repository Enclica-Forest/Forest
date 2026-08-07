#include "include/usb.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/interrupt.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"

#define PCI_CLASS_SERIAL_USB 0x0C
#define PCI_SUBCLASS_USB_UHCI 0x00
#define PCI_SUBCLASS_USB_OHCI 0x10
#define PCI_SUBCLASS_USB_EHCI 0x20
#define PCI_SUBCLASS_USB_XHCI 0x30

#define UHCI_IO_BASE_OFFSET 0x20
#define UHCI_COMMAND_REG 0x00
#define UHCI_STATUS_REG 0x02
#define UHCI_INTERRUPT_REG 0x04
#define UHCI_FRNUM_REG 0x06
#define UHCI_FRBASEADDR_REG 0x08
#define UHCI_SOFMOD_REG 0x0C
#define UHCI_PORTSC1_REG 0x10
#define UHCI_PORTSC2_REG 0x12

#define UHCI_CMD_RUN 0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET 0x0004
#define UHCI_CMD_EGSM 0x0008
#define UHCI_CMD_FGR 0x0010
#define UHCI_CMD_MAXP 0x0080

#define UHCI_STS_USBINT 0x0001
#define UHCI_STS_USBEI 0x0002
#define UHCI_STS_RD 0x0004
#define UHCI_STS_HSE 0x0008
#define UHCI_STS_HCPE 0x0010
#define UHCI_STS_HCH 0x0020

#define UHCI_PORTSC_CURR 0x0001
#define UHCI_PORTSC_CSC 0x0002
#define UHCI_PORTSC_CK 0x0004
#define UHCI_PORTSC_PE 0x0008
#define UHCI_PORTSC_PEDC 0x0010
#define UHCI_PORTSC_LS 0x0060
#define UHCI_PORTSC_PR 0x0100
#define UHCI_PORTSC_SUSP 0x1000
#define UHCI_PORTSC_LSDA 0x2000
#define UHCI_PORTSC_PO 0x4000

#define UHCI_QTD_MAX_LENGTH 0x7FF

typedef struct uhci_qh uhci_qh_t;
typedef struct uhci_td uhci_td_t;

typedef struct uhci_td {
    uint32 link;
    uint32 status;
    uint32 token;
    uint32 buffer;
} __attribute__((packed)) uhci_td_t;

typedef struct uhci_qh {
    uint32 link;
    uint32 element;
} __attribute__((packed)) uhci_qh_t;

typedef struct {
    uhci_qh_t* frame_list;
    uhci_qh_t* control_queue;
    uhci_qh_t* bulk_queue;
    uhci_qh_t* interrupt_queue;
    uhci_td_t* td_pool;
    uint16 io_base;
    uint8 irq;
} uhci_hc_t;

static uhci_hc_t* g_uhci_hcs[8] = {0};
static uint32 g_uhci_count = 0;

static uint16 uhci_readw(uhci_hc_t* hc, uint16 offset) {
    return inportw(hc->io_base + offset);
}

static void uhci_writew(uhci_hc_t* hc, uint16 offset, uint16 value) {
    outportw(hc->io_base + offset, value);
}

static uhci_td_t* uhci_alloc_td(uhci_hc_t* hc) {
    (void)hc;
    static uint32 td_index = 0;
    if (td_index < 1024) {
        uhci_td_t* td = &hc->td_pool[td_index++];
        memory_set((uint8*)td, 0, sizeof(uhci_td_t));
        return td;
    }
    return 0;
}

static uhci_qh_t* uhci_alloc_qh(uhci_hc_t* hc) {
    (void)hc;
    static uint32 qh_index = 0;
    static uhci_qh_t qh_pool[256];
    if (qh_index < 256) {
        uhci_qh_t* qh = &qh_pool[qh_index++];
        memory_set((uint8*)qh, 0, sizeof(uhci_qh_t));
        return qh;
    }
    return 0;
}

static bool uhci_hc_init(usb_host_controller_t* hc) {
    if (!hc) {
        return false;
    }

    uhci_hc_t* private_data = (uhci_hc_t*)kmalloc(sizeof(uhci_hc_t));
    if (!private_data) {
        return false;
    }

    memory_set((uint8*)private_data, 0, sizeof(uhci_hc_t));
    
    pci_device_t pci_device;
    if (!pci_find_by_vendor_device(hc->vendor_id, hc->device_id, &pci_device)) {
        kfree(private_data);
        return false;
    }

    private_data->io_base = pci_device.bar[4] & 0xFFE0;
    private_data->irq = pci_device.device;

    private_data->td_pool = (uhci_td_t*)kmalloc(sizeof(uhci_td_t) * 1024);
    if (!private_data->td_pool) {
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->td_pool, 0, sizeof(uhci_td_t) * 1024);

    private_data->frame_list = (uhci_qh_t*)kmalloc(sizeof(uhci_qh_t) * 1024);
    if (!private_data->frame_list) {
        kfree(private_data->td_pool);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->frame_list, 0, sizeof(uhci_qh_t) * 1024);

    private_data->control_queue = uhci_alloc_qh(private_data);
    private_data->bulk_queue = uhci_alloc_qh(private_data);
    private_data->interrupt_queue = uhci_alloc_qh(private_data);

    if (!private_data->control_queue || !private_data->bulk_queue || !private_data->interrupt_queue) {
        kfree(private_data->td_pool);
        kfree(private_data->frame_list);
        kfree(private_data);
        return false;
    }

    for (uint32 i = 0; i < 1024; i++) {
        private_data->frame_list[i].link = (uint32)(uintptr_t)private_data->control_queue | 0x02;
    }

    private_data->control_queue->link = (uint32)(uintptr_t)private_data->bulk_queue | 0x02;
    private_data->bulk_queue->link = 0x01;

    uint16 base_addr = (uint16)((uintptr_t)private_data->frame_list & 0xFFFF);
    uhci_writew(private_data, UHCI_FRBASEADDR_REG, base_addr);

    uint16 cmd = uhci_readw(private_data, UHCI_COMMAND_REG);
    cmd &= ~UHCI_CMD_RUN;
    uhci_writew(private_data, UHCI_COMMAND_REG, cmd);

    timer_sleep_ms(10);

    cmd |= UHCI_CMD_HCRESET;
    uhci_writew(private_data, UHCI_COMMAND_REG, cmd);

    timer_sleep_ms(10);

    cmd = uhci_readw(private_data, UHCI_COMMAND_REG);
    cmd &= ~UHCI_CMD_HCRESET;
    uhci_writew(private_data, UHCI_COMMAND_REG, cmd);

    timer_sleep_ms(10);

    cmd = uhci_readw(private_data, UHCI_COMMAND_REG);
    cmd |= UHCI_CMD_MAXP;
    cmd |= UHCI_CMD_RUN;
    uhci_writew(private_data, UHCI_COMMAND_REG, cmd);

    uint16 portsc1 = uhci_readw(private_data, UHCI_PORTSC1_REG);
    portsc1 &= ~(UHCI_PORTSC_LSDA | UHCI_PORTSC_PO);
    uhci_writew(private_data, UHCI_PORTSC1_REG, portsc1);

    uint16 portsc2 = uhci_readw(private_data, UHCI_PORTSC2_REG);
    portsc2 &= ~(UHCI_PORTSC_LSDA | UHCI_PORTSC_PO);
    uhci_writew(private_data, UHCI_PORTSC2_REG, portsc2);

    hc->private_data = private_data;

    if (g_uhci_count < 8) {
        g_uhci_hcs[g_uhci_count++] = private_data;
    }

    print("[UHCI] Host controller initialized at 0x");
    printhex(private_data->io_base);
    print("\n");

    return true;
}

static void uhci_hc_shutdown(usb_host_controller_t* hc) {
    if (!hc || !hc->private_data) {
        return;
    }

    uhci_hc_t* private_data = (uhci_hc_t*)hc->private_data;

    uint16 cmd = uhci_readw(private_data, UHCI_COMMAND_REG);
    cmd &= ~UHCI_CMD_RUN;
    uhci_writew(private_data, UHCI_COMMAND_REG, cmd);

    if (private_data->td_pool) {
        kfree(private_data->td_pool);
    }
    if (private_data->frame_list) {
        kfree(private_data->frame_list);
    }

    for (uint32 i = 0; i < g_uhci_count; i++) {
        if (g_uhci_hcs[i] == private_data) {
            g_uhci_hcs[i] = 0;
            break;
        }
    }

    kfree(private_data);
    hc->private_data = 0;
}

static int uhci_control_transfer(usb_host_controller_t* hc, usb_device_t* device,
                               usb_setup_packet_t* setup, void* data, uint32 length) {
    (void)hc;
    (void)device;
    (void)setup;
    (void)data;
    (void)length;
    print("[UHCI] Control transfer not implemented\n");
    return -1;
}

static int uhci_bulk_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                              void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[UHCI] Bulk transfer not implemented\n");
    return -1;
}

static int uhci_interrupt_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                                  void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[UHCI] Interrupt transfer not implemented\n");
    return -1;
}

static usb_host_controller_t g_uhci_hc_template = {
    .type = USB_HC_TYPE_UHCI,
    .init = uhci_hc_init,
    .shutdown = uhci_hc_shutdown,
    .control_transfer = uhci_control_transfer,
    .bulk_transfer = uhci_bulk_transfer,
    .interrupt_transfer = uhci_interrupt_transfer
};

static bool uhci_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    if (device->class_code != PCI_CLASS_SERIAL_USB) {
        return false;
    }

    if (device->subclass != PCI_SUBCLASS_USB_UHCI) {
        return false;
    }

    usb_host_controller_t* hc = (usb_host_controller_t*)kmalloc(sizeof(usb_host_controller_t));
    if (!hc) {
        return false;
    }

    memory_copy((uint8*)&g_uhci_hc_template, (uint8*)hc, sizeof(usb_host_controller_t));

    hc->vendor_id = device->vendor_id;
    hc->device_id = device->device_id;
    hc->base_address = device->bar[4];

    if (hc->init(hc)) {
        return usb_register_host_controller(hc);
    }

    kfree(hc);
    return false;
}

static int uhci_drv_probe(drv_device_t* d, const drv_id_t* id) { (void)d; (void)id; return 0; }
static void uhci_drv_remove(drv_device_t* d) { (void)d; }
static const drv_id_t g_uhci_drv_ids[] = {
    { DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_BUS_PCI,
      0x0C, 0x03, 0x00, DRV_MATCH_CLASS, 0 },
    DRV_ID_TABLE_END
};
static drv_driver_t g_uhci_drv = {
    .name = "uhci", .version = "1.0", .bus = DRV_BUS_PCI,
    .id_table = g_uhci_drv_ids, .probe = uhci_drv_probe,
    .remove = uhci_drv_remove, .flags = DRV_FLAG_HOTPLUG,
};

bool uhci_init(void) {
    print("[UHCI] Initializing UHCI host controllers...\n");

    pci_enumerate(uhci_pci_callback, 0);

    print("[UHCI] Found ");
    printdec(g_uhci_count);
    print(" UHCI host controller(s)\n");

    drv_register(&g_uhci_drv);

    return g_uhci_count > 0;
}
