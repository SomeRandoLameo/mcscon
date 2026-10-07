// Packet-coverage scenario against a real local server: two bots, many console commands, and EVERY clientbound packet
// is decoded with the generated decoders. Reports which of the clientbound packet types were exercised and fails on any
// decode error.    integ_coverage <port> <version>      (driven by tools/integ_coverage.py)
#define MCSCON_PACKET_NAMES
#include <mcscon/session.h>
#include "../platform/posix_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
using namespace mc;

static u32 nowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static void cmd(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void cmd(const char* fmt, ...) { char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); printf("CMD %s\n", b); fflush(stdout); }

struct Bot {
    PosixSocket sock; StaticStorage<81, 400 * 1024, 128, 32>* store; u8 *rx, *sc, *tx; Session* s; const char* name;
    bool seen[256] = {}, failed[256] = {}; int total = 0, kicks = 0; int bad = 0;
    Bot(const char* n, Version v) : name(n) {
        store = new StaticStorage<81, 400 * 1024, 128, 32>; rx = new u8[256 * 1024]; sc = new u8[1024 * 1024]; tx = new u8[8192];
        Buffers b = {rx, 256 * 1024, sc, 1024 * 1024, tx, 8192};
        s = new Session(makePosixTransport(&sock), b, v, store->get());
    }
};
static Bot* g_bots[2]; static Version g_ver; static u32 g_lastLoop = 0, g_maxGap = 0; static int g_ka[2];

static void onPkt(void* u, const Packet& p) {
    Bot* b = (Bot*)u; if (p.state != State::Play) return;
    ++b->total;
    if (p.id >= (u8)ids::PlayCb::Count_) { ++b->bad; printf("FAIL %s unknown wire id 0x%02x\n", b->name, p.wireId); return; }
    if (!cb::validate((ids::PlayCb)p.id, p.r, p.version)) {
        if (!b->failed[p.id]) { b->failed[p.id] = true; printf("FAIL decode %s id=%s wire=0x%02x len=%zu bytes=", b->name, ids::PlayCb_names[p.id], p.wireId, p.r.left()); for (size_t i = 0; i < p.r.left() && i < 48; ++i) printf("%02x", p.r.p[i]); printf("\n"); }
        ++b->bad; return;
    }
    if (!b->seen[p.id]) { b->seen[p.id] = true; if (getenv("MCSCON_TRACE")) printf("INFO first %s saw %s\n", b->name, ids::PlayCb_names[p.id]); }
    if (p.id == (u8)ids::PlayCb::KickDisconnect) ++b->kicks;
    if (p.id == (u8)ids::PlayCb::KeepAlive) ++g_ka[b == g_bots[0] ? 0 : 1];
}

