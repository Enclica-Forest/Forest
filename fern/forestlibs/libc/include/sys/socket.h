/*
 * sys/socket.h - Socket interface
 * 
 * POSIX compatible socket definitions for Fern libc.
 */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* Socket types */
#define SOCK_STREAM     1   /* Stream socket (TCP) */
#define SOCK_DGRAM      2   /* Datagram socket (UDP) */
#define SOCK_RAW        3   /* Raw socket */
#define SOCK_RDM        4   /* Reliably-delivered message */
#define SOCK_SEQPACKET  5   /* Sequential packet socket */
#define SOCK_DCCP       6   /* Datagram Congestion Control Protocol */
#define SOCK_PACKET     10  /* Linux specific packet socket */

/* Socket type modifiers */
#define SOCK_CLOEXEC    02000000    /* Set close-on-exec flag */
#define SOCK_NONBLOCK   00004000    /* Set non-blocking flag */

/* Address families */
#define AF_UNSPEC       0   /* Unspecified */
#define AF_LOCAL        1   /* Local to host (pipes, portals) */
#define AF_UNIX         AF_LOCAL    /* POSIX name for AF_LOCAL */
#define AF_FILE         AF_LOCAL    /* Another non-standard name for AF_LOCAL */
#define AF_INET         2   /* IPv4 */
#define AF_AX25         3   /* Amateur Radio AX.25 */
#define AF_IPX          4   /* Novell IPX */
#define AF_APPLETALK    5   /* AppleTalk */
#define AF_NETROM       6   /* Amateur Radio NET/ROM */
#define AF_BRIDGE       7   /* Multiprotocol bridge */
#define AF_ATMPVC       8   /* ATM PVCs */
#define AF_X25          9   /* X.25 */
#define AF_INET6        10  /* IPv6 */
#define AF_ROSE         11  /* Amateur Radio X.25 PLP */
#define AF_DECnet       12  /* DECnet */
#define AF_NETBEUI      13  /* NetBEUI */
#define AF_SECURITY     14  /* Security callback pseudo AF */
#define AF_KEY          15  /* Key management */
#define AF_NETLINK      16  /* Netlink */
#define AF_ROUTE        AF_NETLINK  /* Alias to AF_NETLINK */
#define AF_PACKET       17  /* Packet family */
#define AF_ASH          18  /* Ash */
#define AF_ECONET       19  /* Acorn Econet */
#define AF_ATMSVC       20  /* ATM SVCs */
#define AF_RDS          21  /* RDS sockets */
#define AF_SNA          22  /* SNA */
#define AF_IRDA         23  /* IRDA sockets */
#define AF_PPPOX        24  /* PPPoX sockets */
#define AF_WANPIPE      25  /* Wanpipe API sockets */
#define AF_LLC          26  /* Linux LLC */
#define AF_IB           27  /* InfiniBand */
#define AF_MPLS         28  /* MPLS */
#define AF_CAN          29  /* CAN bus */
#define AF_TIPC         30  /* TIPC sockets */
#define AF_BLUETOOTH    31  /* Bluetooth sockets */
#define AF_IUCV         32  /* IUCV sockets */
#define AF_RXRPC        33  /* RxRPC sockets */
#define AF_ISDN         34  /* ISDN sockets */
#define AF_PHONET       35  /* Phonet sockets */
#define AF_IEEE802154   36  /* IEEE 802.15.4 sockets */
#define AF_CAIF         37  /* CAIF sockets */
#define AF_ALG          38  /* Algorithm sockets */
#define AF_NFC          39  /* NFC sockets */
#define AF_VSOCK        40  /* vSockets */
#define AF_KCM          41  /* Kernel Connection Multiplexor */
#define AF_QIPCRTR      42  /* Qualcomm IPC Router */
#define AF_SMC          43  /* SMC sockets */
#define AF_XDP          44  /* XDP sockets */
#define AF_MAX          45

