// Edge-case runs against a real server:  integ_edge <port> <version> <mode>
//  online   : server has online-mode=true -> must report EncryptionRequired and close cleanly
//  smallbuf : tiny rx/scratch buffers -> oversized packets (chunks) must be skipped, connection stays healthy for 25 s
//  nolimit  : (any server) stays connected 20 s with a 1-byte-at-a-time transport to stress framing on real traffic
#include <mcscon/session.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
using namespace mc;
static u32 nowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static int g_errs[16], g_total = 0, g_bad = 0; static Error g_firstErr = Error::None;
static void onErr(void*, Error e, i32) { ++g_errs[(int)e]; if (g_firstErr == Error::None) g_firstErr = e; }
static void onPkt(void*, const Packet& p) { ++g_total; if (p.state == State::Play && p.id < (u8)ids::PlayCb::Count_ && !cb::validate((ids::PlayCb)p.id, p.r, p.version)) ++g_bad; }

// transport wrapper delivering at most 1 byte per recv (worst-case fragmentation)
struct Slow { Transport inner; };
static bool sConnect(void* c, const char* h, u16 p) { Slow* s = (Slow*)c; return s->inner.connect(s->inner.ctx, h, p); }
static int sRecv(void* c, u8* b, size_t cap) { Slow* s = (Slow*)c; (void)cap; return s->inner.recv(s->inner.ctx, b, 1); }
static int sSend(void* c, const u8* b, size_t n) { Slow* s = (Slow*)c; return s->inner.send(s->inner.ctx, b, n); }
static void sClose(void* c) { Slow* s = (Slow*)c; s->inner.close(s->inner.ctx); }

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    u16 port = (u16)atoi(argv[1]); const char* vs = argv[2]; const char* mode = argv[3];
    Version v = !strcmp(vs, "1.10") ? Version::V1_10 : !strcmp(vs, "1.11") ? Version::V1_11 : !strcmp(vs, "1.11.2") ? Version::V1_11_2 : !strcmp(vs, "1.12") ? Version::V1_12 : !strcmp(vs, "1.12.1") ? Version::V1_12_1 : Version::V1_12_2;
    PosixSocket sock; Transport t = makePosixTransport(&sock); Slow slow; slow.inner = t; Transport st = {&slow, sConnect, sRecv, sSend, sClose};
    bool small = !strcmp(mode, "smallbuf"), online = !strcmp(mode, "online"), slowMode = !strcmp(mode, "nolimit");
    static u8 rx[512 * 1024], sc[1024 * 1024], tx[8192]; static StaticStorage<81, 400 * 1024, 128, 32> store;
    Buffers b = {rx, small ? 4096u : sizeof rx, sc, small ? 4096u : sizeof sc, tx, sizeof tx};
    Session s(slowMode ? st : t, b, v, store.get()); s.client().onError(onErr);
    s.onPacket(onPkt, 0);
    if (!s.connect("127.0.0.1", port, "edge")) { puts("FAIL connect"); return 1; }
    u32 t0 = nowMs(), limit = online ? 4000 : small ? 25000 : 20000; bool closed = false;
    while (nowMs() - t0 < limit) { if (s.poll() < 0) { closed = true; break; } s.update(nowMs()); usleep(slowMode ? 100 : 3000); }
    bool ok;
    if (online) { ok = g_firstErr == Error::EncryptionRequired && closed && s.client().state() == State::Closed; printf("%s online-mode server: firstErr=%d closed=%d\n", ok ? "PASS" : "FAIL", (int)g_firstErr, closed); }
    else if (small) {
        ok = !closed && s.state().spawned && g_errs[(int)Error::PacketTooLarge] > 0 && g_bad == 0 && g_errs[(int)Error::Protocol] == 0 && g_errs[(int)Error::Inflate] == 0;
        printf("%s smallbuf: packets=%d tooLarge=%d bad=%d protocolErr=%d closed=%d spawned=%d\n", ok ? "PASS" : "FAIL", g_total, g_errs[(int)Error::PacketTooLarge], g_bad, g_errs[(int)Error::Protocol], closed, s.state().spawned);
    } else {
        ok = !closed && s.state().spawned && g_bad == 0 && g_firstErr == Error::None && s.world().loadedColumns() >= 9;
        printf("%s 1-byte-reads: packets=%d bad=%d err=%d columns=%u\n", ok ? "PASS" : "FAIL", g_total, g_bad, (int)g_firstErr, s.world().loadedColumns());
    }
    s.disconnect(); return ok ? 0 : 1;
}