static void pump(u32 ms) { u32 t0 = nowMs(); while (nowMs() - t0 < ms) { u32 n0 = nowMs(); if (g_lastLoop && n0 - g_lastLoop > g_maxGap) g_maxGap = n0 - g_lastLoop; g_lastLoop = n0; for (Bot* b : g_bots) if (b->s->client().state() != State::Closed) { b->s->poll(); b->s->update(nowMs()); } usleep(4000); } }
static const char* N(const char* modern, const char* legacy) { return g_ver == Version::V1_10 ? legacy : modern; }
static const char* killAll = "kill @e[type=!player]";   // 1.10 names the player type "Player"
static bool atLeast(Version v) { return (int)g_ver >= (int)v; }

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    u16 port = (u16)atoi(argv[1]); const char* vs = argv[2];
    g_ver = !strcmp(vs, "1.10") ? Version::V1_10 : !strcmp(vs, "1.11") ? Version::V1_11 : !strcmp(vs, "1.11.2") ? Version::V1_11_2 : !strcmp(vs, "1.12") ? Version::V1_12 : !strcmp(vs, "1.12.1") ? Version::V1_12_1 : Version::V1_12_2;
    if (g_ver == Version::V1_10) killAll = "kill @e[type=!Player]";
    Bot A("mcscon", g_ver), B("mcscon2", g_ver); g_bots[0] = &A; g_bots[1] = &B;
    A.s->onPacket(onPkt, &A); B.s->onPacket(onPkt, &B);
    if (!A.s->connect("127.0.0.1", port, "mcscon") || !B.s->connect("127.0.0.1", port, "mcscon2")) { puts("FAIL connect"); return 1; }
    pump(4000);
    if (!A.s->state().spawned || !B.s->state().spawned) { puts("FAIL join"); return 1; }
    cmd("gamerule doMobSpawning false"); cmd("gamerule doDaylightCycle false"); cmd("gamerule mobGriefing false");
    cmd("gamemode 1 mcscon"); cmd("gamemode 1 mcscon2"); cmd("difficulty 1"); pump(1500);
    Session& a = *A.s; Session& b = *B.s;
    cmd("tp mcscon2 mcscon"); pump(1500);
    const Body& bd = a.body(); int ax = (int)floor(bd.x), ay = (int)floor(bd.y), az = (int)floor(bd.z);
    printf("INFO A at %.1f %.1f %.1f, columns=%u entities=%u\n", bd.x, bd.y, bd.z, a.world().loadedColumns(), a.entities().count());

    // ---- screen / scoreboard / border / effects / sound / particle / weather
    cmd("title mcscon title {\"text\":\"Hello\"}"); cmd("title mcscon subtitle {\"text\":\"sub\"}"); cmd("title mcscon times 5 20 5");
    if (atLeast(Version::V1_11)) cmd("title mcscon actionbar {\"text\":\"bar\"}");
    cmd("title mcscon clear"); cmd("title mcscon reset"); pump(600);
    cmd("scoreboard objectives add obj dummy Obj"); cmd("scoreboard objectives setdisplay sidebar obj"); cmd("scoreboard players set mcscon obj 5");
    cmd("scoreboard players add mcscon obj 2"); cmd("scoreboard teams add red Red"); cmd("scoreboard teams join red mcscon"); cmd("scoreboard teams option red color red");
    cmd("scoreboard teams option red friendlyfire false"); pump(600);
    cmd("scoreboard players reset mcscon obj"); cmd("scoreboard teams leave mcscon"); cmd("scoreboard teams remove red"); cmd("scoreboard objectives remove obj"); pump(400);
    cmd("worldborder set 400"); cmd("worldborder center 10 10"); cmd("worldborder set 300 3"); cmd("worldborder warning distance 7"); cmd("worldborder warning time 9"); cmd("worldborder damage amount 1"); pump(800); cmd("worldborder center 0 0"); cmd("worldborder set 60000000"); pump(500);   // blocks outside the border cannot be used
    cmd("effect mcscon speed 20 2"); cmd("effect mcscon clear"); cmd("effect mcscon night_vision 20 0 true"); pump(500);
    cmd("execute mcscon ~ ~ ~ particle heart ~ ~1 ~ 0 0 0 1 5"); cmd("execute mcscon ~ ~ ~ particle blockcrack ~ ~1 ~ 0 0 0 1 5 normal @a 1");
    cmd("execute mcscon ~ ~ ~ particle iconcrack ~ ~1 ~ 0 0 0 1 5 normal @a 264"); cmd("execute mcscon ~ ~ ~ particle fallingdust ~ ~1 ~ 0 0 0 1 5 normal @a 3");
    cmd("playsound entity.pig.ambient master @a"); cmd("playsound custom.sound.missing master @a"); pump(600);
    cmd("weather rain 100"); cmd("weather clear"); cmd("time set 6000"); cmd("xp 7 mcscon"); cmd("xp 30L mcscon"); cmd("xp -30L mcscon"); pump(600);

    // ---- entities
    const char* pig = N("minecraft:pig", "Pig"); const char* horse = N("minecraft:horse", "Horse"); const char* zombie = N("minecraft:zombie", "Zombie");
    cmd("execute mcscon ~ ~ ~ summon %s ~4 ~ ~", N("minecraft:wither", "WitherBoss"));
    cmd("execute mcscon ~ ~ ~ summon %s ~-4 ~ ~ {ignited:1,Fuse:20}", N("minecraft:creeper", "Creeper"));
    cmd("execute mcscon ~ ~ ~ summon %s ~ ~ ~4 {NoAI:1,ArmorItems:[{},{},{},{id:\"minecraft:iron_helmet\",Count:1b}],HandItems:[{id:\"minecraft:iron_sword\",Count:1b},{}]}", zombie);
    cmd("execute mcscon ~ ~ ~ summon %s ~ ~ ~-4 {Tame:1,SaddleItem:{id:\"minecraft:saddle\",Count:1b},Passengers:[{id:\"%s\"}]}", horse, pig);
    cmd("execute mcscon ~ ~ ~ summon %s ~2 ~ ~2", N("minecraft:armor_stand", "ArmorStand")); cmd("execute mcscon ~ ~ ~ summon %s ~2 ~ ~-2", N("minecraft:xp_orb", "XPOrb"));
    cmd("execute mcscon ~ ~ ~ summon %s ~-6 ~ ~", N("minecraft:lightning_bolt", "LightningBolt")); cmd("execute mcscon ~ ~ ~ summon %s ~5 ~ ~5", N("minecraft:villager", "Villager"));
    cmd("execute mcscon ~ ~ ~ summon %s ~-2 ~ ~3", N("minecraft:boat", "Boat")); cmd("execute mcscon ~ ~ ~ summon %s ~-5 ~ ~-5", N("minecraft:arrow", "Arrow"));
    cmd("execute mcscon ~ ~ ~ summon %s ~1 ~ ~-6 {Item:{id:\"minecraft:apple\",Count:3b},PickupDelay:0}", N("minecraft:item", "Item"));
    cmd("setblock %d %d %d minecraft:fence", ax + 8, ay, az); cmd("execute mcscon ~ ~ ~ summon %s ~8 ~ ~1 {Leashed:1b,Leash:{X:%d,Y:%d,Z:%d}}", pig, ax + 8, ay, az);
    pump(4000);
    printf("INFO entities tracked by A: %u (dropped %u)\n", a.entities().count(), a.entities().dropped());

    // ---- second player: movement, sneaking, swinging, held items, digging, chat
    cmd("give mcscon2 minecraft:diamond_sword 1"); pump(500);
    int sw = b.inventory().find(276); if (sw >= 36) b.selectHotbar((u8)(sw - 36));
    b.controls.forward = true; b.controls.sprint = true; pump(1200); b.controls.sprint = false; b.controls.sneak = true; pump(800); b.controls.sneak = false; b.controls.forward = false;
    pkt::swingArm(b.client()); b.setRotation(90, 20); pump(400); b.setRotation(0, 0); b.say("hello from the second bot"); pump(400);
    cmd("gamemode 0 mcscon2"); pump(700);
    { BlockPos t = {(i32)floor(b.body().x), 3, (i32)floor(b.body().z)}; cmd("setblock %d %d %d minecraft:stone", t.x, t.y, t.z); pump(400);
      pkt::swingArm(b.client()); pkt::blockDig(b.client(), 0, t, 1); for (int i = 0; i < 6; ++i) { pkt::swingArm(b.client()); pump(300); } pkt::blockDig(b.client(), 1, t, 1); }
    cmd("gamemode 1 mcscon2"); pump(300);

    // ---- chat variants
    cmd("tellraw mcscon {\"text\":\"raw message\"}"); cmd("say server message"); pump(400);
    cmd("execute mcscon ~ ~ ~ playsound custom.sound.missing master mcscon ~ ~ ~ 1 1 0"); cmd("execute mcscon ~ ~ ~ playsound entity.pig.ambient master mcscon ~ ~ ~ 1 1 0"); pump(500);

    // ---- vehicle: mount a fresh boat, then move "illegally" so the server corrects with VehicleMove
    cmd("%s", killAll); pump(800);
    cmd("execute mcscon ~ ~ ~ summon %s ~2 ~ ~", N("minecraft:boat", "Boat")); pump(1200);
    a.entities().forEach([&](const Entity& e) { if (e.kind == EntityKind::Object && e.type == 1) pkt::interactEntity(a.client(), e.id); }); pump(1200);
    { sb::VehicleMove vm = {}; vm.x = a.body().x + 40; vm.y = a.body().y; vm.z = a.body().z; vm.yaw = 0; vm.pitch = 0; pkt::send(a.client(), vm); pump(1000); }
    pkt::entityAction(a.client(), a.state().entityId, 1);
    // ---- clean the area (hostile mobs would block sleeping), keep players; stand at the origin again
    cmd("%s", killAll); pump(800); cmd("tp mcscon %d %d %d", ax, ay, az); pump(1500);

    if (atLeast(Version::V1_12)) { cmd("recipe give mcscon *"); pump(500); }
    // ---- container windows next to the player (adjacent block, face west)
    struct Blk { const char* name; } blocks[] = {{"crafting_table"}, {"furnace"}, {"enchanting_table"}, {"anvil"}, {"hopper"}, {"dispenser"}, {"brewing_stand"}, {"chest"}};
    BlockPos tp = {ax + 1, ay, az}; int windows = 0;
    for (auto& k : blocks) {
        cmd("setblock %d %d %d minecraft:%s", tp.x, tp.y, tp.z, k.name);
        if (!strcmp(k.name, "furnace")) { cmd("replaceitem block %d %d %d slot.container.0 minecraft:iron_ore 5", tp.x, tp.y, tp.z); cmd("replaceitem block %d %d %d slot.container.1 minecraft:coal 5", tp.x, tp.y, tp.z); }
        if (!strcmp(k.name, "chest")) cmd("replaceitem block %d %d %d slot.container.0 minecraft:apple 3", tp.x, tp.y, tp.z);
        if (!strcmp(k.name, "crafting_table")) { cmd("gamemode 0 mcscon"); pump(600); }   // ghost recipes are only sent to non-creative players
        pump(700); a.setRotation(270, 0); a.placeBlock(tp, 4); pump(900);
        if (a.inventory().openWindow()) {
            ++windows; printf("INFO window %s type=%s count=%d\n", k.name, a.inventory().openWindow()->type, a.inventory().openWindow()->count);
            if (!strcmp(k.name, "furnace")) pump(2500);                                   // burning -> CraftProgressBar
            if (!strcmp(k.name, "crafting_table") && atLeast(Version::V1_12_1)) { sb::CraftRecipeRequest r = {}; r.windowId = (i8)a.inventory().openWindow()->id; for (int rid = 0; rid < 40; ++rid) { r.recipe = rid; pkt::send(a.client(), r); pump(60); } pump(600); }   // unknown/locked ids are ignored; a valid one yields the ghost recipe if (!strcmp(k.name, "crafting_table")) { cmd("gamemode 1 mcscon"); pump(500); }
            if (!strcmp(k.name, "chest")) { a.inventory().shiftClick(a.client(), 0); pump(800); cmd("setblock %d %d %d minecraft:air", tp.x, tp.y, tp.z); pump(800); }   // server-side close
            else { a.inventory().closeWindow(a.client()); pump(300); }
        }
    }
    printf("INFO windows opened: %d/8\n", windows);
    cmd("setblock %d %d %d minecraft:air", tp.x, tp.y, tp.z); pump(300);
    // villager trade window and horse inventory
    cmd("execute mcscon ~ ~ ~ summon %s ~2 ~ ~", N("minecraft:villager", "Villager")); cmd("execute mcscon ~ ~ ~ summon %s ~-2 ~ ~ {Tame:1,SaddleItem:{id:\"minecraft:saddle\",Count:1b}}", N("minecraft:horse", "Horse")); pump(1500);
    a.entities().forEach([&](const Entity& e) { if (e.kind == EntityKind::Mob && e.type == 120 && !a.inventory().openWindow()) pkt::interactEntity(a.client(), e.id); }); pump(1000);
    if (a.inventory().openWindow()) { printf("INFO villager window %s\n", a.inventory().openWindow()->type); a.inventory().closeWindow(a.client()); pump(300); }
    a.entities().forEach([&](const Entity& e) { if (e.kind == EntityKind::Mob && e.type == 100 && !a.inventory().openWindow()) pkt::interactEntity(a.client(), e.id); }); pump(1000);
    pkt::entityAction(a.client(), a.state().entityId, 7); pump(1000);
    if (a.inventory().openWindow()) { printf("INFO horse window %s entity=%d\n", a.inventory().openWindow()->type, a.inventory().openWindow()->entityId); a.inventory().closeWindow(a.client()); pump(300); }
    pkt::entityAction(a.client(), a.state().entityId, 1); cmd("%s", killAll); pump(800);

    // ---- items: sign placement (open sign editor), painting on a wall, map, cooldown, stats, tab complete
    cmd("give mcscon minecraft:sign 1"); cmd("give mcscon minecraft:painting 1"); cmd("give mcscon minecraft:map 1"); cmd("give mcscon minecraft:ender_pearl 1"); pump(700);
    int sl = a.inventory().find(323); printf("INFO sign slot %d\n", sl); if (sl >= 36) { a.selectHotbar((u8)(sl - 36)); a.setRotation(0, 0); a.placeBlock({ax + 1, ay - 1, az}, 1); pump(900); pkt::closeWindow(a.client(), 0); }
    cmd("fill %d %d %d %d %d %d minecraft:stone", ax - 4, ay - 1, az - 3, ax - 4, ay + 3, az + 3); pump(600);
    sl = a.inventory().find(321); printf("INFO painting slot %d\n", sl); if (sl >= 36) { a.selectHotbar((u8)(sl - 36)); a.setRotation(90, 0); a.placeBlock({ax - 4, ay + 1, az}, 5); pump(1000); }
    sl = a.inventory().find(395); if (sl >= 36) { a.selectHotbar((u8)(sl - 36)); a.useHeldItem(); pump(1200); }
    sl = a.inventory().find(368); if (sl >= 36) { a.selectHotbar((u8)(sl - 36)); a.useHeldItem(); pump(600); }
    pkt::requestStats(a.client()); { sb::TabComplete t = {}; t.text = StrView{"/gam", 4}; pkt::send(a.client(), t); } pump(900);
    cmd("execute mcscon ~ ~ ~ setblock ~ ~ ~7 minecraft:stone"); cmd("execute mcscon ~ ~ ~ setblock ~ ~ ~7 minecraft:air 0 destroy"); pump(500);
    // piston (block action)
    cmd("setblock %d %d %d minecraft:piston 1", ax - 3, ay, az + 6); cmd("setblock %d %d %d minecraft:redstone_block", ax - 3, ay - 1, az + 6); pump(900);
    // bed: other player sleeps at night (use-bed)
    cmd("time set 14000"); cmd("setblock %d %d %d minecraft:bed 0", ax + 3, ay, az + 4); cmd("setblock %d %d %d minecraft:bed 8", ax + 3, ay, az + 5); cmd("tp mcscon2 %d %d %d", ax + 3, ay, az + 3); pump(1500);
    b.placeBlock({ax + 3, ay, az + 4}, 1); pump(1500); if (A.seen[(int)ids::PlayCb::Bed]) pkt::entityAction(b.client(), b.state().entityId, 2); else printf("INFO bed: B did not fall asleep\n"); cmd("time set 6000"); pump(600);
    if (atLeast(Version::V1_12)) {
        cmd("advancement grant mcscon everything"); pump(1500);
        { sb::AdvancementTab t = {}; t.action = 0; t.tabId = StrView{"minecraft:story/root", 20}; pkt::send(a.client(), t); } pump(800);
        cmd("advancement revoke mcscon everything"); cmd("recipe take mcscon *"); pump(800);
    }
    // spectator: attach the camera to the other player
    cmd("tp mcscon mcscon2"); pump(1200); cmd("gamemode 3 mcscon"); pump(900); { int pl = 0; a.entities().forEach([&](const Entity& e2) { if (e2.kind == EntityKind::Player) ++pl; }); printf("INFO spectator: A tracks %d players, gm=%d A=(%.1f,%.1f,%.1f) B=(%.1f,%.1f,%.1f)\n", pl, a.state().gameMode, a.body().x, a.body().y, a.body().z, b.body().x, b.body().y, b.body().z); }
    { a.entities().forEach([&](const Entity& e2) { if (e2.kind == EntityKind::Player) pkt::attackEntity(a.client(), e2.id); }); } pump(1200); cmd("gamemode 1 mcscon"); pump(600);   // spectator left-click attaches the camera

    // ---- far teleport (chunk unload/load), death, kick
    cmd("tp mcscon %d %d %d", ax + 400, ay, az + 400); pump(2500); cmd("tp mcscon %d %d %d", ax, ay, az); pump(2000);
    cmd("kill mcscon"); pump(2500);
    cmd("kick mcscon2 bye"); pump(1500);

    // ---- report
    int bad = A.bad + B.bad; static bool any[256];
    if (B.kicks != 1) { printf("FAIL bot B was dropped unexpectedly (kicks=%d, state=%d)\n", B.kicks, (int)B.s->client().state()); ++bad; }
    if (A.s->client().state() != State::Play) { printf("FAIL bot A was dropped unexpectedly (state=%d)\n", (int)A.s->client().state()); ++bad; }
    for (int i = 0; i < (int)ids::PlayCb::Count_; ++i) any[i] = A.seen[i] || B.seen[i];
    int n = 0; for (int i = 0; i < (int)ids::PlayCb::Count_; ++i) n += any[i];
    // packets that do not exist in this version
    printf("COVERAGE %d/%d clientbound packet types decoded OK (A=%d packets, B=%d packets, decode failures=%d, kicks B=%d)\n", n, (int)ids::PlayCb::Count_, A.total, B.total, bad, B.kicks);
    const ids::Tables& tb = ids::PlayCb_tables[tableIndex(g_ver)]; (void)tb;
    printf("MISSING:"); for (int i = 0; i < (int)ids::PlayCb::Count_; ++i) { bool exists = false; for (int w = 0; w < tb.w2aLen; ++w) if (tb.w2a[w] == i) exists = true; if (exists && !any[i]) printf(" %s", ids::PlayCb_names[i]); } printf("\n");
    fflush(stdout);
    printf("INFO max gap between poll loops: %u ms, keepalives A=%d B=%d, B state=%d\n", g_maxGap, g_ka[0], g_ka[1], (int)B.s->client().state());
    A.s->disconnect(); B.s->disconnect();
    return bad ? 1 : 0;
}