/* Protocol families (same as address families) */
#define PF_UNSPEC       AF_UNSPEC
#define PF_LOCAL        AF_LOCAL
#define PF_UNIX         AF_UNIX
#define PF_FILE         AF_FILE
#define PF_INET         AF_INET
#define PF_AX25         AF_AX25
#define PF_IPX          AF_IPX
#define PF_APPLETALK    AF_APPLETALK
#define PF_NETROM       AF_NETROM
#define PF_BRIDGE       AF_BRIDGE
#define PF_ATMPVC       AF_ATMPVC
#define PF_X25          AF_X25
#define PF_INET6        AF_INET6
#define PF_ROSE         AF_ROSE
#define PF_DECnet       AF_DECnet
#define PF_NETBEUI      AF_NETBEUI
#define PF_SECURITY     AF_SECURITY
#define PF_KEY          AF_KEY
#define PF_NETLINK      AF_NETLINK
#define PF_ROUTE        AF_ROUTE
#define PF_PACKET       AF_PACKET
#define PF_ASH          AF_ASH
#define PF_ECONET       AF_ECONET
#define PF_ATMSVC       AF_ATMSVC
#define PF_RDS          AF_RDS
#define PF_SNA          AF_SNA
#define PF_IRDA         AF_IRDA
#define PF_PPPOX        AF_PPPOX
#define PF_WANPIPE      AF_WANPIPE
#define PF_LLC          AF_LLC
#define PF_IB           AF_IB
#define PF_MPLS         AF_MPLS
#define PF_CAN          AF_CAN
#define PF_TIPC         AF_TIPC
#define PF_BLUETOOTH    AF_BLUETOOTH
#define PF_IUCV         AF_IUCV
#define PF_RXRPC        AF_RXRPC
#define PF_ISDN         AF_ISDN
#define PF_PHONET       AF_PHONET
#define PF_IEEE802154   AF_IEEE802154
#define PF_CAIF         AF_CAIF
#define PF_ALG          AF_ALG
#define PF_NFC          AF_NFC
#define PF_VSOCK        AF_VSOCK
#define PF_KCM          AF_KCM
#define PF_QIPCRTR      AF_QIPCRTR
#define PF_SMC          AF_SMC
#define PF_XDP          AF_XDP
#define PF_MAX          AF_MAX

/* Socket level for setsockopt/getsockopt */
#define SOL_SOCKET      1

/* Socket options */
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_SNDBUFFORCE  32
#define SO_RCVBUFFORCE  33
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_NO_CHECK     11
#define SO_PRIORITY     12
#define SO_LINGER       13
#define SO_BSDCOMPAT    14
#define SO_REUSEPORT    15
#define SO_PASSCRED     16
#define SO_PEERCRED     17
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30
#define SO_PEERSEC      31
#define SO_PASSSEC      34
#define SO_TIMESTAMPNS  35
#define SO_MARK         36
#define SO_TIMESTAMPING 37
#define SO_PROTOCOL     38
#define SO_DOMAIN       39
#define SO_RXQ_OVFL     40
#define SO_WIFI_STATUS  41
#define SO_PEEK_OFF     42
#define SO_NOFCS        43
#define SO_LOCK_FILTER  44
#define SO_SELECT_ERR_QUEUE 45
#define SO_BUSY_POLL    46
#define SO_MAX_PACING_RATE 47
#define SO_BPF_EXTENSIONS 48
#define SO_INCOMING_CPU 49
#define SO_ATTACH_BPF   50
#define SO_DETACH_BPF   SO_DETACH_FILTER
#define SO_ATTACH_REUSEPORT_CBPF 51
#define SO_ATTACH_REUSEPORT_EBPF 52
#define SO_CNX_ADVICE   53
#define SO_MEMINFO      55
#define SO_INCOMING_NAPI_ID 56
#define SO_COOKIE       57
#define SO_PEERGROUPS   59
#define SO_ZEROCOPY     60
#define SO_TXTIME       61

