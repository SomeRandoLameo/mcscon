#include <mcscon/world.h>
#include <mcscon/packets.h>
#include "check.h"
#include "wire_helpers.h"
#include <stdlib.h>
#include <vector>
using namespace mc;

static void testArena() {
    alignas(8) static u8 mem[64 * 1024]; Arena a; a.init(mem, sizeof mem);
    struct B { u32 off, n; u8 fill; }; std::vector<B> live; srand(11);
    for (int i = 0; i < 20000; ++i) {
        if (live.empty() || (rand() % 100 < 55)) {
            u32 n = 1 + rand() % 3000; u32 o = a.alloc(n);
            if (o) { CHECK(o % 8 == 0); u8 f = (u8)rand(); memset(a.ptr(o), f, n); live.push_back({o, n, f}); }
        } else {
            size_t k = rand() % live.size(); B b = live[k];
            for (u32 j = 0; j < b.n; ++j) if (a.ptr(b.off)[j] != b.fill) { CHECK(false); break; }   // no overlap / corruption
            a.release(b.off); live.erase(live.begin() + k);
        }
    }
    for (auto& b : live) a.release(b.off);
    CHECK(a.used() == 0);
    CHECK(a.largestFree() >= sizeof mem - 32);                     // fully coalesced again
    CHECK(a.alloc(sizeof mem) == 0);
}

