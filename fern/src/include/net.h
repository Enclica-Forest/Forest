#ifndef NET_H
#define NET_H

#include "types.h"
#include "driver.h"
#include <stdbool.h>
#include <stddef.h>

/* ---- Address families / socket types ---- */
#define AF_UNSPEC   0
#define AF_UNIX      1
#define AF_LOCAL     AF_UNIX
#define AF_INET      2
#define AF_INET6     10
#define PF_UNSPEC    AF_UNSPEC
#define PF_UNIX      AF_UNIX
#define PF_INET      AF_INET
#define PF_INET6     AF_INET6

#define SOCK_STREAM  1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_RDM      4
#define SOCK_SEQPACKET 5

/* ---- Well-known IPv4 addresses ---- */
#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u
#define INADDR_BROADCAST 0xFFFFFFFFu
#define INADDR_NONE       0xFFFFFFFFu
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

/* ---- Protocol levels ---- */
#define SOL_SOCKET    0xffff
#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

/* ---- Socket options (SOL_SOCKET) ---- */
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21
#define SO_REUSEPORT  15
#define SO_DONTROUTE  5
#define SO_LINGER     13

/* ---- TCP options ---- */
#define TCP_NODELAY    1
#define TCP_MAXSEG     2
#define TCP_KEEPIDLE   4
#define TCP_KEEPINTVL  5
#define TCP_KEEPCNT    6

/* ---- shutdown() how values ---- */
#define SHUT_RD       0
#define SHUT_WR       1
#define SHUT_RDWR     2

/* ---- Well-known service ports used by the loopback fabric ---- */
#define NET_PORT_ECHO      7
#define NET_PORT_FTP       21
#define NET_PORT_SSH       22
#define NET_PORT_HTTP      80
#define NET_PORT_RSYNCD    873
#define NET_PORT_SFTP      115

#ifndef SOCKLEN_T_DEFINED
#define SOCKLEN_T_DEFINED
typedef uint32 socklen_t;
#endif

/* ---- Standard socket address structures ---- */
typedef struct {
    uint16 sa_family;
    char   sa_data[14];
} sockaddr_t;

typedef struct {
    uint16 sin_family;
    uint16 sin_port;
    uint32 sin_addr;
    uint8  sin_zero[8];
} sockaddr_in_t;

typedef struct {
    uint16 sun_family;
    char sun_path[108];
} sockaddr_un_t;

/* ---- Network interface configuration structures ---- */
#define IFNAMSIZ 16

struct in_addr {
    uint32 s_addr;
};

struct ifreq {
    char    ifr_name[IFNAMSIZ];
    union {
        sockaddr_t     ifr_addr;
        sockaddr_in_t  ifr_addr_in;
        sockaddr_t     ifr_dstaddr;
        sockaddr_t     ifr_broadaddr;
        sockaddr_t     ifr_netmask;
        uint8          ifr_hwaddr[8];
        int16          ifr_flags;
        int            ifr_metric;
        int            ifr_mtu;
    } ifr_ifru;
};
#define ifr_addr      ifr_ifru.ifr_addr
#define ifr_addr_in   ifr_ifru.ifr_addr_in
#define ifr_netmask   ifr_ifru.ifr_netmask
#define ifr_hwaddr    ifr_ifru.ifr_hwaddr
#define ifr_flags     ifr_ifru.ifr_flags
#define ifr_mtu       ifr_ifru.ifr_mtu

struct ifconf {
    int     ifc_len;
    union {
        char*           ifcu_buf;
        struct ifreq*   ifcu_req;
    } ifc_ifcu;
};
#define ifc_buf   ifc_ifcu.ifcu_buf
#define ifc_req   ifc_ifcu.ifcu_req

/* ---- Network interface ioctl numbers ---- */
#define SIOCGIFNAME    0x8901
#define SIOCGIFCONF    0x8912
#define SIOCGIFADDR    0x8915
#define SIOCGIFNETMASK 0x891B
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFFLAGS   0x8913
#define SIOCGIFMTU     0x8921
#define SIOCSIFADDR    0x8916
#define SIOCSIFNETMASK 0x891C
#define SIOCSIFHWADDR  0x8924
#define SIOCSIFFLAGS   0x8914
#define SIOCSIFMTU     0x8922
#define SIOCGIFSTATS   0x8936
#define SIOCADDRT      0x890B
#define SIOCDELRT      0x890C

