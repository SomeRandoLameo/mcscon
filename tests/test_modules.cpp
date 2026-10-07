// Offline tests for physics, entities, tab list, inventory and the session logic, using synthetic packets.
#include <mcscon/session.h>
#include <mcscon/nbt.h>
#include "check.h"
#include "wire_helpers.h"
#include <math.h>
#include <functional>
using namespace mc;

static void put(Buf& b, const void* d, size_t n) { b.insert(b.end(), (const u8*)d, (const u8*)d + n); }
static Packet mk(const Buf& pkt, ids::PlayCb id, Version v = Version::V1_12_2) {
    Packet p; p.state = State::Play; p.id = (u8)id; p.wireId = pkt[0]; p.version = v; p.r = Reader(pkt.data() + 1, pkt.size() - 1); return p;
}
static Buf be(i32 id, std::function<void(Writer&)> f) { Buf b; varint(b, id); u8 t[1024]; Writer w(t, sizeof t); f(w); put(b, t, w.p - t); return b; }

// ---- flat world helper: bedrock/dirt/grass up to y=3 in columns -1..1
struct FlatWorld {
    alignas(8) u8 mem[512 * 1024]; World w; Feed feed;
    FlatWorld() {
        WorldConfig cfg = {16, false, false}; w.init(mem, sizeof mem, cfg);
        for (int cx = -1; cx <= 1; ++cx) for (int cz = -1; cz <= 1; ++cz) {
            Buf d; section(d, 4, {0, (u16)(7 << 4), (u16)(3 << 4), (u16)(2 << 4)}, [](int i) { int y = i >> 8; return (u64)(y == 0 ? 1 : y < 3 ? 2 : y == 3 ? 3 : 0); }, true);
            for (int i = 0; i < 256; ++i) d.push_back(1);
            feed(w, mapChunk(cx, cz, true, 1, d), ids::PlayCb::MapChunk);
        }
    }
    void block(int x, int y, int z, u16 st) { w.setBlockState(x, y, z, st); }
};

