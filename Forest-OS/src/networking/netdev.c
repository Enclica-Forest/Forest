#include "netdev.h"
#include "network.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/pci.h"

#include "e1000.h"
#include "rtl8139.h"
#include "ne2000.h"

void network_get_mac_address(uint8_t* mac);
void network_send_packet(uint8_t* data, size_t len);
void network_set_mac_address(uint8_t* mac);
void network_set_netdev_send_callback(int (*callback)(uint8_t*, size_t));
void network_set_initialized(int initialized);

uint16_t pci_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void* malloc(size_t nbytes);
void free(void* ptr);
int strcmp(const char* s1, const char* s2);
void* memset(void* s, int c, size_t n);
char* strcpy(char* dest, const char* src);

static netdev_t* g_netdev_list = NULL;
static netdev_t* g_active_dev = NULL;

int netdev_register(netdev_t* dev) {
    if (!dev) {
        return -1;
    }
    
    dev->next = g_netdev_list;
    g_netdev_list = dev;
    
    debug_print("NETDEV: Registered %s\n", dev->name);
    
    return 0;
}

int netdev_unregister(netdev_t* dev) {
    netdev_t** current = &g_netdev_list;
    
    while (*current) {
        if (*current == dev) {
            *current = dev->next;
            return 0;
        }
        current = &(*current)->next;
    }
    
    return -1;
}

netdev_t* netdev_find_by_name(char* name) {
    netdev_t* current = g_netdev_list;
    
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

netdev_t* netdev_find_by_type(netdev_type_t type) {
    netdev_t* current = g_netdev_list;
    
    while (current) {
        if (current->type == type) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

void netdev_list_all(void) {
    netdev_t* current = g_netdev_list;
    
    debug_print("NETDEV: Available network devices:\n");
    
    while (current) {
        debug_print("  - %s (type: %d)\n", current->name, current->type);
        current = current->next;
    }
}

static int pci_probe_network_devices(void) {
    debug_print("NETDEV: Scanning PCI for network devices...\n");
    
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor_id = pci_config_read_word(bus, slot, func, 0);
                uint16_t device_id = pci_config_read_word(bus, slot, func, 2);
                
                if (vendor_id == 0xFFFF) {
                    continue;
                }
                
                netdev_t* dev = malloc(sizeof(netdev_t));
                if (!dev) {
                    continue;
                }
                
                memset(dev, 0, sizeof(netdev_t));
                
                if (vendor_id == E1000_VENDOR_ID && device_id == E1000_DEVICE_ID) {
                    strcpy(dev->name, "e1000");
                    dev->type = NETDEV_TYPE_E1000;
                } else if (vendor_id == RTL8139_VENDOR_ID && device_id == RTL8139_DEVICE_ID) {
                    strcpy(dev->name, "rtl8139");
                    dev->type = NETDEV_TYPE_RTL8139;
                } else if (vendor_id == NE2000_VENDOR_ID && device_id == NE2000_DEVICE_ID) {
                    strcpy(dev->name, "ne2000");
                    dev->type = NETDEV_TYPE_NE2000;
                } else {
                    free(dev);
                    continue;
                }
                
                uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
                
                if ((bar0 & 0x01) == 0x00) {
                    dev->io_base = bar0 & ~3;
                } else {
                    dev->mem_base = bar0 & ~0xF;
                }
                
                uint16_t status = pci_config_read_word(bus, slot, func, 4);
                uint8_t irq_line = (status >> 8) & 0x0F;
                dev->irq = IRQ0 + irq_line;
                
                pci_config_write_word(bus, slot, func, 4, status | 0x04);
                
                debug_print("NETDEV: Found %s at %04X:%04X:%04X:%04X (IRQ %d)\n",
                         dev->name, bus, slot, func, dev->irq);
                
                switch (dev->type) {
                    case NETDEV_TYPE_E1000:
                        e1000_probe(dev);
                        break;
                    case NETDEV_TYPE_RTL8139:
                        rtl8139_probe(dev);
                        break;
                    case NETDEV_TYPE_NE2000:
                        ne2000_probe(dev);
                        break;
                }
                
                if (dev->init && dev->init(dev) == 0) {
                    netdev_register(dev);
                    
                    if (!g_active_dev) {
                        g_active_dev = dev;
                    }
                } else {
                    free(dev);
                }
            }
        }
    }
    
    return 0;
}

int netdev_init(void) {
    debug_print("NETDEV: Initializing...\n");
    
    g_netdev_list = NULL;
    g_active_dev = NULL;
    
    pci_probe_network_devices();
    
    if (g_active_dev) {
        network_set_mac_address(g_active_dev->mac);
        network_set_netdev_send_callback(g_active_dev->send);
        network_set_initialized(1);
        
        debug_print("NETDEV: Using %s as primary network device\n", g_active_dev->name);
    } else {
        debug_print("NETDEV: No network device found\n");
    }
    
    debug_print("NETDEV: Initialized\n");
    
    return 0;
}

int netdev_send_packet(uint8_t* data, size_t len) {
    if (g_active_dev && g_active_dev->initialized && g_active_dev->send) {
        int rc = g_active_dev->send(g_active_dev, data, len);
        if (rc >= 0) {
            g_active_dev->stats.tx_packets++;
            g_active_dev->stats.tx_bytes += (uint32)len;
        } else {
            g_active_dev->stats.tx_errors++;
        }
        return rc;
    }
    return -1;
}

void netdev_handle_irq(uint8_t irq) {
    if (g_active_dev && g_active_dev->initialized && g_active_dev->irq_handler) {
        g_active_dev->irq_handler(g_active_dev);
    }
}

void netdev_record_rx(netdev_t* dev, size_t len) {
    if (!dev) {
        return;
    }
    dev->stats.rx_packets++;
    dev->stats.rx_bytes += (uint32)len;
}

int32 net_get_if_stats(const char* ifname, net_stats_t* out) {
    if (!ifname || !out) {
        return -22;
    }
    memset(out, 0, sizeof(net_stats_t));
    netdev_t* dev = netdev_find_by_name((char*)ifname);
    if (!dev) {
        return -19;
    }
    memcpy(out, &dev->stats, sizeof(net_stats_t));
    return 0;
}
