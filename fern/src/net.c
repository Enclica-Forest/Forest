#include "include/net.h"
#include "include/util.h"
#include "include/string.h"
#include "include/libc/stdlib.h"
#include "include/libc/errno.h"
#include "include/screen.h"
#include "include/spinlock.h"
#include "include/task.h"
#include "arch/net.h"

#define NET_MAX_SOCKETS 16
#define NET_SOCKET_FD_BASE 64
#define NET_SOCKET_QUEUE 8
#define NET_MAX_PAYLOAD 1024
#define NET_EPHEMERAL_BASE 40000

typedef struct {
    uint32 src_addr;
    uint16 src_port;
    uint32 dest_addr;
    uint16 dest_port;
    uint32 length;
    uint8 data[NET_MAX_PAYLOAD];
} net_datagram_t;

typedef struct {
    bool used;
    bool bound;
    uint16 port;
    uint32 owner_pid;
    net_datagram_t queue[NET_SOCKET_QUEUE];
    uint8 head;
    uint8 tail;
    uint8 count;
    uint32 bytes_sent;
    uint32 bytes_received;
    uint32 last_peer_addr;
    uint16 last_peer_port;
    bool   reuseaddr;
    uint32 sock_type;
    bool   nonblock;
    bool   connected;
    bool   listening;
    int32  backlog;
    uint32 local_addr;
    uint32 peer_addr;
    uint16 peer_port;
    int32  so_rcvbuf;
    int32  so_sndbuf;
    int32  so_rcvtimeo_ms;
    int32  so_sndtimeo_ms;
    bool   keepalive;
    bool   broadcast;
    bool   nodelay;
    int32  maxseg;
    int32  keepidle;
    int32  keepintvl;
    int32  keepcnt;
    bool   linger_on;
    int32  linger_time;
    int32  so_error;
    bool   shut_read;
    bool   shut_write;
} net_socket_t;

static net_socket_t g_sockets[NET_MAX_SOCKETS];
static spinlock_t g_net_lock = SPINLOCK_INIT("net_sockets");
static bool g_net_ready = false;
static uint16 g_next_ephemeral_port = NET_EPHEMERAL_BASE;
static driver_t g_loopback_driver;

static uint16 net_allocate_ephemeral_port(void) {
    uint16 port = g_next_ephemeral_port++;
    if (g_next_ephemeral_port == 0) {
        g_next_ephemeral_port = NET_EPHEMERAL_BASE;
    }
    return port;
}

static net_socket_t* net_socket_from_fd(uint32 fd) {
    if (fd < NET_SOCKET_FD_BASE) {
        return 0;
    }
    uint32 index = fd - NET_SOCKET_FD_BASE;
    if (index >= NET_MAX_SOCKETS) {
        return 0;
    }
    return g_sockets[index].used ? &g_sockets[index] : 0;
}

static net_socket_t* net_socket_by_port(uint16 port) {
    if (!port) {
        return 0;
    }
    for (uint32 i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_sockets[i].used && g_sockets[i].bound && g_sockets[i].port == port) {
            return &g_sockets[i];
        }
    }
    return 0;
}

static bool net_socket_queue_push(net_socket_t* sock, const net_datagram_t* msg) {
    if (!sock || sock->count >= NET_SOCKET_QUEUE) {
        return false;
    }
    sock->queue[sock->tail] = *msg;
    sock->tail = (sock->tail + 1) % NET_SOCKET_QUEUE;
    sock->count++;
    return true;
}

static bool net_socket_queue_pop(net_socket_t* sock, net_datagram_t* out) {
    if (!sock || sock->count == 0) {
        return false;
    }
    *out = sock->queue[sock->head];
    sock->head = (sock->head + 1) % NET_SOCKET_QUEUE;
    sock->count--;
    return true;
}

static bool loopback_driver_init(driver_t* driver) {
    (void)driver;
    print("[NET] Loopback driver online\n");
    return true;
}

