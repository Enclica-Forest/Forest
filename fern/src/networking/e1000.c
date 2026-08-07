#include "e1000.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/io_ports.h"
#include "network.h"

void network_receive_packet(uint8_t* data, size_t len);
void* malloc(size_t nbytes);

#define E1000_RX_BUFFER_SIZE 8192
#define E1000_TX_BUFFER_SIZE 1518

static void e1000_write_reg(uint64_t base, uint16_t offset, uint32_t value) {
    *((volatile uint32_t*)(base + offset)) = value;
}

static uint32_t e1000_read_reg(uint64_t base, uint16_t offset) {
    return *((volatile uint32_t*)(base + offset));
}

static uint16_t e1000_eeprom_read(netdev_t* dev, uint8_t addr) {
    uint64_t base = dev->mem_base;
    
    e1000_write_reg(base, E1000_REG_EERD, (1) | (addr << 8));
    
    while ((e1000_read_reg(base, E1000_REG_EERD) & 0x10) == 0);
    
    return (uint16_t)((e1000_read_reg(base, E1000_REG_EERD) >> 16) & 0xFFFF);
}

static int e1000_detect_eeprom(netdev_t* dev) {
    uint64_t base = dev->mem_base;
    
    e1000_write_reg(base, E1000_REG_EECD, 0x1);
    
    for (int i = 0; i < 1000; i++) {
        if (e1000_read_reg(base, E1000_REG_EECD) & 0x10) {
            return 1;
        }
    }
    
    return 0;
}

static void e1000_read_mac(netdev_t* dev) {
    uint16_t mac_low = e1000_eeprom_read(dev, 0);
    uint16_t mac_mid = e1000_eeprom_read(dev, 1);
    uint16_t mac_high = e1000_eeprom_read(dev, 2);
    
    dev->mac[0] = mac_low & 0xFF;
    dev->mac[1] = (mac_low >> 8) & 0xFF;
    dev->mac[2] = mac_mid & 0xFF;
    dev->mac[3] = (mac_mid >> 8) & 0xFF;
    dev->mac[4] = mac_high & 0xFF;
    dev->mac[5] = (mac_high >> 8) & 0xFF;
    
    debug_print("E1000: MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
             dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);
}

static int e1000_init_rx(netdev_t* dev, e1000_rx_desc_t* rx_descs,
                       uint8_t* rx_buffers) {
    uint64_t base = dev->mem_base;
    
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_descs[i].addr = (uint64_t)((uint64_t)rx_buffers + i * E1000_RX_BUFFER_SIZE);
        rx_descs[i].length = 0;
        rx_descs[i].checksum = 0;
        rx_descs[i].status = 0;
        rx_descs[i].errors = 0;
        rx_descs[i].special = 0;
    }
    
    uint64_t rx_desc_phys = (uint64_t)rx_descs;
    e1000_write_reg(base, E1000_REG_RDBAL, rx_desc_phys & 0xFFFFFFFF);
    e1000_write_reg(base, E1000_REG_RDBAH, rx_desc_phys >> 32);
    e1000_write_reg(base, E1000_REG_RDLEN, E1000_NUM_RX_DESC * 16);
    e1000_write_reg(base, E1000_REG_RDH, 0);
    e1000_write_reg(base, E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
                    E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_8192;
    e1000_write_reg(base, E1000_REG_RCTL, rctl);
    
    return 0;
}

static int e1000_init_tx(netdev_t* dev, e1000_tx_desc_t* tx_descs,
                       uint8_t* tx_buffers) {
    uint64_t base = dev->mem_base;
    
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_descs[i].addr = (uint64_t)((uint64_t)tx_buffers + i * E1000_TX_BUFFER_SIZE);
        tx_descs[i].length = 0;
        tx_descs[i].cso = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = E1000_TX_STATUS_DD;
        tx_descs[i].css = 0;
        tx_descs[i].special = 0;
    }
    
    uint64_t tx_desc_phys = (uint64_t)tx_descs;
    e1000_write_reg(base, E1000_REG_TDBAL, tx_desc_phys & 0xFFFFFFFF);
    e1000_write_reg(base, E1000_REG_TDBAH, tx_desc_phys >> 32);
    e1000_write_reg(base, E1000_REG_TDLEN, E1000_NUM_TX_DESC * 16);
    e1000_write_reg(base, E1000_REG_TDH, 0);
    e1000_write_reg(base, E1000_REG_TDT, 0);
    
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (15 << E1000_TCTL_CT_SHIFT) | (64 << E1000_TCTL_COLD_SHIFT);
    e1000_write_reg(base, E1000_REG_TCTL, tctl);
    
    return 0;
}

typedef struct {
    e1000_rx_desc_t rx_descs[E1000_NUM_RX_DESC];
    e1000_tx_desc_t tx_descs[E1000_NUM_TX_DESC];
    uint8_t rx_buffers[E1000_NUM_RX_DESC * E1000_RX_BUFFER_SIZE];
    uint8_t tx_buffers[E1000_NUM_TX_DESC * E1000_TX_BUFFER_SIZE];
    uint16_t rx_next;
    uint16_t tx_next;
} e1000_private_t;

