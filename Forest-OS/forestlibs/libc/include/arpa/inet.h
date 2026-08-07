/*
 * arpa/inet.h - Internet operations
 * 
 * POSIX compatible internet address manipulation for Fern libc.
 */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <netinet/in.h>

/* Convert address from network byte order to presentation format */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

/* Convert address from presentation format to network byte order */
int inet_pton(int af, const char *src, void *dst);

/* Convert IPv4 address from dotted-decimal to binary (deprecated) */
in_addr_t inet_addr(const char *cp);

/* Convert IPv4 address from dotted-decimal to struct in_addr (deprecated) */
int inet_aton(const char *cp, struct in_addr *inp);

/* Convert IPv4 address from binary to dotted-decimal (deprecated, not thread-safe) */
char *inet_ntoa(struct in_addr in);

/* Convert network number from presentation to binary */
in_addr_t inet_network(const char *cp);

/* Make internet address from network and host parts */
struct in_addr inet_makeaddr(in_addr_t net, in_addr_t host);

/* Extract network number from address */
in_addr_t inet_netof(struct in_addr in);

/* Extract local network address from address */
in_addr_t inet_lnaof(struct in_addr in);

/* Byte order conversion */
static inline uint32_t htonl(uint32_t hostlong) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((hostlong & 0xFF000000) >> 24) |
           ((hostlong & 0x00FF0000) >> 8) |
           ((hostlong & 0x0000FF00) << 8) |
           ((hostlong & 0x000000FF) << 24);
#else
    return hostlong;
#endif
}

static inline uint16_t htons(uint16_t hostshort) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((hostshort & 0xFF00) >> 8) | ((hostshort & 0x00FF) << 8);
#else
    return hostshort;
#endif
}

static inline uint32_t ntohl(uint32_t netlong) {
    return htonl(netlong);
}

static inline uint16_t ntohs(uint16_t netshort) {
    return htons(netshort);
}

#ifdef __cplusplus
}
#endif

#endif /* _ARPA_INET_H */