bool net_init(void) {
    if (g_net_ready) {
        return true;
    }

    memory_set((uint8*)g_sockets, 0, sizeof(g_sockets));

    g_loopback_driver.name = "loopback-net";
    g_loopback_driver.driver_class = DRIVER_CLASS_NETWORK;
    g_loopback_driver.init = loopback_driver_init;
    g_loopback_driver.shutdown = 0;
    g_loopback_driver.context = 0;
    g_loopback_driver.id = 0;
    g_loopback_driver.initialized = false;

    if (!driver_register(&g_loopback_driver)) {
        print("[NET] Failed to register loopback driver\n");
        return false;
    }

    g_net_ready = true;

    /* Initialize the architecture-appropriate NIC driver (PCI on x86,
     * virtio-net MMIO on AArch64/RISC-V). This registers a NIC with
     * the net_nic_driver_t layer so packets can flow. */
    arch_network_init();

    return true;
}

bool net_is_fd(uint32 fd) {
    return net_socket_from_fd(fd) != 0;
}

int32 net_close(uint32 fd) {
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return -1;
    }
    memory_set((uint8*)sock, 0, sizeof(net_socket_t));
    spinlock_release(&g_net_lock);
    return 0;
}

int32 net_socket_create(uint32 domain, uint32 type, uint32 protocol) {
    if (domain != AF_INET) {
        return -1;
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_RAW) {
        return -1;
    }
    (void)protocol;

    spinlock_acquire(&g_net_lock);
    for (uint32 i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_sockets[i].used) {
            memory_set((uint8*)&g_sockets[i], 0, sizeof(net_socket_t));
            g_sockets[i].used = true;
            g_sockets[i].bound = false;
            g_sockets[i].port = 0;
            g_sockets[i].owner_pid = current_task ? current_task->id : 0;
            g_sockets[i].sock_type = type;
            g_sockets[i].so_rcvbuf = 65536;
            g_sockets[i].so_sndbuf = 65536;
            spinlock_release(&g_net_lock);
            return (int32)(NET_SOCKET_FD_BASE + i);
        }
    }
    spinlock_release(&g_net_lock);
    return -1;
}

void net_close_all_for_task(uint32 pid) {
    if (pid == 0) {
        return;
    }
    spinlock_acquire(&g_net_lock);
    for (uint32 i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_sockets[i].used && g_sockets[i].owner_pid == pid) {
            memory_set((uint8*)&g_sockets[i], 0, sizeof(net_socket_t));
        }
    }
    spinlock_release(&g_net_lock);
}

int32 net_bind(uint32 fd, uint16 port) {
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return -1;
    }
    if (sock->bound) {
        spinlock_release(&g_net_lock);
        return -1;
    }
    if (port == 0) {
        port = net_allocate_ephemeral_port();
    }
    if (net_socket_by_port(port)) {
        spinlock_release(&g_net_lock);
        return -1;
    }
    sock->port = port;
    sock->bound = true;
    sock->local_addr = INADDR_LOOPBACK;
    spinlock_release(&g_net_lock);
    return 0;
}

static void net_emit_rx_event(uint16 port, uint32 length) {
    struct {
        uint16 port;
        uint32 length;
    } payload;
    payload.port = port;
    payload.length = length;
    driver_emit_event(g_loopback_driver.id, DRIVER_CLASS_NETWORK,
                      DRIVER_EVENT_NETWORK_RX_READY, &payload, sizeof(payload));
}

static bool net_send_virtual_response(const net_datagram_t* request,
                                      const uint8* payload, uint32 length,
                                      uint16 response_port) {
    net_socket_t* reply = net_socket_by_port(request->src_port);
    if (!reply || !payload || length == 0 || length > NET_MAX_PAYLOAD) {
        return false;
    }

    net_datagram_t response;
    memory_set((uint8*)&response, 0, sizeof(response));
    response.src_addr = request->dest_addr;
    response.src_port = response_port;
    response.dest_addr = request->src_addr;
    response.dest_port = request->src_port;
    response.length = length;
    memory_copy((char*)payload, (char*)response.data, length);

    if (!net_socket_queue_push(reply, &response)) {
        return false;
    }
    reply->last_peer_addr = request->dest_addr;
    reply->last_peer_port = response_port;
    reply->bytes_received += length;
    net_emit_rx_event(reply->port, response.length);
    return true;
}

