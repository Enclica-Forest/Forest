#include "ne2000.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/io_ports.h"
#include "network.h"

static void ne2000_write_reg(uint16_t io_base, uint8_t reg, uint8_t value) {
    outb(io_base + reg, value);
}

static uint8_t ne2000_read_reg(uint16_t io_base, uint8_t reg) {
    return inb(io_base + reg);
}

static int ne2000_init(netdev_t* dev) {
    debug_print("NE2000: Initializing...\n");
    
    uint16_t io_base = (uint16_t)dev->io_base;
    
    ne2000_write_reg(io_base, NE2000_REG_IMR, 0);
    
    ne2000_write_reg(io_base, NE2000_REG_DCR, 0x49);
    
    ne2000_write_reg(io_base, NE2000_REG_PG0, 0x21);
    ne2000_write_reg(io_base, NE2000_REG_PSTART, 0x40);
    ne2000_write_reg(io_base, NE2000_REG_STOP, 0x80);
    ne2000_write_reg(io_base, NE2000_REG_BNRY, 0x40);
    
    ne2000_write_reg(io_base, NE2000_REG_TPSR, 0x40);
    ne2000_write_reg(io_base, NE2000_REG_TBCR0, NE2000_TX_BUFFER_SIZE);
    ne2000_write_reg(io_base, NE2000_REG_TBCR1, 0x00);
    ne2000_write_reg(io_base, NE2000_REG_RCR, 0x04);
    ne2000_write_reg(io_base, NE2000_REG_TCR, 0x00);
    
    ne2000_write_reg(io_base, NE2000_REG_RSAR0, 0);
    ne2000_write_reg(io_base, NE2000_REG_RSAR1, 0);
    ne2000_write_reg(io_base, NE2000_REG_RBCR0, 0);
    ne2000_write_reg(io_base, NE2000_REG_RBCR1, 0);
    ne2000_write_reg(io_base, NE2000_REG_CMD, NE2000_CMD_START);
    
    for (int i = 0; i < 6; i++) {
        dev->mac[i] = ne2000_read_reg(io_base, i);
    }
    
    debug_print("NE2000: MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
             dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);
    
    dev->initialized = 1;
    debug_print("NE2000: Initialized\n");
    
    return 0;
}

int ne2000_send(netdev_t* dev, uint8_t* data, size_t len) {
    if (!dev || !dev->initialized || len == 0 || len > NE2000_TX_BUFFER_SIZE) {
        return -1;
    }
    
    uint16_t io_base = (uint16_t)dev->io_base;
    
    ne2000_write_reg(io_base, NE2000_REG_RSAR0, NE2000_RX_BUFFER_SIZE);
    ne2000_write_reg(io_base, NE2000_REG_RSAR1, 0);
    ne2000_write_reg(io_base, NE2000_REG_RBCR0, len);
    ne2000_write_reg(io_base, NE2000_REG_RBCR1, 0);
    
    for (int i = 0; i < len; i++) {
        ne2000_write_reg(io_base, NE2000_REG_DATA, data[i]);
    }
    
    ne2000_write_reg(io_base, NE2000_REG_CMD, NE2000_CMD_START);
    
    while (ne2000_read_reg(io_base, NE2000_REG_ISR) & 0x02);
    
    return len;
}

void ne2000_irq_handler(netdev_t* dev) {
    if (!dev || !dev->initialized) {
        return;
    }
    
    uint16_t io_base = (uint16_t)dev->io_base;
    
    uint8_t isr = ne2000_read_reg(io_base, NE2000_REG_ISR);
    ne2000_write_reg(io_base, NE2000_REG_ISR, 0xFF);
    
    if (isr & 0x01) {
        uint8_t bnry = ne2000_read_reg(io_base, NE2000_REG_BNRY);
        uint8_t current = bnry + 1;
        if (current == 0x80) {
            current = 0x40;
        }
        
        uint8_t rsar0 = ne2000_read_reg(io_base, NE2000_REG_RSAR0);
        uint8_t rsar1 = ne2000_read_reg(io_base, NE2000_REG_RSAR1);
        uint16_t rsar = rsar0 | (rsar1 << 8);
        
        uint8_t rcr = ne2000_read_reg(io_base, NE2000_REG_RCR);
        uint16_t packet_len = ((rcr >> 5) & 0x03FF);
        
        if (packet_len > 0 && packet_len <= NE2000_RX_BUFFER_SIZE + 4) {
            ne2000_write_reg(io_base, NE2000_REG_RSAR0, rsar0);
            ne2000_write_reg(io_base, NE2000_REG_RSAR1, rsar1);
            ne2000_write_reg(io_base, NE2000_REG_RBCR0, packet_len + 4);
            ne2000_write_reg(io_base, NE2000_REG_RBCR1, 0);
            
            uint8_t data[NE2000_RX_BUFFER_SIZE];
            for (int i = 0; i < packet_len + 4; i++) {
                data[i] = ne2000_read_reg(io_base, NE2000_REG_DATA);
            }
            
            uint8_t bnry2 = ne2000_read_reg(io_base, NE2000_REG_BNRY);
            if (bnry2 == 0xFF) {
                bnry2 = 0x3F;
            }
            
            ne2000_write_reg(io_base, NE2000_REG_BNRY, bnry2);
            
            network_receive_packet(data + 4, packet_len);
        }
    }
}

int ne2000_probe(netdev_t* dev) {
    debug_print("NE2000: Probing...\n");
    dev->type = NETDEV_TYPE_NE2000;
    dev->init = ne2000_init;
    dev->send = ne2000_send;
    dev->irq_handler = ne2000_irq_handler;
    return 0;
}
