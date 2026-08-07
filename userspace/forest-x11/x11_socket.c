#include "x11_socket.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <forestos/syscalls.h>
#include <sys/stat.h>

#ifndef AF_UNIX
#define AF_UNIX AF_LOCAL
#endif

struct x11_sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

static x11_client_t g_clients[X11_MAX_CLIENTS];
static int g_listen_fd = -1;

#define X11_SOCK_PATH "/tmp/.X11-unix/X0"

int x11_socket_init(void) {
    memset(g_clients, 0, sizeof(g_clients));
    mkdir("/tmp", 0755);
    mkdir("/tmp/.X11-unix", 0755);
    unlink(X11_SOCK_PATH);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) return -1;

    struct x11_sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, X11_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, 8) < 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    x11_socket_set_nonblock(g_listen_fd);
    return 0;
}

int x11_socket_accept(void) {
    if (g_listen_fd < 0) return -1;
    int fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0) return -1;

    for (int i = 0; i < X11_MAX_CLIENTS; i++) {
        if (!g_clients[i].used) {
            memset(&g_clients[i], 0, sizeof(x11_client_t));
            g_clients[i].fd = fd;
            g_clients[i].used = 1;
            g_clients[i].seq = 0;
            x11_socket_set_nonblock(fd);
            return i;
        }
    }
    close(fd);
    return -1;
}

void x11_socket_close(int id) {
    if (id < 0 || id >= X11_MAX_CLIENTS) return;
    if (g_clients[id].fd >= 0) close(g_clients[id].fd);
    memset(&g_clients[id], 0, sizeof(x11_client_t));
}

int x11_socket_read(int id, uint8_t* buf, int max) {
    if (id < 0 || id >= X11_MAX_CLIENTS || !g_clients[id].used) return -1;
    return read(g_clients[id].fd, buf, max);
}

int x11_socket_write(int id, const uint8_t* buf, int len) {
    if (id < 0 || id >= X11_MAX_CLIENTS || !g_clients[id].used) return -1;
    return write(g_clients[id].fd, buf, len);
}

uint16_t x11_client_next_seq(int id) {
    if (id < 0 || id >= X11_MAX_CLIENTS) return 0;
    return ++g_clients[id].seq;
}

x11_client_t* x11_get_client(int id) {
    if (id < 0 || id >= X11_MAX_CLIENTS) return NULL;
    if (!g_clients[id].used) return NULL;
    return &g_clients[id];
}

int x11_socket_get_fd(int id) {
    if (id < 0 || id >= X11_MAX_CLIENTS) return -1;
    return g_clients[id].fd;
}

int x11_socket_get_listen_fd(void) {
    return g_listen_fd;
}

void x11_socket_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
