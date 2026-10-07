// Repro: server-side teleports of a Session bot (/tp) must not make the server complain about movement.
#include <mcscon/session.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <initializer_list>
#include <stdarg.h>
using namespace mc;
static u32 nowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static void cmd(const char* fmt, ...) { char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); printf("CMD %s\n", b); fflush(stdout); }
int main(int argc, char** argv) {
    u16 port = (u16)atoi(argv[1]); PosixSocket sock; static StaticStorage<81, 400 * 1024, 64, 8> st; static u8 rx[256 * 1024], sc[1024 * 1024], tx[8192];
    Buffers b = {rx, sizeof rx, sc, sizeof sc, tx, sizeof tx}; Session s(makePosixTransport(&sock), b, Version::V1_12_2, st.get());
    int tps = 0; s.onEvent([](void* u, int ev, i32) { if (ev == EvTeleported) ++*(int*)u; }, &tps);
    s.connect("127.0.0.1", port, "tpbot");
    auto pump = [&](u32 ms) { u32 t0 = nowMs(); while (nowMs() - t0 < ms) { s.poll(); s.update(nowMs()); usleep(4000); } };
    pump(3000);
    printf("INFO start %.2f %.2f %.2f\n", s.body().x, s.body().y, s.body().z);
    for (int mode = 1; mode >= 0; --mode) {
        cmd("gamemode %d tpbot", mode); pump(800);
        int ax = (int)floor(s.body().x), ay = (int)floor(s.body().y), az = (int)floor(s.body().z);
        cmd("setblock %d %d %d minecraft:crafting_table", ax + 1, ay, az); pump(800);
        for (int face : {4, 5, 1}) {
            for (int rot : {270, 0}) {
                s.setRotation((float)rot, 0); s.placeBlock({ax + 1, ay, az}, (i8)face); pump(800);
                printf("INFO mode=%d face=%d rot=%d window=%d state: gm=%d pos=%.2f,%.2f,%.2f\n", mode, face, rot, s.inventory().openWindow() != 0, s.state().gameMode, s.body().x, s.body().y, s.body().z);
                if (s.inventory().openWindow()) { s.inventory().closeWindow(s.client()); pump(300); }
            }
        }
    }
    return 0;
}
