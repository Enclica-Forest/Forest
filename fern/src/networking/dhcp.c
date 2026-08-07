#include "dhcp.h"
#include "network.h"
#include "../include/debug.h"
#include "../include/string.h"

uint32_t network_get_ip_address(void);
void network_get_mac_address(uint8_t* mac);
void network_set_ip_address(uint32_t ip);
void network_set_gateway(uint32_t gateway);
void network_set_subnet_mask(uint32_t mask);
void network_set_dns_server(uint32_t dns);
uint16_t calculate_checksum(uint8_t* data, size_t len);
uint16_t calculate_ip_checksum(ip_header_t* ip);
uint16_t switch_endian16(uint16_t nb);
uint32_t switch_endian32(uint32_t nb);
uint16_t ip_pseudo_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t protocol, uint16_t len);
void network_send_packet(uint8_t* data, size_t len);

static dhcp_callback_t g_dhcp_callback = NULL;
static uint32_t g_requested_ip = 0;
static uint8_t g_server_mac[6] = {0};
static int g_dhcp_completed = 0;

void dhcp_set_callback(dhcp_callback_t callback) {
    g_dhcp_callback = callback;
}

static int dhcp_send_packet(uint8_t* data, size_t len, uint32_t dst_ip) {
    uint8_t packet[ETH_HLEN + IP_HLEN + UDP_HLEN + len];
    eth_header_t* eth = (eth_header_t*)packet;
    
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = 0xFF;
    }
    
    network_get_mac_address(eth->src_mac);
    
    eth->ethertype = switch_endian16(ETH_P_IP);
    
    uint8_t* ip_data = packet + ETH_HLEN;
    ip_header_t* ip = (ip_header_t*)ip_data;
    
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = switch_endian16(IP_HLEN + UDP_HLEN + len);
    ip->id = switch_endian16(0x1234);
    ip->frag_off = switch_endian16(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->check = 0;
    
    uint32_t src_ip = 0;
    ip->saddr = src_ip;
    
    uint32_t dst_ip_addr = switch_endian32(dst_ip);
    ip->daddr = dst_ip_addr;
    
    ip->check = calculate_ip_checksum(ip);
    
    uint8_t* udp_data = ip_data + IP_HLEN;
    udp_header_t* udp = (udp_header_t*)udp_data;
    
    udp->sport = switch_endian16(DHCP_CLIENT_PORT);
    udp->dport = switch_endian16(DHCP_SERVER_PORT);
    udp->len = switch_endian16(UDP_HLEN + len);
    udp->check = 0;
    
    uint8_t* payload = udp_data + UDP_HLEN;
    for (size_t i = 0; i < len; i++) {
        payload[i] = data[i];
    }
    
    uint32_t checksum = ip_pseudo_checksum((uint8_t*)&src_ip, (uint8_t*)&dst_ip,
                                               IP_PROTO_UDP, UDP_HLEN + len);
    udp->check = checksum;
    
    network_send_packet(packet, sizeof(packet));
    
    return 0;
}

int dhcp_discover(void) {
    debug_print("DHCP: Sending DISCOVER\n");
    
    uint8_t packet[sizeof(dhcp_packet_t)];
    dhcp_packet_t* dhcp = (dhcp_packet_t*)packet;
    
    memset(packet, 0, sizeof(dhcp_packet_t));
    
    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = 0x12345678;
    dhcp->secs = 0;
    dhcp->flags = switch_endian16(0x8000);
    
    network_get_mac_address(dhcp->chaddr);
    
    uint8_t* opts = dhcp->options;
    int idx = 0;
    
    uint32_t magic = DHCP_MAGIC_COOKIE;
    for (int i = 0; i < 4; i++) {
        opts[idx++] = ((uint8_t*)&magic)[i];
    }
    
    opts[idx++] = DHCP_OPTION_MESSAGE_TYPE;
    opts[idx++] = 1;
    opts[idx++] = DHCP_DISCOVER;
    
    opts[idx++] = DHCP_OPTION_END;
    
    return dhcp_send_packet(packet, 4 + idx, 0xFFFFFFFF);
}

int dhcp_request(void) {
    debug_print("DHCP: Sending REQUEST\n");
    
    uint8_t packet[sizeof(dhcp_packet_t)];
    dhcp_packet_t* dhcp = (dhcp_packet_t*)packet;
    
    memset(packet, 0, sizeof(dhcp_packet_t));
    
    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = 0x12345678;
    dhcp->secs = 0;
    dhcp->flags = 0;
    
    network_get_mac_address(dhcp->chaddr);
    
    uint32_t req_ip = switch_endian32(g_requested_ip);
    dhcp->ciaddr = req_ip;
    
    uint8_t* opts = dhcp->options;
    int idx = 0;
    
    uint32_t magic = DHCP_MAGIC_COOKIE;
    for (int i = 0; i < 4; i++) {
        opts[idx++] = ((uint8_t*)&magic)[i];
    }
    
    opts[idx++] = DHCP_OPTION_MESSAGE_TYPE;
    opts[idx++] = 1;
    opts[idx++] = DHCP_REQUEST;
    
    opts[idx++] = DHCP_OPTION_REQUESTED_IP;
    opts[idx++] = 4;
    for (int i = 0; i < 4; i++) {
        opts[idx++] = ((uint8_t*)&g_requested_ip)[i];
    }
    
    opts[idx++] = DHCP_OPTION_END;
    
    return dhcp_send_packet(packet, 4 + idx, 0xFFFFFFFF);
}

