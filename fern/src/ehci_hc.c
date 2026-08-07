#include "include/usb.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/interrupt.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/memory_safe.h"
#include "include/string.h"
#include "include/screen.h"

#define PCI_CLASS_SERIAL_USB 0x0C
#define PCI_SUBCLASS_USB_EHCI 0x20
#define PCI_INTERFACE_EHCI 0x20

#define EHCI_CAPLENGTH_REG 0x00
#define EHCI_HCIVERSION_REG 0x02
#define EHCI_HCSPARAMS_REG 0x04
#define EHCI_HCCPARAMS_REG 0x08

#define EHCI_USBCMD_REG 0x00
#define EHCI_USBSTS_REG 0x04
#define EHCI_USBINTR_REG 0x08
#define EHCI_FRINDEX_REG 0x0C
#define EHCI_CTRLDSSEGMENT_REG 0x10
#define EHCI_PERIODICLISTBASE_REG 0x14
#define EHCI_ASYNCLISTADDR_REG 0x18
#define EHCI_CONFIGFLAG_REG 0x40
#define EHCI_PORTSC_REG 0x44

#define EHCI_CMD_RUN 0x00000001
#define EHCI_CMD_HCRESET 0x00000002
#define EHCI_CMD_FLSIZE_1024 0x00000000
#define EHCI_CMD_FLSIZE_512 0x00000200
#define EHCI_CMD_FLSIZE_256 0x00000400
#define EHCI_CMD_ASEN 0x00000010
#define EHCI_CMD_PSEN 0x00000004
#define EHCI_CMD_IAAD 0x00000020
#define EHCI_CMD_LHCR 0x00000080

#define EHCI_STS_USBINT 0x00000001
#define EHCI_STS_USBERRINT 0x00000002
#define EHCI_STS_PCD 0x00000004
#define EHCI_STS_FLR 0x00000008
#define EHCI_STS_HSE 0x00000010
#define EHCI_STS_IAA 0x00000020
#define EHCI_STS_HCH 0x00001000
#define EHCI_STS_HCPE 0x00002000

#define EHCI_INTR_USB 0x00000001
#define EHCI_INTR_ERROR 0x00000002
#define EHCI_INTR_PCD 0x00000004
#define EHCI_INTR_FLR 0x00000008
#define EHCI_INTR_IAA 0x00000020
#define EHCI_INTR_HSE 0x00000010
#define EHCI_INTR_MIE 0x80000000

#define EHCI_PORTSC_CCS 0x00000001
#define EHCI_PORTSC_CSC 0x00000002
#define EHCI_PORTSC_PE 0x00000004
#define EHCI_PORTSC_PEC 0x00000008
#define EHCI_PORTSC_OCA 0x00000010
#define EHCI_PORTSC_OCC 0x00000020
#define EHCI_PORTSC_FPR 0x00000040
#define EHCI_PORTSC_SUSPEND 0x00000080
#define EHCI_PORTSC_RESET 0x00000100
#define EHCI_PORTSC_PP 0x00001000
#define EHCI_PORTSC_OWNER 0x00002000

#define EHCI_CONFIGFLAG_CF 0x00000001

#define EHCI_QH_MAX_PACKET 0x07FF0000

typedef struct ehci_qh ehci_qh_t;

typedef struct ehci_qtd {
    uint32 next_qtd;
    uint32 alt_next_qtd;
    uint32 token;
    uint32 buffer[5];
} __attribute__((packed)) ehci_qtd_t;

struct ehci_qh {
    uint32 horizontal_link;
    uint32 capabilities;
    uint32 current_qtd;
    uint32 next_qtd;
    uint32 alt_next_qtd;
    uint32 token;
    uint32 buffer[5];
} __attribute__((packed));

typedef struct {
    ehci_qh_t* async_queue;
    ehci_qh_t* frame_list;
    ehci_qh_t* qh_pool;
    ehci_qtd_t* qtd_pool;
    uintptr_t cap_base;
    uintptr_t op_base;
    uint32 frame_list_size;
    uint8 irq;
} ehci_hc_t;

static ehci_hc_t* g_ehci_hcs[8] = {0};
static uint32 g_ehci_count = 0;

static uint32 ehci_readl(ehci_hc_t* hc, uint16 offset) {
    return mmio_read32((const volatile void*)(hc->op_base + offset));
}

static void ehci_writel(ehci_hc_t* hc, uint16 offset, uint32 value) {
    mmio_write32((volatile void*)(hc->op_base + offset), value);
}