static void test_physics() {
    FlatWorld f; Body b; b.x = 0.5; b.y = 8; b.z = 0.5; Controls in;
    int ticks = 0; while (!b.onGround && ticks < 100) { physicsTick(b, in, f.w); ++ticks; }
    CHECK(b.onGround && fabs(b.y - 4.0) < 1e-9);                       // falls onto the grass surface (y = 4)
    CHECK(ticks > 6 && ticks < 14);
    // chunks not loaded -> no simulation
    Body far; far.x = 500; far.y = 10; far.z = 500; CHECK(!physicsTick(far, in, f.w));
    // vanilla jump apex: 1.2491870787 blocks
    in.jump = true; double peak = 0; physicsTick(b, in, f.w); in.jump = false;
    for (int i = 0; i < 40; ++i) { physicsTick(b, in, f.w); if (b.y - 4.0 > peak) peak = b.y - 4.0; }
    CHECK(fabs(peak - 1.2491870787) < 1e-3); CHECK(b.onGround);
    // walking speed: 4.317 m/s (0.2158/tick steady state), sprint 5.612 m/s
    Body w1; w1.x = 0.5; w1.y = 4; w1.z = 0.5; w1.onGround = true; Controls fwd; fwd.forward = true;
    for (int i = 0; i < 40; ++i) physicsTick(w1, fwd, f.w);
    double v1 = w1.z - 0.5;                                            // yaw 0 walks toward +z
    for (int i = 0; i < 20; ++i) physicsTick(w1, fwd, f.w);
    double per = (w1.z - 0.5 - v1) / 20; CHECK(fabs(per * 20 - 4.317) < 0.05);
    fwd.sprint = true; Body w2; w2.x = 0.5; w2.y = 4; w2.z = 0.5; w2.onGround = true; for (int i = 0; i < 40; ++i) physicsTick(w2, fwd, f.w);
    double s1 = w2.z; for (int i = 0; i < 20; ++i) physicsTick(w2, fwd, f.w); CHECK(fabs((w2.z - s1) - 5.612) < 0.08);
    // wall: stops at the face, step-up over a 1-block ledge is NOT possible, over a slab-height (0.5) is not modelled (full cubes)
    f.block(0, 4, 5, 1 << 4); f.block(0, 5, 5, 1 << 4);
    Body w3; w3.x = 0.5; w3.y = 4; w3.z = 0.5; w3.onGround = true; Controls run; run.forward = true;
    for (int i = 0; i < 60; ++i) physicsTick(w3, run, f.w);
    CHECK(fabs(w3.z - (5 - 0.3)) < 1e-9 && w3.collidedH);
    // single-block-high ledge (1.0 > 0.6 step height): still blocked
    FlatWorld g; g.block(0, 4, 5, 1 << 4);
    Body w4; w4.x = 0.5; w4.y = 4; w4.z = 0.5; w4.onGround = true; for (int i = 0; i < 60; ++i) physicsTick(w4, run, g.w);
    CHECK(fabs(w4.z - 4.7) < 1e-9);
    // jumping onto the ledge works
    Controls jr = run; jr.jump = true; Body w5; w5.x = 0.5; w5.y = 4; w5.z = 3.5; w5.onGround = true;
    bool stood = false; for (int i = 0; i < 40; ++i) { physicsTick(w5, jr, g.w); if (w5.onGround && fabs(w5.y - 5.0) < 1e-9 && w5.z > 5.0) stood = true; }
    CHECK(stood);                                                       // jumped up and landed on the 1-block ledge
    // water: sinks slowly, jump swims up
    FlatWorld wt; for (int y = 4; y < 8; ++y) wt.block(0, y, 0, 9 << 4);
    Body sw; sw.x = 0.5; sw.y = 7.9; sw.z = 0.5; Controls none; for (int i = 0; i < 20; ++i) physicsTick(sw, none, wt.w);
    CHECK(sw.inWater && sw.y < 7.9 && sw.y > 4.0);
    // ladder: climbing holds position when sneaking
    FlatWorld ld; for (int y = 4; y < 12; ++y) { ld.block(1, y, 0, (65 << 4) | 4); ld.block(2, y, 0, 1 << 4); }
    Body cl; cl.x = 1.3; cl.y = 4; cl.z = 0.5; cl.yaw = 270; cl.onGround = true; Controls up; up.forward = true;
    for (int i = 0; i < 40; ++i) physicsTick(cl, up, ld.w);
    CHECK(cl.onLadder && cl.y > 8.0);
    // half slab (bottom, meta 0) is stepped onto without jumping; a fence (1.5 high) is not
    FlatWorld sl; sl.block(0, 4, 5, 44 << 4);                           // stone_slab bottom half
    Body w6; w6.x = 0.5; w6.y = 4; w6.z = 0.5; w6.onGround = true; for (int i = 0; i < 60; ++i) physicsTick(w6, run, sl.w);
    CHECK(w6.z > 5.5 && fabs(w6.y - 4.0) < 0.6);                        // walked over it (stepped up 0.5, then down again)
    FlatWorld fe; for (int x = -1; x <= 1; ++x) fe.block(x, 4, 5, 85 << 4);   // fence line
    Body w7; w7.x = 0.5; w7.y = 4; w7.z = 0.5; w7.onGround = true; for (int i = 0; i < 80; ++i) physicsTick(w7, run, fe.w);
    CHECK(w7.z > 5.0 && w7.z < 5.1 && w7.collidedH);   // stops at the 3/8 post face (z = 5.375 - 0.3), not a full-block face
    FlatWorld cp; cp.block(0, 4, 3, 171 << 4);                           // carpet: 1/16 high, no hindrance
    Body w8; w8.x = 0.5; w8.y = 4; w8.z = 0.5; w8.onGround = true; for (int i = 0; i < 40; ++i) physicsTick(w8, run, cp.w);
    CHECK(w8.z > 6.0);
    // creative flight: no gravity
    Body fl; fl.x = 0.5; fl.y = 10; fl.z = 0.5; for (int i = 0; i < 20; ++i) physicsTick(fl, none, f.w, true);
    CHECK(fabs(fl.y - 10) < 1e-6);
}

