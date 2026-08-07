#ifndef DRIVER_H
#define DRIVER_H

#include "types.h"
#include <stdbool.h>
#include "driver_config.h"

#define DRIVER_MAX_NAME_LEN 32
#define DRIVER_EVENT_PAYLOAD_MAX 64

typedef enum {
    DRIVER_CLASS_UNKNOWN = 0,
    DRIVER_CLASS_INPUT,
    DRIVER_CLASS_SOUND,
    DRIVER_CLASS_STORAGE,
    DRIVER_CLASS_NETWORK,
    DRIVER_CLASS_MISC
} driver_class_t;

// Common event codes that drivers may emit.
// Additional codes can be layered on top of these ranges.
#define DRIVER_EVENT_STATUS_READY      0x0001
#define DRIVER_EVENT_STATUS_FAILURE    0x0002
#define DRIVER_EVENT_NETWORK_RX_READY  0x0100

struct driver;
typedef struct driver driver_t;

typedef struct {
    uint16 driver_id;
    driver_class_t driver_class;
    uint32 code;
    uint32 payload_length;
    uint8 payload[DRIVER_EVENT_PAYLOAD_MAX];
} driver_event_t;

struct driver {
    const char* name;
    driver_class_t driver_class;
    bool (*init)(driver_t* driver);
    void (*shutdown)(driver_t* driver);
    void (*main)(void);  // Main driver task function
    void* context;
    uint16 id;
    bool initialized;
};

bool driver_manager_init(void);
bool driver_register(driver_t* driver);
driver_t* driver_find(const char* name);
bool driver_emit_event(uint16 driver_id, driver_class_t driver_class,
                       uint32 code, const void* payload, uint32 payload_length);
bool driver_event_pop(driver_event_t* out_event);
void driver_shutdown_all(void);

/* =================================================================== */
/* Unified driver/device model (driver_registry.c / driver_bus.c).     */
/*                                                                     */
/* The legacy API above (driver_register/driver_find/driver_event_*)    */
/* describes task-style background drivers and is kept intact here for  */
/* existing callers (e.g. the loopback driver in net.c).                */
/*                                                                     */
/* The structures below are a separate, additive model: a registry of   */
/* bus-attached driver modules plus an enumerated device list with      */
/* probe/bind, hotplug and power-management hooks. New code should use   */
/* the drv_* API.                                                      */
/* =================================================================== */

typedef enum {
    DRV_BUS_UNKNOWN = 0,
    DRV_BUS_PCI,        /* PCI/PCIe */
    DRV_BUS_USB,        /* USB device */
    DRV_BUS_ISA,        /* legacy ISA */
    DRV_BUS_PNP,        /* PnP ISA */
    DRV_BUS_PLATFORM,   /* platform / SoC / virtual */
    DRV_BUS_VIRTIO,     /* virtio */
    DRV_BUS_PS2,        /* PS/2 keyboard/mouse */
    DRV_BUS_SCSI        /* SCSI target behind another HBA */
} drv_bus_t;

/* driver_t / device_t wildcard match value for vendor/device ids. */
#define DRV_ID_ANY 0xFFFFu

/* Match flags in drv_id_t.flags. */
#define DRV_MATCH_VENDOR  0x0001u   /* match vendor + device */
#define DRV_MATCH_CLASS   0x0002u   /* match class (optionally subclass/prog_if) */
#define DRV_MATCH_SUBSYS  0x0004u   /* match subsystem vendor/device */
#define DRV_MATCH_ANY      0x0008u   /* match every device on this bus */

/* driver_t flags. */
#define DRV_FLAG_HOTPLUG  0x0001u   /* driver wants hotplug notifications */
#define DRV_FLAG_PM       0x0002u   /* driver implements suspend/resume */
#define DRV_FLAG_AUTOBIND 0x0004u   /* bind automatically when device appears */

/* id_table terminator: entry with match==0 and bus==DRV_BUS_UNKNOWN. */
#define DRV_ID_TABLE_END { 0, 0, 0, 0, DRV_BUS_UNKNOWN, 0, 0, 0, 0, 0 }

