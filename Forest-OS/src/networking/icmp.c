#include "icmp.h"
#include "network.h"
#include "ip.h"
#include "../include/debug.h"
#include "../include/string.h"

uint16_t calculate_checksum(uint8_t* data, size_t len);
uint16_t switch_endian16(uint16_t nb);
int ip_send_packet(uint8_t* dst_ip, uint8_t protocol, uint8_t* data, size_t len);

static icmp_echo_callback_t g_echo_callback = NULL;

void icmp_set_echo_callback(icmp_echo_callback_t callback) {
    g_echo_callback = callback;
}

void icmp_send_echo_request(uint32_t dest_ip, uint16_t id, uint16_t seq, uint8_t* data, size_t len) {
    uint8_t packet[ICMP_HLEN + len];
    icmp_packet_t* icmp = (icmp_packet_t*)packet;
    
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = switch_endian16(id);
    icmp->seq = switch_endian16(seq);
    
    if (data && len > 0) {
        for (size_t i = 0; i < len; i++) {
            packet[ICMP_HLEN + i] = data[i];
        }
    }
    
    icmp->checksum = calculate_checksum(packet, ICMP_HLEN + len);
    
    uint8_t dest_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        dest_ip_bytes[i] = ((uint8_t*)&dest_ip)[i];
    }
    
    ip_send_packet(dest_ip_bytes, IP_PROTO_ICMP, packet, ICMP_HLEN + len);
}

static void handle_echo_request(uint32_t src_ip, uint8_t* packet, size_t len) {
    icmp_packet_t* req = (icmp_packet_t*)packet;
    
    if (len < ICMP_HLEN) {
        return;
    }
    
    uint16_t id = switch_endian16(req->id);
    uint16_t seq = switch_endian16(req->seq);
    
    uint8_t reply_packet[ICMP_HLEN + len - ICMP_HLEN];
    icmp_packet_t* reply = (icmp_packet_t*)reply_packet;
    
    reply->type = ICMP_ECHO_REPLY;
    reply->code = 0;
    reply->checksum = 0;
    reply->id = req->id;
    reply->seq = req->seq;
    
    for (size_t i = ICMP_HLEN; i < len; i++) {
        reply_packet[i - ICMP_HLEN] = packet[i];
    }
    
    reply->checksum = calculate_checksum(reply_packet, sizeof(reply_packet));
    
    uint8_t src_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&src_ip)[i];
    }
    
    ip_send_packet(src_ip_bytes, IP_PROTO_ICMP, reply_packet, sizeof(reply_packet));
}

static void handle_echo_reply(uint32_t src_ip, uint8_t* packet, size_t len) {
    icmp_packet_t* reply = (icmp_packet_t*)packet;
    
    if (len < ICMP_HLEN) {
        return;
    }
    
    uint16_t id = switch_endian16(reply->id);
    uint16_t seq = switch_endian16(reply->seq);
    uint8_t* data = packet + ICMP_HLEN;
    size_t data_len = len - ICMP_HLEN;
    
    if (g_echo_callback) {
        g_echo_callback(src_ip, seq, data, data_len);
    }
}

void icmp_init(void) {
    debug_print("ICMP: Initializing...\n");
    debug_print("ICMP: Initialized\n");
}

void icmp_handle_packet(uint8_t* packet, size_t len) {
    if (len < ICMP_HLEN) {
        return;
    }
    
    icmp_packet_t* icmp = (icmp_packet_t*)packet;
    uint8_t type = icmp->type;
    
    uint32_t src_ip = 0;
    
    switch (type) {
        case ICMP_ECHO_REQUEST:
            handle_echo_request(src_ip, packet, len);
            break;
        case ICMP_ECHO_REPLY:
            handle_echo_reply(src_ip, packet, len);
            break;
        default:
            break;
    }
}
