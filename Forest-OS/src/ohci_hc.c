#include "include/usb.h"
#include "include/pci.h"
#include "include/system.h"
#include "include/interrupt.h"
#include "include/driver.h"
#include "include/memory.h"
#include "include/mm.h"
#include "include/string.h"
#include "include/screen.h"

#define GFP_KERNEL 0x01

#define PCI_CLASS_SERIAL_USB 0x0C
#define PCI_SUBCLASS_USB_OHCI 0x10

#define OHCI_HCCA_SIZE 256
#define OHCI_ED_MAX 32
#define OHCI_TD_MAX 64

#define OHCI_CONTROL_REG 0x00
#define OHCI_COMMAND_STATUS_REG 0x08
#define OHCI_INTERRUPT_STATUS_REG 0x0C
#define OHCI_INTERRUPT_ENABLE_REG 0x10
#define OHCI_INTERRUPT_DISABLE_REG 0x14
#define OHCI_HCCA_REG 0x18
#define OHCI_PERIOD_CURRENT_ED_REG 0x1C
#define OHCI_CONTROL_HEAD_ED_REG 0x20
#define OHCI_CONTROL_CURRENT_ED_REG 0x24
#define OHCI_BULK_HEAD_ED_REG 0x28
#define OHCI_BULK_CURRENT_ED_REG 0x2C
#define OHCI_DONE_HEAD_REG 0x30
#define OHCI_FM_INTERVAL_REG 0x34
#define OHCI_FM_REMAINING_REG 0x38
#define OHCI_FM_NUMBER_REG 0x3C
#define OHCI_PERIODIC_START_REG 0x40
#define OHCI_LS_THRESHOLD_REG 0x44
#define OHCI_RH_DESCRIPTOR_A_REG 0x48
#define OHCI_RH_DESCRIPTOR_B_REG 0x4C
#define OHCI_RH_STATUS_REG 0x50
#define OHCI_RH_PORT_STATUS_REG 0x54

#define OHCI_CTRL_CBSR 0x00000003
#define OHCI_CTRL_PLE 0x00000004
#define OHCI_CTRL_CLE 0x00000010
#define OHCI_CTRL_BLE 0x00000020
#define OHCI_CTRL_HCFS 0x000000C0
#define OHCI_CTRL_HCFS_RESET 0x00000000
#define OHCI_CTRL_HCFS_RESUME 0x00000040
#define OHCI_CTRL_HCFS_OPERATIONAL 0x00000080
#define OHCI_CTRL_HCFS_SUSPEND 0x000000C0
#define OHCI_CTRL_IE 0x00000100
#define OHCI_CTRL_CCR 0x00000200
#define OHCI_CTRL_PSR 0x00000400

#define OHCI_STS_HCR 0x00000001
#define OHCI_STS_CLF 0x00000002
#define OHCI_STS_BLF 0x00000004
#define OHCI_STS_OCR 0x00000008
#define OHCI_STS_SOC 0x00000010

#define OHCI_INT_SO 0x00000001
#define OHCI_INT_WDH 0x00000002
#define OHCI_INT_SF 0x00000004
#define OHCI_INT_RD 0x00000008
#define OHCI_INT_UE 0x00000010
#define OHCI_INT_FNO 0x00000020
#define OHCI_INT_RHSC 0x00000040
#define OHCI_INT_OC 0x40000000
#define OHCI_INT_MIE 0x80000000

typedef struct {
    uint32 HccaInterruptTable[32];
    uint16 HccaFrameNumber;
    uint16 HccaPad1;
    uint32 HccaDoneHead;
    volatile uint8 HccaReserved[116];
} __attribute__((packed)) ohci_hcca_t;

typedef struct ohci_ed ohci_ed_t;

typedef struct ohci_td {
    uint32 flags;
    uint32 cbp;
    uint32 next_td;
    uint32 be;
} __attribute__((packed)) ohci_td_t;

struct ohci_ed {
    uint32 flags;
    uint32 tail_td;
    uint32 head_td;
    uint32 next_ed;
} __attribute__((packed));

typedef struct {
    ohci_hcca_t* hcca;
    ohci_ed_t* control_ed_pool;
    ohci_ed_t* bulk_ed_pool;
    ohci_td_t* td_pool;
    uintptr_t base_address;
    uint8 irq;
} ohci_hc_t;

static ohci_hc_t* g_ohci_hcs[8] = {0};
static uint32 g_ohci_count = 0;

static uint32 ohci_readl(ohci_hc_t* hc, uint16 offset) {
    return mmio_read32((const volatile void*)(hc->base_address + offset));
}

static void ohci_writel(ohci_hc_t* hc, uint16 offset, uint32 value) {
    mmio_write32((volatile void*)(hc->base_address + offset), value);
}

