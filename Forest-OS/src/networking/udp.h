#ifndef UDP_H
#define UDP_H

#include "network.h"
#include <stdint.h>

#define UDP_MAX_SOCKETS 32

typedef void (*udp_receive_callback_t)(uint32_t src_ip, uint16_t src_port, uint8_t* data, size_t len);

typedef struct {
    uint16_t port;
    udp_receive_callback_t callback;
    int bound;
} udp_socket_t;

void udp_init(void);
void udp_handle_packet(uint8_t* src_ip, uint16_t src_port, uint8_t* data, size_t len);
int udp_bind(uint16_t port, udp_receive_callback_t callback);
int udp_send(uint32_t dst_ip, uint16_t dst_port, uint8_t* data, size_t len);
int udp_sendto(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
               uint8_t* data, size_t len);

#endif
