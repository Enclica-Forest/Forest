#ifndef X11_SOCKET_H
#define X11_SOCKET_H

#include <stdint.h>

#define X11_MAX_CLIENTS 16
#define X11_SOCKET_BUF  8192

typedef struct {
    int    fd;
    int    used;
    uint16_t seq;
    uint8_t  rx_buf[X11_SOCKET_BUF];
    int    rx_pos;
    int    rx_len;
    uint8_t  tx_buf[X11_SOCKET_BUF];
    int    tx_pos;
    int    tx_len;
} x11_client_t;

int          x11_socket_init(void);
int          x11_socket_accept(void);
void         x11_socket_close(int id);
int          x11_socket_read(int id, uint8_t* buf, int max);
int          x11_socket_write(int id, const uint8_t* buf, int len);
uint16_t       x11_client_next_seq(int id);
x11_client_t* x11_get_client(int id);
int          x11_socket_get_fd(int id);
int          x11_socket_get_listen_fd(void);
void         x11_socket_set_nonblock(int fd);

#endif