static bool net_try_virtual_service(const net_datagram_t* msg) {
    if (!msg) {
        return false;
    }

    switch (msg->dest_port) {
        case NET_PORT_ECHO:
            return net_send_virtual_response(msg, msg->data, msg->length, NET_PORT_ECHO);
        case NET_PORT_HTTP: {
            const char body[] = "Forest loopback HTTP endpoint.\n"
                                "Available services: echo, ssh, ftp, rsync.\n";
            char buffer[NET_MAX_PAYLOAD];
            memory_set((uint8*)buffer, 0, sizeof(buffer));
            const char header[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";
            uint32 header_len = (uint32)strlen(header);
            memory_copy((char*)header, buffer, header_len);
            char length_field[16];
            uint32 body_len = (uint32)strlen(body);
            itoa((int)body_len, length_field, 10);
            uint32 len_len = (uint32)strlen(length_field);
            memory_copy(length_field, buffer + header_len, len_len);
            const char end_headers[] = "\r\n\r\n";
            memory_copy(end_headers, buffer + header_len + len_len, 4);
            memory_copy((char*)body, buffer + header_len + len_len + 4, body_len);
            uint32 total_len = header_len + len_len + 4 + body_len;
            return net_send_virtual_response(msg, (uint8*)buffer, total_len, NET_PORT_HTTP);
        }
        case NET_PORT_FTP: {
            const char banner[] = "220 Forest loopback FTP ready. Try wget/curl for HTTP.\n";
            return net_send_virtual_response(msg, (const uint8*)banner,
                                             (uint32)strlen(banner), NET_PORT_FTP);
        }
        case NET_PORT_SSH: {
            const char banner[] = "SSH-0.1-ForestOS loopback\n";
            return net_send_virtual_response(msg, (const uint8*)banner,
                                             (uint32)strlen(banner), NET_PORT_SSH);
        }
        case NET_PORT_RSYNCD: {
            const char banner[] = "@RSYNCD: 0.1 Forest loopback ready\n";
            return net_send_virtual_response(msg, (const uint8*)banner,
                                             (uint32)strlen(banner), NET_PORT_RSYNCD);
        }
        case NET_PORT_SFTP: {
            const char banner[] = "115 Forest SFTP loopback greeting\n";
            return net_send_virtual_response(msg, (const uint8*)banner,
                                             (uint32)strlen(banner), NET_PORT_SFTP);
        }
        default:
            return false;
    }
}

static int32 net_deliver_local(const net_datagram_t* msg) {
    net_socket_t* dest = net_socket_by_port(msg->dest_port);
    if (!dest) {
        return net_try_virtual_service(msg) ? (int32)msg->length : (int32)msg->length;
    }
    if (!net_socket_queue_push(dest, msg)) {
        return -1;
    }
    dest->last_peer_addr = msg->src_addr;
    dest->last_peer_port = msg->src_port;
    dest->bytes_received += msg->length;
    net_emit_rx_event(dest->port, msg->length);
    return (int32)msg->length;
}

int32 net_send_datagram(uint32 fd, const uint8* buffer, uint32 length,
                        uint32 dest_addr, uint16 dest_port) {
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock || !buffer || length == 0 || length > NET_MAX_PAYLOAD) {
        return -1;
    }
    if (!sock->bound) {
        // Automatically bind if not already bound
        if (net_bind(fd, 0) != 0) {
            return -1;
        }
    }

    net_datagram_t msg;
    memory_set((uint8*)&msg, 0, sizeof(msg));
    msg.src_addr = INADDR_LOOPBACK;
    msg.src_port = sock->port;
    msg.dest_addr = dest_addr ? dest_addr : INADDR_LOOPBACK;
    msg.dest_port = dest_port;
    msg.length = length;
    memory_copy((char*)buffer, (char*)msg.data, length);

    sock->bytes_sent += length;
    sock->last_peer_addr = dest_addr;
    sock->last_peer_port = dest_port;

    return net_deliver_local(&msg);
}

int32 net_recv_datagram(uint32 fd, uint8* buffer, uint32 length,
                        uint32* out_addr, uint16* out_port) {
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock || !buffer || length == 0) {
        return -1;
    }
    net_datagram_t msg;
    if (!net_socket_queue_pop(sock, &msg)) {
        if (sock->nonblock) {
            return -EAGAIN;
        }
        return -1;
    }

    uint32 to_copy = (msg.length < length) ? msg.length : length;
    memory_copy((char*)msg.data, (char*)buffer, to_copy);
    if (out_addr) {
        *out_addr = msg.src_addr;
    }
    if (out_port) {
        *out_port = msg.src_port;
    }
    sock->bytes_received += to_copy;
    return (int32)to_copy;
}

