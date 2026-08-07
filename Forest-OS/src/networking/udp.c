#include "udp.h"
#include "network.h"
#include "ip.h"
#include "../include/debug.h"
#include "../include/string.h"

uint16_t calculate_checksum(uint8_t* data, size_t len);
uint16_t switch_endian16(uint16_t nb);
uint32_t switch_endian32(uint32_t nb);
uint16_t ip_pseudo_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t protocol, uint16_t len);
int ip_send_packet(uint8_t* dst_ip, uint8_t protocol, uint8_t* data, size_t len);

static udp_socket_t udp_sockets[UDP_MAX_SOCKETS];

void udp_init(void) {
    debug_print("UDP: Initializing...\n");
    
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        udp_sockets[i].port = 0;
        udp_sockets[i].callback = NULL;
        udp_sockets[i].bound = 0;
    }
    
    debug_print("UDP: Initialized\n");
}

int udp_bind(uint16_t port, udp_receive_callback_t callback) {
    if (port == 0) {
        return -1;
    }
    
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!udp_sockets[i].bound) {
            udp_sockets[i].port = port;
            udp_sockets[i].callback = callback;
            udp_sockets[i].bound = 1;
            debug_print("UDP: Bound to port %d\n", port);
            return 0;
        }
    }
    
    return -1;
}

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint8_t* data, size_t len) {
    return udp_sendto(0, 0, dst_ip, dst_port, data, len);
}

int udp_sendto(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
               uint8_t* data, size_t len) {
    uint8_t packet[UDP_HLEN + len];
    udp_header_t* udp = (udp_header_t*)packet;
    uint8_t* payload = packet + UDP_HLEN;
    
    udp->sport = switch_endian16(src_port);
    udp->dport = switch_endian16(dst_port);
    udp->len = switch_endian16(UDP_HLEN + len);
    udp->check = 0;
    
    for (size_t i = 0; i < len; i++) {
        payload[i] = data[i];
    }
    
    uint8_t src_ip_bytes[4];
    uint8_t dst_ip_bytes[4];
    
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&src_ip)[i];
        dst_ip_bytes[i] = ((uint8_t*)&dst_ip)[i];
    }
    
    uint16_t checksum = ip_pseudo_checksum(src_ip_bytes, dst_ip_bytes, 
                                         IP_PROTO_UDP, UDP_HLEN + len);
    udp->check = checksum;
    
    uint8_t dst_addr[4];
    for (int i = 0; i < 4; i++) {
        dst_addr[i] = ((uint8_t*)&dst_ip)[i];
    }
    
    return ip_send_packet(dst_addr, IP_PROTO_UDP, packet, sizeof(packet));
}

void udp_handle_packet(uint8_t* src_ip, uint16_t src_port, uint8_t* data, size_t len) {
    uint16_t sport = src_port;
    
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (udp_sockets[i].bound && udp_sockets[i].port == sport) {
            if (udp_sockets[i].callback) {
                uint32_t src_ip_addr = 0;
                for (int j = 0; j < 4; j++) {
                    ((uint8_t*)&src_ip_addr)[j] = src_ip[j];
                }
                udp_sockets[i].callback(src_ip_addr, sport, data, len);
            }
            return;
        }
    }
}