static uint16 ehci_readw(ehci_hc_t* hc, uint16 offset) {
    return mmio_read16((const volatile void*)(hc->cap_base + offset));
}

static bool ehci_hc_init(usb_host_controller_t* hc) {
    if (!hc) {
        return false;
    }

    ehci_hc_t* private_data = (ehci_hc_t*)kmalloc(sizeof(ehci_hc_t));
    if (!private_data) {
        return false;
    }

    memory_set((uint8*)private_data, 0, sizeof(ehci_hc_t));
    
    pci_device_t pci_device;
    if (!pci_find_by_vendor_device(hc->vendor_id, hc->device_id, &pci_device)) {
        kfree(private_data);
        return false;
    }

    private_data->cap_base = pci_device.bar[0] & ~0xF;
    private_data->op_base = private_data->cap_base + ehci_readw(private_data, EHCI_CAPLENGTH_REG);
    private_data->irq = pci_device.device;

    uint32 hcsparams = ehci_readl(private_data, EHCI_HCSPARAMS_REG);
    uint32 ports = (hcsparams >> 0) & 0xF;
    uint32 pcc = (hcsparams >> 8) & 0xF;

    private_data->frame_list_size = 1024;

    private_data->frame_list = (ehci_qh_t*)kmalloc(sizeof(ehci_qh_t) * private_data->frame_list_size);
    if (!private_data->frame_list) {
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->frame_list, 0, sizeof(ehci_qh_t) * private_data->frame_list_size);

    private_data->qh_pool = (ehci_qh_t*)kmalloc(sizeof(ehci_qh_t) * 256);
    if (!private_data->qh_pool) {
        kfree(private_data->frame_list);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->qh_pool, 0, sizeof(ehci_qh_t) * 256);

    private_data->qtd_pool = (ehci_qtd_t*)kmalloc(sizeof(ehci_qtd_t) * 512);
    if (!private_data->qtd_pool) {
        kfree(private_data->frame_list);
        kfree(private_data->qh_pool);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->qtd_pool, 0, sizeof(ehci_qtd_t) * 512);

    private_data->async_queue = &private_data->qh_pool[0];
    private_data->async_queue->horizontal_link = (uint32)(uintptr_t)private_data->async_queue | 0x02;
    private_data->async_queue->capabilities = 0;
    private_data->async_queue->current_qtd = 0x01;

    ehci_writel(private_data, EHCI_USBCMD_REG, 0);
    timer_sleep_ms(5);

    ehci_writel(private_data, EHCI_USBCMD_REG, EHCI_CMD_HCRESET);
    while (ehci_readl(private_data, EHCI_USBCMD_REG) & EHCI_CMD_HCRESET) {
        timer_sleep_ms(1);
    }

    ehci_writel(private_data, EHCI_USBCMD_REG, EHCI_CMD_FLSIZE_1024);

    ehci_writel(private_data, EHCI_PERIODICLISTBASE_REG, 
                (uint32)(uintptr_t)private_data->frame_list);

    ehci_writel(private_data, EHCI_ASYNCLISTADDR_REG, 
                (uint32)(uintptr_t)private_data->async_queue);

    ehci_writel(private_data, EHCI_USBINTR_REG, EHCI_INTR_MIE | EHCI_INTR_USB | EHCI_INTR_ERROR | EHCI_INTR_IAA);
    ehci_writel(private_data, EHCI_USBSTS_REG, 0x3F);

    ehci_writel(private_data, EHCI_CONFIGFLAG_REG, EHCI_CONFIGFLAG_CF);
    timer_sleep_ms(100);

    ehci_writel(private_data, EHCI_USBCMD_REG, 
                EHCI_CMD_RUN | EHCI_CMD_ASEN | EHCI_CMD_PSEN);

    for (uint32 i = 0; i < ports; i++) {
        uint32 portsc = ehci_readl(private_data, EHCI_PORTSC_REG + (i * 4));
        
        if (portsc & EHCI_PORTSC_OWNER) {
            portsc &= ~EHCI_PORTSC_OWNER;
            ehci_writel(private_data, EHCI_PORTSC_REG + (i * 4), portsc);
            timer_sleep_ms(50);
        }

        portsc = ehci_readl(private_data, EHCI_PORTSC_REG + (i * 4));
        portsc |= EHCI_PORTSC_PP;
        ehci_writel(private_data, EHCI_PORTSC_REG + (i * 4), portsc);
        timer_sleep_ms(20);

        portsc = ehci_readl(private_data, EHCI_PORTSC_REG + (i * 4));
        if (portsc & EHCI_PORTSC_CCS) {
            print("[EHCI] Port ");
            print_dec(i);
            print(": Device connected\n");
        }
    }

    hc->private_data = private_data;

    if (g_ehci_count < 8) {
        g_ehci_hcs[g_ehci_count++] = private_data;
    }

    print("[EHCI] Host controller initialized at 0x");
    print_hex(private_data->cap_base);
    print(" (");
    print_dec(ports);
    print(" ports)\n");

    return true;
}

