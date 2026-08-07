/*
 * virtio_net.c - VirtIO network driver for MMIO transport
 *
 * Targets AArch64 and RISC-V QEMU virt platforms where virtio devices
 * are exposed via memory-mapped I/O (MMIO) at 0x0a000000+.
 *
 * This driver:
 *   1. Parses the FDT to locate the virtio-mmio node
 *   2. Reads the MMIO base address from the "reg" property
 *   3. Initializes the VirtIO device via MMIO registers
 *   4. Sets up split virtqueues for TX (queue 0) and RX (queue 1)
 *   5. Registers as a net_nic_driver_t with the kernel networking core
 *
 * On x86, PCI-based virtio is handled by the existing virtio.c / netdev.c.
 */

#include "virtio_net.h"
#include "fdt.h"
#include "arch/arch.h"
#include "include/net.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/system.h"
#include "include/spinlock.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

static virtio_net_state_t g_virtio_net;
static net_nic_driver_t   g_virtio_nic;
static bool               g_nic_registered = false;
static spinlock_t         g_vq_lock = SPINLOCK_INIT("virtio_net");

/* ------------------------------------------------------------------ */
/* MMIO access helpers (with memory barriers)                          */
/* ------------------------------------------------------------------ */

static inline uint32_t vio_mmio_read(uint32_t offset)
{
    uint32_t val = mmio_read32((const void*)(uintptr_t)(g_virtio_net.mmio_phys + offset));
    return val;
}

static inline void vio_mmio_write(uint32_t offset, uint32_t val)
{
    mmio_write32((volatile void*)(uintptr_t)(g_virtio_net.mmio_phys + offset), val);
}

/* ------------------------------------------------------------------ */
/* Virtqueue management                                                */
/* ------------------------------------------------------------------ */