static bool ohci_hc_init(usb_host_controller_t* hc) {
    if (!hc) {
        return false;
    }

    ohci_hc_t* private_data = (ohci_hc_t*)kmalloc(sizeof(ohci_hc_t));
    if (!private_data) {
        return false;
    }

    memory_set((uint8*)private_data, 0, sizeof(ohci_hc_t));
    
    pci_device_t pci_device;
    if (!pci_find_by_vendor_device(hc->vendor_id, hc->device_id, &pci_device)) {
        kfree(private_data);
        return false;
    }

    private_data->base_address = pci_device.bar[0] & ~0xF;
    private_data->irq = pci_device.device;

    private_data->hcca = (ohci_hcca_t*)kmalloc(OHCI_HCCA_SIZE);
    private_data->td_pool = (ohci_td_t*)kmalloc(sizeof(ohci_td_t) * OHCI_TD_MAX);
    private_data->control_ed_pool = (ohci_ed_t*)kmalloc(sizeof(ohci_ed_t) * OHCI_ED_MAX);
    private_data->bulk_ed_pool = (ohci_ed_t*)kmalloc(sizeof(ohci_ed_t) * OHCI_ED_MAX);
    if (!private_data->bulk_ed_pool) {
        kfree(private_data->hcca);
        kfree(private_data->td_pool);
        kfree(private_data->control_ed_pool);
        kfree(private_data);
        return false;
    }
    memory_set((uint8*)private_data->bulk_ed_pool, 0, sizeof(ohci_ed_t) * OHCI_ED_MAX);

    ohci_writel(private_data, OHCI_INTERRUPT_DISABLE_REG, 0xFFFFFFFF);
    ohci_writel(private_data, OHCI_INTERRUPT_STATUS_REG, 0xFFFFFFFF);

    ohci_writel(private_data, OHCI_CONTROL_REG, OHCI_CTRL_HCFS_SUSPEND);
    timer_sleep_ms(10);

    ohci_writel(private_data, OHCI_CONTROL_REG, OHCI_CTRL_HCFS_RESET);
    timer_sleep_ms(50);

    ohci_writel(private_data, OHCI_CONTROL_REG, OHCI_CTRL_HCFS_OPERATIONAL);
    timer_sleep_ms(10);

    ohci_writel(private_data, OHCI_HCCA_REG, (uint32)(uintptr_t)private_data->hcca);

    ohci_writel(private_data, OHCI_CONTROL_REG, 
                OHCI_CTRL_CLE | OHCI_CTRL_BLE | OHCI_CTRL_IE | OHCI_CTRL_HCFS_OPERATIONAL);

    ohci_writel(private_data, OHCI_INTERRUPT_ENABLE_REG, 
                OHCI_INT_MIE | OHCI_INT_WDH | OHCI_INT_RHSC | OHCI_INT_UE);

    hc->private_data = private_data;

    if (g_ohci_count < 8) {
        g_ohci_hcs[g_ohci_count++] = private_data;
    }

    print("[OHCI] Host controller initialized at 0x");
    print_hex(private_data->base_address);
    print("\n");

    return true;
}

static void ohci_hc_shutdown(usb_host_controller_t* hc) {
    if (!hc || !hc->private_data) {
        return;
    }

    ohci_hc_t* private_data = (ohci_hc_t*)hc->private_data;

    ohci_writel(private_data, OHCI_CONTROL_REG, OHCI_CTRL_HCFS_SUSPEND);

    if (private_data->hcca) {
        kfree(private_data->hcca);
    }
    if (private_data->td_pool) {
        kfree(private_data->td_pool);
    }
    if (private_data->control_ed_pool) {
        kfree(private_data->control_ed_pool);
    }
    if (private_data->bulk_ed_pool) {
        kfree(private_data->bulk_ed_pool);
    }

    for (uint32 i = 0; i < g_ohci_count; i++) {
        if (g_ohci_hcs[i] == private_data) {
            g_ohci_hcs[i] = 0;
            break;
        }
    }

    kfree(private_data);
    hc->private_data = 0;
}

static int ohci_control_transfer(usb_host_controller_t* hc, usb_device_t* device,
                                usb_setup_packet_t* setup, void* data, uint32 length) {
    (void)hc;
    (void)device;
    (void)setup;
    (void)data;
    (void)length;
    print("[OHCI] Control transfer not implemented\n");
    return -1;
}

static int ohci_bulk_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                               void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[OHCI] Bulk transfer not implemented\n");
    return -1;
}

static int ohci_interrupt_transfer(usb_host_controller_t* hc, usb_endpoint_t* endpoint,
                                    void* data, uint32 length, usb_direction_t direction) {
    (void)hc;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)direction;
    print("[OHCI] Interrupt transfer not implemented\n");
    return -1;
}

static usb_host_controller_t g_ohci_hc_template = {
    .type = USB_HC_TYPE_OHCI,
    .init = ohci_hc_init,
    .shutdown = ohci_hc_shutdown,
    .control_transfer = ohci_control_transfer,
    .bulk_transfer = ohci_bulk_transfer,
    .interrupt_transfer = ohci_interrupt_transfer
};

static bool ohci_pci_callback(const pci_device_t* device, void* context) {
    (void)context;

    if (device->class_code != PCI_CLASS_SERIAL_USB) {
        return false;
    }

    if (device->subclass != PCI_SUBCLASS_USB_OHCI) {
        return false;
    }

    usb_host_controller_t* hc = (usb_host_controller_t*)kmalloc(sizeof(usb_host_controller_t));
    if (!hc) {
        return false;
    }

    memory_copy((uint8*)&g_ohci_hc_template, (uint8*)hc, sizeof(usb_host_controller_t));

    hc->vendor_id = device->vendor_id;
    hc->device_id = device->device_id;
    hc->base_address = device->bar[0];

    if (hc->init(hc)) {
        return usb_register_host_controller(hc);
    }

    kfree(hc);
    return false;
}

bool ohci_init(void) {
    print("[OHCI] Initializing OHCI host controllers...\n");

    pci_enumerate(ohci_pci_callback, 0);

    print("[OHCI] Found ");
    print_dec(g_ohci_count);
    print(" OHCI host controller(s)\n");

    return g_ohci_count > 0;
}
