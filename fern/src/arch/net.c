/*
 * net.c - Cross-architecture network initialization
 *
 * Provides arch_network_init() which detects the platform and initializes
 * the appropriate network driver:
 *   - x86: PCI-based NIC drivers (e1000, rtl8139, ne2000) via netdev_init()
 *   - AArch64/RISC-V: VirtIO-MMIO (QEMU virt) via virtio_net_init()
 *
 * Also provides the unified send/recv/get_mac wrappers that work across
 * all architectures.
 */

#include "arch/net.h"
#include "arch/arch.h"
#include "../include/net.h"
#include "../include/string.h"
#include "../include/memory.h"
#include "../include/util.h"
#include "../include/debuglog.h"

#if ARCH_X86_64 || ARCH_X86_32
/* x86 uses PCI-based netdev layer. */
#include "../networking/netdev.h"
#endif

#if ARCH_ARM64 || ARCH_RISCV64
/* AArch64/RISC-V use virtio-net MMIO. */
#include "../virtio_net.h"
#endif

/* Forward declarations for networking stack init (networking/network.c). */
void network_init(void);
void network_set_netdev_send_callback(int (*callback)(uint8_t*, size_t));
void network_set_mac_address(uint8_t* mac);

/* Forward declarations for netdev (x86 networking/netdev.h). */
void netdev_init(void);

/* Forward declarations for net_nic_driver_t access (include/net.h). */
typedef struct net_nic_driver net_nic_driver_t;
net_nic_driver_t* net_active_nic(void);

/* ------------------------------------------------------------------ */
/* Virtio-net send adapter for the networking stack callback.          */
/* ------------------------------------------------------------------ */

#if ARCH_ARM64 || ARCH_RISCV64
static int virtio_net_send_adapter(uint8_t* data, size_t len)
{
    return virtio_net_send(data, (uint32_t)len);
}
#endif

/* ------------------------------------------------------------------ */
/* Architecture-aware init                                            */
/* ------------------------------------------------------------------ */

int arch_network_init(void)
{
#if ARCH_X86_64 || ARCH_X86_32
    /* x86: use the existing PCI-based NIC probe. */
    debuglog(DEBUG_INFO, "NET: Initializing x86 PCI network devices\n");
    netdev_init();
    /* Initialize the higher-level networking stack (ARP, IP, TCP, UDP,
     * DHCP, DNS) so packets can flow through the NIC. */
    network_init();
    return 0;
#elif ARCH_ARM64 || ARCH_RISCV64
    /* AArch64 / RISC-V: use virtio-net MMIO. */
    debuglog(DEBUG_INFO, "NET: Initializing virtio-net MMIO\n");
    if (virtio_net_init() != 0) {
        debuglog(DEBUG_WARN, "NET: No virtio-net device found\n");
        return -1;
    }

    /* Hook virtio-net into the networking stack's send callback so
     * ARP/IP/TCP/UDP can transmit through the real NIC. */
    network_set_netdev_send_callback(virtio_net_send_adapter);

    /* Set the MAC address in the networking stack. */
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    network_set_mac_address(mac);

    /* Initialize the higher-level networking stack (ARP, IP, TCP, UDP,
     * DHCP, DNS) so packets can actually flow. */
    network_init();

    return 0;
#else
    debuglog(DEBUG_WARN, "NET: No network driver for this architecture\n");
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Unified send / recv / get_mac                                       */
/* ------------------------------------------------------------------ */

int arch_net_send(const uint8_t* data, uint32_t len)
{
    net_nic_driver_t* nic = net_active_nic();
    if (!nic || !nic->tx)
        return -1;

    int rc = nic->tx(nic, data, len);
    return rc;
}

int arch_net_recv(uint8_t* buf, uint32_t max_len, uint32_t* out_len)
{
    net_nic_driver_t* nic = net_active_nic();
    if (!nic || !nic->rx)
        return -1;

    return nic->rx(nic, buf, max_len, out_len);
}

void arch_net_get_mac(uint8_t mac[6])
{
    net_nic_driver_t* nic = net_active_nic();
    if (nic && nic->get_mac) {
        nic->get_mac(nic, mac);
    } else {
        memory_set((uint8_t*)mac, 0, 6);
    }
}