static void vq_init(virtio_vq_t* vq)
{
    /* Build the free descriptor list. */
    vq->num_free = VIRTIO_NET_VQ_SIZE;
    vq->free_head = 0;
    vq->last_used_idx = 0;

    /* Chain all descriptors into the free list. */
    for (uint16_t i = 0; i < VIRTIO_NET_VQ_SIZE - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[VIRTIO_NET_VQ_SIZE - 1].next = 0xFFFF; /* end sentinel */

    /* Zero out available and used rings. */
    memory_set((uint8_t*)vq->avail, 0, sizeof(vring_avail_t));
    memory_set((uint8_t*)vq->used, 0, sizeof(vring_used_t));
}

static uint16_t vq_alloc_desc(virtio_vq_t* vq)
{
    if (vq->num_free == 0)
        return 0xFFFF;

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void vq_free_desc(virtio_vq_t* vq, uint16_t idx)
{
    vq->desc[idx].next = vq->free_head;
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = 0;
    vq->free_head = idx;
    vq->num_free++;
}

static void vq_submit(virtio_vq_t* vq, uint16_t head_idx, uint16_t queue_idx)
{
    /* Add descriptor chain to available ring. */
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % VIRTIO_NET_VQ_SIZE] = head_idx;
    /* Memory barrier before updating index. */
    arch_wmb();
    vq->avail->idx = avail_idx + 1;

    /* Notify the device. */
    vio_mmio_write(VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
}

/* ------------------------------------------------------------------ */
/* TX path                                                             */
/* ------------------------------------------------------------------ */

int virtio_net_send(const uint8_t* data, uint32_t len)
{
    if (!g_virtio_net.initialized || !data || len == 0 || len > 1518)
        return -1;

    spinlock_acquire(&g_vq_lock);
    virtio_vq_t* vq = &g_virtio_net.vq[0]; /* TX queue */

    if (vq->num_free < 2) {
        spinlock_release(&g_vq_lock);
        return -1; /* not enough descriptors */
    }

    /* Allocate descriptor for virtio-net header. */
    uint16_t hdr_desc = vq_alloc_desc(vq);
    if (hdr_desc == 0xFFFF) {
        spinlock_release(&g_vq_lock);
        return -1;
    }

    /* Allocate descriptor for packet data. */
    uint16_t data_desc = vq_alloc_desc(vq);
    if (data_desc == 0xFFFF) {
        vq_free_desc(vq, hdr_desc);
        spinlock_release(&g_vq_lock);
        return -1;
    }

    /* Prepare the virtio-net header (zero flags = no GSO, no checksum offload). */
    virtio_net_hdr_t* hdr = (virtio_net_hdr_t*)vq->buf[hdr_desc];
    memory_set((uint8_t*)hdr, 0, sizeof(virtio_net_hdr_t));

    /* Copy packet data into the vring buffer. */
    memory_copy((const char*)data, (char*)vq->buf[data_desc], len);

    /* Set up descriptor chain: header -> data. */
    vq->desc[hdr_desc].addr = (uint64_t)(uintptr_t)vq->buf[hdr_desc];
    vq->desc[hdr_desc].len  = sizeof(virtio_net_hdr_t);
    vq->desc[hdr_desc].flags = VRING_DESC_F_NEXT;
    vq->desc[hdr_desc].next  = data_desc;

    vq->desc[data_desc].addr = (uint64_t)(uintptr_t)vq->buf[data_desc];
    vq->desc[data_desc].len  = len;
    vq->desc[data_desc].flags = 0; /* last in chain */

    /* Submit to the device. */
    vq_submit(vq, hdr_desc, 0);

    spinlock_release(&g_vq_lock);
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* RX path                                                             */
/* ------------------------------------------------------------------ */

static void refill_rx_queue(void)
{
    virtio_vq_t* vq = &g_virtio_net.vq[1]; /* RX queue */

    while (vq->num_free > 0) {
        uint16_t desc = vq_alloc_desc(vq);
        if (desc == 0xFFFF)
            break;

        /* Set up the descriptor to receive into the pre-allocated buffer. */
        vq->desc[desc].addr = (uint64_t)(uintptr_t)vq->buf[desc];
        vq->desc[desc].len  = 1520;
        vq->desc[desc].flags = VRING_DESC_F_WRITE;
        vq->desc[desc].next  = 0;

        /* Add to available ring. */
        uint16_t avail_idx = vq->avail->idx;
        vq->avail->ring[avail_idx % VIRTIO_NET_VQ_SIZE] = desc;
        arch_wmb();
        vq->avail->idx = avail_idx + 1;
    }

    /* Notify device that new RX buffers are available. */
    vio_mmio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 1);
}

int virtio_net_recv(uint8_t* buf, uint32_t max_len, uint32_t* out_len)
{
    if (!g_virtio_net.initialized || !buf || max_len == 0)
        return -1;

    spinlock_acquire(&g_vq_lock);
    virtio_vq_t* vq = &g_virtio_net.vq[1]; /* RX queue */

    /* Check if there's a used buffer. */
    arch_rmb();
    if (vq->used->idx == vq->last_used_idx) {
        spinlock_release(&g_vq_lock);
        return -1; /* nothing received yet */
    }

    uint16_t used_idx = vq->last_used_idx % VIRTIO_NET_VQ_SIZE;
    vring_used_elem_t* elem = &vq->used->ring[used_idx];
    uint16_t desc_idx = (uint16_t)elem->id;
    uint32_t total_len = elem->len;

    /* The received data includes the virtio-net header + Ethernet frame.
     * Skip the header (sizeof(virtio_net_hdr_t)). */
    uint32_t hdr_size = sizeof(virtio_net_hdr_t);
    uint32_t frame_len = (total_len > hdr_size) ? (total_len - hdr_size) : 0;

    if (frame_len > max_len)
        frame_len = max_len;

    if (frame_len > 0) {
        memory_copy((const char*)(vq->buf[desc_idx] + hdr_size),
                    (char*)buf, frame_len);
    }

    if (out_len)
        *out_len = frame_len;

    /* Return the descriptor to the free list. */
    vq_free_desc(vq, desc_idx);
    vq->last_used_idx++;

    /* Refill the RX queue with fresh buffers. */
    refill_rx_queue();

    spinlock_release(&g_vq_lock);
    return (int)frame_len;
}

/* ------------------------------------------------------------------ */
/* MAC address                                                         */
/* ------------------------------------------------------------------ */

void virtio_net_get_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++)
        mac[i] = g_virtio_net.mac[i];
}

