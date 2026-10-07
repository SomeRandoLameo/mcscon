// Scenario test for the high-level Session against a real local server. Driven by tools/integ_session.py:
// the bot prints "CMD <console command>" lines which the orchestrator forwards to the server console.
//   integ_session <port> <1.10|1.11|1.11.2|1.12|1.12.2>
#include <mcscon/session.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
using namespace mc;

static u32 nowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static int g_pass = 0, g_fail = 0;
static void report(const char* name, bool ok, const char* extra = "") { printf("%s %s %s\n", ok ? "PASS" : "FAIL", name, extra); fflush(stdout); ok ? ++g_pass : ++g_fail; }
static void cmd(const char* c) { printf("CMD %s\n", c); fflush(stdout); }

static StaticStorage<81, 400 * 1024, 128, 32> g_store;
static u8 rx[256 * 1024], sc[1024 * 1024], tx[8192];
static int g_teleports = 0, g_deaths = 0, g_respawns = 0, g_kicks = 0, g_chats = 0; static char g_lastChat[512];
static void onEvent(void*, int ev, i32) { if (ev == EvTeleported) ++g_teleports; if (ev == EvDied) ++g_deaths; if (ev == EvRespawned) ++g_respawns; if (ev == EvKicked) ++g_kicks; }
static void onChat(void*, const char* text, u8, StrView) { ++g_chats; snprintf(g_lastChat, sizeof g_lastChat, "%s", text); }

