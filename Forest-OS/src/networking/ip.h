#ifndef IP_H
#define IP_H

#include "network.h"
#include <stdint.h>

typedef struct {
    uint8_t src_ip[4];
    uint8_t dst_ip[4];
    uint8_t protocol;
    uint16_t data_len;
    uint8_t* data;
    size_t data_offset;
} ip_packet_info_t;

typedef void (*ip_receive_handler_t)(ip_packet_info_t* info);

void ip_init(void);
void ip_handle_packet(uint8_t* packet, size_t len);
int ip_send_packet(uint8_t* dst_ip, uint8_t protocol, uint8_t* data, size_t len);
void ip_register_handler(uint8_t protocol, ip_receive_handler_t handler);
uint16_t ip_pseudo_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t protocol, uint16_t len);

#endif
