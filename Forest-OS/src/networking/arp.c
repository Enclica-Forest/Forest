#include "arp.h"
#include "network.h"
#include "../include/debug.h"
#include "../include/string.h"

void network_get_mac_address(uint8_t* mac);
uint32_t network_get_ip_address(void);
void network_send_packet(uint8_t* data, size_t len);

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_initialized = 0;

void arp_init(void) {
    debug_print("ARP: Initializing...\n");
    
    if (!arp_cache_initialized) {
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            arp_cache[i].valid = 0;
        }
        arp_cache_initialized = 1;
    }
    
    debug_print("ARP: Initialized\n");
}

static int arp_cache_add(uint8_t* ip, uint8_t* mac) {
    int oldest = 0;
    uint32_t oldest_time = (uint32_t)-1;
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && 
            memcmp(arp_cache[i].ip, ip, 4) == 0) {
            for (int j = 0; j < 6; j++) {
                arp_cache[i].mac[j] = mac[j];
            }
            return 1;
        }
    }
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            for (int j = 0; j < 4; j++) {
                arp_cache[i].ip[j] = ip[j];
            }
            for (int j = 0; j < 6; j++) {
                arp_cache[i].mac[j] = mac[j];
            }
            arp_cache[i].valid = 1;
            return 1;
        }
    }
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            oldest = i;
            break;
        }
    }
    
    for (int j = 0; j < 4; j++) {
        arp_cache[oldest].ip[j] = ip[j];
    }
    for (int j = 0; j < 6; j++) {
        arp_cache[oldest].mac[j] = mac[j];
    }
    arp_cache[oldest].valid = 1;
    
    return 1;
}

int arp_resolve(uint8_t* ip, uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && 
            memcmp(arp_cache[i].ip, ip, 4) == 0) {
            for (int j = 0; j < 6; j++) {
                mac[j] = arp_cache[i].mac[j];
            }
            return 1;
        }
    }
    return 0;
}

void arp_send_request(uint32_t ip) {
    uint8_t packet[ETH_HLEN + ARP_HLEN];
    eth_header_t* eth = (eth_header_t*)packet;
    arp_packet_t* arp = (arp_packet_t*)(packet + ETH_HLEN);
    
    network_get_mac_address(eth->src_mac);
    
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = 0xFF;
    }
    
    eth->ethertype = switch_endian16(ETH_P_ARP);
    
    arp->htype = switch_endian16(0x0001);
    arp->ptype = switch_endian16(ETH_P_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = switch_endian16(ARP_OP_REQUEST);
    
    network_get_mac_address(arp->sha);
    network_get_mac_address(eth->src_mac);
    
    uint32_t local_ip = network_get_ip_address();
    for (int i = 0; i < 4; i++) {
        arp->spa[i] = ((uint8_t*)&local_ip)[i];
    }
    
    for (int i = 0; i < 6; i++) {
        arp->tha[i] = 0;
    }
    
    for (int i = 0; i < 4; i++) {
        arp->tpa[i] = ((uint8_t*)&ip)[i];
    }
    
    network_send_packet(packet, sizeof(packet));
}

void arp_send_reply(uint32_t ip, uint8_t* mac) {
    uint8_t packet[ETH_HLEN + ARP_HLEN];
    eth_header_t* eth = (eth_header_t*)packet;
    arp_packet_t* arp = (arp_packet_t*)(packet + ETH_HLEN);
    
    network_get_mac_address(eth->src_mac);
    
    for (int i = 0; i < 6; i++) {
        eth->dst_mac[i] = mac[i];
    }
    
    eth->ethertype = switch_endian16(ETH_P_ARP);
    
    arp->htype = switch_endian16(0x0001);
    arp->ptype = switch_endian16(ETH_P_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = switch_endian16(ARP_OP_REPLY);
    
    network_get_mac_address(arp->sha);
    
    uint32_t local_ip = network_get_ip_address();
    for (int i = 0; i < 4; i++) {
        arp->spa[i] = ((uint8_t*)&local_ip)[i];
    }
    
    for (int i = 0; i < 6; i++) {
        arp->tha[i] = mac[i];
    }
    
    for (int i = 0; i < 4; i++) {
        arp->tpa[i] = ((uint8_t*)&ip)[i];
    }
    
    network_send_packet(packet, sizeof(packet));
}

void arp_handle_packet(uint8_t* packet, size_t len) {
    if (len < ARP_HLEN) {
        return;
    }
    
    arp_packet_t* arp = (arp_packet_t*)packet;
    
    uint16_t htype = switch_endian16(arp->htype);
    uint16_t ptype = switch_endian16(arp->ptype);
    uint16_t opcode = switch_endian16(arp->opcode);
    
    if (htype != 0x0001 || ptype != ETH_P_IP) {
        return;
    }
    
    uint32_t local_ip = network_get_ip_address();
    
    if (opcode == ARP_OP_REQUEST) {
        for (int i = 0; i < 4; i++) {
            if (arp->tpa[i] != ((uint8_t*)&local_ip)[i]) {
                return;
            }
        }
        
        uint32_t target_ip = 0;
        for (int i = 0; i < 4; i++) {
            ((uint8_t*)&target_ip)[i] = arp->spa[i];
        }
        
        uint8_t target_mac[6];
        for (int i = 0; i < 6; i++) {
            target_mac[i] = arp->sha[i];
        }
        
        arp_cache_add((uint8_t*)&target_ip, target_mac);
        arp_send_reply(target_ip, target_mac);
        
        debug_print("ARP: Received request from %d.%d.%d.%d, sent reply\n",
                 arp->spa[0], arp->spa[1], arp->spa[2], arp->spa[3]);
    } else if (opcode == ARP_OP_REPLY) {
        uint32_t sender_ip = 0;
        for (int i = 0; i < 4; i++) {
            ((uint8_t*)&sender_ip)[i] = arp->spa[i];
        }
        
        uint8_t sender_mac[6];
        for (int i = 0; i < 6; i++) {
            sender_mac[i] = arp->sha[i];
        }
        
        arp_cache_add((uint8_t*)&sender_ip, sender_mac);
        
        debug_print("ARP: Received reply from %d.%d.%d.%d at %02X:%02X:%02X:%02X:%02X:%02X\n",
                 sender_mac[0], sender_mac[1], sender_mac[2], sender_mac[3],
                 sender_mac[4], sender_mac[5]);
    }
}