/* ------------------------------------------------------------------ */
/* Interrupt handler                                                   */
/* ------------------------------------------------------------------ */

void virtio_net_irq_handler(void)
{
    /* Read and acknowledge the interrupt. */
    uint32_t isr = vio_mmio_read(VIRTIO_MMIO_INTR_STATUS);
    vio_mmio_write(VIRTIO_MMIO_INTR_ACK, isr);

    /* For now, just log. A production driver would process RX completions here. */
    if (isr & 1) {
        /* Used buffer notification - RX packets available. */
    }
}

/* ------------------------------------------------------------------ */
/* Device initialization                                              */
/* ------------------------------------------------------------------ */

static int negotiate_features(void)
{
    g_virtio_net.host_features = vio_mmio_read(VIRTIO_MMIO_DEVICE_FEAT);

    /* We accept: MAC address, status, notify-on-empty. */
    uint32_t desired = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_F_NOTIFY_ON_EMPTY;
    g_virtio_net.guest_features = g_virtio_net.host_features & desired;

    vio_mmio_write(VIRTIO_MMIO_DRIVER_FEAT, g_virtio_net.guest_features);
    return 0;
}

static int setup_virtqueue(int index)
{
    virtio_vq_t* vq = &g_virtio_net.vq[index];

    /* Select this queue. */
    vio_mmio_write(VIRTIO_MMIO_QUEUE_SEL, (uint32_t)index);

    /* Check queue size. */
    uint32_t qsize = vio_mmio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qsize == 0) {
        debuglog(DEBUG_WARN, "VIRTIO-NET: VQ %d not present\n", index);
        return -1;
    }
    if (qsize > VIRTIO_NET_VQ_SIZE)
        qsize = VIRTIO_NET_VQ_SIZE;

    /* Allocate and align vring structures in the pre-allocated memory.
     * Layout: desc (4096-aligned) | avail (after desc, page-aligned) | used (after avail, page-aligned). */
    uint8_t* base = g_virtio_net.vring_mem[index];

    /* Descriptor table: 16 bytes * qsize, 16-byte aligned. */
    uint32_t desc_size = sizeof(vring_desc_t) * qsize;
    uint32_t avail_size = sizeof(vring_avail_t);
    uint32_t used_offset = (sizeof(vring_avail_t) + 4095) & ~4095u; /* page-align used ring */

    vq->desc  = (vring_desc_t*)(base);
    vq->avail = (vring_avail_t*)(base + desc_size);
    vq->used  = (vring_used_t*)(base + used_offset);

    /* Zero the structures. */
    memory_set(base, 0, used_offset + sizeof(vring_used_t));

    /* Initialize the free descriptor list. */
    vq_init(vq);

    /* Tell the device the physical page number of the vring. */
    uint64_t paddr = (uint64_t)(uintptr_t)base;
    uint32_t pfn = (uint32_t)(paddr >> 12);
    vio_mmio_write(VIRTIO_MMIO_QUEUE_PFN, pfn);

    debuglog(DEBUG_INFO, "VIRTIO-NET: VQ %d size=%u pfn=0x%x free=%u\n",
             index, qsize, pfn, vq->num_free);
    return 0;
}