uint32 net_snapshot(net_socket_info_t* out, uint32 max_entries) {
    if (!out || max_entries == 0) {
        return 0;
    }

    uint32 count = 0;
    for (uint32 i = 0; i < NET_MAX_SOCKETS && count < max_entries; i++) {
        if (!g_sockets[i].used) {
            continue;
        }
        net_socket_info_t* dst = &out[count++];
        dst->used = g_sockets[i].used;
        dst->bound = g_sockets[i].bound;
        dst->port = g_sockets[i].port;
        dst->queue_depth = g_sockets[i].count;
        dst->queue_capacity = NET_SOCKET_QUEUE;
        dst->bytes_sent = g_sockets[i].bytes_sent;
        dst->bytes_received = g_sockets[i].bytes_received;
        dst->last_peer_addr = g_sockets[i].last_peer_addr;
        dst->last_peer_port = g_sockets[i].last_peer_port;
    }
    return count;
}

/* ===================================================================
 * POSIX-compatible socket surface + SIOC* ioctls + NIC registration.
 * Augments the original loopback datagram fabric with connection state,
 * socket options, non-blocking semantics, and the network interface
 * ioctls used by ifconfig/ip/netstat-style userspace tools.
 * =================================================================== */

#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ 89
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 102
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT 123
#endif

static int32 net_errno(int32 code) { return -code; }

int32 net_socket_set_nonblocking(uint32 fd, bool nonblock) {
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return -1;
    }
    sock->nonblock = nonblock;
    spinlock_release(&g_net_lock);
    return 0;
}

int32 net_socket_type(uint32 fd) {
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        return -1;
    }
    return (int32)sock->sock_type;
}

int32 net_connect(uint32 fd, const sockaddr_in_t* addr, int32 addrlen) {
    if (!addr || addrlen < (int32)sizeof(sockaddr_in_t)) {
        return net_errno(EINVAL);
    }
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return net_errno(EBADF);
    }
    sock->connected = true;
    sock->peer_addr = addr->sin_addr;
    sock->peer_port = ntohs(addr->sin_port);
    sock->so_error = 0;
    spinlock_release(&g_net_lock);
    return 0;
}

int32 net_listen(uint32 fd, int32 backlog) {
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return net_errno(EBADF);
    }
    if (sock->sock_type != SOCK_STREAM) {
        spinlock_release(&g_net_lock);
        return net_errno(EOPNOTSUPP);
    }
    sock->listening = true;
    sock->backlog = backlog > 0 ? backlog : 1;
    spinlock_release(&g_net_lock);
    return 0;
}

int32 net_accept(uint32 fd, sockaddr_in_t* addr, int32* addrlen) {
    (void)addr;
    (void)addrlen;
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        return net_errno(EBADF);
    }
    if (!sock->listening) {
        return net_errno(EINVAL);
    }
    if (sock->nonblock) {
        return net_errno(EAGAIN);
    }
    return net_errno(EAGAIN);
}

int32 net_send(uint32 fd, const void* buf, uint32 len, int32 flags) {
    (void)flags;
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock || !buf || len == 0) {
        return net_errno(EINVAL);
    }
    if (sock->shut_write) {
        return net_errno(EPIPE);
    }
    uint32 dest_addr = sock->connected ? sock->peer_addr : 0;
    uint16 dest_port = sock->connected ? sock->peer_port : 0;
    if (!dest_port) {
        if (sock->nonblock) {
            return net_errno(EDESTADDRREQ);
        }
        return net_errno(EDESTADDRREQ);
    }
    return net_send_datagram(fd, (const uint8*)buf, len, dest_addr, dest_port);
}

int32 net_recv(uint32 fd, void* buf, uint32 len, int32 flags) {
    (void)flags;
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock || !buf || len == 0) {
        return net_errno(EINVAL);
    }
    if (sock->shut_read) {
        return 0;
    }
    return net_recv_datagram(fd, (uint8*)buf, len, 0, 0);
}

int32 net_sendto(uint32 fd, const void* buf, uint32 len, int32 flags,
                 const sockaddr_in_t* addr, int32 addrlen) {
    (void)flags;
    if (addr) {
        if (addrlen < (int32)sizeof(sockaddr_in_t)) {
            return net_errno(EINVAL);
        }
        return net_send_datagram(fd, (const uint8*)buf, len,
                                 addr->sin_addr, ntohs(addr->sin_port));
    }
    return net_send(fd, buf, len, flags);
}