// ---- entities / tab list
struct Ev { int n = 0; EntityEvent last; i32 arg = 0; };
static void entEv(void* u, EntityEvent e, const Entity&, i32 arg) { Ev* x = (Ev*)u; ++x->n; x->last = e; x->arg = arg; }
static void test_entities() {
    static Entity store[8]; EntityTracker t; t.init(store, 8); Ev ev; t.onEvent(entEv, &ev);
    // Spawn mob 77: pig, metadata: flags(0)=0x01, health(7)=10.0
    Buf b = be(0x03, [](Writer& w) { w.varint(77); for (int i = 0; i < 16; ++i) w.u8_((u8)i); w.varint(90); w.f64_(10); w.f64_(64); w.f64_(-5); w.u8_(64); w.u8_(32); w.u8_(16); w.u16_(100); w.u16_(0); w.u16_((u16)-200);
        w.u8_(0); w.u8_(0); w.u8_(1); w.u8_(7); w.u8_(2); w.f32_(10.0f); w.u8_(0xFF); });
    CHECK(t.handle(mk(b, ids::PlayCb::SpawnEntityLiving)));
    const Entity* e = t.find(77);
    CHECK(e && e->kind == EntityKind::Mob && e->type == 90 && e->x == 10 && e->y == 64 && e->z == -5 && e->flags == 1 && e->hasHealth && e->health == 10.0f && e->vz == -200 && e->uuid.b[5] == 5);
    CHECK(ev.n == 1 && ev.last == EntityEvent::Spawned);
    // relative move: 1 block in +x (4096 units), 0.5 in -z
    b = be(0x26, [](Writer& w) { w.varint(77); w.u16_(4096); w.u16_(0); w.u16_((u16)-2048); w.bool_(true); });
    t.handle(mk(b, ids::PlayCb::RelEntityMove)); CHECK(e->x == 11 && e->z == -5.5 && e->onGround);
    b = be(0x4C, [](Writer& w) { w.varint(77); w.f64_(1); w.f64_(2); w.f64_(3); w.u8_(128); w.u8_(0); w.bool_(false); });
    t.handle(mk(b, ids::PlayCb::EntityTeleport)); CHECK(e->x == 1 && e->y == 2 && e->z == 3 && fabs(e->yawDeg() - 180) < 1e-3);
    // equipment (slot 5 = head: diamond helmet id 310)
    b = be(0x3F, [](Writer& w) { w.varint(77); w.varint(5); w.u16_(310); w.u8_(1); w.u16_(0); w.u8_(0); });
    t.handle(mk(b, ids::PlayCb::EntityEquipment)); CHECK(e->equipment[5].id == 310 && e->equipment[5].count == 1);
    // status event (death animation 3)
    b = be(0x1B, [](Writer& w) { w.i32_(77); w.u8_(3); }); t.handle(mk(b, ids::PlayCb::EntityStatus)); CHECK(ev.last == EntityEvent::Status && ev.arg == 3);
    // vehicle: passengers
    t.handle(mk(be(0x05, [](Writer& w) { w.varint(80); for (int i = 0; i < 16; ++i) w.u8_(1); w.f64_(0); w.f64_(0); w.f64_(0); w.u8_(0); w.u8_(0); w.u8_(0xFF); }), ids::PlayCb::NamedEntitySpawn));
    CHECK(t.find(80) && t.find(80)->kind == EntityKind::Player);
    t.handle(mk(be(0x43, [](Writer& w) { w.varint(77); w.varint(1); w.varint(80); }), ids::PlayCb::SetPassengers)); CHECK(t.find(80)->vehicle == 77);
    t.handle(mk(be(0x43, [](Writer& w) { w.varint(77); w.varint(0); }), ids::PlayCb::SetPassengers)); CHECK(t.find(80)->vehicle == -1);
    CHECK(t.nearest(1, 2, 3, 5, (int)EntityKind::Mob) == t.find(77) && t.nearest(100, 2, 3, 5) == 0);
    // destroy
    t.handle(mk(be(0x32, [](Writer& w) { w.varint(2); w.varint(77); w.varint(999); }), ids::PlayCb::EntityDestroy));
    CHECK(t.find(77) == 0 && ev.last == EntityEvent::Destroying && t.count() == 1);
    // table full -> counted, no crash
    for (int i = 0; i < 20; ++i) t.handle(mk(be(0x01, [&](Writer& w) { w.varint(1000 + i); w.f64_(0); w.f64_(0); w.f64_(0); w.u16_(1); }), ids::PlayCb::SpawnEntityExperienceOrb));
    CHECK(t.count() == 8 && t.dropped() == 13);
    // malformed packets are consumed without effect
    Buf bad = be(0x26, [](Writer& w) { w.varint(1); }); t.handle(mk(bad, ids::PlayCb::RelEntityMove)); CHECK(true);
}
static void test_playerlist() {
    static PlayerEntry store[4]; PlayerList pl; pl.init(store, 4);
    auto add = [&](u8 id, const char* name, int gm) {
        Buf b = be(0x2E, [&](Writer& w) { w.varint(0); w.varint(1); for (int i = 0; i < 16; ++i) w.u8_(id); w.str(name); w.varint(1); w.str("textures"); w.str("val"); w.bool_(false); w.varint(gm); w.varint(42); w.bool_(false); });
        pl.handle(mk(b, ids::PlayCb::PlayerInfo));
    };
    add(1, "Alice", 0); add(2, "Bob", 1);
    CHECK(pl.count() == 2 && pl.findByName("Bob") && pl.findByName("Bob")->gameMode == 1 && pl.findByName("Alice")->ping == 42);
    Buf lat = be(0x2E, [](Writer& w) { w.varint(2); w.varint(1); for (int i = 0; i < 16; ++i) w.u8_(2); w.varint(150); });
    pl.handle(mk(lat, ids::PlayCb::PlayerInfo)); CHECK(pl.findByName("Bob")->ping == 150);
    Buf rm = be(0x2E, [](Writer& w) { w.varint(4); w.varint(1); for (int i = 0; i < 16; ++i) w.u8_(1); });
    pl.handle(mk(rm, ids::PlayCb::PlayerInfo)); CHECK(pl.count() == 1 && !pl.findByName("Alice"));
}

