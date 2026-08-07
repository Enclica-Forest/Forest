#ifndef RTL8139_H
#define RTL8139_H

#include "netdev.h"
#include "../include/pci.h"
#include <stdint.h>

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

#define RTL8139_REG_IDR 0x0000
#define RTL8139_REG_RBSTART 0x30
#define RTL8139_REG_CMD 0x37
#define RTL8139_REG_CAPR 0x38
#define RTL8139_REG_IMR 0x3C
#define RTL8139_REG_ISR 0x3E

#define RTL8139_CMD_TX_ENABLE 0x08
#define RTL8139_CMD_RX_ENABLE 0x04

#define RTL8139_NUM_TX_DESC 4
#define RTL8139_RX_BUFFER_SIZE 8192 + 16

#define RTL8139_TX_FIFO_SIZE 2048

typedef struct {
    uint32_t addr;
    uint32_t len;
} rtl8139_tx_desc_t;

typedef struct {
    uint8_t header[4];
    uint16_t status;
    uint16_t len;
} rtl8139_rx_header_t;

int rtl8139_probe(netdev_t* dev);
int rtl8139_init(netdev_t* dev);
int rtl8139_send(netdev_t* dev, uint8_t* data, size_t len);
void rtl8139_irq_handler(netdev_t* dev);

#endif