int32 net_recvfrom(uint32 fd, void* buf, uint32 len, int32 flags,
                   sockaddr_in_t* addr, int32* addrlen) {
    (void)flags;
    uint32 src_addr = 0;
    uint16 src_port = 0;
    int32 got = net_recv_datagram(fd, (uint8*)buf, len, &src_addr, &src_port);
    if (got < 0) {
        return got;
    }
    if (addr && addrlen && *addrlen >= (int32)sizeof(sockaddr_in_t)) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(src_port);
        addr->sin_addr = src_addr;
        memory_set((uint8*)addr->sin_zero, 0, sizeof(addr->sin_zero));
        *addrlen = sizeof(sockaddr_in_t);
    }
    return got;
}

int32 net_setsockopt(uint32 fd, int32 level, int32 optname,
                     const void* optval, int32 optlen) {
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return net_errno(EBADF);
    }
    if (!optval || optlen < (int32)sizeof(int32)) {
        spinlock_release(&g_net_lock);
        return net_errno(EINVAL);
    }
    int32 val = *(const int32*)optval;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:  sock->reuseaddr = (val != 0); break;
            case SO_KEEPALIVE:  sock->keepalive = (val != 0); break;
            case SO_BROADCAST:  sock->broadcast = (val != 0); break;
            case SO_RCVBUF:     sock->so_rcvbuf = val; break;
            case SO_SNDBUF:      sock->so_sndbuf = val; break;
            case SO_RCVTIMEO:   sock->so_rcvtimeo_ms = val; break;
            case SO_SNDTIMEO:   sock->so_sndtimeo_ms = val; break;
            case SO_LINGER: {
                if (optlen >= (int32)(sizeof(int32) * 2)) {
                    const int32* lv = (const int32*)optval;
                    sock->linger_on = (lv[0] != 0);
                    sock->linger_time = lv[1];
                } else {
                    sock->linger_on = (val != 0);
                }
                break;
            }
            default:
                spinlock_release(&g_net_lock);
                return net_errno(ENOPROTOOPT);
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
            case TCP_NODELAY:   sock->nodelay = (val != 0); break;
            case TCP_MAXSEG:    sock->maxseg = val; break;
            case TCP_KEEPIDLE:  sock->keepidle = val; break;
            case TCP_KEEPINTVL: sock->keepintvl = val; break;
            case TCP_KEEPCNT:   sock->keepcnt = val; break;
            default:
                spinlock_release(&g_net_lock);
                return net_errno(ENOPROTOOPT);
        }
    } else {
        spinlock_release(&g_net_lock);
        return net_errno(EOPNOTSUPP);
    }
    spinlock_release(&g_net_lock);
    return 0;
}

int32 net_getsockopt(uint32 fd, int32 level, int32 optname,
                     void* optval, int32* optlen) {
    if (!optval || !optlen || *optlen < (int32)sizeof(int32)) {
        return net_errno(EINVAL);
    }
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return net_errno(EBADF);
    }
    int32 val = 0;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:  val = sock->reuseaddr ? 1 : 0; break;
            case SO_KEEPALIVE:  val = sock->keepalive ? 1 : 0; break;
            case SO_BROADCAST:  val = sock->broadcast ? 1 : 0; break;
            case SO_RCVBUF:     val = sock->so_rcvbuf; break;
            case SO_SNDBUF:     val = sock->so_sndbuf; break;
            case SO_RCVTIMEO:   val = sock->so_rcvtimeo_ms; break;
            case SO_SNDTIMEO:   val = sock->so_sndtimeo_ms; break;
            case SO_TYPE:       val = (int32)sock->sock_type; break;
            case SO_ERROR:      val = sock->so_error; sock->so_error = 0; break;
            case SO_LINGER: {
                if (*optlen >= (int32)(sizeof(int32) * 2)) {
                    int32* lv = (int32*)optval;
                    lv[0] = sock->linger_on ? 1 : 0;
                    lv[1] = sock->linger_time;
                    *optlen = (int32)(sizeof(int32) * 2);
                    spinlock_release(&g_net_lock);
                    return 0;
                }
                val = sock->linger_on ? 1 : 0;
                break;
            }
            default:
                spinlock_release(&g_net_lock);
                return net_errno(ENOPROTOOPT);
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
            case TCP_NODELAY:   val = sock->nodelay ? 1 : 0; break;
            case TCP_MAXSEG:    val = sock->maxseg; break;
            case TCP_KEEPIDLE:  val = sock->keepidle; break;
            case TCP_KEEPINTVL: val = sock->keepintvl; break;
            case TCP_KEEPCNT:   val = sock->keepcnt; break;
            default:
                spinlock_release(&g_net_lock);
                return net_errno(ENOPROTOOPT);
        }
    } else {
        spinlock_release(&g_net_lock);
        return net_errno(EOPNOTSUPP);
    }
    *(int32*)optval = val;
    *optlen = sizeof(int32);
    spinlock_release(&g_net_lock);
    return 0;
}

