#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "../include/types.h"
#include "../include/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VirtIO network driver for MMIO transport (QEMU virt platform).
 *
 * On AArch64 and RISC-V, QEMU's "virt" machine exposes virtio devices
 * through MMIO at 0x0a000000 (and subsequent addresses for additional
 * devices).  This driver parses the FDT to locate the virtio-mmio node,
 * initializes the device via MMIO registers, sets up split virtqueues
 * for TX and RX, and registers as a net_nic_driver_t with the kernel.
 */

/* Maximum number of scatter-gather entries per virtqueue descriptor. */
#define VIRTIO_NET_SG_MAX       8

/* VirtIO MMIO default base address on QEMU virt. */
#define VIRTIO_MMIO_BASE_DEFAULT 0x0a000000u

/* VirtIO MMIO register offsets (legacy 0x100-byte region). */
#define VIRTIO_MMIO_MAGIC       0x000
#define VIRTIO_MMIO_VERSION     0x004
#define VIRTIO_MMIO_DEVICE_ID   0x008
#define VIRTIO_MMIO_VENDOR_ID   0x00c
#define VIRTIO_MMIO_DEVICE_FEAT 0x010
#define VIRTIO_MMIO_DRIVER_FEAT 0x020
#define VIRTIO_MMIO_QUEUE_SEL   0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM   0x044
#define VIRTIO_MMIO_QUEUE_ALIGN 0x048
#define VIRTIO_MMIO_QUEUE_PFN   0x050
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTR_STATUS 0x060
#define VIRTIO_MMIO_INTR_ACK   0x064
#define VIRTIO_MMIO_STATUS     0x070
#define VIRTIO_MMIO_CONFIG     0x100

/* VirtIO device status bits. */
#define VIRTIO_STATUS_ACK       0x01
#define VIRTIO_STATUS_DRIVER    0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEAT_OK   0x08
#define VIRTIO_STATUS_FAILED    0x80

/* VirtIO feature bits for network devices. */
#define VIRTIO_NET_F_MAC        (1u << 5)
#define VIRTIO_NET_F_STATUS     (1u << 16)
#define VIRTIO_F_NOTIFY_ON_EMPTY (1u << 24)

/* VirtIO split virtqueue descriptor flags. */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

/* Number of descriptors per virtqueue. Must be a power of 2. */
#define VIRTIO_NET_VQ_SIZE      256

/* VirtIO network header prepended to each TX/RX packet. */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    /* header_len is only present when VIRTIO_F_VERSION_1 is negotiated;
     * we treat it as reserved for legacy transport. */
} virtio_net_hdr_t;

/* Vring descriptor (16 bytes, per VirtIO spec). */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

/* Vring available ring. */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_NET_VQ_SIZE];
} vring_avail_t;

/* Vring used element. */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

/* Vring used ring. */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[VIRTIO_NET_VQ_SIZE];
} vring_used_t;

/* Per-virtqueue state. */
typedef struct {
    vring_desc_t*   desc;
    vring_avail_t*  avail;
    vring_used_t*   used;
    uint16_t        num_free;
    uint16_t        free_head;
    uint16_t        last_used_idx;
    uint8_t         queue_notify_off;
    /* Pre-allocated buffers for TX/RX data. */
    uint8_t         buf[VIRTIO_NET_VQ_SIZE][1520];
} virtio_vq_t;

/* Overall virtio-net device state. */
typedef struct {
    volatile uint32_t*  mmio_base;   /* MMIO register base (virtual address) */
    uint32_t            mmio_phys;   /* MMIO register base (physical address) */
    uint32_t            irq;
    uint8_t             mac[6];
    bool                initialized;

    /* Feature negotiation. */
    uint32_t            host_features;
    uint32_t            guest_features;

    /* Two virtqueues: 0=TX, 1=RX. */
    virtio_vq_t         vq[2];

    /* Aligned memory for vring structures (page-aligned). */
    uint8_t             vring_mem[2][4096 * 3];
} virtio_net_state_t;

/* Initialize the virtio-net device.
 * Parses FDT to find virtio-mmio node, maps MMIO, negotiates features,
 * sets up virtqueues, and registers as net_nic_driver_t.
 * Returns 0 on success, -1 on failure. */
int virtio_net_init(void);

/* Send a raw Ethernet frame through virtio-net. */
int virtio_net_send(const uint8_t* data, uint32_t len);

/* Receive a raw Ethernet frame from virtio-net.
 * Returns number of bytes received, or -1 if nothing available. */
int virtio_net_recv(uint8_t* buf, uint32_t max_len, uint32_t* out_len);

/* Get the MAC address of the virtio-net device. */
void virtio_net_get_mac(uint8_t mac[6]);

/* Handle a virtio-net interrupt (call from IRQ handler). */
void virtio_net_irq_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_NET_H */