// ---- inventory with a captured send sink
struct Sink { Buf out; };
static bool sinkConnect(void*, const char*, u16) { return true; }
static int sinkRecv(void*, u8*, size_t) { return 0; }
static int sinkSend(void* c, const u8* b, size_t n) { ((Sink*)c)->out.insert(((Sink*)c)->out.end(), b, b + n); return (int)n; }
static void sinkClose(void*) {}
static void test_inventory() {
    Sink sink; Transport tr = {&sink, sinkConnect, sinkRecv, sinkSend, sinkClose};
    static u8 rx[1024], tx[1024]; Buffers bufs = {rx, sizeof rx, 0, 0, tx, sizeof tx};
    Client c(tr, bufs, Version::V1_12_2); c.connectLogin("h", 1, "b"); sink.out.clear();
    Inventory inv;
    // WindowItems for window 0: 46 slots, diamonds x5 in hotbar slot 38
    Buf wi = be(0x14, [](Writer& w) { w.u8_(0); w.u16_(46); for (int i = 0; i < 46; ++i) { if (i == 38) { w.u16_(264); w.u8_(5); w.u16_(0); w.u8_(0); } else w.u16_(0xFFFF); } });
    CHECK(inv.handle(mk(wi, ids::PlayCb::WindowItems), c));
    CHECK(inv.countOf(264) == 5 && inv.find(264) == 38 && inv.firstEmptySlot() == 36);
    // SetSlot cursor
    Buf cur = be(0x16, [](Writer& w) { w.u8_(0xFF); w.u16_(0xFFFF); w.u16_(1); w.u8_(3); w.u16_(0); w.u8_(0); });
    inv.handle(mk(cur, ids::PlayCb::SetSlot), c); CHECK(inv.cursor().id == 1 && inv.cursor().count == 3);
    // open a chest: 27 slots -> window of 63; the last 36 mirror into the player inventory
    Buf ow = be(0x13, [](Writer& w) { w.u8_(3); w.str("minecraft:chest"); w.str("{\"text\":\"Chest\"}"); w.u8_(27); });
    inv.handle(mk(ow, ids::PlayCb::OpenWindow), c); CHECK(inv.openWindow() && inv.openWindow()->typeIs("minecraft:chest") && inv.openWindow()->count == 63 && !inv.openWindow()->loaded);
    Buf ci = be(0x14, [](Writer& w) { w.u8_(3); w.u16_(63); for (int i = 0; i < 63; ++i) { if (i == 0) { w.u16_(260); w.u8_(3); w.u16_(0); w.u8_(0); } else if (i == 62) { w.u16_(1); w.u8_(1); w.u16_(0); w.u8_(0); } else w.u16_(0xFFFF); } });
    inv.handle(mk(ci, ids::PlayCb::WindowItems), c); CHECK(inv.openWindow()->loaded);
    CHECK(inv.openWindow()->slots[0].id == 260 && inv.player().slots[44].id == 1);      // window slot 62 = last hotbar slot = player slot 44
    CHECK(inv.itemAt(3, 0).id == 260 && inv.itemAt(9, 0).id == -1);
    // unaccepted transaction is acknowledged automatically
    sink.out.clear(); inv.handle(mk(be(0x11, [](Writer& w) { w.u8_(3); w.u16_(7); w.bool_(false); }), ids::PlayCb::Transaction), c);
    CHECK((sink.out == Buf{5, 0x05, 3, 0, 7, 1}));                                       // len 5, id 0x05, windowId, action, accepted
    // click sends WindowClick for window 3 with an "impossible" clicked item to force a resync
    sink.out.clear(); CHECK(inv.shiftClick(c, 0)); CHECK(sink.out.size() > 6 && sink.out[1] == 0x07 && sink.out[2] == 3);
    inv.handle(mk(be(0x12, [](Writer& w) { w.u8_(3); }), ids::PlayCb::CloseWindow), c); CHECK(inv.openWindow() == 0);
    // creative set updates local state immediately
    CHECK(inv.creativeSet(c, 36, 276, 1) && inv.player().slots[36].id == 276);
    CHECK(inv.selectHotbar(c, 8) && !inv.selectHotbar(c, 9) && inv.heldSlot() == 8);
}