static void dhcp_handle_offer(uint8_t* data, size_t len) {
    debug_print("DHCP: Received OFFER\n");
    
    g_requested_ip = switch_endian32(((dhcp_packet_t*)data)->yiaddr);
    
    for (int i = 0; i < 6; i++) {
        g_server_mac[i] = ((dhcp_packet_t*)data)->chaddr[i];
    }
    
    dhcp_request();
}

static void dhcp_handle_ack(uint8_t* data, size_t len) {
    debug_print("DHCP: Received ACK\n");
    
    uint32_t ip = switch_endian32(((dhcp_packet_t*)data)->yiaddr);
    uint32_t server_ip = switch_endian32(((dhcp_packet_t*)data)->siaddr);
    
    uint32_t gateway = 0;
    uint32_t netmask = 0;
    uint32_t dns = 0;
    
    uint8_t* opts = ((dhcp_packet_t*)data)->options;
    int idx = 4;
    
    uint32_t magic = *((uint32_t*)(opts));
    if (magic != DHCP_MAGIC_COOKIE) {
        return;
    }
    idx += 4;
    
    while (idx < DHCP_OPTIONS_MAX) {
        uint8_t opt = opts[idx++];
        if (opt == DHCP_OPTION_END) {
            break;
        }
        
        uint8_t opt_len = opts[idx++];
        
        switch (opt) {
            case DHCP_OPTION_SUBNET_MASK:
                if (opt_len == 4) {
                    for (int i = 0; i < 4; i++) {
                        ((uint8_t*)&netmask)[i] = opts[idx++];
                    }
                }
                break;
            case DHCP_OPTION_ROUTER:
                if (opt_len == 4) {
                    for (int i = 0; i < 4; i++) {
                        ((uint8_t*)&gateway)[i] = opts[idx++];
                    }
                }
                break;
            case DHCP_OPTION_DNS:
                if (opt_len == 4) {
                    for (int i = 0; i < 4; i++) {
                        ((uint8_t*)&dns)[i] = opts[idx++];
                    }
                }
                break;
            default:
                idx += opt_len;
                break;
        }
    }
    
    network_set_ip_address(ip);
    network_set_gateway(gateway);
    network_set_subnet_mask(netmask);
    network_set_dns_server(dns);
    
    debug_print("DHCP: IP: %d.%d.%d.%d\n", ((uint8_t*)&ip)[0], ((uint8_t*)&ip)[1],
             ((uint8_t*)&ip)[2], ((uint8_t*)&ip)[3]);
    debug_print("DHCP: Gateway: %d.%d.%d.%d\n", ((uint8_t*)&gateway)[0], ((uint8_t*)&gateway)[1],
             ((uint8_t*)&gateway)[2], ((uint8_t*)&gateway)[3]);
    debug_print("DHCP: Netmask: %d.%d.%d.%d\n", ((uint8_t*)&netmask)[0], ((uint8_t*)&netmask)[1],
             ((uint8_t*)&netmask)[2], ((uint8_t*)&netmask)[3]);
    debug_print("DHCP: DNS: %d.%d.%d.%d\n", ((uint8_t*)&dns)[0], ((uint8_t*)&dns)[1],
             ((uint8_t*)&dns)[2], ((uint8_t*)&dns)[3]);
    
    g_dhcp_completed = 1;
    
    if (g_dhcp_callback) {
        g_dhcp_callback(1, ip, gateway, netmask, dns);
    }
}

int dhcp_init(void) {
    debug_print("DHCP: Initializing...\n");
    
    g_dhcp_callback = NULL;
    g_requested_ip = 0;
    g_dhcp_completed = 0;
    
    debug_print("DHCP: Initialized\n");
    
    return 0;
}

void dhcp_handle_packet(uint8_t* data, size_t len) {
    if (len < sizeof(dhcp_packet_t)) {
        return;
    }
    
    dhcp_packet_t* dhcp = (dhcp_packet_t*)data;
    
    if (dhcp->op != 2) {
        return;
    }
    
    uint8_t* opts = dhcp->options;
    uint32_t magic = *((uint32_t*)(opts));
    if (magic != DHCP_MAGIC_COOKIE) {
        return;
    }
    
    int idx = 4;
    while (idx < DHCP_OPTIONS_MAX) {
        uint8_t opt = opts[idx++];
        if (opt == DHCP_OPTION_END) {
            break;
        }
        
        uint8_t opt_len = opts[idx++];
        
        if (opt == DHCP_OPTION_MESSAGE_TYPE && opt_len == 1) {
            uint8_t msg_type = opts[idx];
            
            switch (msg_type) {
                case DHCP_OFFER:
                    dhcp_handle_offer(data, len);
                    break;
                case DHCP_ACK:
                    dhcp_handle_ack(data, len);
                    break;
                case DHCP_NAK:
                    debug_print("DHCP: Received NAK\n");
                    break;
            }
            break;
        } else {
            idx += opt_len;
        }
    }
}
