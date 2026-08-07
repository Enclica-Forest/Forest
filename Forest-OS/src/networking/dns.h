#ifndef DNS_H
#define DNS_H

#include "network.h"
#include <stdint.h>

#define DNS_PORT 53
#define DNS_MAX_QNAME 255
#define DNS_FLAG_RECURSION 0x0100
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

typedef void (*dns_callback_t)(int success, uint8_t* name, uint32_t* ip_addrs, int num_addrs);

int dns_init(void);
int dns_query(uint8_t* name, uint8_t dns_server, dns_callback_t callback);

#endif
