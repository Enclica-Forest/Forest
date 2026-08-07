/*
 * virtio.c - baseline virtio PCI transport for Forest-OS
 *
 * Probe scope:
 *   * vendor 0x1AF4 with device id in 0x1000..0x103F (legacy transitional)
 *   * vendor 0x1AF4 with device id in 0x1040..0x107F (modern)
 *
 * We finish at read-only registration of virtio-net and virtio-blk discovery.
 * Actual I/O submission must come after the virtqueue ring allocator is wired
 * into the page allocator (follow-up work).
 */

#include "include/virtio.h"
#include "include/pci.h"
#include "include/io_ports.h"
#include "include/string.h"
#include "include/debuglog.h"

#define VIRTIO_MAX_DISCOVERED   32

static virtio_device_t g_virtio[VIRTIO_MAX_DISCOVERED];
static uint32          g_virtio_count = 0;

static virtio_kind_t classify(uint16 device_id) {
    uint32 v;
    if (VIRTIO_MODERN(device_id)) v = device_id - 0x1040;
    else                          v = device_id & 0xFF;
    switch (v) {
        case 0x00: return VIRTIO_DEV_NET;
        case 0x01: return VIRTIO_DEV_BLK;
        case 0x03: return VIRTIO_DEV_CONSOLE;
        case 0x05: return VIRTIO_DEV_BALLOON;
        case 0x09: return VIRTIO_DEV_INPUT;
        case 0x10: return VIRTIO_DEV_GPU;
        default:   return VIRTIO_DEV_UNKNOWN;
    }
}

static const char* kind_name(virtio_kind_t k) {
    switch (k) {
        case VIRTIO_DEV_NET:     return "net";
        case VIRTIO_DEV_BLK:     return "blk";
        case VIRTIO_DEV_CONSOLE: return "console";
        case VIRTIO_DEV_BALLOON: return "balloon";
        case VIRTIO_DEV_INPUT:   return "input";
        case VIRTIO_DEV_GPU:     return "gpu";
        default:                 return "unknown";
    }
}

static bool try_register(const pci_device_t *pci, void *ctx) {
    (void)ctx;
    if (pci->vendor_id != VIRTIO_PCI_VENDOR) return true;
    if (!VIRTIO_TRANSITIONAL(pci->device_id) && !VIRTIO_MODERN(pci->device_id))
        return true;
    if (g_virtio_count >= VIRTIO_MAX_DISCOVERED) return false;

    virtio_device_t *v = &g_virtio[g_virtio_count++];
    memset(v, 0, sizeof(*v));
    v->pci        = *pci;
    v->transport  = VIRTIO_MODERN(pci->device_id)
                    ? VIRTIO_TRANSPORT_MODERN : VIRTIO_TRANSPORT_LEGACY;
    v->kind      = classify(pci->device_id);

    /* Pickup legacy BAR0: bit 0 = IO space. We use the IO form. */
    uint32_t bar0 = pci->bar[0];
    if (bar0 & 0x1) {
        v->bar_io   = bar0 & ~0x3;
        v->bar_mmio = 0;
    } else {
        v->bar_mmio = bar0 & ~0xF;
        v->bar_io   = 0;
    }

    debuglog(DEBUG_INFO,
             "VIRTIO: %s %s at %04x:%02x:%02x.%x dev_id=%04x bar_io=%#x bar_mmio=%#x\n",
             v->transport == VIRTIO_TRANSPORT_MODERN ? "modern" : "legacy",
             kind_name(v->kind),
             pci->segment, pci->bus, pci->device, pci->function,
             pci->device_id, v->bar_io, v->bar_mmio);
    return true;
}

int virtio_pci_probe(void) {
#if ENABLE_VIRTIO
    g_virtio_count = 0;
    pci_init();
    pci_enumerate(try_register, NULL);

    /* Read-only registration with the driver model: net + blk only. */
    for (uint32 i = 0; i < g_virtio_count; i++) {
        virtio_device_t *v = &g_virtio[i];
        if (v->kind == VIRTIO_DEV_NET || v->kind == VIRTIO_DEV_BLK) {
            v->registered = true;
            debuglog(DEBUG_INFO,
                     "VIRTIO: registered %s device (%s transport)\n",
                     kind_name(v->kind),
                     v->transport == VIRTIO_TRANSPORT_MODERN ? "modern" : "legacy");
        }
    }
    return 0;
#else
    return -1;
#endif
}

