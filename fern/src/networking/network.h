#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stddef.h>

#define NETWORK_MAX_PACKET_SIZE 1518
#define NETWORK_MIN_PACKET_SIZE 64
#define NETWORK_MAX_FRAME_SIZE 1520

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_MTU 1500

#define ARP_HLEN 28
#define IP_HLEN 20
#define ICMP_HLEN 8
#define UDP_HLEN 8
#define TCP_HLEN 20

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_RARP 0x8035
#define ETH_P_IPV6 0x86DD

typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype;
} __attribute__((packed)) eth_header_t;

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t opcode;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed)) arp_packet_t;

typedef struct {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed)) ip_header_t;

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_packet_t;

typedef struct {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t check;
} __attribute__((packed)) udp_header_t;

typedef struct {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t offset_res;
    uint8_t flags;
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
} __attribute__((packed)) tcp_header_t;

typedef void (*network_receive_callback_t)(uint8_t* data, size_t len);

uint16_t switch_endian16(uint16_t nb);
uint32_t switch_endian32(uint32_t nb);
uint16_t calculate_checksum(uint8_t* data, size_t len);
uint16_t calculate_ip_checksum(ip_header_t* ip);

void network_init(void);
void network_send_packet(uint8_t* data, size_t len);
void network_set_receive_callback(network_receive_callback_t callback);

#endif
