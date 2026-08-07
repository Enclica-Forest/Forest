/*
 * driver_bus.c — glue that wraps the existing PCI/USB enumeration so the
 * devices they discover get registered with the unified driver model.
 *
 * The bus enumerators in src/pci.c and src/usb.c retain their existing
 * implementations; this file only adds a layer on top that fans their
 * discovered devices into drv_bus_register().
 */

#include "include/driver.h"
#include "include/driver_config.h"
#include "include/pci.h"
#include "include/usb.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/util.h"

#if ENABLE_DRIVER_MODEL

static char* drv_fmt_pci_name(char* buf, size_t cap,
                              uint16 seg, uint8 bus, uint8 dev, uint8 fn) {
    /* Turn %04x:%02x:%02x.%x into a name without depending on snprintf
       availability in the kernel; build it manually. */
    static const char hex[] = "0123456789abcdef";
    if (cap < 12) return buf;
    buf[0] = hex[(seg >> 12) & 0xF];
    buf[1] = hex[(seg >>  8) & 0xF];
    buf[2] = hex[(seg >>  4) & 0xF];
    buf[3] = hex[(seg >>  0) & 0xF];
    buf[4] = ':';
    buf[5] = hex[(bus >> 4) & 0xF];
    buf[6] = hex[(bus >> 0) & 0xF];
    buf[7] = ':';
    buf[8] = hex[(dev >> 4) & 0xF];
    buf[9] = hex[(dev >> 0) & 0xF];
    buf[10] = '.';
    buf[11] = hex[(fn >> 0) & 0xF];
    if (cap > 12) buf[12] = 0;
    return buf;
}

static bool drv_pci_enum_cb(const pci_device_t* pci, void* ctx) {
    (void)ctx;
    if (!pci) return true;

    drv_device_t* dev = (drv_device_t*)kmalloc(sizeof(drv_device_t));
    if (!dev) return true;
    memory_set((uint8*)dev, 0, sizeof(drv_device_t));

    drv_fmt_pci_name(dev->name, DRV_DEVICE_NAME_MAX,
                     pci->segment, pci->bus, pci->device, pci->function);
    dev->bus        = DRV_BUS_PCI;
    dev->vendor     = pci->vendor_id;
    dev->device_id  = pci->device_id;
    dev->class_code = pci->class_code;
    dev->subclass   = pci->subclass;
    dev->prog_if    = pci->prog_if;
    dev->bus_data   = (void*)pci;

    int rc = drv_bus_register(dev);
    if (rc < 0) {
        kfree(dev);
    }
    return true;  /* keep enumerating */
}

int drv_bus_enumerate_pci(void) {
    pci_enumerate(drv_pci_enum_cb, NULL);
    return 0;
}

int drv_bus_enumerate_usb(void) {
    /* The USB core keeps an array of registered host controllers and of
       enumerated USB devices; we surface each HC as a platform device and
       rely on usb.c's hotplug hook (added below) to surface full USB
       devices individually as they get addressed. The HC entries give us
       something to bind platform/USB-HC class drivers to. */
#ifdef ENABLE_USB
    for (uint32 i = 0; i < usb_get_hc_count(); i++) {
        usb_host_controller_t* hc = usb_get_hc(i);
        if (!hc) continue;

        drv_device_t* dev = (drv_device_t*)kmalloc(sizeof(drv_device_t));
        if (!dev) continue;
        memory_set((uint8*)dev, 0, sizeof(drv_device_t));

        const char* type = "usb-hc";
        memory_copy((char*)type, dev->name, 6);
        /* Append index as decimal in-name. */
        dev->name[6] = '0' + (char)(i % 10);
        dev->name[7] = 0;
        dev->bus      = DRV_BUS_PLATFORM;
        dev->bus_data = hc;
        int rc = drv_bus_register(dev);
        if (rc < 0) kfree(dev);
    }
#endif /* ENABLE_USB */
    return 0;
}

#else /* !ENABLE_DRIVER_MODEL */

int drv_bus_enumerate_pci(void) { return 0; }
int drv_bus_enumerate_usb(void) { return 0; }

#endif /* ENABLE_DRIVER_MODEL */