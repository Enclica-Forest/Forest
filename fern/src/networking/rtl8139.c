#include "rtl8139.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/io_ports.h"
#include "network.h"

#define RTL8139_TX_BUFFER_SIZE 1518

static void rtl8139_write_reg(uint16_t io_base, uint16_t offset, uint8_t value) {
    outb(io_base + offset, value);
}

static uint8_t rtl8139_read_reg(uint16_t io_base, uint16_t offset) {
    return inb(io_base + offset);
}

static uint16_t rtl8139_read_reg16(uint16_t io_base, uint16_t offset) {
    return inw(io_base + offset);
}

static void rtl8139_write_reg32(uint16_t io_base, uint16_t offset, uint32_t value) {
    outl(io_base + offset, value);
}

static int rtl8139_init(netdev_t* dev) {
    debug_print("RTL8139: Initializing...\n");
    
    uint16_t io_base = (uint16_t)dev->io_base;
    
    rtl8139_write_reg(io_base, RTL8139_REG_CMD, 0x10);
    while (rtl8139_read_reg(io_base, RTL8139_REG_CMD) & 0x10);
    
    rtl8139_write_reg32(io_base, RTL8139_REG_RBSTART, RTL8139_RX_BUFFER_SIZE);
    
    rtl8139_write_reg16(io_base, RTL8139_REG_IMR, 0x05);
    rtl8139_write_reg16(io_base, RTL8139_REG_ISR, 0x05);
    
    for (int i = 0; i < 6; i++) {
        dev->mac[i] = rtl8139_read_reg(io_base, RTL8139_REG_IDR + i);
    }
    
    debug_print("RTL8139: MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
             dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);
    
    rtl8139_write_reg(io_base, RTL8139_REG_CMD,
                 RTL8139_CMD_TX_ENABLE | RTL8139_CMD_RX_ENABLE);
    
    dev->initialized = 1;
    debug_print("RTL8139: Initialized\n");
    
    return 0;
}

int rtl8139_send(netdev_t* dev, uint8_t* data, size_t len) {
    if (!dev || !dev->initialized || len == 0 || len > RTL8139_TX_BUFFER_SIZE) {
        return -1;
    }
    
    uint16_t io_base = (uint16_t)dev->io_base;
    
    for (int i = 0; i < 4; i++) {
        uint32_t status = rtl8139_read_reg32(io_base, RTL8139_REG_TX_FIFO_SIZE + i * 4);
        if (status & 0x2000) {
            for (size_t j = 0; j < len && j < RTL8139_TX_BUFFER_SIZE; j++) {
                rtl8139_write_reg32(io_base, RTL8139_REG_TX_FIFO_SIZE + i * 4 + 4, data[j]);
            }
            return len;
        }
    }
    
    return 0;
}

void rtl8139_irq_handler(netdev_t* dev) {
    if (!dev || !dev->initialized) {
        return;
    }
    
    uint16_t io_base = (uint16_t)dev->io_base;
    
    uint16_t isr = rtl8139_read_reg16(io_base, RTL8139_REG_ISR);
    rtl8139_write_reg16(io_base, RTL8139_REG_ISR, isr);
    
    if (isr & 0x01) {
        uint32_t capr = rtl8139_read_reg32(io_base, RTL8139_REG_CAPR) + 16;
        
        if (capr & 0x01) {
            while (capr & 0x01) {
                uint16_t len = rtl8139_read_reg16(io_base, RTL8139_REG_RX_FIFO_SIZE + 2);
                
                if (len > 0 && len <= RTL8139_RX_BUFFER_SIZE - 4) {
                    for (int i = 0; i < len; i++) {
                        rtl8139_write_reg32(io_base, RTL8139_REG_RX_FIFO_SIZE + 4 + i * 4,
                                          *((uint32_t*)(data + i)));
                    }
                    
                    network_receive_packet(data, len);
                }
                
                capr = rtl8139_read_reg32(io_base, RTL8139_REG_CAPR) + 16;
            }
        }
    }
}

int rtl8139_probe(netdev_t* dev) {
    debug_print("RTL8139: Probing...\n");
    dev->type = NETDEV_TYPE_RTL8139;
    dev->init = rtl8139_init;
    dev->send = rtl8139_send;
    dev->irq_handler = rtl8139_irq_handler;
    return 0;
}