/* Interface flags for SIOCGIFFLAGS / SIOCSIFFLAGS */
#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_LOOPBACK    0x0008
#define IFF_RUNNING     0x0040
#define IFF_NOARP       0x0080
#define IFF_MULTICAST   0x1000

/* ---- Per-interface statistics ---- */
typedef struct {
    uint32 rx_packets;
    uint32 tx_packets;
    uint32 rx_bytes;
    uint32 tx_bytes;
    uint32 rx_errors;
    uint32 tx_errors;
    uint32 rx_dropped;
    uint32 tx_dropped;
} net_stats_t;

/* ---- Routing table entry ---- */
#define NET_ROUTE_TABLE_SIZE 16
#define NET_IFNAME_LEN 16

typedef struct {
    bool   used;
    uint32 dest;
    uint32 mask;
    uint32 gateway;
    char   ifname[NET_IFNAME_LEN];
    int    metric;
    net_stats_t stats;
} net_route_t;

/* ---- NIC driver registration API ----
 * Independent of the lower-level netdev_t mechanism so NIC drivers
 * can register themselves with the kernel networking core without
 * pulling in driver.c. The core keeps a singly-linked list and uses
 * the active device (the first registered) as the outbound interface.
 */
typedef struct net_nic_driver net_nic_driver_t;

typedef int  (*net_nic_probe_fn)(net_nic_driver_t* self);
typedef int  (*net_nic_reset_fn)(net_nic_driver_t* self);
typedef int  (*net_nic_tx_fn)(net_nic_driver_t* self, const void* frame, uint32 len);
typedef int  (*net_nic_rx_fn)(net_nic_driver_t* self, void* frame, uint32 cap, uint32* out_len);
typedef void (*net_nic_get_mac_fn)(net_nic_driver_t* self, uint8 mac[6]);
typedef void (*net_nic_irq_fn)(net_nic_driver_t* self);
typedef void (*net_nic_get_stats_fn)(net_nic_driver_t* self, net_stats_t* out);

struct net_nic_driver {
    net_nic_driver_t*   next;
    char                name[NET_IFNAME_LEN];
    uint16              vendor_id;
    uint16              device_id;
    uint16              io_base;
    uint64              mem_base;
    uint16              irq;
    uint8               mac[6];
    bool                initialized;
    void*               private_data;

    net_nic_probe_fn      probe;
    net_nic_reset_fn      reset;
    net_nic_tx_fn         tx;
    net_nic_rx_fn         rx;
    net_nic_get_mac_fn    get_mac;
    net_nic_irq_fn        irq_handler;
    net_nic_get_stats_fn  get_stats;
};

/* ---- Socket info exposed via /proc/net / netinfo syscall ---- */
typedef struct {
    bool   used;
    bool   bound;
    uint16 port;
    uint8  queue_depth;
    uint8  queue_capacity;
    uint32 bytes_sent;
    uint32 bytes_received;
    uint32 last_peer_addr;
    uint16 last_peer_port;
} net_socket_info_t;

/* ---- Byte order helpers ---- */
static inline uint16 htons(uint16 value) {
    return (uint16)((value << 8) | (value >> 8));
}

static inline uint16 ntohs(uint16 value) {
    return htons(value);
}

static inline uint32 htonl(uint32 value) {
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8)  |
           ((value & 0x00ff0000U) >> 8)  |
           ((value & 0xff000000U) >> 24);
}

static inline uint32 ntohl(uint32 value) {
    return htonl(value);
}

/* ---- Address string helpers (kernelspace implementations in net.c) ---- */
uint32 inet_addr(const char* cp);
int   inet_pton(int af, const char* src, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);
char* inet_ntoa(struct in_addr in);