static int read_mac_from_device(void)
{
    if (g_virtio_net.host_features & VIRTIO_NET_F_MAC) {
        /* MAC is available at MMIO config offset 0. */
        volatile uint8_t* cfg = (volatile uint8_t*)(uintptr_t)
            (g_virtio_net.mmio_phys + VIRTIO_MMIO_CONFIG);
        for (int i = 0; i < 6; i++)
            g_virtio_net.mac[i] = cfg[i];
        return 0;
    }

    /* No MAC feature: generate a random-ish MAC with the locally-administered bit set. */
    g_virtio_net.mac[0] = 0x52;
    g_virtio_net.mac[1] = 0x54;
    g_virtio_net.mac[2] = 0x00;
    g_virtio_net.mac[3] = 0x12;
    g_virtio_net.mac[4] = 0x34;
    g_virtio_net.mac[5] = 0x56;
    return 0;
}

/* ------------------------------------------------------------------ */
/* FDT discovery (AArch64 / RISC-V)                                   */
/* ------------------------------------------------------------------ */

static int discover_from_fdt(void)
{
#if ARCH_ARM64 || ARCH_RISCV64
    /* QEMU virt typically has: /soc/virtio_mmio@0a000000 */
    const char* paths[] = {
        "/soc/virtio_mmio@0a000000",
        "/soc/virtio_mmio@0a000800",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        const void* node = fdt_find_node(paths[i]);
        if (!node)
            continue;

        /* Read the "reg" property (16 bytes: 8-byte addr + 8-byte size for #address-cells=2, #size-cells=2). */
        uint32_t prop_len = 0;
        const uint32_t* reg = (const uint32_t*)fdt_get_property(paths[i], "reg", &prop_len);
        if (!reg || prop_len < 8)
            continue;

        /* For AArch64/RISC-V QEMU virt, reg is typically:
         * - 2 cells address (big-endian) + 2 cells size
         * We read the first 4 bytes of address (handles up to 4GB). */
        uint32_t addr_hi = fdt32_to_cpu(reg[0]);
        uint32_t addr_lo = fdt32_to_cpu(reg[1]);
        (void)addr_hi;
        uint32_t mmio_phys = addr_lo;

        /* Read interrupt. */
        uint32_t irq = 0;
        const uint32_t* int_prop = (const uint32_t*)fdt_get_property(paths[i], "interrupts", &prop_len);
        if (int_prop && prop_len >= 4) {
            /* GIC SPI interrupts: first cell = flags, second cell = SPI number. */
            if (prop_len >= 8) {
                irq = fdt32_to_cpu(int_prop[1]);
            } else {
                irq = fdt32_to_cpu(int_prop[0]);
            }
        }

        debuglog(DEBUG_INFO, "VIRTIO-NET: FDT node %s at 0x%x irq=%u\n",
                 paths[i], mmio_phys, irq);

        g_virtio_net.mmio_phys = mmio_phys;
        g_virtio_net.irq = (uint16_t)irq;
        return 0;
    }
#endif
    return -1;
}

