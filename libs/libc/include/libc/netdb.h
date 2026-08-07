/*
 * netdb.h - Database operations for the network
 *
 * POSIX-compatible name resolution for Fern libc.
 */
#ifndef _NETDB_H
#define _NETDB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/socket.h>

struct hostent {
    char  *h_name;       /* Official name of host */
    char **h_aliases;    /* Alias list */
    int    h_addrtype;   /* Host address type */
    int    h_length;     /* Length of address */
    char **h_addr_list;  /* List of addresses */
};

#define h_addr h_addr_list[0]

struct netent {
    char        *n_name;     /* Official network name */
    char       **n_aliases;  /* Alias list */
    int          n_addrtype; /* Net address type */
    unsigned int n_net;      /* Network number */
};

struct protoent {
    char  *p_name;    /* Official protocol name */
    char **p_aliases; /* Alias list */
    int    p_proto;   /* Protocol number */
};

struct servent {
    char  *s_name;    /* Official service name */
    char **s_aliases; /* Alias list */
    int    s_port;    /* Port number */
    char  *s_proto;   /* Protocol to use */
};

/* addrinfo struct (must precede function declarations) */
struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

/* ai_flags values */
#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_ADDRCONFIG  0x0020
#define AI_V4MAPPED    0x0008
#define AI_ALL         0x0010

/* gai_strerror values */
#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_SOCKTYPE   -7
#define EAI_SERVICE    -8
#define EAI_MEMORY     -10
#define EAI_SYSTEM     -11
#define EAI_OVERFLOW   -12

/* Port/host byte order conversion (defined in arpa/inet.h) */
#include <arpa/inet.h>

/* Host operations */
struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
int gethostbyname_r(const char *name, struct hostent *ret, char *buf,
                    size_t buflen, struct hostent **result, int *h_errnop);

/* Network operations */
struct netent *getnetbyname(const char *name);
struct netent *getnetbyaddr(unsigned long net, int type);

/* Protocol operations */
struct protoent *getprotobyname(const char *name);
struct protoent *getprotobynumber(int proto);

/* Service operations */
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);

/* Name resolution */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);

/* NI flags */
#define NI_NUMERICHOST  1
#define NI_NUMERICSERV  2
#define NI_NOFQDN       4
#define NI_NAMEREQD     8
#define NI_DGRAM        16
#define NI_MAXHOST      1025
#define NI_MAXSERV      32

/* Herror/H_errno */
extern int h_errno;
void herror(const char *s);
const char *hstrerror(int err);

/* Error constants */
#define NETDB_INTERNAL -1
#define NETDB_SUCCESS  0
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA

#ifdef __cplusplus
}
#endif

#endif /* _NETDB_H */
