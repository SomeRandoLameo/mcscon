#include <mcscon/client.h>
#include <mcscon/zlib.h>
#include <mcscon/packets.h>
#include "check.h"
#include "zlib_vectors.h"
#include <stdlib.h>
#include <vector>
#include <string>
using namespace mc;
typedef std::vector<u8> Buf;

// ---- scripted in-memory "server": test pushes bytes to `in`, client output lands in `out`.
struct Pipe {
    Buf in, out; size_t inPos = 0; size_t chunk = 1 << 20; bool connected = false, closed = false;
    static bool connect(void* c, const char*, u16) { ((Pipe*)c)->connected = true; return true; }
    static int recv(void* c, u8* b, size_t cap) {
        Pipe* p = (Pipe*)c; if (p->closed) return -1;
        size_t n = p->in.size() - p->inPos; if (n > cap) n = cap; if (n > p->chunk) n = p->chunk;
        memcpy(b, p->in.data() + p->inPos, n); p->inPos += n; return (int)n;
    }
    static int send(void* c, const u8* b, size_t n) { Pipe* p = (Pipe*)c; p->out.insert(p->out.end(), b, b + n); return (int)n; }
    static void close(void* c) { ((Pipe*)c)->closed = true; }
    Transport t() { Transport x = {this, connect, recv, send, close}; return x; }
};

static void putVar(Buf& b, i32 v) { u8 t[5]; Writer w(t, 5); w.varint(v); b.insert(b.end(), t, w.p); }
// frame = [len][payload]; with compression: [len][dataLen][payload or zlib(payload)]
static Buf frame(const Buf& payload, i32 threshold, bool deflateIt = false) {
    Buf f;
    if (threshold < 0) { putVar(f, (i32)payload.size()); f.insert(f.end(), payload.begin(), payload.end()); return f; }
    Buf inner;
    if (!deflateIt) { putVar(inner, 0); inner.insert(inner.end(), payload.begin(), payload.end()); }
    else {   // build a zlib "stored" stream: legal and needs no compressor
        putVar(inner, (i32)payload.size());
        inner.push_back(0x78); inner.push_back(0x01);
        size_t pos = 0;
        do {
            size_t n = payload.size() - pos; if (n > 65535) n = 65535;
            bool last = pos + n == payload.size();
            inner.push_back(last ? 1 : 0); inner.push_back(n & 0xFF); inner.push_back(n >> 8);
            inner.push_back(~n & 0xFF); inner.push_back((~n >> 8) & 0xFF);
            inner.insert(inner.end(), payload.begin() + pos, payload.begin() + pos + n); pos += n;
        } while (pos < payload.size());
        inner.insert(inner.end(), 4, 0);   // adler not verified by client
    }
    putVar(f, (i32)inner.size()); f.insert(f.end(), inner.begin(), inner.end()); return f;
}
static Buf mk(i32 id, std::initializer_list<u8> body = {}) { Buf b; putVar(b, id); b.insert(b.end(), body); return b; }
static Buf withStr(i32 id, const std::string& s) { Buf b; putVar(b, id); putVar(b, (i32)s.size()); b.insert(b.end(), s.begin(), s.end()); return b; }

struct Rec { std::vector<std::pair<State, int>> pkts; std::vector<Error> errs; std::string lastStr; std::vector<Buf> bodies; };
static void onPkt(void* u, const Packet& p) {
    Rec* r = (Rec*)u; r->pkts.push_back({p.state, p.id});
    r->bodies.push_back(Buf(p.r.p, p.r.end));
}
static void onErr(void* u, Error e, i32) { /* rec via global */ ((Rec*)u)->errs.push_back(e); }
static Rec* g_rec;
static void onErrG(void*, Error e, i32) { g_rec->errs.push_back(e); }

struct Rig {
    Pipe pipe; Rec rec; std::vector<u8> rx, sc, tx; Client* c;
    Rig(Version v, size_t rxCap = 4096, size_t scCap = 4096) : rx(rxCap), sc(scCap), tx(1024) {
        Buffers b = {rx.data(), rx.size(), sc.data(), sc.size(), tx.data(), tx.size()};
        c = new Client(pipe.t(), b, v); c->onPacket(onPkt, &rec); g_rec = &rec; c->onError(onErrG);
    }
    ~Rig() { delete c; }
    int pumpAll() { int n = 0, r; do { r = c->poll(); if (r > 0) n += r; } while (r > 0 || pipe.inPos < pipe.in.size()); return r < 0 ? -1 : n; }
};