int e1000_init(netdev_t* dev) {
    debug_print("E1000: Initializing...\n");
    
    e1000_private_t* priv = malloc(sizeof(e1000_private_t));
    if (!priv) {
        return -1;
    }
    dev->private_data = priv;
    
    uint64_t base = dev->mem_base;
    
    debug_print("E1000: Resetting device...\n");
    e1000_write_reg(base, E1000_REG_CTRL, E1000_CTRL_RST);
    while (e1000_read_reg(base, E1000_REG_CTRL) & E1000_CTRL_RST);
    
    e1000_write_reg(base, E1000_REG_CTRL, 
                 E1000_CTRL_ASDE | E1000_CTRL_SLU);
    
    if (e1000_detect_eeprom(dev)) {
        e1000_read_mac(dev);
    } else {
        debug_print("E1000: EEPROM not detected, reading from register\n");
        uint32_t ral0 = e1000_read_reg(base, E1000_REG_RAL0);
        uint32_t rah0 = e1000_read_reg(base, E1000_REG_RAH0);
        
        dev->mac[0] = ral0 & 0xFF;
        dev->mac[1] = (ral0 >> 8) & 0xFF;
        dev->mac[2] = (ral0 >> 16) & 0xFF;
        dev->mac[3] = (ral0 >> 24) & 0xFF;
        dev->mac[4] = rah0 & 0xFF;
        dev->mac[5] = (rah0 >> 8) & 0xFF;
        
        debug_print("E1000: MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                 dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);
    }
    
    e1000_write_reg(base, E1000_REG_RAL0, 
                 dev->mac[0] | (dev->mac[1] << 8) | (dev->mac[2] << 16) | (dev->mac[3] << 24));
    e1000_write_reg(base, E1000_REG_RAH0, dev->mac[4] | (dev->mac[5] << 8));
    
    if (e1000_init_rx(dev, priv->rx_descs, priv->rx_buffers) != 0) {
        return -1;
    }
    
    if (e1000_init_tx(dev, priv->tx_descs, priv->tx_buffers) != 0) {
        return -1;
    }
    
    priv->rx_next = 0;
    priv->tx_next = 0;
    
    debug_print("E1000: Enabling interrupts...\n");
    e1000_write_reg(base, E1000_REG_IMS, 0x1F6DC);
    e1000_read_reg(base, E1000_REG_ICR);
    
    dev->initialized = 1;
    debug_print("E1000: Initialized\n");
    
    return 0;
}

int e1000_send(netdev_t* dev, uint8_t* data, size_t len) {
    if (!dev || !dev->initialized || len == 0) {
        return -1;
    }
    
    if (len > E1000_TX_BUFFER_SIZE) {
        return -1;
    }
    
    e1000_private_t* priv = (e1000_private_t*)dev->private_data;
    uint64_t base = dev->mem_base;
    uint16_t tx_next = priv->tx_next;
    
    e1000_tx_desc_t* desc = &priv->tx_descs[tx_next];
    
    if (!(desc->status & E1000_TX_STATUS_DD)) {
        return 0;
    }
    
    for (size_t i = 0; i < len; i++) {
        priv->tx_buffers[tx_next * E1000_TX_BUFFER_SIZE + i] = data[i];
    }
    
    desc->length = len;
    desc->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    
    e1000_write_reg(base, E1000_REG_TDT, (tx_next + 1) % E1000_NUM_TX_DESC);
    
    priv->tx_next = (tx_next + 1) % E1000_NUM_TX_DESC;
    
    while (!(desc->status & E1000_TX_STATUS_DD));
    
    return len;
}

void e1000_irq_handler(netdev_t* dev) {
    if (!dev || !dev->initialized) {
        return;
    }
    
    e1000_private_t* priv = (e1000_private_t*)dev->private_data;
    uint64_t base = dev->mem_base;
    
    uint32_t icr = e1000_read_reg(base, E1000_REG_ICR);
    e1000_write_reg(base, E1000_REG_ICR, icr);
    
    if (icr & 0x04) {
        while (1) {
            e1000_rx_desc_t* desc = &priv->rx_descs[priv->rx_next];
            
            if (!(desc->status & 0x01)) {
                break;
            }
            
            uint16_t len = desc->length;
            uint8_t* data = &priv->rx_buffers[priv->rx_next * E1000_RX_BUFFER_SIZE];
            
            network_receive_packet(data, len);
            
            desc->status = 0;
            
            priv->rx_next = (priv->rx_next + 1) % E1000_NUM_RX_DESC;
            
            e1000_write_reg(base, E1000_REG_RDT, (priv->rx_next + E1000_NUM_RX_DESC - 1) % E1000_NUM_RX_DESC);
        }
    }
}

int e1000_probe(netdev_t* dev) {
    debug_print("E1000: Probing...\n");
    dev->type = NETDEV_TYPE_E1000;
    dev->init = e1000_init;
    dev->send = e1000_send;
    dev->irq_handler = e1000_irq_handler;
    return 0;
}
