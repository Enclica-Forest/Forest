#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/utsname.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* Forest OS libc netdb.h may lack these */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_NAMEREQD
#define NI_NAMEREQD 8
#endif

#define MAX_HOSTNAME 256
#define MAX_LINE     1024

static int show_short  = 0;
static int show_domain = 0;
static int show_fqdn   = 0;
static int show_ip     = 0;
static int show_alias  = 0;
static int show_nis    = 0;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [NAME] [-s] [-d] [-f] [-i] [-a] [-y]\n", prog);
    fprintf(stderr, "  -s, --short     short host name\n");
    fprintf(stderr, "  -d, --domain    DNS domain name\n");
    fprintf(stderr, "  -f, --fqdn      fully qualified domain name\n");
    fprintf(stderr, "  -i, --ip        IP addresses\n");
    fprintf(stderr, "  -a, --alias     all aliases\n");
    fprintf(stderr, "  -y, --yp        NIS/YP domain name\n");
}

static int get_hostname(char *buf, size_t buflen) {
    struct utsname uts;
    if (uname(&uts) < 0) {
        perror("uname");
        return -1;
    }
    strncpy(buf, uts.nodename, buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

static int set_hostname(const char *name) {
    if (sethostname(name, strlen(name)) < 0) {
        perror("sethostname");
        return -1;
    }
    return 0;
}

static int lookup_fqdn(const char *hostname, char *buf, size_t buflen) {
    struct addrinfo hints, *res, *rp;
    char hbuf[NI_MAXHOST];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(hostname, NULL, &hints, &res);
    if (rc != 0) {
        strncpy(buf, hostname, buflen - 1);
        buf[buflen - 1] = '\0';
        return 0;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        if (getnameinfo(rp->ai_addr, rp->ai_addrlen, hbuf, sizeof(hbuf),
                        NULL, 0, NI_NAMEREQD) == 0) {
            strncpy(buf, hbuf, buflen - 1);
            buf[buflen - 1] = '\0';
            freeaddrinfo(res);
            return 0;
        }
    }

    freeaddrinfo(res);
    strncpy(buf, hostname, buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

static int resolve_domain(const char *hostname, char *buf, size_t buflen) {
    struct addrinfo hints, *res;
    char hbuf[NI_MAXHOST];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(hostname, NULL, &hints, &res);
    if (rc != 0) {
        buf[0] = '\0';
        return -1;
    }

    if (getnameinfo(res->ai_addr, res->ai_addrlen, hbuf, sizeof(hbuf),
                    NULL, 0, NI_NAMEREQD) == 0) {
        char *dot = strchr(hbuf, '.');
        if (dot) {
            strncpy(buf, dot + 1, buflen - 1);
            buf[buflen - 1] = '\0';
        } else {
            buf[0] = '\0';
        }
    } else {
        buf[0] = '\0';
    }

    freeaddrinfo(res);
    return 0;
}

static int get_ip_addresses(const char *hostname, char *buf, size_t buflen) {
    struct addrinfo hints, *res, *rp;
    char hbuf[NI_NUMERICHOST];
    size_t pos = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(hostname, NULL, &hints, &res);
    if (rc != 0) {
        buf[0] = '\0';
        return -1;
    }

    buf[0] = '\0';
    for (rp = res; rp; rp = rp->ai_next) {
        if (getnameinfo(rp->ai_addr, rp->ai_addrlen, hbuf, sizeof(hbuf),
                        NULL, 0, NI_NUMERICHOST) == 0) {
            size_t len = strlen(hbuf);
            if (pos + len + 2 < buflen) {
                if (pos > 0) {
                    buf[pos++] = ' ';
                }
                memcpy(buf + pos, hbuf, len);
                pos += len;
            }
        }
    }
    buf[pos] = '\0';

    freeaddrinfo(res);
    return 0;
}

static int get_aliases(const char *hostname, char *buf, size_t buflen) {
    struct addrinfo hints, *res;
    char hbuf[NI_MAXHOST];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(hostname, NULL, &hints, &res);
    if (rc != 0) {
        strncpy(buf, hostname, buflen - 1);
        buf[buflen - 1] = '\0';
        return 0;
    }

    if (getnameinfo(res->ai_addr, res->ai_addrlen, hbuf, sizeof(hbuf),
                    NULL, 0, 0) == 0) {
        strncpy(buf, hbuf, buflen - 1);
        buf[buflen - 1] = '\0';
    } else {
        strncpy(buf, hostname, buflen - 1);
        buf[buflen - 1] = '\0';
    }

    freeaddrinfo(res);
    return 0;
}

int main(int argc, char *argv[]) {
    char hostname[MAX_HOSTNAME];
    char result[MAX_HOSTNAME * 4];
    int do_set = 0;
    const char *new_name = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "sdfiay:h")) != -1) {
        switch (opt) {
            case 's': show_short = 1; break;
            case 'd': show_domain = 1; break;
            case 'f': show_fqdn = 1; break;
            case 'i': show_ip = 1; break;
            case 'a': show_alias = 1; break;
            case 'y': show_nis = 1; break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind < argc) {
        new_name = argv[optind];
        do_set = 1;
    }

    if (do_set) {
        if (set_hostname(new_name) < 0)
            return 1;
        if (!show_short && !show_domain && !show_fqdn &&
            !show_ip && !show_alias && !show_nis) {
            return 0;
        }
    }

    if (get_hostname(hostname, sizeof(hostname)) < 0)
        return 1;

    if (!show_short && !show_domain && !show_fqdn &&
        !show_ip && !show_alias && !show_nis) {
        printf("%s\n", hostname);
        return 0;
    }

    if (show_short) {
        printf("%s\n", hostname);
    }

    if (show_fqdn || show_domain) {
        if (lookup_fqdn(hostname, result, sizeof(result)) == 0) {
            if (show_fqdn)
                printf("%s\n", result);
            if (show_domain) {
                if (resolve_domain(hostname, result, sizeof(result)) == 0 && result[0])
                    printf("%s\n", result);
            }
        }
    }

    if (show_ip) {
        if (get_ip_addresses(hostname, result, sizeof(result)) == 0)
            printf("%s\n", result);
    }

    if (show_alias) {
        if (get_aliases(hostname, result, sizeof(result)) == 0)
            printf("%s\n", result);
    }

    if (show_nis) {
        printf("(none)\n");
    }

    return 0;
}