template<class F> static bool waitFor(Session& s, F cond, u32 timeoutMs) {
    u32 t0 = nowMs();
    while (nowMs() - t0 < timeoutMs) { if (s.poll() < 0) return cond(); s.update(nowMs()); if (cond()) return true; usleep(5000); }
    return cond();
}
static void settle(Session& s, u32 ms) { u32 t0 = nowMs(); while (nowMs() - t0 < ms) { if (s.poll() < 0) return; s.update(nowMs()); usleep(5000); } }

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    u16 port = (u16)atoi(argv[1]); const char* vs = argv[2];
    Version v = !strcmp(vs, "1.10") ? Version::V1_10 : !strcmp(vs, "1.11") ? Version::V1_11 : !strcmp(vs, "1.11.2") ? Version::V1_11_2 : !strcmp(vs, "1.12") ? Version::V1_12 : !strcmp(vs, "1.12.1") ? Version::V1_12_1 : Version::V1_12_2;
    bool legacyNames = v == Version::V1_10;
    PosixSocket sock; Buffers b = {rx, sizeof rx, sc, sizeof sc, tx, sizeof tx};
    Session s(makePosixTransport(&sock), b, v, g_store.get());
    s.onEvent(onEvent, 0); s.onChat(onChat, 0);
    if (getenv("MCSCON_TRACE")) s.entities().onEvent([](void*, EntityEvent ev, const Entity& e, i32 arg) { if (ev != EntityEvent::Moved && ev != EntityEvent::Updated) printf("INFO entity ev=%d id=%d kind=%d type=%d at %.1f,%.1f,%.1f arg=%d\n", (int)ev, e.id, (int)e.kind, e.type, e.x, e.y, e.z, arg); }, 0);
    if (!s.connect("127.0.0.1", port, "mcscon")) { report("connect", false); return 1; }
    report("join", waitFor(s, [&] { return s.state().spawned; }, 8000));
    cmd("gamemode 1 mcscon");
    report("state-gamemode-creative", waitFor(s, [&] { return s.state().gameMode == Creative && (s.state().abilities & AbilityAllowFlying); }, 4000));
    settle(s, 2500);                                                       // chunks + physics settle
    const Body& bd = s.body();
    report("world-chunks-loaded", s.world().loadedColumns() >= 9);
    int fx = (int)floor(bd.x), fy = (int)floor(bd.y), fz = (int)floor(bd.z);
    char buf[160]; snprintf(buf, sizeof buf, "pos=%.2f,%.2f,%.2f columns=%u arena=%u/%u", bd.x, bd.y, bd.z, s.world().loadedColumns(), s.world().arenaUsed(), s.world().arenaCapacity());
    printf("INFO %s\n", buf);
    report("world-flat-layout", s.world().blockId(fx, 0, fz) == 7 && s.world().blockId(fx, 1, fz) == 3 && s.world().blockId(fx, 2, fz) == 3 && s.world().blockId(fx, 3, fz) == 2 && s.world().blockId(fx, 4, fz) == 0 && s.world().highestBlock(fx, fz) == 3);
    report("physics-standing-onground", bd.onGround && fabs(bd.y - 4.0) < 1e-6);
    report("tablist-self", s.players().findByName("mcscon") != 0);
    report("own-entity-id", s.state().entityId >= 0);

    // ---- survival: server-side movement validation is active now
    cmd("gamemode 0 mcscon");
    report("gamemode-change", waitFor(s, [&] { return s.state().gameMode == Survival; }, 4000));
    settle(s, 500);
    int tp0 = g_teleports; double x0 = bd.x, z0 = bd.z;
    s.controls.forward = true; s.controls.sprint = true; settle(s, 2000); s.controls.forward = false; s.controls.sprint = false; settle(s, 600);
    double dist = sqrt((bd.x - x0) * (bd.x - x0) + (bd.z - z0) * (bd.z - z0));
    snprintf(buf, sizeof buf, "dist=%.2f teleports=%d", dist, g_teleports - tp0);
    report("physics-sprint-walk", dist > 8.0 && dist < 13.0 && g_teleports == tp0, buf);     // ~5.6 m/s for 2 s
    double y0 = bd.y; s.controls.jump = true; double maxY = y0; u32 t0 = nowMs();
    while (nowMs() - t0 < 400) { s.poll(); s.update(nowMs()); if (bd.y > maxY) maxY = bd.y; if (nowMs() - t0 > 60) s.controls.jump = false; usleep(5000); }
    s.controls.jump = false; settle(s, 800);
    snprintf(buf, sizeof buf, "peak=%.3f", maxY - y0);
    report("physics-jump", maxY - y0 > 1.1 && maxY - y0 < 1.3 && bd.onGround && fabs(bd.y - y0) < 1e-6 && g_teleports == tp0, buf);

    // ---- world edits
    fx = (int)floor(bd.x); fy = (int)floor(bd.y); fz = (int)floor(bd.z);
    cmd("execute mcscon ~ ~ ~ setblock ~ ~-1 ~ minecraft:gold_block");
    report("blockchange-gold", waitFor(s, [&] { return s.world().blockId(fx, fy - 1, fz) == 41; }, 3000));
    settle(s, 400);
    report("physics-onground-after-blockchange", bd.onGround && g_teleports == tp0);
    cmd("execute mcscon ~ ~ ~ fill ~2 ~-1 ~-1 ~4 ~1 ~1 minecraft:stone");     // wall in +x direction (multi block change)
    report("multiblockchange-fill", waitFor(s, [&] { return s.world().blockId(fx + 3, fy, fz) == 1 && s.world().blockId(fx + 2, fy + 1, fz + 1) == 1; }, 3000));
    // walking into the stone block must be stopped by collision, no rubber banding
    double zWall = bd.z;
    s.setRotation(270, 0);                                                   // forward = +x
    s.controls.forward = true; settle(s, 2500); s.controls.forward = false; settle(s, 300);
    snprintf(buf, sizeof buf, "x=%.3f wallFaceAt=%d teleports=%d", bd.x, fx + 2, g_teleports - tp0);
    report("physics-collision", fabs(bd.x - (fx + 2 - 0.3)) < 0.01 && fabs(bd.z - zWall) < 0.05, buf);
    report("physics-collision-no-rubberband", g_teleports == tp0);

    // ---- entities
    cmd(legacyNames ? "execute mcscon ~ ~ ~ summon Pig ~-3 ~ ~-3" : "execute mcscon ~ ~ ~ summon minecraft:pig ~-3 ~ ~-3");
    report("entity-spawn-pig", waitFor(s, [&] { return s.entities().nearest(bd.x, bd.y, bd.z, 10, (int)EntityKind::Mob) != 0; }, 3000));
    const Entity* pig = s.entities().nearest(bd.x, bd.y, bd.z, 10, (int)EntityKind::Mob);
    snprintf(buf, sizeof buf, "type=%d health=%.1f", pig ? pig->type : -1, pig ? pig->health : -1);
    report("entity-pig-type-and-health", pig && pig->type == 90 && pig->hasHealth && pig->health == 10.0f, buf);
    i32 pigId = pig ? pig->id : -1; double pig0x = pig ? pig->x : 0, pig0z = pig ? pig->z : 0;
    cmd("execute mcscon ~ ~ ~ tp @e[type=Pig,r=10] ~ ~ ~5");
    cmd(legacyNames ? "execute mcscon ~ ~ ~ tp @e[type=Pig,r=20] ~ ~ ~5" : "execute mcscon ~ ~ ~ tp @e[type=pig,r=20] ~ ~ ~5");
    report("entity-teleport-tracked", waitFor(s, [&] { const Entity* e = s.entities().find(pigId); return e && (fabs(e->x - pig0x) > 1 || fabs(e->z - pig0z) > 1); }, 3000));
    cmd(legacyNames ? "kill @e[type=Pig]" : "kill @e[type=pig]");
    report("entity-destroyed", waitFor(s, [&] { return s.entities().find(pigId) == 0; }, 3000));

    // ---- inventory
    cmd(legacyNames ? "give mcscon diamond 5" : "give mcscon minecraft:diamond 5");
    report("inventory-give-diamond", waitFor(s, [&] { return s.inventory().countOf(264) == 5; }, 3000));
    int slot = s.inventory().find(264);
    report("inventory-slot-hotbar", slot >= SlotHotbarFirst && slot < SlotOffhand);
    report("inventory-click-pickup", s.inventory().click(s.client(), (i16)slot) && waitFor(s, [&] { return s.inventory().cursor().id == 264; }, 3000));
    s.inventory().click(s.client(), (i16)slot);                                       // put it back
    report("inventory-click-putback", waitFor(s, [&] { return s.inventory().cursor().id == -1 && s.inventory().countOf(264) == 5; }, 3000));
    report("inventory-hotbar-select", s.selectHotbar(4) && s.inventory().heldSlot() == 4);

    // ---- container window: chest next to us with an item inside, open it by right click
    BlockPos chest = {(i32)floor(bd.x) - 1, (i32)floor(bd.y), (i32)floor(bd.z)};
    cmd("execute mcscon ~ ~ ~ setblock ~-1 ~ ~ minecraft:chest");
    cmd("execute mcscon ~ ~ ~ replaceitem block ~-1 ~ ~ slot.container.0 minecraft:apple 3");
    settle(s, 600);
    s.lookAt(chest.x + 0.5, chest.y + 0.5, chest.z + 0.5);
    s.placeBlock(chest, 5);
    bool opened = waitFor(s, [&] { return s.inventory().openWindow() != 0 && s.inventory().openWindow()->loaded; }, 3000);
    report("window-open-chest", opened && s.inventory().openWindow()->typeIs("minecraft:chest"));
    if (opened) { const Window* w = s.inventory().openWindow(); snprintf(buf, sizeof buf, "count=%d slot0=%d x%d", w->count, w->slots[0].id, w->slots[0].count); report("window-chest-contents", w->slots[0].id == 260 && w->slots[0].count == 3, buf);
        static bool trace = false; trace = getenv("MCSCON_TRACE") != 0;
        s.onPacket([](void*, const Packet& p) { if (trace && p.state == State::Play && (p.id == (u8)ids::PlayCb::Transaction || p.id == (u8)ids::PlayCb::WindowItems || p.id == (u8)ids::PlayCb::SetSlot)) { printf("INFO pkt id=%d len=%zu\n", p.id, p.r.left()); } }, 0);
        s.inventory().shiftClick(s.client(), 0);
        report("window-shift-click", waitFor(s, [&] { return s.inventory().countOf(260) == 3; }, 3000)); { const Window* w2 = s.inventory().openWindow(); if (w2) { int tot = 0; for (int i = 0; i < w2->count; ++i) if (w2->slots[i].id == 260) { printf("INFO apple in window slot %d x%d\n", i, w2->slots[i].count); tot++; } printf("INFO count=%d apples-in-window=%d cursor=%d\n", w2->count, tot, s.inventory().cursor().id); } }
        s.inventory().closeWindow(s.client()); settle(s, 300); report("window-close", s.inventory().openWindow() == 0); }

    // ---- chat
    int c0 = g_chats; s.say("hello from mcscon");
    report("chat-echo", waitFor(s, [&] { return g_chats > c0 && strstr(g_lastChat, "<mcscon> hello from mcscon"); }, 3000), g_lastChat);
    cmd("say server-says-hi");
    report("chat-server-say", waitFor(s, [&] { return strstr(g_lastChat, "server-says-hi") != 0; }, 3000), g_lastChat);

    // ---- death & respawn
    cmd("kill mcscon");
    report("death-detected", waitFor(s, [&] { return g_deaths >= 1; }, 4000));
    report("auto-respawn", waitFor(s, [&] { return g_respawns >= 1 && s.state().health > 0 && s.state().spawned; }, 4000));
    settle(s, 1500);
    report("respawn-world-reloaded", s.world().loadedColumns() >= 9 && s.body().onGround);
    report("never-kicked", g_kicks == 0 && s.client().state() == State::Play);
    printf("SUMMARY pass=%d fail=%d\n", g_pass, g_fail); fflush(stdout);
    s.disconnect(); return g_fail ? 1 : 0;
}