static int discover_from_default(void)
{
    /* Fallback: try the default MMIO base address. */
    volatile uint32_t* test = (volatile uint32_t*)(uintptr_t)VIRTIO_MMIO_BASE_DEFAULT;
    uint32_t magic = mmio_read32((const void*)test);
    if (magic == 0x74726976) { /* "virt" in little-endian */
        g_virtio_net.mmio_phys = VIRTIO_MMIO_BASE_DEFAULT;
        g_virtio_net.irq = 0;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* net_nic_driver_t callbacks                                          */
/* ------------------------------------------------------------------ */

static int virtio_nic_probe(net_nic_driver_t* self)
{
    (void)self;
    /* Device discovery and MMIO init are done in virtio_net_init(). */
    return g_virtio_net.initialized ? 0 : -1;
}

static int virtio_nic_tx(net_nic_driver_t* self, const void* frame, uint32_t len)
{
    (void)self;
    return virtio_net_send((const uint8_t*)frame, len);
}

static int virtio_nic_rx(net_nic_driver_t* self, void* frame, uint32_t cap, uint32_t* out_len)
{
    (void)self;
    return virtio_net_recv((uint8_t*)frame, cap, out_len);
}

static void virtio_nic_get_mac(net_nic_driver_t* self, uint8_t mac[6])
{
    (void)self;
    virtio_net_get_mac(mac);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int virtio_net_init(void)
{
    if (g_virtio_net.initialized)
        return 0;

    memory_set((uint8_t*)&g_virtio_net, 0, sizeof(g_virtio_net));

    /* Step 1: Discover the MMIO device. */
    if (discover_from_fdt() != 0 && discover_from_default() != 0) {
        debuglog(DEBUG_WARN, "VIRTIO-NET: No virtio-mmio device found\n");
        return -1;
    }

    /* Verify magic value. */
    uint32_t magic = vio_mmio_read(VIRTIO_MMIO_MAGIC);
    if (magic != 0x74726976) { /* "virt" */
        debuglog(DEBUG_ERROR, "VIRTIO-NET: Bad MMIO magic 0x%x at 0x%x\n",
                 magic, g_virtio_net.mmio_phys);
        return -1;
    }

    uint32_t dev_id = vio_mmio_read(VIRTIO_MMIO_DEVICE_ID);
    if (dev_id != 1) { /* device ID 1 = network */
        debuglog(DEBUG_WARN, "VIRTIO-NET: MMIO device at 0x%x is not network (id=%u)\n",
                 g_virtio_net.mmio_phys, dev_id);
        return -1;
    }

    debuglog(DEBUG_INFO, "VIRTIO-NET: Found at 0x%x, version=%u\n",
             g_virtio_net.mmio_phys,
             vio_mmio_read(VIRTIO_MMIO_VERSION));

    /* Step 2: Reset device. */
    vio_mmio_write(VIRTIO_MMIO_STATUS, 0);

    /* Step 3: ACK + DRIVER status. */
    vio_mmio_write(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    vio_mmio_write(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Step 4: Feature negotiation. */
    negotiate_features();

    /* Step 5: Set up TX virtqueue (queue 0). */
    if (setup_virtqueue(0) != 0) {
        debuglog(DEBUG_ERROR, "VIRTIO-NET: Failed to set up TX virtqueue\n");
        vio_mmio_write(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Step 6: Set up RX virtqueue (queue 1) and pre-fill with buffers. */
    if (setup_virtqueue(1) != 0) {
        debuglog(DEBUG_ERROR, "VIRTIO-NET: Failed to set up RX virtqueue\n");
        vio_mmio_write(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    refill_rx_queue();

    /* Step 7: Read MAC address. */
    read_mac_from_device();
    debuglog(DEBUG_INFO, "VIRTIO-NET: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_virtio_net.mac[0], g_virtio_net.mac[1], g_virtio_net.mac[2],
             g_virtio_net.mac[3], g_virtio_net.mac[4], g_virtio_net.mac[5]);

    /* Step 8: DRIVER_OK - device is live. */
    vio_mmio_write(VIRTIO_MMIO_STATUS,
                   VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_virtio_net.initialized = true;

    /* Step 9: Register with the kernel NIC layer. */
    memory_set((uint8_t*)&g_virtio_nic, 0, sizeof(g_virtio_nic));
    strncpy(g_virtio_nic.name, "virtio-net", NET_IFNAME_LEN - 1);
    g_virtio_nic.probe     = virtio_nic_probe;
    g_virtio_nic.tx        = virtio_nic_tx;
    g_virtio_nic.rx        = virtio_nic_rx;
    g_virtio_nic.get_mac   = virtio_nic_get_mac;
    g_virtio_nic.irq       = g_virtio_net.irq;
    memory_copy((const char*)g_virtio_net.mac, (char*)g_virtio_nic.mac, 6);

    net_register_nic(&g_virtio_nic);
    g_nic_registered = true;

    debuglog(DEBUG_INFO, "VIRTIO-NET: Initialized and registered as NIC\n");
    return 0;
}
