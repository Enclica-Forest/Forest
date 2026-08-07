#include "tcp.h"
#include "network.h"
#include "ip.h"
#include "../include/debug.h"
#include "../include/string.h"

uint32_t network_get_ip_address(void);
uint16_t switch_endian16(uint16_t nb);
uint32_t switch_endian32(uint32_t nb);
uint16_t calculate_checksum(uint8_t* data, size_t len);
uint16_t ip_pseudo_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t protocol, uint16_t len);
int ip_send_packet(uint8_t* dst_ip, uint8_t protocol, uint8_t* data, size_t len);

static tcp_connection_t tcp_connections[TCP_MAX_CONNECTIONS];
static tcp_connection_t* tcp_listen_socket = NULL;

void tcp_init(void) {
    debug_print("TCP: Initializing...\n");
    
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connections[i].state = TCP_STATE_CLOSED;
        tcp_connections[i].next = NULL;
    }
    
    debug_print("TCP: Initialized\n");
}

static uint32_t generate_isn(void) {
    static uint32_t isn = 12345;
    isn += 65536;
    return isn;
}

static tcp_connection_t* tcp_find_connection(uint32_t local_ip, uint16_t local_port,
                                      uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state != TCP_STATE_CLOSED &&
            tcp_connections[i].local_ip == local_ip &&
            tcp_connections[i].local_port == local_port &&
            tcp_connections[i].remote_ip == remote_ip &&
            tcp_connections[i].remote_port == remote_port) {
            return &tcp_connections[i];
        }
    }
    return NULL;
}

static tcp_connection_t* tcp_allocate_connection(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_STATE_CLOSED) {
            return &tcp_connections[i];
        }
    }
    return NULL;
}

tcp_connection_t* tcp_connect(uint32_t ip, uint16_t port) {
    uint32_t local_ip = network_get_ip_address();
    tcp_connection_t* conn = tcp_find_connection(local_ip, 0, ip, port);
    
    if (conn) {
        return conn;
    }
    
    conn = tcp_allocate_connection();
    if (!conn) {
        return NULL;
    }
    
    conn->local_ip = local_ip;
    conn->remote_ip = ip;
    conn->local_port = 0;
    conn->remote_port = port;
    conn->local_seq = generate_isn();
    conn->remote_seq = 0;
    conn->state = TCP_STATE_CLOSED;
    conn->window = TCP_WINDOW_SIZE;
    conn->retries = 0;
    conn->receive_cb = NULL;
    conn->connect_cb = NULL;
    conn->close_cb = NULL;
    conn->user_data = NULL;
    conn->next = NULL;
    
    uint8_t packet[ETH_HLEN + IP_HLEN + TCP_HLEN];
    tcp_header_t* tcp = (tcp_header_t*)(packet + ETH_HLEN + IP_HLEN);
    
    tcp->sport = switch_endian16(conn->local_port);
    tcp->dport = switch_endian16(conn->remote_port);
    tcp->seq = switch_endian32(conn->local_seq);
    tcp->ack = switch_endian32(0);
    tcp->offset_res = 0x50;
    tcp->flags = TCP_FLAG_SYN;
    tcp->window = switch_endian16(conn->window);
    tcp->check = 0;
    tcp->urg_ptr = 0;
    
    uint8_t remote_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        remote_ip_bytes[i] = ((uint8_t*)&ip)[i];
    }
    
    uint8_t src_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&local_ip)[i];
    }
    
    tcp->check = ip_pseudo_checksum(src_ip_bytes, remote_ip_bytes, IP_PROTO_TCP, TCP_HLEN);
    tcp->check = calculate_checksum((uint8_t*)tcp, TCP_HLEN);
    
    ip_send_packet(remote_ip_bytes, IP_PROTO_TCP, (uint8_t*)tcp, TCP_HLEN);
    
    conn->state = TCP_STATE_SYN_SENT;
    conn->local_seq++;
    
    return conn;
}