/* Message flags for send/recv */
#define MSG_OOB         0x01    /* Out-of-band data */
#define MSG_PEEK        0x02    /* Peek at incoming messages */
#define MSG_DONTROUTE   0x04    /* Don't use routing tables */
#define MSG_CTRUNC      0x08    /* Control data truncated */
#define MSG_PROXY       0x10    /* Supply or ask second address */
#define MSG_TRUNC       0x20    /* Message truncated */
#define MSG_DONTWAIT    0x40    /* Non-blocking I/O */
#define MSG_EOR         0x80    /* End of record */
#define MSG_WAITALL     0x100   /* Wait for a full request */
#define MSG_FIN         0x200   /* FIN flag */
#define MSG_SYN         0x400   /* SYN flag */
#define MSG_CONFIRM     0x800   /* Confirm path validity */
#define MSG_RST         0x1000  /* RST flag */
#define MSG_ERRQUEUE    0x2000  /* Fetch message from error queue */
#define MSG_NOSIGNAL    0x4000  /* Don't generate SIGPIPE */
#define MSG_MORE        0x8000  /* More data coming */
#define MSG_WAITFORONE  0x10000 /* Wait for at least one packet */
#define MSG_BATCH       0x40000 /* Batch messages */
#define MSG_FASTOPEN    0x20000000 /* Send data in TCP SYN */
#define MSG_CMSG_CLOEXEC 0x40000000 /* Set close-on-exec flag */

/* Shutdown types */
#define SHUT_RD         0   /* No more receptions */
#define SHUT_WR         1   /* No more transmissions */
#define SHUT_RDWR       2   /* No more receptions or transmissions */

/* Maximum queue length */
#define SOMAXCONN       4096

/* Socket address structure */
struct sockaddr {
    sa_family_t sa_family;  /* Address family */
    char sa_data[14];       /* Socket address data */
};

/* Socket address storage (large enough for any socket address) */
struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_padding[128 - sizeof(sa_family_t) - sizeof(unsigned long)];
    unsigned long __ss_align;
};

/* Message header for sendmsg/recvmsg */
struct msghdr {
    void *msg_name;             /* Optional address */
    socklen_t msg_namelen;      /* Size of address */
    struct iovec *msg_iov;      /* Scatter/gather array */
    size_t msg_iovlen;          /* # elements in msg_iov */
    void *msg_control;          /* Ancillary data */
    size_t msg_controllen;      /* Ancillary data buffer len */
    int msg_flags;              /* Flags on received message */
};

/* Control message header */
struct cmsghdr {
    size_t cmsg_len;    /* Data byte count, including header */
    int cmsg_level;     /* Originating protocol */
    int cmsg_type;      /* Protocol-specific type */
};

/* Ancillary data macros */
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_FIRSTHDR(mhdr) \
    ((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
     ? (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(mhdr, cmsg) \
    ((cmsg)->cmsg_len < sizeof(struct cmsghdr) ? (struct cmsghdr *)0 : \
     (struct cmsghdr *)0)

/* Linger structure for SO_LINGER */
struct linger {
    int l_onoff;    /* Linger active */
    int l_linger;   /* How long to linger for */
};

/* I/O vector for scatter/gather I/O */
struct iovec {
    void *iov_base;     /* Starting address */
    size_t iov_len;     /* Number of bytes */
};

/* Socket functions */
int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int shutdown(int sockfd, int how);

/* Send/receive */
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

/* Socket options */
int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);

/* Socket name operations */
int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/* Socket pair */
int socketpair(int domain, int type, int protocol, int sv[2]);

/* Multiple messages */
struct mmsghdr {
    struct msghdr msg_hdr;  /* Message header */
    unsigned int msg_len;   /* Number of received bytes */
};

int sendmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags);
int recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags,
             struct timespec *timeout);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SOCKET_H */
