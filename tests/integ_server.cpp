// Integration test against a REAL local server:  integ_server <port> <1.10|1.11|1.11.2|1.12|1.12.2> [host=127.0.0.1]
// Status ping, offline login, join, validates EVERY received packet against the generated decoders,
// teleport-confirms, sends chat and expects it back, then idles to see keep-alives.
#include <mcscon/packets.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <map>
using namespace mc;

struct T {
    Client* c; Version v; bool gotStatus = false, joined = false, gotPos = false, gotChat = false, chatSent = false, gotKA = false, kicked = false;
    int bad = 0, total = 0; std::map<int, int> seen, badIds; double lastPosX = 0;
};
static T t;
static void onPacket(void*, const Packet& p) {
    ++t.total;
    if (p.state == State::Status) { Reader r = p.r; StrView s = r.str(); printf("  status: %.*s\n", (int)s.len, s.data); t.gotStatus = true; return; }
    if (p.state != State::Play) return;
    ++t.seen[p.id];
    if (p.id == (u8)ids::PlayCb::Invalid) { ++t.bad; printf("  UNKNOWN wire id 0x%02x\n", p.wireId); return; }
    if (!cb::validate((ids::PlayCb)p.id, p.r, p.version)) { ++t.bad; ++t.badIds[p.id]; if (t.badIds[p.id] == 1) printf("  DECODE FAILED id=%d wire=0x%02x len=%zu\n", p.id, p.wireId, p.r.left()); }
    cb::Login lg; cb::Position pos; cb::Chat ch; cb::KickDisconnect kd;
    if (pkt::read(p, lg)) { t.joined = true; printf("  join: eid=%d gm=%u dim=%d type=%.*s\n", lg.entityId, lg.gameMode, lg.dimension, (int)lg.levelType.len, lg.levelType.data); pkt::clientSettings(*t.c); }
    else if (pkt::read(p, pos)) { t.gotPos = true; printf("  pos: %.1f %.1f %.1f tp=%d\n", pos.x, pos.y, pos.z, pos.teleportId); pkt::teleportConfirm(*t.c, pos.teleportId); pkt::positionLook(*t.c, pos.x, pos.y, pos.z, pos.yaw, pos.pitch, true); }
    else if (pkt::read(p, ch)) { printf("  chat: %.*s\n", (int)ch.message.len, ch.message.data); if (memmem(ch.message.data, ch.message.len, "mcscon-hello", 12)) t.gotChat = true; }
    else if (pkt::read(p, kd)) { t.kicked = true; printf("  kicked: %.*s\n", (int)kd.reason.len, kd.reason.data); }
}
static Error g_err = Error::None;
static void onError(void*, Error e, i32 info) { g_err = e; printf("  error %d info %d\n", (int)e, info); }
static double now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: integ_server <port> <version> [host]"); return 2; }
    u16 port = (u16)atoi(argv[1]); const char* vs = argv[2]; const char* host = argc > 3 ? argv[3] : "127.0.0.1";
    Version v = !strcmp(vs, "1.10") ? Version::V1_10 : !strcmp(vs, "1.11") ? Version::V1_11 : !strcmp(vs, "1.11.2") ? Version::V1_11_2 : !strcmp(vs, "1.12") ? Version::V1_12 : !strcmp(vs, "1.12.1") ? Version::V1_12_1 : Version::V1_12_2;
    static u8 rx[256 * 1024], sc[1024 * 1024], tx[8192];
    Buffers b = {rx, sizeof rx, sc, sizeof sc, tx, sizeof tx};
    t.v = v;
    { // 1) status
        PosixSocket s; Client c(makePosixTransport(&s), b, v); c.onPacket(onPacket, 0); c.onError(onError); t.c = &c;
        if (!c.connectStatus(host, port)) { puts("FAIL: connect (status)"); return 1; }
        for (double end = now() + 3; now() < end && !t.gotStatus && c.poll() >= 0;) usleep(5000);
        if (!t.gotStatus) { puts("FAIL: no status"); return 1; }
        c.disconnect();
    }
    PosixSocket s; Client c(makePosixTransport(&s), b, v); c.onPacket(onPacket, 0); c.onError(onError); t.c = &c;
    if (!c.connectLogin(host, port, "mcscon")) { puts("FAIL: connect (login)"); return 1; }
    double start = now(), chatAt = 0; double secs = getenv("MCSCON_SECS") ? atof(getenv("MCSCON_SECS")) : 12;
    while (now() - start < secs && c.poll() >= 0) {
        if (t.joined && t.gotPos && !t.chatSent) { sb::Chat m = {}; m.message = StrView{"mcscon-hello", 12}; pkt::send(c, m); t.chatSent = true; chatAt = now(); }
        if (t.gotChat && now() - chatAt > 0.5 && now() - start > secs - 1) break;
        usleep(5000);
    }
    // idle a little more so the server's keep-alive (every 15s in vanilla) is not required; we only report
    printf("keep-alives seen: %d (auto-answered)\n", t.seen[(int)ids::PlayCb::KeepAlive]);
    printf("packets: %d total, %zu distinct play ids, %d failed to decode\n", t.total, t.seen.size(), t.bad);
    bool ok = t.joined && t.gotPos && t.gotChat && t.bad == 0 && !t.kicked && c.state() == State::Play;
    printf("%s  joined=%d position=%d chat-echo=%d kicked=%d state=%d\n", ok ? "PASS" : "FAIL", t.joined, t.gotPos, t.gotChat, t.kicked, (int)c.state());
    c.disconnect(); return ok ? 0 : 1;
}
