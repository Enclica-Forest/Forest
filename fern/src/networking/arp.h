#ifndef ARP_H
#define ARP_H

#include "network.h"
#include <stdint.h>

#define ARP_CACHE_SIZE 32

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
} arp_entry_t;

void arp_init(void);
void arp_handle_packet(uint8_t* packet, size_t len);
int arp_resolve(uint8_t* ip, uint8_t* mac);
void arp_send_request(uint32_t ip);
void arp_send_reply(uint32_t ip, uint8_t* mac);

#endif
