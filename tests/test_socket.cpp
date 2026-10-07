// Real TCP over loopback with a tiny threaded fake server: exercises the POSIX transport end to end.
#include <mcscon/client.h>
#include "../platform/posix_transport.h"
#include "check.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <thread>
#include <vector>
#include <atomic>
using namespace mc;

static std::atomic<int> g_keepaliveEcho{0};

static void sendFrame(int fd, const std::vector<u8>& payload, bool comp) {
    std::vector<u8> f; u8 t[5]; Writer w(t, 5);
    w.varint((i32)payload.size() + (comp ? 1 : 0)); f.insert(f.end(), t, w.p); if (comp) f.push_back(0);
    f.insert(f.end(), payload.begin(), payload.end());
    for (size_t off = 0; off < f.size();) { ssize_t n = write(fd, f.data() + off, f.size() - off); if (n <= 0) return; off += n; }
}
static void server(int lfd) {
    int fd = accept(lfd, 0, 0);
    u8 buf[256]; size_t got = 0; ssize_t n;
    while (got < 4 || memcmp(buf + got - 4, "\x03" "Bot", 4) != 0) { n = read(fd, buf + got, sizeof buf - got); if (n <= 0) return; got += n; }   // handshake + login start (TCP may split them)
    sendFrame(fd, {0x03, 0x80, 0x02}, false);                             // SetCompression 256
    std::vector<u8> ok = {0x02, 1, 'u', 3, 'B', 'o', 't'}; sendFrame(fd, ok, true);
    sendFrame(fd, {0x1F, 0, 0, 0, 0, 0, 0, 0x12, 0x34}, true);           // keepalive
    got = 0;                                                              // echo: len=10, 0, 0x0B, 8 bytes
    while (got < 11 && (n = read(fd, buf + got, 11 - got)) > 0) got += n;
    n = (ssize_t)got;
    if (n == 11 && buf[0] == 10 && buf[2] == 0x0B && buf[9] == 0x12 && buf[10] == 0x34) g_keepaliveEcho = 1;
    close(fd);
}
static int g_count = 0;
static void onPkt(void*, const Packet&) { ++g_count; }

static void test_loopback() {
    int lfd = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (sockaddr*)&a, sizeof a); listen(lfd, 1); socklen_t l = sizeof a; getsockname(lfd, (sockaddr*)&a, &l);
    std::thread th(server, lfd);

    PosixSocket sock; std::vector<u8> rx(4096), sc(4096), tx(512);
    Buffers b = {rx.data(), rx.size(), sc.data(), sc.size(), tx.data(), tx.size()};
    Client c(makePosixTransport(&sock), b, Version::V1_12_2); c.onPacket(onPkt, 0);
    CHECK(c.connectLogin("127.0.0.1", ntohs(a.sin_port), "Bot"));
    for (int i = 0; i < 500 && g_count < 3 && c.poll() >= 0; ++i) usleep(2000);
    CHECK(g_count == 3); CHECK(c.state() == State::Play);
    for (int i = 0; i < 50; ++i) { usleep(2000); c.poll(); }
    th.join(); close(lfd);
    CHECK(g_keepaliveEcho == 1);
    for (int i = 0; i < 50 && c.poll() >= 0; ++i) usleep(2000);
    CHECK(c.state() == State::Closed);   // server closed -> detected
    PosixSocket dead; Client d(makePosixTransport(&dead), b, Version::V1_12_2);
    CHECK(!d.connectLogin("127.0.0.1", 1, "x"));   // refused
}
int main() { RUN(test_loopback); DONE(); }