int tcp_listen(uint16_t port) {
    if (tcp_listen_socket) {
        return -1;
    }
    
    tcp_listen_socket = tcp_allocate_connection();
    if (!tcp_listen_socket) {
        return -1;
    }
    
    uint32_t local_ip = network_get_ip_address();
    tcp_listen_socket->local_ip = local_ip;
    tcp_listen_socket->remote_ip = 0;
    tcp_listen_socket->local_port = port;
    tcp_listen_socket->remote_port = 0;
    tcp_listen_socket->local_seq = generate_isn();
    tcp_listen_socket->remote_seq = 0;
    tcp_listen_socket->state = TCP_STATE_LISTEN;
    tcp_listen_socket->window = TCP_WINDOW_SIZE;
    tcp_listen_socket->retries = 0;
    tcp_listen_socket->receive_cb = NULL;
    tcp_listen_socket->connect_cb = NULL;
    tcp_listen_socket->close_cb = NULL;
    tcp_listen_socket->user_data = NULL;
    tcp_listen_socket->next = NULL;
    
    return 0;
}

tcp_connection_t* tcp_accept(tcp_connection_t* listen_conn) {
    if (listen_conn != tcp_listen_socket) {
        return NULL;
    }
    
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].state == TCP_STATE_ESTABLISHED &&
            tcp_connections[i].local_ip == listen_conn->local_ip &&
            tcp_connections[i].local_port == listen_conn->local_port) {
            
            tcp_connections[i].state = TCP_STATE_CLOSED;
            
            tcp_connection_t* new_conn = tcp_allocate_connection();
            if (new_conn) {
                *new_conn = tcp_connections[i];
                new_conn->state = TCP_STATE_ESTABLISHED;
                return new_conn;
            }
            
            tcp_connections[i].state = TCP_STATE_ESTABLISHED;
            return &tcp_connections[i];
        }
    }
    
    return NULL;
}

int tcp_send(tcp_connection_t* conn, uint8_t* data, size_t len) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED || len == 0) {
        return -1;
    }
    
    if (len > NETWORK_MAX_PACKET_SIZE - IP_HLEN - TCP_HLEN) {
        return -1;
    }
    
    uint8_t packet[ETH_HLEN + IP_HLEN + TCP_HLEN + len];
    tcp_header_t* tcp = (tcp_header_t*)(packet + ETH_HLEN + IP_HLEN);
    uint8_t* payload = packet + ETH_HLEN + IP_HLEN + TCP_HLEN;
    
    tcp->sport = switch_endian16(conn->local_port);
    tcp->dport = switch_endian16(conn->remote_port);
    tcp->seq = switch_endian32(conn->local_seq);
    tcp->ack = switch_endian32(conn->remote_seq);
    tcp->offset_res = (5 << 4) | TCP_FLAG_PSH | TCP_FLAG_ACK;
    tcp->window = switch_endian16(conn->window);
    tcp->check = 0;
    tcp->urg_ptr = 0;
    
    for (size_t i = 0; i < len; i++) {
        payload[i] = data[i];
    }
    
    uint8_t remote_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        remote_ip_bytes[i] = ((uint8_t*)&conn->remote_ip)[i];
    }
    
    uint8_t src_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&conn->local_ip)[i];
    }
    
    tcp->check = ip_pseudo_checksum(src_ip_bytes, remote_ip_bytes, IP_PROTO_TCP, TCP_HLEN + len);
    tcp->check = calculate_checksum((uint8_t*)tcp, TCP_HLEN + len);
    
    ip_send_packet(remote_ip_bytes, IP_PROTO_TCP, (uint8_t*)tcp, TCP_HLEN + len);
    
    conn->local_seq += len;
    
    return len;
}