typedef struct drv_id {
    uint16 vendor;        /* PCI/USB vendor id, or DRV_ID_ANY */
    uint16 device;        /* PCI/USB device id, or DRV_ID_ANY */
    uint16 subvendor;     /* subsystem vendor, or DRV_ID_ANY */
    uint16 subdevice;     /* subsystem device, or DRV_ID_ANY */
    drv_bus_t bus;        /* bus this entry applies to */
    uint8  class_code;    /* base class, or 0xFF for wildcard */
    uint8  subclass;      /* subclass, or 0xFF for wildcard */
    uint8  prog_if;       /* programming interface, or 0xFF for wildcard */
    uint32 flags;         /* DRV_MATCH_* mask */
    uintptr_t driver_data;/* opaque per-match cookie the probe callback may read */
} drv_id_t;

struct drv_device;
typedef struct drv_device drv_device_t;
struct drv_driver;
typedef struct drv_driver drv_driver_t;

typedef enum {
    DEVICE_STATE_INIT = 0,
    DEVICE_STATE_BOUND,
    DEVICE_STATE_RUNNING,
    DEVICE_STATE_SUSPENDED,
    DEVICE_STATE_REMOVED
} drv_device_state_t;

#define DRV_DEVICE_NAME_MAX 64

struct drv_driver {
    const char* name;
    const char* version;
    drv_bus_t bus;
    const drv_id_t* id_table;            /* NULL or NULL-terminated list */
    int  (*probe)(drv_device_t* dev, const drv_id_t* id);
    void (*remove)(drv_device_t* dev);
    int  (*suspend)(drv_device_t* dev, int level);   /* level: 0..3 */
    int  (*resume)(drv_device_t* dev, int level);   /* level: 0..3 */
    int  (*ioctl)(drv_device_t* dev, uint32_t cmd, void* arg);
    uint32_t flags;                      /* DRV_FLAG_* */
    struct drv_driver* next;             /* registry link */
};

struct drv_device {
    char name[DRV_DEVICE_NAME_MAX];
    drv_bus_t bus;
    uint32_t id;                 /* unique registry id */
    uint16_t vendor;             /* PCI/USB vendor */
    uint16_t device_id;          /* PCI/USB device */
    uint16_t subvendor;          /* subsystem vendor */
    uint16_t subdevice;          /* subsystem device */
    uint8  class_code;           /* base class */
    uint8  subclass;
    uint8  prog_if;
    void* bus_data;              /* PCI cfg space ptr, USB dev ptr, etc. */
    void* driver_data;            /* driver-private */
    drv_driver_t* bound;          /* bound driver or NULL */
    int state;                    /* drv_device_state_t */
    int refcount;                 /* references held by drivers/userspace */
    struct drv_device* next;      /* registry link */
};

/* Initialisation. Called once at boot before first drv_register. */
bool drv_core_init(void);
void drv_core_shutdown(void);

/* Registry. */
int  drv_register(const drv_driver_t* drv);
void drv_unregister(const drv_driver_t* drv);

/* Bus enumerators / hotplug. */
int  drv_bus_register(drv_device_t* dev);   /* add device, attempt bind */
int  drv_bus_unregister(drv_device_t* dev); /* call remove, drop device */
int  drv_hotplug_add(drv_device_t* dev);
int  drv_hotplug_remove(drv_device_t* dev);

/* Bind / unbind (driver side). */
int  drv_bind(drv_device_t* dev);
void drv_unbind(drv_device_t* dev);

/* Power management. */
int  drv_suspend(drv_device_t* dev, int level);
int  drv_resume(drv_device_t* dev, int level);

/* Reference counting. */
drv_device_t* drv_device_get(drv_device_t* dev);
void          drv_device_put(drv_device_t* dev);

/* Lookup. */
drv_device_t* drv_device_get_by_name(const char* name);

/* Iteration. callback returns 0 to continue, non-zero to stop early.
   Returns the driver/device that stopped iteration, or NULL. */
const drv_driver_t* drv_iter_drivers(int (*cb)(const drv_driver_t*, void*), void* user);
const drv_device_t* drv_iter_devices(int (*cb)(const drv_device_t*, void*), void* user);

/* Re-scan: try to bind every unbound device. Returns number newly bound. */
int drv_rescan(void);

/* Bus-enumerator glue implemented in driver_bus.c. */
int  drv_bus_enumerate_pci(void);
int  drv_bus_enumerate_usb(void);

#endif
