#include "dns.h"
#include "network.h"
#include "ip.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "udp.h"

uint16_t switch_endian16(uint16_t nb);
uint32_t switch_endian32(uint32_t nb);
int udp_send(uint32_t dst_ip, uint16_t dst_port, uint8_t* data, size_t len);
int udp_bind(uint16_t port, udp_receive_callback_t callback);

static uint16_t dns_query_id = 1;

static uint8_t* dns_encode_name(uint8_t* name, uint8_t* output) {
    uint8_t* in = name;
    uint8_t* out = output;
    
    while (*in) {
        uint8_t* label_start = in;
        uint8_t len = 0;
        
        while (*in && *in != '.') {
            in++;
            len++;
        }
        
        *out++ = len;
        
        for (uint8_t i = 0; i < len; i++) {
            *out++ = label_start[i];
        }
        
        if (*in) {
            in++;
        }
    }
    
    *out = 0;
    
    return out + 1 - output;
}

static int dns_decode_name(uint8_t* packet, int offset, uint8_t* name, int max_len) {
    int orig_offset = offset;
    int name_len = 0;
    
    while (packet[offset] != 0 && name_len < max_len) {
        if (packet[offset] & 0xC0) {
            int ptr_offset = ((packet[offset] & 0x3F) << 8) | packet[offset + 1];
            ptr_offset = orig_offset + ptr_offset;
            offset = ptr_offset;
            continue;
        }
        
        uint8_t len = packet[offset++];
        
        if (name_len > 0 && name_len < max_len) {
            name[name_len++] = '.';
        }
        
        for (uint8_t i = 0; i < len && name_len < max_len; i++) {
            name[name_len++] = packet[offset++];
        }
    }
    
    if (name_len < max_len) {
        name[name_len] = 0;
    }
    
    offset++;
    
    return offset - orig_offset;
}

static int dns_send_query(uint8_t* name, uint8_t dns_server) {
    uint8_t packet[sizeof(dns_header_t) + DNS_MAX_QNAME + 4 + 4];
    
    dns_header_t* header = (dns_header_t*)packet;
    
    header->id = switch_endian16(dns_query_id++);
    header->flags = switch_endian16(DNS_FLAG_RECURSION);
    header->qdcount = switch_endian16(1);
    header->ancount = 0;
    header->nscount = 0;
    header->arcount = 0;
    
    uint8_t* qname = packet + sizeof(dns_header_t);
    int qname_len = dns_encode_name(name, qname);
    
    uint8_t* qtype = qname + qname_len;
    qtype[0] = 0;
    qtype[1] = DNS_TYPE_A;
    qtype[2] = 0;
    qtype[3] = DNS_CLASS_IN;
    
    int packet_len = sizeof(dns_header_t) + qname_len + 4;
    
    return udp_send(dns_server, DNS_PORT, packet, packet_len);
}

static void dns_handle_response(uint8_t* packet, size_t len, dns_callback_t callback) {
    if (len < sizeof(dns_header_t)) {
        return;
    }
    
    dns_header_t* header = (dns_header_t*)packet;
    
    if (switch_endian16(header->ancount) == 0) {
        return;
    }
    
    int offset = sizeof(dns_header_t);
    
    for (uint16_t i = 0; i < switch_endian16(header->qdcount); i++) {
        uint8_t name[DNS_MAX_QNAME];
        offset += dns_decode_name(packet, offset, name, DNS_MAX_QNAME);
        offset += 4;
    }
    
    for (uint16_t i = 0; i < switch_endian16(header->ancount); i++) {
        offset += dns_decode_name(packet, offset, NULL, 0);
        offset += 2;
        offset += 2;
        offset += 4;
        
        uint16_t rdlength = switch_endian16(*((uint16_t*)(packet + offset)));
        offset += 2;
        
        uint16_t rrtype = switch_endian16(*((uint16_t*)(packet + offset)));
        offset += 2;
        uint16_t rclass = switch_endian16(*((uint16_t*)(packet + offset)));
        offset += 2;
        uint32_t ttl = switch_endian32(*((uint32_t*)(packet + offset)));
        offset += 4;
        
        uint16_t rdlength2 = switch_endian16(*((uint16_t*)(packet + offset)));
        offset += 2;
        
        uint32_t ip_addrs[16];
        int num_addrs = 0;
        
        int rdend = offset + rdlength2;
        while (offset < rdend && num_addrs < 16) {
            if (packet[offset] & 0xC0) {
                int ptr_offset = ((packet[offset] & 0x3F) << 8) | packet[offset + 1];
                offset = sizeof(dns_header_t) + ptr_offset;
                continue;
            }
            
            if (packet[offset] == 0) {
                offset++;
            } else {
                offset += packet[offset] + 1;
            }
            
            ip_addrs[num_addrs++] = switch_endian32(*((uint32_t*)(packet + offset)));
            offset += 4;
        }
        
        if (callback) {
            callback(1, NULL, ip_addrs, num_addrs);
        }
        
        break;
    }
}

static dns_callback_t g_dns_callback = NULL;
static uint8_t g_query_name[DNS_MAX_QNAME] = {0};

int dns_init(void) {
    debug_print("DNS: Initializing...\n");
    dns_query_id = 1;
    g_dns_callback = NULL;
    debug_print("DNS: Initialized\n");
    return 0;
}

static void dns_udp_handler(uint32_t src_ip, uint16_t src_port,
                         uint8_t* data, size_t len) {
    if (src_port == DNS_PORT && g_dns_callback) {
        dns_handle_response(data, len, g_dns_callback);
        g_dns_callback = NULL;
    }
}

int dns_query(uint8_t* name, uint8_t dns_server, dns_callback_t callback) {
    if (g_dns_callback) {
        return -1;
    }
    
    for (int i = 0; i < DNS_MAX_QNAME; i++) {
        g_query_name[i] = name[i];
    }
    
    g_dns_callback = callback;
    
    udp_bind(DNS_PORT, dns_udp_handler);
    
    return dns_send_query(g_query_name, dns_server);
}