// ---- session: join, teleport handling, health/death, chat, resource pack
struct Pipe2 { Buf in, out; size_t pos = 0; };
static bool pConnect(void*, const char*, u16) { return true; }
static int pRecv(void* c, u8* b, size_t cap) { Pipe2* p = (Pipe2*)c; size_t n = p->in.size() - p->pos; if (n > cap) n = cap; memcpy(b, p->in.data() + p->pos, n); p->pos += n; return (int)n; }
static int pSend(void* c, const u8* b, size_t n) { ((Pipe2*)c)->out.insert(((Pipe2*)c)->out.end(), b, b + n); return (int)n; }
static void pClose(void*) {}
static Buf framed(const Buf& payload) { Buf f; varint(f, (i32)payload.size()); put(f, payload.data(), payload.size()); return f; }
static std::vector<int> sentIds(const Buf& out) {      // serverbound wire ids in an uncompressed byte stream
    std::vector<int> r; Reader rd(out.data(), out.size());
    while (rd.left()) { i32 len = rd.varint(); Reader p(rd.p, len); r.push_back(p.varint()); rd.skip(len); }
    return r;
}
struct SEv { int joined = 0, spawned = 0, died = 0, respawned = 0, tp = 0; char chat[128] = {0}; };
static void sEv(void* u, int ev, i32) { SEv* s = (SEv*)u; if (ev == EvJoined) ++s->joined; if (ev == EvSpawned) ++s->spawned; if (ev == EvDied) ++s->died; if (ev == EvRespawned) ++s->respawned; if (ev == EvTeleported) ++s->tp; }
static void sChat(void* u, const char* t, u8, StrView) { snprintf(((SEv*)u)->chat, 128, "%s", t); }
static void test_session() {
    Pipe2 pipe; Transport tr = {&pipe, pConnect, pRecv, pSend, pClose};
    static u8 rx[8192], tx[4096]; Buffers bufs = {rx, sizeof rx, 0, 0, tx, sizeof tx};
    static StaticStorage<9, 64 * 1024, 16, 8> st;
    Session s(tr, bufs, Version::V1_12_2, st.get()); SEv ev; s.onEvent(sEv, &ev); s.onChat(sChat, &ev);
    CHECK(s.connect("h", 1, "bot")); pipe.out.clear();
    Buf succ = be(0x02, [](Writer& w) { w.str("00000000-0000-0000-0000-000000000000"); w.str("bot"); });
    Buf join = be(0x23, [](Writer& w) { w.i32_(12); w.u8_(0); w.i32_(0); w.u8_(0); w.u8_(20); w.str("flat"); w.bool_(false); });
    Buf pos = be(0x2F, [](Writer& w) { w.f64_(1.5); w.f64_(4); w.f64_(2.5); w.f32_(90); w.f32_(10); w.u8_(0); w.varint(7); });
    pipe.in = framed(succ); { Buf t = framed(join); put(pipe.in, t.data(), t.size()); t = framed(pos); put(pipe.in, t.data(), t.size()); }
    while (s.poll() > 0) {}
    CHECK(ev.joined == 1 && ev.spawned == 1 && s.state().joined && s.state().spawned && s.state().entityId == 12 && s.state().gameMode == Survival);
    CHECK(s.body().x == 1.5 && s.body().yaw == 90 && s.body().pitch == 10);
    std::vector<int> ids = sentIds(pipe.out);
    CHECK((ids == std::vector<int>{0x04, 0x09, 0x00, 0x0E}));            // settings, MC|Brand, teleport confirm 7, position+look
    // relative flags: x,y relative (bit0,1), yaw relative (0x10)
    pipe.out.clear(); pipe.in.clear(); pipe.pos = 0;
    pipe.in = framed(be(0x2F, [](Writer& w) { w.f64_(1); w.f64_(1); w.f64_(9); w.f32_(45); w.f32_(0); w.u8_(0x13); w.varint(8); }));
    while (s.poll() > 0) {}
    CHECK(s.body().x == 2.5 && s.body().y == 5 && s.body().z == 9 && s.body().yaw == 135 && s.body().pitch == 0 && ev.tp == 1);
    // chat
    pipe.in = framed(be(0x0F, [](Writer& w) { w.str("{\"translate\":\"chat.type.text\",\"with\":[{\"text\":\"Al\"},\"yo\"]}"); w.u8_(0); })); pipe.pos = 0;
    while (s.poll() > 0) {}
    CHECK(!strcmp(ev.chat, "<Al> yo"));
    // resource pack: accepted then loaded
    pipe.out.clear(); pipe.pos = 0; pipe.in = framed(be(0x34, [](Writer& w) { w.str("http://x/p.zip"); w.str("hash"); }));
    while (s.poll() > 0) {}
    int rpId = ids::PlaySb_tables[3].a2w[(u8)ids::PlaySb::ResourcePackReceive];
    CHECK((sentIds(pipe.out) == std::vector<int>{rpId, rpId}));
    // death -> auto respawn (ClientCommand 0x03)
    pipe.out.clear(); pipe.pos = 0; pipe.in = framed(be(0x41, [](Writer& w) { w.f32_(0); w.varint(20); w.f32_(5); }));
    while (s.poll() > 0) {}
    CHECK(ev.died == 1 && s.state().dead() && (sentIds(pipe.out) == std::vector<int>{0x03}));
    // ticking while dead / without loaded chunks is harmless
    s.update(1000); CHECK(s.update(1100) == 2);
    // say() splits long messages at 256 bytes without cutting UTF-8
    pipe.out.clear(); std::string longMsg(255, 'a'); longMsg += "\xc3\xa4"; longMsg += std::string(10, 'b');
    CHECK(s.say(longMsg.c_str()) && sentIds(pipe.out).size() == 2);
}
static void test_nbt() {
    // {"": {display:{Name:"Sword", Lore:["a","bb"]}, Damage:7s, Count:3b, big:12345678901L, f:1.5f}}
    Buf b; auto name = [&](u8 tag, const char* n) { b.push_back(tag); b.push_back(0); b.push_back((u8)strlen(n)); put(b, n, strlen(n)); };
    auto str = [&](const char* s2) { b.push_back(0); b.push_back((u8)strlen(s2)); put(b, s2, strlen(s2)); };
    b.push_back(10); b.push_back(0); b.push_back(0);
    name(10, "display"); name(8, "Name"); str("Sword"); name(9, "Lore"); b.push_back(8); b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(2); str("a"); str("bb"); b.push_back(0);
    name(2, "Damage"); b.push_back(0); b.push_back(7); name(1, "Count"); b.push_back(3);
    name(4, "big"); { u8 t[8]; Writer w(t, 8); w.i64_(12345678901LL); put(b, t, 8); }
    name(5, "f"); { u8 t[4]; Writer w(t, 4); w.f32_(1.5f); put(b, t, 4); } b.push_back(0);
    Bytes nb = {b.data(), (u32)b.size()}; Reader chk(b.data(), b.size()); Bytes got; CHECK(readNbt(chk, got, false) && chk.left() == 0);
    NbtEntry e;
    CHECK(nbtPath(nb, "display/Name", e) && e.type == NbtString && streq(e.asString(), "Sword"));
    CHECK(nbtPath(nb, "Damage", e) && e.asInt() == 7); CHECK(nbtPath(nb, "Count", e) && e.asInt() == 3);
    CHECK(nbtPath(nb, "big", e) && e.asInt() == 12345678901LL); CHECK(nbtPath(nb, "f", e) && e.asFloat() == 1.5);
    CHECK(!nbtPath(nb, "display/Nope", e) && !nbtPath(nb, "Damage/x", e));
    CHECK(nbtPath(nb, "display/Lore", e)); NbtList l = e.asList(); NbtEntry it; int n = 0; const char* want[] = {"a", "bb"};
    while (l.next(it)) { CHECK(streq(it.asString(), want[n])); ++n; } CHECK(n == 2);
    NbtCompound root; CHECK(nbtRoot(nb, root)); int cnt = 0; while (root.next(e)) ++cnt; CHECK(cnt == 5);
    Bytes empty = {0, 0}; CHECK(!nbtRoot(empty, root));
}
int main() { RUN(test_nbt); RUN(test_physics); RUN(test_entities); RUN(test_playerlist); RUN(test_inventory); RUN(test_session); DONE(); }
