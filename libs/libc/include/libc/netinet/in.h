/*
 * netinet/in.h - Internet protocol family
 * 
 * POSIX compatible Internet address definitions for Fern libc.
 */
#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/socket.h>

/* Internet address */
struct in_addr {
    in_addr_t s_addr;
};

/* IPv6 address */
struct in6_addr {
    union {
        uint8_t __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
#define s6_addr     __in6_u.__u6_addr8
#define s6_addr16   __in6_u.__u6_addr16
#define s6_addr32   __in6_u.__u6_addr32
};

/* IPv4 socket address */
struct sockaddr_in {
    sa_family_t sin_family;     /* AF_INET */
    in_port_t sin_port;         /* Port number */
    struct in_addr sin_addr;    /* Internet address */
    unsigned char sin_zero[8];  /* Padding */
};

/* IPv6 socket address */
struct sockaddr_in6 {
    sa_family_t sin6_family;    /* AF_INET6 */
    in_port_t sin6_port;        /* Port number */
    uint32_t sin6_flowinfo;     /* Traffic class and flow info */
    struct in6_addr sin6_addr;  /* IPv6 address */
    uint32_t sin6_scope_id;     /* Scope ID */
};

/* IP protocol numbers */
#define IPPROTO_IP      0       /* Dummy protocol for IP */
#define IPPROTO_ICMP    1       /* Internet Control Message Protocol */
#define IPPROTO_IGMP    2       /* Internet Group Management Protocol */
#define IPPROTO_TCP     6       /* Transmission Control Protocol */
#define IPPROTO_UDP     17      /* User Datagram Protocol */
#define IPPROTO_IPV6    41      /* IPv6 header */
#define IPPROTO_ICMPV6  58      /* ICMPv6 */
#define IPPROTO_RAW     255     /* Raw IP packets */

/* Special addresses */
#define INADDR_ANY          ((in_addr_t)0x00000000)
#define INADDR_BROADCAST    ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK     ((in_addr_t)0x7f000001)
#define INADDR_NONE         ((in_addr_t)0xffffffff)

/* IPv6 special addresses */
extern const struct in6_addr in6addr_any;       /* :: */
extern const struct in6_addr in6addr_loopback;  /* ::1 */

#define IN6ADDR_ANY_INIT        {{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }}
#define IN6ADDR_LOOPBACK_INIT   {{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }}

/* IPv6 address testing macros */
#define IN6_IS_ADDR_UNSPECIFIED(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == 0 && (a)->s6_addr32[3] == 0)

#define IN6_IS_ADDR_LOOPBACK(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == 0 && (a)->s6_addr32[3] == htonl(1))

#define IN6_IS_ADDR_MULTICAST(a) ((a)->s6_addr[0] == 0xff)

#define IN6_IS_ADDR_LINKLOCAL(a) \
    (((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0x80))

#define IN6_IS_ADDR_SITELOCAL(a) \
    (((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0xc0))

#define IN6_IS_ADDR_V4MAPPED(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == htonl(0xffff))

#define IN6_IS_ADDR_V4COMPAT(a) \
    ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
     (a)->s6_addr32[2] == 0 && ntohl((a)->s6_addr32[3]) > 1)

/* Byte order conversion functions (static inline implementations in arpa/inet.h) */
#include <arpa/inet.h>

/* IP options for setsockopt/getsockopt */
#define IP_TOS              1   /* IP type of service */
#define IP_TTL              2   /* IP time to live */
#define IP_HDRINCL          3   /* Header is included with data */
#define IP_OPTIONS          4   /* IP options */
#define IP_ROUTER_ALERT     5   /* Router alert option */
#define IP_RECVOPTS         6   /* Receive all IP options */
#define IP_RETOPTS          7   /* Set/get IP options */
#define IP_PKTINFO          8   /* Packet information */
#define IP_PKTOPTIONS       9   /* Packet options */
#define IP_MTU_DISCOVER     10  /* Path MTU discovery */
#define IP_RECVERR          11  /* Receive errors */
#define IP_RECVTTL          12  /* Receive TTL */
#define IP_RECVTOS          13  /* Receive TOS */
#define IP_MTU              14  /* Current path MTU */
#define IP_FREEBIND         15  /* Allow binding to any address */
#define IP_MULTICAST_IF     32  /* Multicast interface */
#define IP_MULTICAST_TTL    33  /* Multicast TTL */
#define IP_MULTICAST_LOOP   34  /* Multicast loopback */
#define IP_ADD_MEMBERSHIP   35  /* Add multicast membership */
#define IP_DROP_MEMBERSHIP  36  /* Drop multicast membership */

/* Multicast request structure */
struct ip_mreq {
    struct in_addr imr_multiaddr;   /* Multicast group address */
    struct in_addr imr_interface;   /* Local interface address */
};

struct ip_mreqn {
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int imr_ifindex;
};

/* IPv6 options */
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_ADD_MEMBERSHIP 20
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_V6ONLY         26
#define IPV6_RECVPKTINFO    49
#define IPV6_PKTINFO        50
#define IPV6_RECVHOPLIMIT   51
#define IPV6_HOPLIMIT       52
#define IPV6_RECVHOPOPTS    53
#define IPV6_HOPOPTS        54
#define IPV6_RTHDRDSTOPTS   55
#define IPV6_RECVRTHDR      56
#define IPV6_RTHDR          57
#define IPV6_RECVDSTOPTS    58
#define IPV6_DSTOPTS        59

/* IPv6 multicast request */
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};

#ifdef __cplusplus
}
#endif

#endif /* _NETINET_IN_H */
