#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"
#include "pci.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ENABLE_VIRTIO
#  define ENABLE_VIRTIO 1
#endif

/* Virtio PCI vendor. */
#define VIRTIO_PCI_VENDOR          0x1AF4

/* Subsystem / device class codes. */
#define VIRTIO_CLASS_NETWORK       0x00
#define VIRTIO_CLASS_BLOCK         0x01
#define VIRTIO_CLASS_CONSOLE       0x03
#define VIRTIO_CLASS_BALLOON       0x05
#define VIRTIO_CLASS_INPUT         0x09
#define VIRTIO_CLASS_GPU           0x10

/* Transitional device IDs (0x1000..0x103F). */
#define VIRTIO_TRANSITIONAL(dev_id)  ((dev_id) >= 0x1000 && (dev_id) <= 0x103F)
/* Modern (1.0+) device IDs (0x1040..0x107F). */
#define VIRTIO_MODERN(dev_id)        ((dev_id) >= 0x1040 && (dev_id) <= 0x107F)

/* Virtio device id -> virtio subclass translation. */
#define VIRTIO_ID_FROM_PCI(dev_id)   \
    (VIRTIO_TRANSITIONAL(dev_id) ? ((dev_id) & 0xFF) \
                                 : ((dev_id) - 0x1040))

/* Legacy register offsets (within BAR0, IO space). */
#define VIRTIO_LEG_HOST_FEATURES     0
#define VIRTIO_LEG_GUEST_FEATURES    4
#define VIRTIO_LEG_QUEUE_PFN         8
#define VIRTIO_LEG_QUEUE_NOTIFY     12
#define VIRTIO_LEG_QUEUE_SEL        14
#define VIRTIO_LEG_QUEUE_NUM        16
#define VIRTIO_LEG_DEVICE_STATUS    18
#define VIRTIO_LEG_ISR_STATUS       19

/* Status bits. */
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_NEEDS_RESET  0x40
#define VIRTIO_STATUS_FAILED        0x80

typedef enum {
    VIRTIO_TRANSPORT_LEGACY = 0,
    VIRTIO_TRANSPORT_MODERN = 1
} virtio_transport_t;

typedef enum {
    VIRTIO_DEV_NONE = 0,
    VIRTIO_DEV_NET,
    VIRTIO_DEV_BLK,
    VIRTIO_DEV_CONSOLE,
    VIRTIO_DEV_BALLOON,
    VIRTIO_DEV_INPUT,
    VIRTIO_DEV_GPU,
    VIRTIO_DEV_UNKNOWN
} virtio_kind_t;

typedef struct virtio_device {
    pci_device_t      pci;
    virtio_transport_t transport;
    virtio_kind_t     kind;
    uint32_t          bar_io;       /* selected legacy bar IO addr or 0 */
    uint32_t          bar_mmio;     /* MMIO bar (modern) or 0 */
    uint32_t          features_host;
    uint32_t          features_guest;
    bool              registered;
} virtio_device_t;

/* Driver-model integration (read-only registration). */
int  virtio_pci_probe(void);
int  virtio_device_init(virtio_device_t *dev);
uint64_t virtio_get_features(virtio_device_t *dev);
int  virtio_set_features(virtio_device_t *dev, uint64_t feats);
int  virtio_vq_init(virtio_device_t *dev, uint16 vq_index,
                    void *ring, uint32 ring_size_pages);

/* Enumerate & report via debug log. */
void virtio_enumerate(void);

/* Number of virtio devices discovered by the last probe. */
uint32 virtio_count(void);

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_H */