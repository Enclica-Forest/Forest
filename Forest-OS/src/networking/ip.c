#include "ip.h"
#include "network.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "arp.h"
#include "icmp.h"
#include "tcp.h"
#include "udp.h"
#include "route.h"

static void icmp_packet_handler(ip_packet_info_t* info);
static void udp_packet_handler(ip_packet_info_t* info);
static void tcp_packet_handler(ip_packet_info_t* info);

uint32_t network_get_ip_address(void);
void network_get_mac_address(uint8_t* mac);
void network_send_packet(uint8_t* data, size_t len);
int arp_resolve(uint8_t* ip, uint8_t* mac);
void arp_send_request(uint32_t ip);
void icmp_handle_packet(uint8_t* packet, size_t len);
void udp_handle_packet(uint8_t* src_ip, uint16_t src_port, uint8_t* data, size_t len);
void tcp_handle_packet(uint8_t* packet, size_t len);
uint16_t switch_endian16(uint16_t nb);

static ip_receive_handler_t ip_handlers[256];

void ip_init(void) {
    debug_print("IP: Initializing...\n");
    
    for (int i = 0; i < 256; i++) {
        ip_handlers[i] = NULL;
    }
    
    ip_register_handler(IP_PROTO_ICMP, icmp_packet_handler);
    ip_register_handler(IP_PROTO_UDP, udp_packet_handler);
    ip_register_handler(IP_PROTO_TCP, tcp_packet_handler);
    
    debug_print("IP: Initialized\n");
}

void ip_register_handler(uint8_t protocol, ip_receive_handler_t handler) {
    if (protocol < 256) {
        ip_handlers[protocol] = handler;
    }
}

static int ip_checksum_valid(ip_header_t* ip) {
    uint16_t checksum = calculate_checksum((uint8_t*)ip, IP_HLEN);
    return (checksum == 0);
}

void ip_handle_packet(uint8_t* packet, size_t len) {
    if (len < IP_HLEN) {
        return;
    }
    
    ip_header_t* ip = (ip_header_t*)packet;
    
    if ((ip->ver_ihl & 0xF0) != 0x40) {
        return;
    }
    
    uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (len < ihl) {
        return;
    }
    
    if (!ip_checksum_valid(ip)) {
        return;
    }
    
    uint32_t local_ip = network_get_ip_address();
    uint32_t dest_ip = switch_endian32(ip->daddr);
    
    if (dest_ip != 0xFFFFFFFF && dest_ip != local_ip) {
        return;
    }
    
    uint8_t* data = packet + ihl;
    size_t data_len = len - ihl;
    
    if (ip->protocol < 256 && ip_handlers[ip->protocol]) {
        ip_packet_info_t info;
        
        for (int i = 0; i < 4; i++) {
            info.src_ip[i] = ((uint8_t*)&ip->saddr)[i];
            info.dst_ip[i] = ((uint8_t*)&ip->daddr)[i];
        }
        
        info.protocol = ip->protocol;
        info.data_len = (uint16_t)switch_endian16(ip->tot_len) - ihl;
        info.data = data;
        info.data_offset = ihl;
        
        ip_handlers[ip->protocol](&info);
    }
}

uint16_t ip_pseudo_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t protocol, uint16_t len) {
    uint32_t sum = 0;
    
    for (int i = 0; i < 4; i += 2) {
        sum += *((uint16_t*)(src_ip + i));
        sum += *((uint16_t*)(dst_ip + i));
    }
    
    sum += (uint16_t)protocol;
    sum += len;
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)(~sum);
}

int ip_send_packet(uint8_t* dst_ip, uint8_t protocol, uint8_t* data, size_t len) {
    if (len + IP_HLEN > NETWORK_MAX_PACKET_SIZE) {
        return -1;
    }

    uint8_t packet[ETH_HLEN + IP_HLEN + len];
    eth_header_t* eth = (eth_header_t*)packet;
    ip_header_t* ip = (ip_header_t*)(packet + ETH_HLEN);
    uint8_t* payload = packet + ETH_HLEN + IP_HLEN;

    uint32_t target_ip = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&target_ip)[i] = dst_ip[i];
    }

    uint32_t next_hop = 0;
    if (route_lookup(target_ip, &next_hop, 0) == 0 && next_hop != 0 && next_hop != target_ip) {
        uint8_t gw_ip[4];
        for (int i = 0; i < 4; i++) {
            gw_ip[i] = ((uint8_t*)&next_hop)[i];
        }
        dst_ip = gw_ip;
    }

    uint8_t dst_mac[6];
    if (!arp_resolve(dst_ip, dst_mac)) {
        arp_send_request(target_ip);
        return 0;
    }
    
    network_get_mac_address(eth->src_mac);
    
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = dst_mac[i];
    }
    
    eth->ethertype = switch_endian16(ETH_P_IP);
    
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = switch_endian16(IP_HLEN + len);
    ip->id = switch_endian16(0x1234);
    ip->frag_off = switch_endian16(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->check = 0;
    
    uint32_t src_ip = network_get_ip_address();
    ip->saddr = src_ip;
    
    uint32_t dst_ip_addr = 0;
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&dst_ip_addr)[i] = dst_ip[i];
    }
    ip->daddr = dst_ip_addr;
    
    ip->check = calculate_ip_checksum(ip);
    
    for (size_t i = 0; i < len; i++) {
        payload[i] = data[i];
    }
    
    network_send_packet(packet, ETH_HLEN + IP_HLEN + len);
    
    return len;
}

static void icmp_packet_handler(ip_packet_info_t* info) {
    icmp_handle_packet(info->data, info->data_len);
}

static void udp_packet_handler(ip_packet_info_t* info) {
    uint16_t src_port = switch_endian16(((udp_header_t*)(info->data))->sport);
    uint8_t* udp_data = info->data + UDP_HLEN;
    size_t udp_len = info->data_len - UDP_HLEN;
    
    udp_handle_packet(info->src_ip, src_port, udp_data, udp_len);
}

static void tcp_packet_handler(ip_packet_info_t* info) {
    uint8_t* tcp_data = info->data;
    size_t tcp_len = info->data_len;
    
    tcp_handle_packet(tcp_data, tcp_len);
}
