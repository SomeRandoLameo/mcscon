#pragma once
#include <mcscon/world.h>
#include <mcscon/packets.h>
#include <vector>
namespace mc_test {
using namespace mc;
typedef std::vector<u8> Buf;
// ---- build wire sections
static void be64(Buf& b, u64 v) { for (int i = 7; i >= 0; --i) b.push_back((u8)(v >> (i * 8))); }
static void varint(Buf& b, i32 v) { u8 t[5]; Writer w(t, 5); w.varint(v); b.insert(b.end(), t, w.p); }
template<class F> static void section(Buf& b, int bpb, const std::vector<u16>& pal, F value, bool sky) {
    b.push_back((u8)bpb); varint(b, (i32)pal.size()); for (u16 p : pal) varint(b, p);
    std::vector<u64> d(64 * bpb, 0);
    for (int i = 0; i < 4096; ++i) {
        u64 v = value(i); int bit = i * bpb, w = bit / 64, off = bit % 64;
        d[w] |= v << off; if (off + bpb > 64) d[w + 1] |= v >> (64 - off);
    }
    varint(b, (i32)d.size()); for (u64 x : d) be64(b, x);
    for (int i = 0; i < 2048; ++i) b.push_back(0x55);          // block light
    if (sky) for (int i = 0; i < 2048; ++i) b.push_back(0xFF); // sky light
}
static inline u16 refState(int sec, int i) { return (u16)(((sec * 7 + i * 31) % 90) << 4 | (i % 16)); }

static Buf mapChunk(i32 cx, i32 cz, bool groundUp, u32 mask, const Buf& data) {
    Buf p; varint(p, 0x20); u8 t[16]; Writer w(t, 16); w.i32_(cx); w.i32_(cz); w.bool_(groundUp); p.insert(p.end(), t, w.p);
    varint(p, mask); varint(p, (i32)data.size()); p.insert(p.end(), data.begin(), data.end()); varint(p, 0); return p;
}
struct Feed {
    Version v = Version::V1_12_2;
    bool operator()(World& w, const Buf& pkt, ids::PlayCb id) {
        Packet p; p.state = State::Play; p.id = (u8)id; p.wireId = pkt[0]; p.version = v; p.r = Reader(pkt.data() + 1, pkt.size() - 1); return w.handle(p);
    }
};

}
using namespace mc_test;
