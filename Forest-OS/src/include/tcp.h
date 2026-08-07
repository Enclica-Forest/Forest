#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stdbool.h>

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20
#define TCP_FLAG_ECE 0x40
#define TCP_FLAG_CWR 0x80

#define TCP_STATE_CLOSED 0
#define TCP_STATE_LISTEN 1
#define TCP_STATE_SYN_SENT 2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED 4
#define TCP_STATE_FIN_WAIT_1 5
#define TCP_STATE_FIN_WAIT_2 6
#define TCP_STATE_CLOSE_WAIT 7
#define TCP_STATE_CLOSING 8
#define TCP_STATE_LAST_ACK 9
#define TCP_STATE_TIME_WAIT 10

#define TCP_MAX_CONNECTIONS 16
#define TCP_WINDOW_SIZE 8192
#define TCP_MAX_RETRIES 3
#define TCP_RETRY_TIMEOUT_MS 5000
#define TCP_DEFAULT_MSS 1460
#define TCP_KEEPALIVE_TIMEOUT_MS 7200000

typedef struct tcp_connection tcp_connection_t;

typedef void (*tcp_receive_callback_t)(tcp_connection_t* conn, uint8_t* data, size_t len);
typedef void (*tcp_connect_callback_t)(tcp_connection_t* conn, int connected);
typedef void (*tcp_close_callback_t)(tcp_connection_t* conn);

struct tcp_connection {
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_seq;
    uint32_t remote_seq;
    uint8_t state;
    uint16_t window;
    uint8_t retries;
    uint32_t last_activity;
    
    tcp_receive_callback_t receive_cb;
    tcp_connect_callback_t connect_cb;
    tcp_close_callback_t close_cb;
    void* user_data;
    
    struct tcp_connection* next;
};

void tcp_init(void);
void tcp_handle_packet(uint8_t* packet, size_t len);
tcp_connection_t* tcp_connect(uint32_t ip, uint16_t port);
int tcp_listen(uint16_t port);
tcp_connection_t* tcp_accept(tcp_connection_t* listen_conn);
int tcp_send(tcp_connection_t* conn, uint8_t* data, size_t len);
int tcp_close(tcp_connection_t* conn);
void tcp_set_callbacks(tcp_connection_t* conn, tcp_receive_callback_t recv_cb,
                      tcp_connect_callback_t conn_cb, tcp_close_callback_t close_cb);
void tcp_set_user_data(tcp_connection_t* conn, void* data);
void tcp_keepalive(void);

#endif