uint32 virtio_count(void) { return g_virtio_count; }

int virtio_device_init(virtio_device_t *dev) {
#if ENABLE_VIRTIO
    if (!dev || dev->transport != VIRTIO_TRANSPORT_LEGACY || !dev->bar_io) {
        /* Modern transport requires MMIO + capability parsing - follow-up. */
        return -1;
    }
    /* Legacy reset & feature negotiation. */
    outportb(dev->bar_io + VIRTIO_LEG_DEVICE_STATUS, 0);
    outportb(dev->bar_io + VIRTIO_LEG_DEVICE_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE);
    outportb(dev->bar_io + VIRTIO_LEG_DEVICE_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    /* Read host features. */
    uint32_t host = inportd(dev->bar_io + VIRTIO_LEG_HOST_FEATURES);
    /* Mask to a safe subset (no coalescing / event_idx plumbing yet). */
    dev->features_host = host;
    dev->features_guest = host & 0x0; /* accept none for now */
    outportd(dev->bar_io + VIRTIO_LEG_GUEST_FEATURES, dev->features_guest);
    outportb(dev->bar_io + VIRTIO_LEG_DEVICE_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    return 0;
#else
    return -1;
#endif
}

uint64_t virtio_get_features(virtio_device_t *dev) {
    if (!dev) return 0;
    return dev->features_host;
}

int virtio_set_features(virtio_device_t *dev, uint64_t feats) {
#if ENABLE_VIRTIO
    if (!dev || dev->transport != VIRTIO_TRANSPORT_LEGACY || !dev->bar_io)
        return -1;
    /* Mask against host advertised features. */
    uint32_t masked = (uint32_t)feats & dev->features_host;
    dev->features_guest = masked;
    outportd(dev->bar_io + VIRTIO_LEG_GUEST_FEATURES, masked);
    return 0;
#else
    (void)dev; (void)feats;
    return -1;
#endif
}

int virtio_vq_init(virtio_device_t *dev, uint16 vq_index,
                   void *ring, uint32 ring_size_pages) {
#if ENABLE_VIRTIO
    if (!dev || !ring || dev->transport != VIRTIO_TRANSPORT_LEGACY || !dev->bar_io)
        return -1;
    /* Ask the host about the queue. */
    outportw(dev->bar_io + VIRTIO_LEG_QUEUE_SEL, vq_index);
    uint16 qsize = inportw(dev->bar_io + VIRTIO_LEG_QUEUE_NUM);
    if (qsize == 0) {
        debuglog(DEBUG_WARN, "VIRTIO: VQ %u not present\n", vq_index);
        return -1;
    }
    /* ring is provided pre-allocated & page-aligned by caller. Split ring
     * descriptor area is qsize*16 + driver/event areas. For now we just
     * record the PFN; the host won't notify until queue is ready. */
    (void)ring_size_pages;
    uint64_t paddr = (uint64_t)(uint32_t)ring;
    uint32_t pfn = (uint32_t)(paddr >> 12);
    outportd(dev->bar_io + VIRTIO_LEG_QUEUE_PFN, pfn);
    debuglog(DEBUG_INFO, "VIRTIO: VQ %u size=%u pfn=0x%x\n",
             vq_index, qsize, pfn);
    return qsize;
#else
    (void)dev; (void)vq_index; (void)ring; (void)ring_size_pages;
    return -1;
#endif
}

void virtio_enumerate(void) {
#if ENABLE_VIRTIO
    if (g_virtio_count == 0) virtio_pci_probe();
    for (uint32 i = 0; i < g_virtio_count; i++) {
        const virtio_device_t *v = &g_virtio[i];
        debuglog(DEBUG_INFO, "  [%u] %s %s registered=%d\n", i,
                 kind_name(v->kind),
                 v->transport == VIRTIO_TRANSPORT_MODERN ? "modern" : "legacy",
                 v->registered);
    }
#else
    debuglog(DEBUG_INFO, "VIRTIO: disabled\n");
#endif
}