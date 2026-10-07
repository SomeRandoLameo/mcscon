// Chunk/world model for 1.10-1.12.2. Sections stay in their compact wire form (palette + bit-packed indices)
// inside a caller-provided arena, so a 16-section column costs a few KB instead of 128 KB.
#pragma once
#include "arena.h"
#include "blocks_gen.h"
#include "client.h"
#include "wire.h"

namespace mc {

struct WorldConfig {
    u16 maxColumns;     // simultaneously loaded 16x16 columns (view distance 4 -> 81, 8 -> 289)
    bool storeLight;    // keep block/sky light nibbles (+4 KB per section)
    bool storeBiomes;   // keep the 256-byte biome map per column
};

// Section block layout: header, palette (u16[], padded to 8 bytes; absent when bpb > 8), u64 data[64*bpb], optional 4 KB light.
struct SecHdr { u8 bpb; u8 light; u16 palLen; u32 pad; };

struct BlockInfo { u16 state; u8 id() const { return (u8)(state >> 4); } u8 meta() const { return state & 15; } };

class World {
public:
    // Bytes needed for the bookkeeping table: pass `memory` of at least columnTableBytes(cfg) + arena size.
    static size_t columnTableBytes(const WorldConfig& c) { return (size_t)c.maxColumns * sizeof(Column); }
    World() : cols_(0), cfg_(), dim_(0), lastHit_(0) {}
    // memory must be 8-byte aligned.
    void init(void* memory, size_t size, const WorldConfig& cfg);
    void clear();

    // Feed packets; returns true if the packet was a world packet (MapChunk, UnloadChunk, BlockChange,
    // MultiBlockChange, Explosion, Respawn, Login).
    bool handle(const Packet& p);

    // --- queries (state = blockId << 4 | meta). Unloaded space reads as air; use isLoaded() to tell.
    bool isLoaded(i32 x, i32 y, i32 z) const { return y >= 0 && y < 256 && findCol(x >> 4, z >> 4) >= 0; }
    bool columnLoaded(i32 cx, i32 cz) const { return findCol(cx, cz) >= 0; }
    u16 blockState(i32 x, i32 y, i32 z) const;
    u8 blockId(i32 x, i32 y, i32 z) const { return (u8)(blockState(x, y, z) >> 4); }
    // true if the block state is a full cube (exact per-meta shapes: see blocks::shapeOf / physics)
    bool solid(i32 x, i32 y, i32 z) const { return blocks::shapeOf(blockState(x, y, z)) == 1; }
    // has any collision box (slabs, stairs, fences, ladders, ...)
    bool collides(i32 x, i32 y, i32 z) const { return blocks::shapeOf(blockState(x, y, z)) != 0; }
    bool liquid(i32 x, i32 y, i32 z) const { return blocks::kFlags[blockId(x, y, z)] & blocks::Liquid; }
    bool climbable(i32 x, i32 y, i32 z) const { return blocks::kFlags[blockId(x, y, z)] & blocks::Climbable; }
    i32 highestBlock(i32 x, i32 z) const;                 // y of top non-air block, -1 if none
    u8 biome(i32 x, i32 z) const;                         // 0xFF if unknown
    u8 blockLight(i32 x, i32 y, i32 z) const { return light(x, y, z, false); }
    u8 skyLight(i32 x, i32 y, i32 z) const { return light(x, y, z, true); }

    bool setBlockState(i32 x, i32 y, i32 z, u16 state);   // local edit (also used by the packet handlers)

    // --- stats
    u32 loadedColumns() const;
    u32 arenaUsed() const { return arena_.used(); }
    u32 arenaCapacity() const { return arena_.capacity(); }
    i32 dimension() const { return dim_; }

    // optional: called for every tile entity NBT (MapChunk block entities + TileEntityData). Bytes valid only in callback.
    typedef void (*TileFn)(void* user, BlockPos pos, Bytes nbt);
    void onTileEntity(TileFn fn, void* user) { tileFn_ = fn; tileUser_ = user; }

private:
    struct Column { i32 x, z; u32 sec[16]; u32 biomes; u16 mask; u8 used; u8 pad; };
    static u32 palBytes(u32 palLen) { return (palLen * 2 + 7) & ~7u; }
    static u32 dataBytes(u32 bpb) { return 512u * bpb; }
    static u32 secBytes(u32 bpb, u32 palLen, bool light) { return sizeof(SecHdr) + (bpb <= 8 ? palBytes(palLen) : 0) + dataBytes(bpb) + (light ? 4096 : 0); }

    int findCol(i32 cx, i32 cz) const;
    int newCol(i32 cx, i32 cz);
    void freeCol(int i);
    void freeSections(Column& c);
    bool readColumn(Column& c, Reader& r, u32 mask, bool groundUp);
    u32 allocSection(u32 bpb, u32 palLen, bool light);
    u16 getInSection(const SecHdr* s, u32 idx) const;
    bool setInSection(Column& c, int sy, u32 idx, u16 state);
    u8 light(i32 x, i32 y, i32 z, bool sky) const;
    bool dimHasSky() const { return dim_ == 0; }

    Column* cols_; WorldConfig cfg_; Arena arena_; i32 dim_; mutable int lastHit_;
    TileFn tileFn_ = 0; void* tileUser_ = 0;
};

} // namespace mc
