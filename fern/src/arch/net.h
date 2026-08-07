#ifndef ARCH_NET_H
#define ARCH_NET_H

#include "../include/types.h"
#include "../include/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cross-architecture network interface.
 *
 * Provides a unified API that each architecture or platform implements
 * using the appropriate NIC driver:
 *   - x86: PCI-based drivers (e1000, rtl8139, ne2000) via networking/netdev.c
 *   - AArch64/RISC-V: VirtIO-MMIO (QEMU virt) via virtio_net.c
 *
 * The arch_network_init() function detects the platform's network device
 * and registers it with the kernel networking core (net_nic_driver_t).
 */

/* Initialize the architecture-appropriate network driver.
 * Called from net_init() after the socket layer is set up.
 * Returns 0 on success, -1 if no network device found. */
int arch_network_init(void);

/* Low-level send/receive through whichever NIC is active.
 * These wrap the active net_nic_driver_t's tx/rx callbacks. */
int  arch_net_send(const uint8_t* data, uint32_t len);
int  arch_net_recv(uint8_t* buf, uint32_t max_len, uint32_t* out_len);
void arch_net_get_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* ARCH_NET_H */