int32 net_getsockname(uint32 fd, sockaddr_in_t* addr, int32* addrlen) {
    if (!addr || !addrlen || *addrlen < (int32)sizeof(sockaddr_in_t)) {
        return net_errno(EINVAL);
    }
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        return net_errno(EBADF);
    }
    addr->sin_family = AF_INET;
    addr->sin_port = htons(sock->port);
    addr->sin_addr = sock->bound ? sock->local_addr : INADDR_ANY;
    memory_set((uint8*)addr->sin_zero, 0, sizeof(addr->sin_zero));
    *addrlen = sizeof(sockaddr_in_t);
    return 0;
}

int32 net_getpeername(uint32 fd, sockaddr_in_t* addr, int32* addrlen) {
    if (!addr || !addrlen || *addrlen < (int32)sizeof(sockaddr_in_t)) {
        return net_errno(EINVAL);
    }
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        return net_errno(EBADF);
    }
    if (!sock->connected) {
        return net_errno(ENOTCONN);
    }
    addr->sin_family = AF_INET;
    addr->sin_port = htons(sock->peer_port);
    addr->sin_addr = sock->peer_addr;
    memory_set((uint8*)addr->sin_zero, 0, sizeof(addr->sin_zero));
    *addrlen = sizeof(sockaddr_in_t);
    return 0;
}

int32 net_shutdown(uint32 fd, int32 how) {
    if (how < SHUT_RD || how > SHUT_RDWR) {
        return net_errno(EINVAL);
    }
    spinlock_acquire(&g_net_lock);
    net_socket_t* sock = net_socket_from_fd(fd);
    if (!sock) {
        spinlock_release(&g_net_lock);
        return net_errno(EBADF);
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        sock->shut_read = true;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        sock->shut_write = true;
        sock->connected = false;
    }
    spinlock_release(&g_net_lock);
    return 0;
}

/* ---- Address string helpers ---- */

static int net_isdigit(char c) { return c >= '0' && c <= '9'; }

uint32 inet_addr(const char* cp) {
    if (!cp) {
        return INADDR_NONE;
    }
    uint32 parts[4] = {0, 0, 0, 0};
    int idx = 0;
    for (int i = 0; i < 4 && cp; i++) {
        uint32 v = 0;
        int digits = 0;
        while (net_isdigit(*cp) && digits < 3) {
            v = v * 10 + (uint32)(*cp - '0');
            cp++;
            digits++;
        }
        parts[idx++] = v;
        if (i < 3) {
            if (*cp != '.') {
                return INADDR_NONE;
            }
            cp++;
        }
    }
    if (idx != 4 || *cp != '\0') {
        return INADDR_NONE;
    }
    return (parts[3] << 24) | (parts[2] << 16) | (parts[1] << 8) | parts[0];
}

