#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <forestos/syscalls.h>

static int g_errno = 0;
int* __errno_location(void) { return &g_errno; }

#define SYS_brk       10
#define SYS_unlink     6
#define SYS_socket    41
#define SYS_connect   42
#define SYS_accept    43
#define SYS_sendto    44
#define SYS_recvfrom  45
#define SYS_shutdown  48
#define SYS_bind      49
#define SYS_listen    50
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_socketpair 53
#define SYS_setsockopt 54
#define SYS_getsockopt 55
#define SYS_fcntl     72
#define SYS_mkdir     83

static inline int sys1(int n, int a) { int r; __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory"); return r; }
static inline int sys2(int n, int a, int b) { int r; __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory"); return r; }
static inline int sys3(int n, int a, int b, int c) { int r; __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static inline int sys4(int n, int a, int b, int c, int d) { int r; __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory"); return r; }
static inline int sys5(int n, int a, int b, int c, int d, int e) { int r; __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory"); return r; }

int unlink(const char* pathname) {
    int r = sys1(SYS_unlink, (int)pathname);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int mkdir(const char* pathname, mode_t mode) {
    int r = sys2(SYS_mkdir, (int)pathname, mode);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

void* sbrk(intptr_t increment) {
    void* cur = (void*)sys1(SYS_brk, 0);
    if (increment != 0) sys1(SYS_brk, (int)((char*)cur + increment));
    return cur;
}

int socket(int domain, int type, int protocol) {
    int r = sys3(SYS_socket, domain, type, protocol);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    int r = sys3(SYS_bind, sockfd, (int)addr, addrlen);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int listen(int sockfd, int backlog) {
    int r = sys2(SYS_listen, sockfd, backlog);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    int r = sys3(SYS_accept, sockfd, (int)addr, (int)addrlen);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
    int r = sys4(SYS_accept, sockfd, (int)addr, (int)addrlen, flags);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    int r = sys3(SYS_connect, sockfd, (int)addr, addrlen);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int shutdown(int sockfd, int how) {
    int r = sys2(SYS_shutdown, sockfd, how);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    ssize_t r = sys4(SYS_sendto, sockfd, (int)buf, (int)len, flags);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    ssize_t r = sys4(SYS_recvfrom, sockfd, (int)buf, (int)len, flags);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    int r = sys5(SYS_setsockopt, sockfd, level, optname, (int)optval, optlen);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
    int r = sys5(SYS_getsockopt, sockfd, level, optname, (int)optval, (int)optlen);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    int r = sys3(SYS_getsockname, sockfd, (int)addr, (int)addrlen);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    int r = sys3(SYS_getpeername, sockfd, (int)addr, (int)addrlen);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    int r = sys4(SYS_socketpair, domain, type, protocol, (int)sv);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    int r = sys3(SYS_fcntl, fd, cmd, arg);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int select(int nfds, void* readfds, void* writefds, void* exceptfds, struct timeval* timeout) {
    int r = sys5(23, nfds, (int)readfds, (int)writefds, (int)exceptfds, (int)timeout);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int poll(struct pollfd* fds, unsigned long nfds, int timeout) {
    (void)fds; (void)nfds; (void)timeout;
    return -1;
}

int pipe(int pipefd[2]) {
    (void)pipefd;
    return -1;
}

void _start(void) {
    extern int main(void);
    int ret = main();
    syscall1(11, ret);
    __builtin_unreachable();
}