static Buf cat(Buf a, const Buf& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

static void test_handshake_bytes() {
    Rig r(Version::V1_12_2); CHECK(r.c->connectLogin("localhost", 25565, "Bot"));
    // len, id 0, proto 340 (0xd4 0x02), host, port, next=2 ; then login start
    Buf expect = {0x10, 0x00, 0xd4, 0x02, 9, 'l','o','c','a','l','h','o','s','t', 0x63, 0xdd, 0x02, 0x05, 0x00, 3, 'B','o','t'};
    CHECK(r.pipe.out == expect);
    CHECK(r.c->state() == State::Login);
}
static void test_status() {
    Rig r(Version::V1_10); r.c->connectStatus("h", 1);
    CHECK((r.pipe.out == Buf{0x08, 0x00, 0xd2, 0x01, 1, 'h', 0, 1, 1, 0x01, 0x00}));
    r.pipe.in = frame(withStr(0x00, "{\"x\":1}"), -1);
    CHECK(r.pumpAll() == 1);
    CHECK(r.rec.pkts[0].first == State::Status && r.rec.pkts[0].second == (int)ids::StatusCb::ServerInfo);
    r.c->sendStatusPing(42);
}
static void loginAndPlay(Rig& r, i32 threshold, bool deflate) {
    r.c->connectLogin("h", 25565, "Bot");
    Buf in;
    if (threshold >= 0) { Buf p = mk(0x03); putVar(p, threshold); in = frame(p, -1); }   // set compression (uncompressed frame)
    Buf succ = withStr(0x02, "11111111-2222-3333-4444-555555555555"); putVar(succ, 3); succ.insert(succ.end(), {'B','o','t'});
    in = cat(in, frame(succ, threshold));
    // play: keepalive (1.12.2: i64)
    in = cat(in, frame(mk(0x1F, {0,0,0,0,0,0,1,2}), threshold, deflate));
    // play: chat
    in = cat(in, frame(withStr(0x0F, "{\"text\":\"hi\"}"), threshold, deflate));
    r.pipe.in = in;
}
static void test_login_plain() {
    Rig r(Version::V1_12_2); loginAndPlay(r, -1, false); r.pipe.out.clear();
    CHECK(r.pumpAll() == 3);
    CHECK(r.c->state() == State::Play);
    CHECK(r.rec.pkts[0].second == (int)ids::LoginCb::Success);
    CHECK(r.rec.pkts[1].second == (int)ids::PlayCb::KeepAlive);
    CHECK(r.rec.pkts[2].second == (int)ids::PlayCb::Chat);
    CHECK(r.rec.errs.empty());
    CHECK((r.pipe.out == Buf{9, 0x0B, 0,0,0,0,0,0,1,2}));   // auto keepalive echo, serverbound id 0x0B
}
static void test_login_compressed() {
    for (bool deflate : {false, true}) {
        Rig r(Version::V1_12_2); loginAndPlay(r, 256, deflate); r.pipe.out.clear();
        CHECK(r.pumpAll() == 4);
        CHECK(r.c->compressionThreshold() == 256);
        CHECK(r.rec.pkts.size() == 4 && r.rec.pkts[2].second == (int)ids::PlayCb::KeepAlive);
        CHECK((r.pipe.out == Buf{10, 0, 0x0B, 0,0,0,0,0,0,1,2}));   // compressed framing: len, dataLen=0
        CHECK(r.rec.errs.empty());
    }
}
static void test_version_ids() {
    // 1.10: keepalive is varint, wire id 0x1F; serverbound keepalive 0x0B
    Rig r(Version::V1_10); r.c->connectLogin("h", 1, "B"); r.pipe.out.clear();
    Buf succ = withStr(0x02, "u"); putVar(succ, 1); succ.push_back('B');
    r.pipe.in = cat(frame(succ, -1), frame(mk(0x1F, {0x05}), -1));
    r.pumpAll();
    CHECK(r.rec.pkts.size() == 2 && r.rec.pkts[1].second == (int)ids::PlayCb::KeepAlive);
    CHECK((r.pipe.out == Buf{2, 0x0B, 0x05}));
    // 1.12.2 PlayCb 0x1F is KeepAlive but 1.11 position 0x2E is PlayerInfo
    Writer w = r.c->begin(ids::PlaySb::Chat); w.str("hi"); CHECK(r.c->commit(w));
    // 1.10 serverbound Chat = 0x02
    CHECK(r.pipe.out.size() == 8 && r.pipe.out[4] == 0x02);
}
static void test_chunked_reads() {
    for (size_t chunk : {1, 2, 3, 7, 64}) {
        Rig r(Version::V1_12_2); loginAndPlay(r, 256, true); r.pipe.chunk = chunk; r.pipe.out.clear();
        CHECK(r.pumpAll() == 4);
        CHECK(r.rec.errs.empty());
    }
}
static void test_large_and_skip() {
    // an uncompressed 3000-byte packet with rx window of 1024: must be skipped, next packet still parsed
    Rig r(Version::V1_12_2, 1024, 1024); r.c->connectLogin("h", 1, "B"); r.pipe.out.clear();
    Buf succ = withStr(0x02, "u"); putVar(succ, 1); succ.push_back('B');
    Buf big = mk(0x20); big.insert(big.end(), 3000, 0xAB);                // chunk data
    r.pipe.in = cat(cat(frame(succ, -1), frame(big, -1)), frame(withStr(0x0F, "x"), -1));
    r.pipe.chunk = 500;
    CHECK(r.pumpAll() == 2);
    CHECK(r.rec.errs.size() == 1 && r.rec.errs[0] == Error::PacketTooLarge);
    CHECK(r.rec.pkts.back().second == (int)ids::PlayCb::Chat);
    // exactly-fitting frame (rx 1024: 3 hdr + 1021) passes
    Rig r2(Version::V1_12_2, 1024, 1024); r2.c->connectLogin("h", 1, "B");
    r2.pipe.in = cat(frame(succ, -1), frame([] { Buf b = mk(0x20); b.insert(b.end(), 1018, 1); return b; }(), -1));
    CHECK(r2.pumpAll() == 2 && r2.rec.errs.empty());
    // compressed big packet using a stored stream; scratch big enough
    Rig r3(Version::V1_12_2, 8192, 8192); loginAndPlay(r3, 256, false); r3.pipe.in = r3.pipe.in; 
    Buf b2 = mk(0x20); b2.insert(b2.end(), 5000, 7);
    r3.pipe.in = cat(r3.pipe.in, frame(b2, 256, true));
    CHECK(r3.pumpAll() == 5 && r3.rec.errs.empty() && r3.rec.bodies.back().size() == 5000);
    // dataLen > scratch: error, stream continues
    Rig r4(Version::V1_12_2, 8192, 1024); loginAndPlay(r4, 256, false); r4.pipe.in = cat(r4.pipe.in, frame(b2, 256, true));
    r4.pipe.in = cat(r4.pipe.in, frame(withStr(0x0F, "x"), 256));
    r4.pumpAll(); CHECK(r4.rec.errs.size() == 1 && r4.rec.errs[0] == Error::PacketTooLarge && r4.rec.pkts.back().second == (int)ids::PlayCb::Chat);
}
static void test_disconnect_and_errors() {
    Rig r(Version::V1_12_2); r.c->connectLogin("h", 1, "B");
    r.pipe.in = frame(withStr(0x00, "{\"text\":\"banned\"}"), -1);   // login disconnect
    r.pumpAll(); CHECK(r.rec.errs.size() == 1 && r.rec.errs[0] == Error::Disconnected);
    Rig e(Version::V1_12_2); e.c->connectLogin("h", 1, "B");
    e.pipe.in = frame(withStr(0x01, "serverid"), -1);                // encryption request
    e.pumpAll(); CHECK(e.rec.errs.size() == 1 && e.rec.errs[0] == Error::EncryptionRequired);
    Rig g(Version::V1_12_2); g.c->connectLogin("h", 1, "B");
    g.pipe.in = {0x00}; CHECK(g.c->poll() == -1); CHECK(g.c->state() == State::Closed);   // zero-length frame
    Rig z(Version::V1_12_2); z.c->connectLogin("h", 1, "B"); z.pipe.closed = true; CHECK(z.c->poll() == -1);
    // corrupt compressed stream
    Rig c(Version::V1_12_2); loginAndPlay(c, 256, false); Buf bad = {0x09, 0x20, 0x78, 0x01, 1, 2, 3, 4, 5, 6};
    c.pipe.in = cat(c.pipe.in, bad); c.pumpAll(); CHECK(!c.rec.errs.empty() && c.rec.errs.back() == Error::Inflate);
}

static void test_typed() {
    Rig r(Version::V1_12_2); r.c->connectLogin("h", 1, "B");
    Buf succ = withStr(0x02, "u"); putVar(succ, 1); succ.push_back('B');
    Buf join = mk(0x23, {0,0,0,5, 1, 0,0,0,0, 2, 20}); putVar(join, 7); join.insert(join.end(), {'d','e','f','a','u','l','t'}); join.push_back(0);
    Buf pos = mk(0x2F); { u8 t[64]; Writer w(t, 64); w.f64_(1.5); w.f64_(64); w.f64_(-3); w.f32_(90); w.f32_(10); w.u8_(0); w.varint(300); pos.insert(pos.end(), t, w.p); }
    r.pipe.in = cat(cat(frame(succ, -1), frame(join, -1)), frame(pos, -1)); r.pumpAll();
    CHECK(r.rec.pkts.size() == 3);
    Reader jr(r.rec.bodies[1].data(), r.rec.bodies[1].size()); cb::Login j; CHECK(cb::decode(jr, j, Version::V1_12_2));
    CHECK(j.entityId == 5 && j.gameMode == 1 && j.dimension == 0 && j.difficulty == 2 && j.maxPlayers == 20 && j.levelType.len == 7 && !j.reducedDebugInfo);
    Reader pr(r.rec.bodies[2].data(), r.rec.bodies[2].size()); cb::Position p; CHECK(cb::decode(pr, p, Version::V1_12_2));
    CHECK(p.x == 1.5 && p.y == 64 && p.z == -3 && p.yaw == 90 && p.teleportId == 300);
    Reader bad(r.rec.bodies[2].data(), 10); CHECK(!cb::decode(bad, p, Version::V1_12_2));   // truncated
    r.pipe.out.clear();
    CHECK(pkt::teleportConfirm(*r.c, 300)); CHECK((r.pipe.out == Buf{3, 0x00, 0xAC, 0x02}));
    r.pipe.out.clear(); pkt::chat(*r.c, "hi"); CHECK((r.pipe.out == Buf{4, 0x02, 2, 'h', 'i'}));
    r.pipe.out.clear(); pkt::respawn(*r.c);
    CHECK((r.pipe.out == Buf{2, 0x03, 0x00}));
    r.pipe.out.clear(); pkt::positionLook(*r.c, 0, 0, 0, 0, 0, true); CHECK(r.pipe.out.size() == 1 + 1 + 8 * 3 + 8 + 1 && r.pipe.out[1] == 0x0E);
    r.pipe.out.clear(); pkt::blockPlace(*r.c, {1, 2, 3}, 1, 0, .5f, .5f, .5f); CHECK(r.pipe.out.size() == 1 + 1 + 8 + 1 + 1 + 12 && r.pipe.out[1] == 0x1F);
    Rig o(Version::V1_10); o.c->connectLogin("h", 1, "B"); o.pipe.in = frame(([] { Buf s = withStr(0x02, "u"); putVar(s, 1); s.push_back('B'); return s; })(), -1); o.pumpAll(); o.pipe.out.clear();
    pkt::blockPlace(*o.c, {1, 2, 3}, 1, 0, .5f, .5f, .5f); CHECK(o.pipe.out.size() == 1 + 1 + 8 + 1 + 1 + 3 && o.pipe.out[1] == 0x1C);   // 1.10 wire id + byte cursor
}
static void test_fuzz() {
    srand(3);
    for (int i = 0; i < 3000; ++i) {
        Rig r(Version::V1_12_2, 256, 512); r.c->connectLogin("h", 1, "B");
        size_t n = 1 + rand() % 400; r.pipe.in.resize(n); for (auto& b : r.pipe.in) b = (u8)rand();
        if (rand() & 1) r.pipe.in[0] = 0x02; r.pipe.chunk = 1 + rand() % 50;
        for (int k = 0; k < 400; ++k) if (r.c->poll() < 0) break;
    }
    CHECK(true);
}
int main() {
    RUN(test_handshake_bytes); RUN(test_status); RUN(test_login_plain); RUN(test_login_compressed);
    RUN(test_version_ids); RUN(test_chunked_reads); RUN(test_large_and_skip); RUN(test_disconnect_and_errors); RUN(test_typed); RUN(test_fuzz);
    DONE();
}