int tcp_close(tcp_connection_t* conn) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) {
        return -1;
    }
    
    uint8_t packet[ETH_HLEN + IP_HLEN + TCP_HLEN];
    tcp_header_t* tcp = (tcp_header_t*)(packet + ETH_HLEN + IP_HLEN);
    
    tcp->sport = switch_endian16(conn->local_port);
    tcp->dport = switch_endian16(conn->remote_port);
    tcp->seq = switch_endian32(conn->local_seq);
    tcp->ack = switch_endian32(conn->remote_seq);
    tcp->offset_res = (5 << 4) | TCP_FLAG_FIN | TCP_FLAG_ACK;
    tcp->window = switch_endian16(conn->window);
    tcp->check = 0;
    tcp->urg_ptr = 0;
    
    uint8_t remote_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        remote_ip_bytes[i] = ((uint8_t*)&conn->remote_ip)[i];
    }
    
    uint8_t src_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&conn->local_ip)[i];
    }
    
    tcp->check = ip_pseudo_checksum(src_ip_bytes, remote_ip_bytes, IP_PROTO_TCP, TCP_HLEN);
    tcp->check = calculate_checksum((uint8_t*)tcp, TCP_HLEN);
    
    ip_send_packet(remote_ip_bytes, IP_PROTO_TCP, (uint8_t*)tcp, TCP_HLEN);
    
    conn->state = TCP_STATE_FIN_WAIT_1;
    conn->local_seq++;
    
    return 0;
}

void tcp_set_callbacks(tcp_connection_t* conn, tcp_receive_callback_t recv_cb,
                      tcp_connect_callback_t conn_cb, tcp_close_callback_t close_cb) {
    if (conn) {
        conn->receive_cb = recv_cb;
        conn->connect_cb = conn_cb;
        conn->close_cb = close_cb;
    }
}

void tcp_set_user_data(tcp_connection_t* conn, void* data) {
    if (conn) {
        conn->user_data = data;
    }
}

static void tcp_send_ack(tcp_connection_t* conn) {
    uint8_t packet[ETH_HLEN + IP_HLEN + TCP_HLEN];
    tcp_header_t* tcp = (tcp_header_t*)(packet + ETH_HLEN + IP_HLEN);
    
    tcp->sport = switch_endian16(conn->local_port);
    tcp->dport = switch_endian16(conn->remote_port);
    tcp->seq = switch_endian32(conn->local_seq);
    tcp->ack = switch_endian32(conn->remote_seq);
    tcp->offset_res = (5 << 4) | TCP_FLAG_ACK;
    tcp->window = switch_endian16(conn->window);
    tcp->check = 0;
    tcp->urg_ptr = 0;
    
    uint8_t remote_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        remote_ip_bytes[i] = ((uint8_t*)&conn->remote_ip)[i];
    }
    
    uint8_t src_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&conn->local_ip)[i];
    }
    
    tcp->check = ip_pseudo_checksum(src_ip_bytes, remote_ip_bytes, IP_PROTO_TCP, TCP_HLEN);
    tcp->check = calculate_checksum((uint8_t*)tcp, TCP_HLEN);
    
    ip_send_packet(remote_ip_bytes, IP_PROTO_TCP, (uint8_t*)tcp, TCP_HLEN);
}

static void tcp_send_syn_ack(tcp_connection_t* conn) {
    uint8_t packet[ETH_HLEN + IP_HLEN + TCP_HLEN];
    tcp_header_t* tcp = (tcp_header_t*)(packet + ETH_HLEN + IP_HLEN);
    
    tcp->sport = switch_endian16(conn->local_port);
    tcp->dport = switch_endian16(conn->remote_port);
    tcp->seq = switch_endian32(conn->local_seq);
    tcp->ack = switch_endian32(conn->remote_seq);
    tcp->offset_res = (5 << 4) | TCP_FLAG_SYN | TCP_FLAG_ACK;
    tcp->window = switch_endian16(conn->window);
    tcp->check = 0;
    tcp->urg_ptr = 0;
    
    uint8_t remote_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        remote_ip_bytes[i] = ((uint8_t*)&conn->remote_ip)[i];
    }
    
    uint8_t src_ip_bytes[4];
    for (int i = 0; i < 4; i++) {
        src_ip_bytes[i] = ((uint8_t*)&conn->local_ip)[i];
    }
    
    tcp->check = ip_pseudo_checksum(src_ip_bytes, remote_ip_bytes, IP_PROTO_TCP, TCP_HLEN);
    tcp->check = calculate_checksum((uint8_t*)tcp, TCP_HLEN);
    
    ip_send_packet(remote_ip_bytes, IP_PROTO_TCP, (uint8_t*)tcp, TCP_HLEN);
    
    conn->local_seq++;
}

