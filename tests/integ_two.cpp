// Two sessions in one thread idling for a while: neither may be dropped by the server (keep-alive handling).
#include <mcscon/session.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
using namespace mc;
static u32 nowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
struct Bot { PosixSocket sock; StaticStorage<25, 200 * 1024, 32, 8>* st; u8 *rx, *sc, *tx; Session* s; int ka = 0, kicks = 0; Error err = Error::None; };
int main(int argc, char** argv) {
    u16 port = (u16)atoi(argv[1]); int secs = argc > 2 ? atoi(argv[2]) : 40; Bot bots[2]; const char* names[2] = {"idleA", "idleB"};
    for (int i = 0; i < 2; ++i) { Bot& b = bots[i]; b.st = new StaticStorage<25, 200 * 1024, 32, 8>; b.rx = new u8[128 * 1024]; b.sc = new u8[512 * 1024]; b.tx = new u8[4096];
        Buffers bf = {b.rx, 128 * 1024, b.sc, 512 * 1024, b.tx, 4096}; b.s = new Session(makePosixTransport(&b.sock), bf, Version::V1_12_2, b.st->get());
        b.s->onPacket([](void* u, const Packet& p) { Bot* bb = (Bot*)u; if (p.state == State::Play && p.id == (u8)ids::PlayCb::KeepAlive) ++bb->ka; }, &b);
        b.s->client().onError([](void*, Error e, i32) { printf("error %d\n", (int)e); });
        b.s->connect("127.0.0.1", port, names[i]); }
    u32 t0 = nowMs();
    while (nowMs() - t0 < (u32)secs * 1000) { for (Bot& b : bots) if (b.s->client().state() != State::Closed) { b.s->poll(); b.s->update(nowMs()); } usleep(4000); }
    bool ok = true;
    for (int i = 0; i < 2; ++i) { bool alive = bots[i].s->client().state() == State::Play; printf("%s: state=%d keepalives=%d\n", names[i], (int)bots[i].s->client().state(), bots[i].ka); ok &= alive; }
    printf("%s\n", ok ? "PASS both alive" : "FAIL a bot was dropped"); return ok ? 0 : 1;
}
