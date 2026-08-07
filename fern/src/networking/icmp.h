#ifndef ICMP_H
#define ICMP_H

#include "network.h"
#include <stdint.h>

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY 0
#define ICMP_DEST_UNREACHABLE 3
#define ICMP_TIME_EXCEEDED 11

typedef void (*icmp_echo_callback_t)(uint32_t src_ip, uint16_t seq, uint8_t* data, size_t len);

void icmp_init(void);
void icmp_send_echo_request(uint32_t dest_ip, uint16_t id, uint16_t seq, uint8_t* data, size_t len);
void icmp_set_echo_callback(icmp_echo_callback_t callback);

#endif
