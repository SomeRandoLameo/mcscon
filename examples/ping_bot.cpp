// usage: ping_bot <host> [port] [version 1.10|1.11|1.12|1.12.2]   -> pings, then joins offline-mode as "mcscon" and prints chat.
#include <mcscon/packets.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
using namespace mc;

static void onPacket(void* u, const Packet& p) {
    Client& c = *(Client*)u;
    if (p.state == State::Status && p.id == (u8)ids::StatusCb::ServerInfo) {
        Reader r = p.r; StrView s = r.str(); printf("status: %.*s\n", (int)s.len, s.data);
    } else if (p.state == State::Login && p.id == (u8)ids::LoginCb::Success) {
        puts("login ok");
    } else if (p.state == State::Play) {
        cb::Login j; cb::Position pp; cb::Chat m; cb::KickDisconnect k; cb::UpdateHealth h;
        if (pkt::read(p, j)) { printf("joined: eid=%d gm=%u\n", j.entityId, j.gameMode); pkt::clientSettings(c); }
        else if (pkt::read(p, pp)) { printf("pos %.1f %.1f %.1f\n", pp.x, pp.y, pp.z); pkt::teleportConfirm(c, pp.teleportId); pkt::positionLook(c, pp.x, pp.y, pp.z, pp.yaw, pp.pitch, true); }
        else if (pkt::read(p, m)) printf("chat: %.*s\n", (int)m.message.len, m.message.data);
        else if (pkt::read(p, k)) printf("kicked: %.*s\n", (int)k.reason.len, k.reason.data);
        else if (pkt::read(p, h)) { printf("health %.1f\n", h.health); if (h.health <= 0) pkt::respawn(c); }
    }
}
static void onError(void*, Error e, i32 info) { printf("error %d (%d)\n", (int)e, info); }

int main(int argc, char** argv) {
    if (argc < 2) { puts("usage: ping_bot <host> [port] [1.10|1.11|1.12|1.12.2]"); return 1; }
    u16 port = argc > 2 ? (u16)atoi(argv[2]) : 25565;
    Version v = Version::V1_12_2;
    if (argc > 3) { if (!strcmp(argv[3], "1.10")) v = Version::V1_10; else if (!strcmp(argv[3], "1.11")) v = Version::V1_11; else if (!strcmp(argv[3], "1.12")) v = Version::V1_12; }
    static u8 rx[64 * 1024], sc[256 * 1024], tx[4096];     // ~324 KB total; shrink if you do not need chunks
    Buffers b = {rx, sizeof rx, sc, sizeof sc, tx, sizeof tx};
    for (int pass = 0; pass < 2; ++pass) {
        PosixSocket s; Client c(makePosixTransport(&s), b, v);
        c.onPacket(onPacket, &c); c.onError(onError);
        bool ok = pass == 0 ? c.connectStatus(argv[1], port) : c.connectLogin(argv[1], port, "mcscon");
        if (!ok) { puts("connect failed"); return 1; }
        for (int i = 0; i < (pass == 0 ? 500 : 6000) && c.poll() >= 0; ++i) {
            usleep(10000);
        }
        c.disconnect();
    }
}
