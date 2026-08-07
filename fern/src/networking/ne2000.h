#ifndef NE2000_H
#define NE2000_H

#include "netdev.h"
#include "../include/pci.h"
#include <stdint.h>

#define NE2000_VENDOR_ID 0x10EC
#define NE2000_DEVICE_ID 0x8029

#define NE2000_REG_PG0 0x01
#define NE2000_REG_PSTART 0x02
#define NE2000_REG_STOP 0x03
#define NE2000_REG_BNRY 0x04
#define NE2000_REG_TPSR 0x04
#define NE2000_REG_TBCR0 0x05
#define NE2000_REG_TBCR1 0x06
#define NE2000_REG_RSAR0 0x08
#define NE2000_REG_RSAR1 0x09
#define NE2000_REG_RBCR0 0x0A
#define NE2000_REG_RBCR1 0x0B
#define NE2000_REG_RCR 0x0C
#define NE2000_REG_TCR 0x0D
#define NE2000_REG_DCR 0x0E
#define NE2000_REG_IMR 0x0F
#define NE2000_REG_ISR 0x07
#define NE2000_REG_DATA 0x10

#define NE2000_CMD_PAGE0 0x20
#define NE2000_CMD_PAGE1 0x21
#define NE2000_CMD_START 0x22
#define NE2000_CMD_STOP 0x21

#define NE2000_NUM_TX_DESC 4
#define NE2000_RX_BUFFER_SIZE 64
#define NE2000_TX_BUFFER_SIZE 64

int ne2000_probe(netdev_t* dev);
int ne2000_init(netdev_t* dev);
int ne2000_send(netdev_t* dev, uint8_t* data, size_t len);
void ne2000_irq_handler(netdev_t* dev);

#endif
