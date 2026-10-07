#include "posix_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mc {
namespace {
bool doConnect(void* c, const char* host, u16 port) {
    PosixSocket* s = (PosixSocket*)c;
    char ps[8]; snprintf(ps, sizeof ps, "%u", port);
    struct addrinfo hints = {}, *res = 0;
    hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return false;
    s->fd = -1;
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) { s->fd = fd; break; }
        close(fd);
    }
    freeaddrinfo(res);
    if (s->fd < 0) return false;
    int one = 1;
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL, 0) | O_NONBLOCK);
    return true;
}
int doRecv(void* c, u8* buf, size_t cap) {
    PosixSocket* s = (PosixSocket*)c;
    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n > 0) return (int)n;
    if (n == 0) return -1;                                   // orderly close
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}
int doSend(void* c, const u8* buf, size_t len) {
    PosixSocket* s = (PosixSocket*)c;
#ifdef MSG_NOSIGNAL
    ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);
#else
    ssize_t n = send(s->fd, buf, len, 0);
#endif
    if (n >= 0) return (int)n;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}
void doClose(void* c) {
    PosixSocket* s = (PosixSocket*)c;
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}
} // namespace

Transport makePosixTransport(PosixSocket* s) {
    s->fd = -1;
    Transport t = {s, doConnect, doRecv, doSend, doClose};
    return t;
}
} // namespace mc