int inet_pton(int af, const char* src, void* dst) {
    if (af != AF_INET || !src || !dst) {
        return 0;
    }
    uint32 addr = inet_addr(src);
    if (addr == INADDR_NONE) {
        return 0;
    }
    *(uint32*)dst = addr;
    return 1;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    if (af != AF_INET || !src || !dst || size < INET_ADDRSTRLEN) {
        return 0;
    }
    uint32 a = *(const uint32*)src;
    uint8 b[4];
    b[0] = (uint8)(a & 0xff);
    b[1] = (uint8)((a >> 8) & 0xff);
    b[2] = (uint8)((a >> 16) & 0xff);
    b[3] = (uint8)((a >> 24) & 0xff);
    /* manual formatting to avoid pulling in sprintf */
    char tmp[INET_ADDRSTRLEN];
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        uint8 n = b[i];
        char rev[4];
        int r = 0;
        if (n == 0) {
            rev[r++] = '0';
        }
        while (n > 0) {
            rev[r++] = (char)('0' + (n % 10));
            n /= 10;
        }
        while (r > 0) {
            tmp[pos++] = rev[--r];
        }
        if (i > 0) {
            tmp[pos++] = '.';
        }
    }
    tmp[pos] = '\0';
    for (int i = 0; i <= pos; i++) {
        dst[i] = tmp[i];
    }
    return dst;
}

char* inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in.s_addr, buf, sizeof(buf));
    return buf;
}

/* ---- SIOC* network interface ioctls ---- */

static void net_fill_sockaddr_in(sockaddr_in_t* out, uint32 addr, uint16 port) {
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    out->sin_addr = addr;
    memory_set((uint8*)out->sin_zero, 0, sizeof(out->sin_zero));
}

/* Per-interface address table. The loopback fabric exposes a single
 * fixed interface ("lo", 127.0.0.1/8). Registered NICs (see net.register_nic)
 * join the same table so ifconfig/ip can enumerate them. */
#define NET_IFACE_MAX 8
typedef struct {
    bool   used;
    char   name[NET_IFNAME_LEN];
    uint32 addr;
    uint32 netmask;
    uint32 flags;
    uint8  hwaddr[6];
} net_iface_t;
static net_iface_t g_ifaces[NET_IFACE_MAX];

static net_iface_t* net_iface_find(const char* name) {
    if (!name) {
        return 0;
    }
    for (uint32 i = 0; i < NET_IFACE_MAX; i++) {
        if (g_ifaces[i].used && strncmp(g_ifaces[i].name, name, NET_IFNAME_LEN) == 0) {
            return &g_ifaces[i];
        }
    }
    return 0;
}

static net_iface_t* net_iface_alloc(const char* name) {
    net_iface_t* existing = net_iface_find(name);
    if (existing) {
        return existing;
    }
    for (uint32 i = 0; i < NET_IFACE_MAX; i++) {
        if (!g_ifaces[i].used) {
            memory_set((uint8*)&g_ifaces[i], 0, sizeof(net_iface_t));
            g_ifaces[i].used = true;
            strncpy(g_ifaces[i].name, name, NET_IFNAME_LEN - 1);
            return &g_ifaces[i];
        }
    }
    return 0;
}

static void net_ifaces_init(void) {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;
    net_iface_t* lo = net_iface_alloc("lo");
    if (lo) {
        lo->addr = INADDR_LOOPBACK;
        lo->netmask = 0x000000ffu; /* /8 */
        lo->flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
    }
}

