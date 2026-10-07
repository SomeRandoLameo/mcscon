#include <mcscon/world.h>
#include <mcscon/packets.h>
#include <mcscon/nbt.h>

namespace mc {

void World::init(void* memory, size_t size, const WorldConfig& cfg) {
    cfg_ = cfg; dim_ = 0; lastHit_ = 0;
    size_t tbl = (columnTableBytes(cfg) + 7) & ~(size_t)7;
    cols_ = (Column*)memory;
    arena_.init((u8*)memory + tbl, size > tbl ? size - tbl : 0);
    memset(cols_, 0, columnTableBytes(cfg));
}

void World::clear() {
    for (u32 i = 0; i < cfg_.maxColumns; ++i) if (cols_[i].used) freeCol((int)i);
}

int World::findCol(i32 cx, i32 cz) const {
    if (!cols_) return -1;
    const Column& h = cols_[lastHit_];
    if (h.used && h.x == cx && h.z == cz) return lastHit_;
    for (u32 i = 0; i < cfg_.maxColumns; ++i)
        if (cols_[i].used && cols_[i].x == cx && cols_[i].z == cz) { lastHit_ = (int)i; return (int)i; }
    return -1;
}
int World::newCol(i32 cx, i32 cz) {
    for (u32 i = 0; i < cfg_.maxColumns; ++i) if (!cols_[i].used) {
        memset(&cols_[i], 0, sizeof(Column)); cols_[i].used = 1; cols_[i].x = cx; cols_[i].z = cz; return (int)i;
    }
    return -1;
}
void World::freeSections(Column& c) {
    for (int s = 0; s < 16; ++s) { arena_.release(c.sec[s]); c.sec[s] = 0; }
    arena_.release(c.biomes); c.biomes = 0; c.mask = 0;
}
void World::freeCol(int i) { freeSections(cols_[i]); cols_[i].used = 0; }
u32 World::loadedColumns() const { u32 n = 0; for (u32 i = 0; i < cfg_.maxColumns; ++i) n += cols_[i].used; return n; }

u32 World::allocSection(u32 bpb, u32 palLen, bool lightOn) {
    u32 bytes = secBytes(bpb, palLen, lightOn);
    u32 off = arena_.alloc(bytes);
    if (!off) return 0;
    memset(arena_.ptr(off), 0, bytes);
    SecHdr* h = (SecHdr*)arena_.ptr(off); h->bpb = (u8)bpb; h->palLen = (u16)palLen; h->light = lightOn;
    return off;
}

static inline u16* palOf(SecHdr* h) { return (u16*)(h + 1); }
static inline u64* dataOf(u8* sec, u32 bpb, u32 palLen) { return (u64*)(sec + 8 + (bpb <= 8 ? ((palLen * 2 + 7) & ~7u) : 0)); }

u16 World::getInSection(const SecHdr* hc, u32 idx) const {
    SecHdr* h = const_cast<SecHdr*>(hc);
    const u64* d = dataOf((u8*)h, h->bpb, h->palLen);
    u32 bpb = h->bpb; u32 bit = idx * bpb, w = bit >> 6, off = bit & 63;
    u64 v = d[w] >> off;
    if (off + bpb > 64) v |= d[w + 1] << (64 - off);
    v &= ((u64)1 << bpb) - 1;
    if (bpb <= 8) return v < h->palLen ? palOf(h)[v] : 0;
    return (u16)v;
}
static void putPacked(u64* d, u32 bpb, u32 idx, u32 v) {
    u32 bit = idx * bpb, w = bit >> 6, off = bit & 63; u64 mask = ((u64)1 << bpb) - 1;
    d[w] = (d[w] & ~(mask << off)) | ((u64)v << off);
    if (off + bpb > 64) { u32 sh = 64 - off; d[w + 1] = (d[w + 1] & ~(mask >> sh)) | ((u64)v >> sh); }
}

u16 World::blockState(i32 x, i32 y, i32 z) const {
    if (y < 0 || y > 255) return 0;
    int ci = findCol(x >> 4, z >> 4); if (ci < 0) return 0;
    u32 so = cols_[ci].sec[y >> 4]; if (!so) return 0;
    return getInSection((const SecHdr*)arena_.ptr(so), ((y & 15) << 8) | ((z & 15) << 4) | (x & 15));
}

bool World::setInSection(Column& c, int sy, u32 idx, u16 state) {
    if (!c.sec[sy]) {
        if (state == 0) return true;
        u32 off = allocSection(4, 1, cfg_.storeLight); if (!off) return false;
        c.sec[sy] = off; c.mask |= (u16)(1u << sy);       // palette[0] = air, all indices 0
    }
    SecHdr* h = (SecHdr*)arena_.ptr(c.sec[sy]);
    u32 bpb = h->bpb;
    if (bpb > 8) { putPacked(dataOf((u8*)h, bpb, 0), bpb, idx, state); return true; }
    u16* pal = palOf(h); u32 pi = 0;
    for (; pi < h->palLen; ++pi) if (pal[pi] == state) break;
    if (pi == h->palLen) {
        if (h->palLen < (1u << bpb)) {                    // room in the palette: append (padding bytes may need to grow)
            u32 newLen = h->palLen + 1;
            if (palBytes(newLen) != palBytes(h->palLen)) {   // palette crosses an 8-byte boundary -> move data
                u32 nb = secBytes(bpb, newLen, h->light);
                u32 off = arena_.alloc(nb); if (!off) return false;
                u8* np = arena_.ptr(off); memset(np, 0, nb);
                SecHdr* nh = (SecHdr*)np; *nh = *h; nh->palLen = (u16)newLen;
                memcpy(palOf(nh), pal, h->palLen * 2u);
                memcpy(dataOf(np, bpb, newLen), dataOf((u8*)h, bpb, h->palLen), dataBytes(bpb) + (h->light ? 4096 : 0));
                arena_.release(c.sec[sy]); c.sec[sy] = off; h = nh; pal = palOf(h);
            } else h->palLen = (u16)newLen;
            pal[pi] = state;
        } else {                                          // grow bits per block
            u32 nbpb = bpb < 8 ? bpb + 1 : 13, nlen = nbpb <= 8 ? h->palLen + 1 : 0;
            u32 nb = secBytes(nbpb, nlen, h->light);
            u32 off = arena_.alloc(nb); if (!off) return false;
            u8* np = arena_.ptr(off); memset(np, 0, nb);
            SecHdr* nh = (SecHdr*)np; nh->bpb = (u8)nbpb; nh->palLen = (u16)nlen; nh->light = h->light;
            u64* nd = dataOf(np, nbpb, nlen); const u64* od = dataOf((u8*)h, bpb, h->palLen);
            if (nbpb <= 8) { memcpy(palOf(nh), pal, h->palLen * 2u); palOf(nh)[h->palLen] = state; }
            for (u32 i = 0; i < 4096; ++i) {
                u32 bit = i * bpb, w = bit >> 6, o2 = bit & 63; u64 v = od[w] >> o2;
                if (o2 + bpb > 64) v |= od[w + 1] << (64 - o2);
                v &= ((u64)1 << bpb) - 1;
                putPacked(nd, nbpb, i, nbpb <= 8 ? (u32)v : (u32)(v < h->palLen ? pal[v] : 0));
            }
            if (h->light) memcpy((u8*)nd + dataBytes(nbpb), (u8*)od + dataBytes(bpb), 4096);
            arena_.release(c.sec[sy]); c.sec[sy] = off; h = nh; bpb = nbpb; pal = palOf(h);
            if (bpb > 8) { putPacked(dataOf(np, bpb, 0), bpb, idx, state); return true; }
            pi = h->palLen - 1;
        }
        bpb = h->bpb;
    }
    putPacked(dataOf((u8*)h, bpb, h->palLen), bpb, idx, pi);
    return true;
}

bool World::setBlockState(i32 x, i32 y, i32 z, u16 state) {
    if (y < 0 || y > 255) return false;
    int ci = findCol(x >> 4, z >> 4); if (ci < 0) return false;
    return setInSection(cols_[ci], y >> 4, ((y & 15) << 8) | ((z & 15) << 4) | (x & 15), state);
}

i32 World::highestBlock(i32 x, i32 z) const {
    int ci = findCol(x >> 4, z >> 4); if (ci < 0) return -1;
    for (int sy = 15; sy >= 0; --sy) {
        u32 so = cols_[ci].sec[sy]; if (!so) continue;
        for (int ly = 15; ly >= 0; --ly)
            if (getInSection((const SecHdr*)arena_.ptr(so), (ly << 8) | ((z & 15) << 4) | (x & 15)) >> 4) return sy * 16 + ly;
    }
    return -1;
}
u8 World::biome(i32 x, i32 z) const {
    int ci = findCol(x >> 4, z >> 4); if (ci < 0 || !cols_[ci].biomes) return 0xFF;
    return arena_.ptr(cols_[ci].biomes)[((z & 15) << 4) | (x & 15)];
}
u8 World::light(i32 x, i32 y, i32 z, bool sky) const {
    if (y < 0 || y > 255) return sky ? 15 : 0;
    int ci = findCol(x >> 4, z >> 4); if (ci < 0) return 0;
    u32 so = cols_[ci].sec[y >> 4]; if (!so) return sky ? 15 : 0;     // missing section: open sky above / no block light
    SecHdr* h = (SecHdr*)arena_.ptr(so); if (!h->light) return 0;
    u8* l = (u8*)dataOf((u8*)h, h->bpb, h->palLen) + dataBytes(h->bpb) + (sky ? 2048 : 0);
    u32 idx = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
    return (l[idx >> 1] >> ((idx & 1) * 4)) & 15;
}

bool World::readColumn(Column& c, Reader& r, u32 mask, bool groundUp) {
    for (int sy = 0; sy < 16; ++sy) {
        if (!(mask & (1u << sy))) continue;
        u32 bpb = r.u8_(); i32 palLen = r.varint();
        if (!r.ok() || bpb < 1 || bpb > 13 || palLen < 0 || palLen > 256) return false;
        u16 pal[256];
        for (i32 i = 0; i < palLen; ++i) pal[i] = (u16)r.varint();
        i32 longs = r.varint();
        if (!r.ok() || longs != (i32)(64 * bpb)) return false;
        const u8* src = r.bytes((size_t)longs * 8);
        const u8* bl = r.bytes(2048); const u8* sl = dimHasSky() ? r.bytes(2048) : 0;
        if (!r.ok()) return false;
        arena_.release(c.sec[sy]); c.sec[sy] = 0;
        u32 stored = bpb <= 8 ? (u32)palLen : 0;
        if (bpb <= 8 && (bpb < 4 || palLen == 0)) { if (bpb < 4) return false; }
        u32 off = allocSection(bpb, stored, cfg_.storeLight);
        if (!off) continue;                                   // out of memory: leave this section as air
        c.sec[sy] = off; c.mask |= (u16)(1u << sy);
        SecHdr* h = (SecHdr*)arena_.ptr(off);
        if (bpb <= 8) memcpy(palOf(h), pal, (size_t)palLen * 2);
        u64* d = dataOf((u8*)h, bpb, stored);
        for (i32 i = 0; i < longs; ++i) {
            u64 v = 0; for (int k = 0; k < 8; ++k) v = (v << 8) | src[i * 8 + k];
            d[i] = v;
        }
        if (cfg_.storeLight) { u8* l = (u8*)d + dataBytes(bpb); memcpy(l, bl, 2048); if (sl) memcpy(l + 2048, sl, 2048); }
    }
    if (groundUp) {
        const u8* bio = r.bytes(256);
        if (bio && cfg_.storeBiomes) { u32 off = arena_.alloc(256); if (off) { memcpy(arena_.ptr(off), bio, 256); c.biomes = off; } }
    }
    return r.ok();
}

bool World::handle(const Packet& p) {
    if (p.state != State::Play) return false;
    switch ((ids::PlayCb)p.id) {
    case ids::PlayCb::MapChunk: {
        cb::MapChunk m; if (!pkt::read(p, m)) return true;
        int ci = findCol(m.x, m.z);
        if (m.groundUp) { if (ci >= 0) freeCol(ci); ci = newCol(m.x, m.z); }
        if (ci < 0) return true;                               // partial update for unknown column, or table full
        Reader r(m.chunkData.data, m.chunkData.len);
        if (!readColumn(cols_[ci], r, (u32)m.bitMap, m.groundUp)) freeCol(ci);
        else if (tileFn_) {
            Bytes b; Array<Bytes> arr = m.blockEntities;
            while (arr.next(b)) {
                NbtCompound c; NbtEntry e; BlockPos bp = {0, 0, 0}; bool okx = false, oky = false, okz = false;
                if (nbtRoot(b, c)) {
                    if (c.find("x", e)) { bp.x = (i32)e.asInt(); okx = true; }
                    if (c.find("y", e)) { bp.y = (i32)e.asInt(); oky = true; }
                    if (c.find("z", e)) { bp.z = (i32)e.asInt(); okz = true; }
                }
                if (okx && oky && okz) tileFn_(tileUser_, bp, b);
            }
        }
        return true;
    }
    case ids::PlayCb::UnloadChunk: { cb::UnloadChunk u; if (pkt::read(p, u)) { int ci = findCol(u.chunkX, u.chunkZ); if (ci >= 0) freeCol(ci); } return true; }
    case ids::PlayCb::BlockChange: { cb::BlockChange b; if (pkt::read(p, b)) setBlockState(b.location.x, b.location.y, b.location.z, (u16)b.type); return true; }
    case ids::PlayCb::MultiBlockChange: {
        cb::MultiBlockChange m; if (!pkt::read(p, m)) return true;
        cb::MultiBlockChange_records rec;
        while (m.records.next(rec)) setBlockState(m.chunkX * 16 + (rec.horizontalPos >> 4), rec.y, m.chunkZ * 16 + (rec.horizontalPos & 15), (u16)rec.blockId);
        return true;
    }
    case ids::PlayCb::Explosion: {
        cb::Explosion e; if (!pkt::read(p, e)) return true;
        i32 ox = (i32)(e.x < 0 ? e.x - 1 : e.x), oy = (i32)(e.y < 0 ? e.y - 1 : e.y), oz = (i32)(e.z < 0 ? e.z - 1 : e.z);
        cb::Explosion_affectedBlockOffsets o;
        while (e.affectedBlockOffsets.next(o)) setBlockState(ox + o.x, oy + o.y, oz + o.z, 0);
        return true;
    }
    case ids::PlayCb::TileEntityData: {
        cb::TileEntityData t; if (pkt::read(p, t) && tileFn_ && t.nbtData.len) tileFn_(tileUser_, t.location, t.nbtData);
        return true;
    }
    case ids::PlayCb::Login: { cb::Login l; if (pkt::read(p, l)) { clear(); dim_ = l.dimension; } return false; }
    case ids::PlayCb::Respawn: { cb::Respawn r; if (pkt::read(p, r)) { clear(); dim_ = r.dimension; } return false; }
    default: return false;
    }
}

} // namespace mc