void tcp_handle_packet(uint8_t* packet, size_t len) {
    if (len < TCP_HLEN) {
        return;
    }
    
    tcp_header_t* tcp = (tcp_header_t*)packet;
    
    uint16_t sport = switch_endian16(tcp->sport);
    uint16_t dport = switch_endian16(tcp->dport);
    uint32_t seq = switch_endian32(tcp->seq);
    uint32_t ack = switch_endian32(tcp->ack);
    uint8_t flags = tcp->flags;
    uint16_t window = switch_endian16(tcp->window);
    
    uint8_t offset = (tcp->offset_res & 0xF0) >> 4;
    uint8_t data_offset = offset * 4;
    
    uint8_t* data = packet + data_offset;
    size_t data_len = len - data_offset;
    
    uint32_t local_ip = network_get_ip_address();
    uint32_t src_ip = 0;
    
    tcp_connection_t* conn = tcp_find_connection(local_ip, dport, src_ip, sport);
    
    if (!conn && tcp_listen_socket && tcp_listen_socket->local_port == dport &&
        (flags & TCP_FLAG_SYN)) {
        conn = tcp_allocate_connection();
        if (conn) {
            conn->local_ip = local_ip;
            conn->remote_ip = src_ip;
            conn->local_port = dport;
            conn->remote_port = sport;
            conn->local_seq = tcp_listen_socket->local_seq;
            conn->remote_seq = seq + 1;
            conn->state = TCP_STATE_SYN_RECEIVED;
            conn->window = window;
            conn->retries = 0;
            conn->receive_cb = tcp_listen_socket->receive_cb;
            conn->connect_cb = tcp_listen_socket->connect_cb;
            conn->close_cb = tcp_listen_socket->close_cb;
            conn->user_data = tcp_listen_socket->user_data;
            conn->next = NULL;
            
            tcp_send_syn_ack(conn);
            conn->local_seq++;
        }
    } else if (conn) {
        if (flags & TCP_FLAG_SYN && flags & TCP_FLAG_ACK) {
            if (conn->state == TCP_STATE_SYN_SENT) {
                conn->state = TCP_STATE_ESTABLISHED;
                conn->remote_seq = seq + 1;
                
                if (conn->connect_cb) {
                    conn->connect_cb(conn, 1);
                }
            }
        } else if (flags & TCP_FLAG_FIN) {
            if (conn->state == TCP_STATE_ESTABLISHED ||
                conn->state == TCP_STATE_FIN_WAIT_2) {
                
                conn->remote_seq = seq + 1;
                tcp_send_ack(conn);
                
                if (conn->state == TCP_STATE_ESTABLISHED) {
                    conn->state = TCP_STATE_CLOSE_WAIT;
                } else {
                    conn->state = TCP_STATE_CLOSED;
                    
                    if (conn->close_cb) {
                        conn->close_cb(conn);
                    }
                }
            }
        } else if (flags & TCP_FLAG_ACK) {
            conn->remote_seq = seq + data_len;
            
            if (ack > conn->local_seq) {
                conn->window = window;
            }
            
            if (data_len > 0 && conn->receive_cb) {
                conn->receive_cb(conn, data, data_len);
            }
            
            if (!(flags & TCP_FLAG_PSH)) {
                tcp_send_ack(conn);
            }
        }
    }
}