int32 net_ioctl(uint32 fd, uint32 request, void* argp) {
    (void)fd;
    net_ifaces_init();

    if (!argp && request != SIOCGIFCONF) {
        return net_errno(EFAULT);
    }

    switch (request) {
        case SIOCGIFCONF: {
            struct ifconf* ifc = (struct ifconf*)argp;
            if (!ifc) {
                return net_errno(EFAULT);
            }
            int off = 0;
            int count = 0;
            for (uint32 i = 0; i < NET_IFACE_MAX && off + (int)sizeof(struct ifreq) <= ifc->ifc_len; i++) {
                if (!g_ifaces[i].used) {
                    continue;
                }
                struct ifreq* entry = &ifc->ifc_req[count];
                memory_set((uint8*)entry, 0, sizeof(*entry));
                strncpy(entry->ifr_name, g_ifaces[i].name, IFNAMSIZ - 1);
                net_fill_sockaddr_in(&entry->ifr_addr_in, g_ifaces[i].addr, 0);
                off += sizeof(struct ifreq);
                count++;
            }
            ifc->ifc_len = off;
            return 0;
        }
        case SIOCGIFADDR: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            net_fill_sockaddr_in(&req->ifr_addr_in, iface->addr, 0);
            return 0;
        }
        case SIOCGIFNETMASK: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            net_fill_sockaddr_in(&req->ifr_addr_in, iface->netmask, 0);
            return 0;
        }
        case SIOCGIFHWADDR: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            memory_copy((const char*)iface->hwaddr, (char*)req->ifr_hwaddr, 6);
            return 0;
        }
        case SIOCGIFFLAGS: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            req->ifr_flags = (int16)(iface->flags & 0xffff);
            return 0;
        }
        case SIOCGIFMTU: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            req->ifr_mtu = 1500;
            return 0;
        }
        case SIOCGIFSTATS: {
            net_stats_t stats;
            struct {
                char name[IFNAMSIZ];
                net_stats_t stats;
            }* out = (void*)argp;
            int32 rc = net_get_if_stats(out->name, &stats);
            if (rc < 0) {
                return rc;
            }
            memory_copy((const char*)&stats, (char*)&out->stats, sizeof(stats));
            return 0;
        }
        case SIOCSIFADDR: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_alloc(req->ifr_name);
            if (!iface) {
                return net_errno(ENOMEM);
            }
            iface->addr = req->ifr_addr_in.sin_addr;
            iface->flags |= IFF_UP;
            return 0;
        }
        case SIOCSIFNETMASK: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            iface->netmask = req->ifr_addr_in.sin_addr;
            return 0;
        }
        case SIOCSIFHWADDR: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            memory_copy((const char*)req->ifr_hwaddr, (char*)iface->hwaddr, 6);
            return 0;
        }
        case SIOCSIFFLAGS: {
            struct ifreq* req = (struct ifreq*)argp;
            net_iface_t* iface = net_iface_find(req->ifr_name);
            if (!iface) {
                return net_errno(ENODEV);
            }
            iface->flags = (uint32)(req->ifr_flags & 0xffff);
            return 0;
        }
        case SIOCSIFMTU: {
            (void)argp;
            return 0;
        }
        case SIOCADDRT: {
            struct ifreq* req = (struct ifreq*)argp;
            uint32 dest = req->ifr_addr_in.sin_addr;
            uint32 mask = req->ifr_addr_in.sin_port ? (uint32)ntohs(req->ifr_addr_in.sin_port) : 0xffffffffu;
            return net_route_add(dest, mask, 0, req->ifr_name, 0);
        }
        case SIOCDELRT: {
            struct ifreq* req = (struct ifreq*)argp;
            return net_route_del(req->ifr_addr_in.sin_addr, 0xffffffffu);
        }
        default:
            return net_errno(EINVAL);
    }
}

/* ---- NIC driver registration (parallel to the netdev_t layer) ---- */

static net_nic_driver_t* g_nic_list = 0;
static net_nic_driver_t* g_active_nic = 0;

int32 net_register_nic(net_nic_driver_t* drv) {
    if (!drv) {
        return net_errno(EINVAL);
    }
    drv->next = g_nic_list;
    g_nic_list = drv;
    if (!g_active_nic) {
        g_active_nic = drv;
    }
    return 0;
}

int32 net_unregister_nic(net_nic_driver_t* drv) {
    net_nic_driver_t** cur = &g_nic_list;
    while (*cur) {
        if (*cur == drv) {
            *cur = drv->next;
            if (g_active_nic == drv) {
                g_active_nic = g_nic_list;
            }
            return 0;
        }
        cur = &(*cur)->next;
    }
    return net_errno(ENOENT);
}

net_nic_driver_t* net_first_nic(void) {
    return g_nic_list;
}

net_nic_driver_t* net_active_nic(void) {
    return g_active_nic;
}

net_nic_driver_t* net_find_nic_by_name(const char* name) {
    if (!name) {
        return 0;
    }
    net_nic_driver_t* cur = g_nic_list;
    while (cur) {
        if (strncmp(cur->name, name, NET_IFNAME_LEN) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return 0;
}

int32 net_get_if_stats(const char* ifname, net_stats_t* out) {
    if (!ifname || !out) {
        return net_errno(EINVAL);
    }
    memory_set((uint8*)out, 0, sizeof(net_stats_t));
    net_nic_driver_t* nic = net_find_nic_by_name(ifname);
    if (nic && nic->get_stats) {
        nic->get_stats(nic, out);
        return 0;
    }
    if (strncmp(ifname, "lo", NET_IFNAME_LEN) == 0) {
        out->rx_packets = out->tx_packets = 0;
        return 0;
    }
    return net_errno(ENODEV);
}