static void ehci_hc_shutdown(usb_host_controller_t* hc) {
    if (!hc || !hc->private_data) {
        return;
    }

    ehci_hc_t* private_data = (ehci_hc_t*)hc->private_data;

    ehci_writel(private_data, EHCI_USBCMD_REG, 0);
    timer_sleep_ms(10);

    ehci_writel(private_data, EHCI_CONFIGFLAG_REG, 0);
    ehci_writel(private_data, EHCI_USBCMD_REG, EHCI_CMD_HCRESET);

    if (private_data->frame_list) {
        kfree(private_data->frame_list);
    }
    if (private_data->qh_pool) {
        kfree(private_data->qh_pool);
    }
    if (private_data->qtd_pool) {
        kfree(private_data->qtd_pool);
    }

    for (uint32 i = 0; i < g_ehci_count; i++) {
        if (g_ehci_hcs[i] == private_data) {
            g_ehci_hcs[i] = 0;
            break;
        }
    }

    kfree(private_data);
    hc->private_data = 0;
}

static int ehci_control_transfer(usb_host_controller_t* hc, usb_device_t* device,
                                usb_setup_packet_t* setup, void* data, uint32 length) {
    (void)hc;
    (void)device;
    (void)setup;
    (void)data;
    (void)length;
    print("[EHCI] Control transfer not implemented\n");
    return -1;
}

static int ehci_bulk_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                               void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[EHCI] Bulk transfer not implemented\n");
    return -1;
}

static int ehci_interrupt_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                                    void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[EHCI] Interrupt transfer not implemented\n");
    return -1;
}

static usb_host_controller_t g_ehci_hc_template = {
    .type = USB_HC_TYPE_EHCI,
    .init = ehci_hc_init,
    .shutdown = ehci_hc_shutdown,
    .control_transfer = ehci_control_transfer,
    .bulk_transfer = ehci_bulk_transfer,
    .interrupt_transfer = ehci_interrupt_transfer
};

static bool ehci_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    if (device->class_code != PCI_CLASS_SERIAL_USB) {
        return false;
    }

    if (device->subclass != PCI_SUBCLASS_USB_EHCI || device->prog_if != PCI_INTERFACE_EHCI) {
        return false;
    }

    usb_host_controller_t* hc = (usb_host_controller_t*)kmalloc(sizeof(usb_host_controller_t));
    if (!hc) {
        return false;
    }

    memory_copy((uint8*)&g_ehci_hc_template, (uint8*)hc, sizeof(usb_host_controller_t));

    hc->vendor_id = device->vendor_id;
    hc->device_id = device->device_id;
    hc->base_address = device->bar[0];

    if (hc->init(hc)) {
        return usb_register_host_controller(hc);
    }

    kfree(hc);
    return false;
}

#include "include/driver.h"
static int ehci_drv_probe(drv_device_t* d, const drv_id_t* id) { (void)d; (void)id; return 0; }
static void ehci_drv_remove(drv_device_t* d) { (void)d; }
static const drv_id_t g_ehci_drv_ids[] = {
    { DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_ID_ANY, DRV_BUS_PCI,
      0x0C, 0x03, 0x20, DRV_MATCH_CLASS, 0 },
    DRV_ID_TABLE_END
};
static drv_driver_t g_ehci_drv = {
    .name = "ehci", .version = "1.0", .bus = DRV_BUS_PCI,
    .id_table = g_ehci_drv_ids, .probe = ehci_drv_probe,
    .remove = ehci_drv_remove, .flags = DRV_FLAG_HOTPLUG,
};

bool ehci_init(void) {
    print("[EHCI] Initializing EHCI host controllers...\n");

    pci_enumerate(ehci_pci_callback, 0);

    print("[EHCI] Found ");
    print_dec(g_ehci_count);
    print(" EHCI host controller(s)\n");

    drv_register(&g_ehci_drv);

    return g_ehci_count > 0;
}