static void testChunkAndEdits() {
    alignas(8) static u8 mem[512 * 1024]; World w; WorldConfig cfg = {16, true, true}; w.init(mem, sizeof mem, cfg); Feed feed;
    Buf data;
    // section 0: bpb 4 w/ 12-entry palette; section 2: bpb 8 w/ 200 entries; section 5: bpb 13 direct
    std::vector<u16> p4; for (int i = 0; i < 12; ++i) p4.push_back(refState(0, i));
    section(data, 4, p4, [&](int i) { return (u64)(i % 12); }, true);
    std::vector<u16> p8; for (int i = 0; i < 200; ++i) p8.push_back((u16)(i << 4 | 3));
    section(data, 8, p8, [&](int i) { return (u64)((i * 7) % 200); }, true);
    section(data, 13, {}, [&](int i) { return (u64)(((i * 13) % 250) << 4 | (i & 15)); }, true);
    for (int i = 0; i < 256; ++i) data.push_back((u8)(i % 40));    // biomes
    CHECK(feed(w, mapChunk(-3, 7, true, (1u << 0) | (1u << 2) | (1u << 5), data), ids::PlayCb::MapChunk));
    CHECK(w.loadedColumns() == 1 && w.columnLoaded(-3, 7) && !w.columnLoaded(0, 0));
    int bad = 0;
    for (int i = 0; i < 4096; ++i) {
        int x = -48 + (i & 15), z = 112 + ((i >> 4) & 15), y = i >> 8;
        if (w.blockState(x, y, z) != p4[i % 12]) ++bad;
        if (w.blockState(x, 32 + y, z) != p8[(i * 7) % 200]) ++bad;
        if (w.blockState(x, 80 + y, z) != (u16)(((i * 13) % 250) << 4 | (i & 15))) ++bad;
        if (w.blockState(x, 16 + y, z) != 0) ++bad;                // section 1 absent -> air
    }
    CHECK(bad == 0);
    CHECK(w.biome(-48 + 5, 112 + 2) == (2 * 16 + 5) % 40);
    CHECK(w.blockLight(-48, 0, 112) == 5 && w.skyLight(-48, 0, 112) == 15 && w.skyLight(-48, 16, 112) == 15);
    CHECK(w.highestBlock(-48, 112) >= 80);
    CHECK(!w.isLoaded(0, 0, 0) && w.blockState(0, 0, 0) == 0);

    // random edits checked against a shadow copy; forces palette growth 4 -> 5 -> ... -> 13 bpb
    std::vector<u16> shadow(4096); for (int i = 0; i < 4096; ++i) shadow[i] = p4[i % 12];
    srand(9);
    for (int k = 0; k < 6000; ++k) {
        int i = rand() % 4096; u16 st = (u16)((rand() % 220) << 4 | (rand() % 16));
        CHECK(w.setBlockState(-48 + (i & 15), i >> 8, 112 + ((i >> 4) & 15), st)); shadow[i] = st;
        if (k % 997 == 0) for (int j = 0; j < 4096; ++j) if (w.blockState(-48 + (j & 15), j >> 8, 112 + ((j >> 4) & 15)) != shadow[j]) { CHECK(false); break; }
    }
    bad = 0; for (int j = 0; j < 4096; ++j) if (w.blockState(-48 + (j & 15), j >> 8, 112 + ((j >> 4) & 15)) != shadow[j]) ++bad;
    CHECK(bad == 0);
    CHECK(w.blockLight(-48, 0, 112) == 5);                         // light survived the re-packing
    // writing into an absent section allocates it
    CHECK(w.setBlockState(-47, 20, 113, (4 << 4) | 2) && w.blockState(-47, 20, 113) == ((4 << 4) | 2) && w.blockState(-47, 21, 113) == 0);
    // partial update replaces only listed sections; unload frees everything
    Buf d2; section(d2, 4, {(u16)(9 << 4)}, [](int) { return 0; }, true);
    CHECK(feed(w, mapChunk(-3, 7, false, 1u << 0, d2), ids::PlayCb::MapChunk));
    CHECK(w.blockState(-48, 0, 112) == (9 << 4) && w.blockState(-48, 32, 112) != 0);
    Buf un; varint(un, 0x1D); u8 t[8]; Writer uw(t, 8); uw.i32_(-3); uw.i32_(7); un.insert(un.end(), t, uw.p);
    CHECK(feed(w, un, ids::PlayCb::UnloadChunk)); CHECK(w.loadedColumns() == 0 && w.arenaUsed() == 0);
}
static void testPackets() {
    alignas(8) static u8 mem[256 * 1024]; World w; WorldConfig cfg = {4, false, false}; w.init(mem, sizeof mem, cfg); Feed feed;
    Buf data; section(data, 4, {0, (u16)(1 << 4)}, [](int i) { return (u64)(i < 256 ? 1 : 0); }, true);
    feed(w, mapChunk(0, 0, true, 1, data), ids::PlayCb::MapChunk);       // biome bytes missing -> must be rejected
    CHECK(w.loadedColumns() == 0);
    for (int i = 0; i < 256; ++i) data.push_back(1);
    feed(w, mapChunk(0, 0, true, 1, data), ids::PlayCb::MapChunk);
    CHECK(w.loadedColumns() == 1 && w.blockState(3, 0, 3) == (1 << 4) && w.blockState(3, 1, 3) == 0);
    // BlockChange
    Buf bc; varint(bc, 0x0B); u8 t[16]; Writer bw(t, 16); bw.u64_(packPos(3, 5, 4)); bc.insert(bc.end(), t, bw.p); varint(bc, (35 << 4) | 14);
    CHECK(feed(w, bc, ids::PlayCb::BlockChange) && w.blockState(3, 5, 4) == ((35 << 4) | 14));
    // MultiBlockChange: chunk (0,0), two records
    Buf mb; varint(mb, 0x10); Writer mw(t, 16); mw.i32_(0); mw.i32_(0); mb.insert(mb.end(), t, mw.p); varint(mb, 2);
    mb.push_back((1 << 4) | 2); mb.push_back(10); varint(mb, 20 << 4); mb.push_back((15 << 4) | 15); mb.push_back(255); varint(mb, (7 << 4) | 1);
    CHECK(feed(w, mb, ids::PlayCb::MultiBlockChange) && w.blockState(1, 10, 2) == (20 << 4) && w.blockState(15, 255, 15) == ((7 << 4) | 1));
    // Explosion clears blocks relative to the floored center
    Buf ex; varint(ex, 0x1C); u8 t2[64]; Writer ew(t2, 64); ew.f32_(3.5f); ew.f32_(0.5f); ew.f32_(3.5f); ew.f32_(4); ew.i32_(2);
    ew.u8_(0); ew.u8_(0); ew.u8_(0); ew.u8_(1); ew.u8_(0); ew.u8_(0); ew.f32_(0); ew.f32_(0); ew.f32_(0); ex.insert(ex.end(), t2, ew.p);
    CHECK(feed(w, ex, ids::PlayCb::Explosion) && w.blockState(3, 0, 3) == 0 && w.blockState(4, 0, 3) == 0 && w.blockState(5, 0, 3) == (1 << 4));
    // table full: 4 columns max, a 5th is dropped
    for (int i = 1; i <= 5; ++i) feed(w, mapChunk(i, 0, true, 0, [&] { Buf b; for (int k = 0; k < 256; ++k) b.push_back(0); return b; }()), ids::PlayCb::MapChunk);
    CHECK(w.loadedColumns() == 4);
    // memory exhaustion must degrade (missing sections), not crash
    alignas(8) static u8 tiny[4096 + 4 * 80]; World t3; WorldConfig c3 = {4, false, false}; t3.init(tiny, sizeof tiny, c3);
    Buf big; for (int s = 0; s < 8; ++s) section(big, 8, std::vector<u16>(256, 1 << 4), [](int i) { return (u64)(i & 255); }, true);
    for (int k = 0; k < 256; ++k) big.push_back(0);
    feed(t3, mapChunk(0, 0, true, 0xFF, big), ids::PlayCb::MapChunk); CHECK(true);
}
int main() { RUN(testArena); RUN(testChunkAndEdits); RUN(testPackets); DONE(); }