/* ---- Core networking lifecycle / legacy loopback fabric ---- */
bool   net_init(void);
bool   net_is_fd(uint32 fd);
int32  net_close(uint32 fd);
void   net_close_all_for_task(uint32 pid);
int32  net_socket_create(uint32 domain, uint32 type, uint32 protocol);
int32  net_bind(uint32 fd, uint16 port);

int32  net_send_datagram(uint32 fd, const uint8* buffer, uint32 length,
                         uint32 dest_addr, uint16 dest_port);
int32  net_recv_datagram(uint32 fd, uint8* buffer, uint32 length,
                         uint32* out_addr, uint16* out_port);
uint32 net_snapshot(net_socket_info_t* out, uint32 max_entries);

/* ---- POSIX-compatible socket syscall surface ----
 * These extend the original UDP datagram API to support TCP and a richer
 * set of socket options. The syscall layer (src/syscall.c) is expected to
 * dispatch the matching syscalls here. All functions return a negative
 * `-ENOSYS` style value on failure (see `src/include/libc/errno.h`).
 */
int32 net_connect(uint32 fd, const sockaddr_in_t* addr, int32 addrlen);
int32 net_listen(uint32 fd, int32 backlog);
int32 net_accept(uint32 fd, sockaddr_in_t* addr, int32* addrlen);
int32 net_send(uint32 fd, const void* buf, uint32 len, int32 flags);
int32 net_recv(uint32 fd, void* buf, uint32 len, int32 flags);
int32 net_sendto(uint32 fd, const void* buf, uint32 len, int32 flags,
                 const sockaddr_in_t* addr, int32 addrlen);
int32 net_recvfrom(uint32 fd, void* buf, uint32 len, int32 flags,
                   sockaddr_in_t* addr, int32* addrlen);
int32 net_setsockopt(uint32 fd, int32 level, int32 optname,
                     const void* optval, int32 optlen);
int32 net_getsockopt(uint32 fd, int32 level, int32 optname,
                     void* optval, int32* optlen);
int32 net_getsockname(uint32 fd, sockaddr_in_t* addr, int32* addrlen);
int32 net_getpeername(uint32 fd, sockaddr_in_t* addr, int32* addrlen);
int32 net_shutdown(uint32 fd, int32 how);
int32 net_socket_set_nonblocking(uint32 fd, bool nonblock);

/* ---- Network interface ioctls ---- */
int32 net_ioctl(uint32 fd, uint32 request, void* argp);

/* ---- Protocol helpers (guarded by ENABLE_* features) ----
 * - DHCP discover/request cycle when ENABLE_DHCP=y
 * - DNS A-record resolution when ENABLE_DNS=y
 * - ICMP echo request when ENABLE_ICMP=y
 * All return a negative errno on failure or when the corresponding
 * feature is disabled at build time.
 */
int32 net_dhcp_request(void);
int32 net_dns_resolve(const char* hostname, uint32* out_addr);
int32 net_icmp_ping(uint32 dest_ip, uint16 id, uint16 seq,
                    const uint8* payload, uint32 payload_len,
                    uint32 timeout_ms);

/* ---- Route table API ---- */
int32 net_route_add(uint32 dest, uint32 mask, uint32 gateway,
                    const char* ifname, int metric);
int32 net_route_del(uint32 dest, uint32 mask);
int32 net_route_lookup(uint32 dest_ip, uint32* next_hop,
                       char ifname[NET_IFNAME_LEN]);
int32 net_route_table_dump(net_route_t* out, int32 max_entries);

/* ---- NIC driver registration API ---- */
int32 net_register_nic(net_nic_driver_t* drv);
int32 net_unregister_nic(net_nic_driver_t* drv);
net_nic_driver_t* net_first_nic(void);
net_nic_driver_t* net_active_nic(void);
net_nic_driver_t* net_find_nic_by_name(const char* name);

/* ---- Interface statistics ---- */
int32 net_get_if_stats(const char* ifname, net_stats_t* out);

/* ---- Socket type accessors (for SO_TYPE) ---- */
int32 net_socket_type(uint32 fd);

#endif /* NET_H */