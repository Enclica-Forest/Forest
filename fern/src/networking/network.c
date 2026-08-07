#include "network.h"
#include "route.h"
#include "../include/debug.h"
#include "../include/string.h"

void arp_init(void);
void arp_handle_packet(uint8_t* packet, size_t len);
void ip_init(void);
void ip_handle_packet(uint8_t* packet, size_t len);
void tcp_init(void);
void udp_init(void);
int dhcp_init(void);
int dns_init(void);
int netdev_init(void);
void netdev_handle_irq(uint8_t irq);
void* malloc(size_t nbytes);
void free(void *ptr);
int dhcp_init(void);
int dns_init(void);

uint16_t switch_endian16(uint16_t nb) {
    return (nb >> 8) | (nb << 8);
}

uint32_t switch_endian32(uint32_t nb) {
    return ((nb >> 24) & 0xFF) |
           ((nb << 8) & 0xFF0000) |
           ((nb >> 8) & 0xFF00) |
           ((nb << 24) & 0xFF000000);
}

uint16_t calculate_checksum(uint8_t* data, size_t len) {
    uint32_t sum = 0;
    
    while (len > 1) {
        sum += *((uint16_t*)data);
        data += 2;
        len -= 2;
    }
    
    if (len > 0) {
        sum += (uint16_t)(*data << 8);
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)(~sum);
}

uint16_t calculate_ip_checksum(ip_header_t* ip) {
    ip->check = 0;
    return calculate_checksum((uint8_t*)ip, IP_HLEN);
}

static uint8_t g_mac_address[6] = {0, 0, 0, 0, 0, 0};
static uint8_t g_ip_address[4] = {0, 0, 0, 0};
static uint8_t g_gateway[4] = {0, 0, 0, 0};
static uint8_t g_subnet_mask[4] = {0, 0, 0, 0};
static uint8_t g_dns_server[4] = {0, 0, 0, 0};

static int (*g_netdev_send_packet)(uint8_t* data, size_t len) = NULL;

void network_set_netdev_send_callback(int (*callback)(uint8_t*, size_t)) {
    g_netdev_send_packet = callback;
}

void network_set_mac_address(uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        g_mac_address[i] = mac[i];
    }
}

void network_get_mac_address(uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        mac[i] = g_mac_address[i];
    }
}

void network_set_ip_address(uint32_t ip) {
    ip = switch_endian32(ip);
    for (int i = 0; i < 4; i++) {
        g_ip_address[i] = ((uint8_t*)&ip)[i];
    }
}

uint32_t network_get_ip_address(void) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&ip)[i] = g_ip_address[i];
    }
    return switch_endian32(ip);
}

void network_set_gateway(uint32_t gateway) {
    gateway = switch_endian32(gateway);
    for (int i = 0; i < 4; i++) {
        g_gateway[i] = ((uint8_t*)&gateway)[i];
    }
}

uint32_t network_get_gateway(void) {
    uint32_t gateway = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&gateway)[i] = g_gateway[i];
    }
    return switch_endian32(gateway);
}

void network_set_subnet_mask(uint32_t mask) {
    mask = switch_endian32(mask);
    for (int i = 0; i < 4; i++) {
        g_subnet_mask[i] = ((uint8_t*)&mask)[i];
    }
}

uint32_t network_get_subnet_mask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&mask)[i] = g_subnet_mask[i];
    }
    return switch_endian32(mask);
}

void network_set_dns_server(uint32_t dns) {
    dns = switch_endian32(dns);
    for (int i = 0; i < 4; i++) {
        g_dns_server[i] = ((uint8_t*)&dns)[i];
    }
}

uint32_t network_get_dns_server(void) {
    uint32_t dns = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&dns)[i] = g_dns_server[i];
    }
    return switch_endian32(dns);
}

void network_send_packet(uint8_t* data, size_t len) {
    if (g_netdev_send_packet) {
        g_netdev_send_packet(data, len);
    }
}

static void process_ethernet_frame(uint8_t* frame, size_t len) {
    if (len < ETH_HLEN) {
        return;
    }
    
    eth_header_t* eth = (eth_header_t*)frame;
    
    uint16_t ethertype = switch_endian16(eth->ethertype);
    
    if (ethertype == ETH_P_ARP) {
        arp_handle_packet(frame + ETH_HLEN, len - ETH_HLEN);
    } else if (ethertype == ETH_P_IP) {
        ip_handle_packet(frame + ETH_HLEN, len - ETH_HLEN);
    }
}

void network_receive_packet(uint8_t* data, size_t len) {
    process_ethernet_frame(data, len);
}

void network_init(void) {
    debug_print("Network stack initialization...\n");

    netdev_init();
    route_init();

    arp_init();
    ip_init();
    tcp_init();
    udp_init();
    dhcp_init();
    dns_init();

    debug_print("Network stack initialized\n");
}
