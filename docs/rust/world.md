# `world` and `blocks`: the block model

`World` keeps the chunk columns the server sends, so you can ask "what block is at x, y, z?". Sections stay in their
compact wire form (palette + bit-packed indices) inside a caller-provided arena, so a column costs a few KB instead of
128 KB. Memory sizing: [Memory sizing](../memory.md#world-memory).

## Block states

A block state is a `u16`: **`block_id << 4 | meta`** (1.10 – 1.12.2 numeric ids; `meta` 0 – 15). The helper
`BlockInfo { state }` offers `id()` and `meta()`: `BlockInfo { state: world.block_state(x, y, z) }.meta()`. Block ids are the classic numbers: 0 air, 1 stone, 2 grass, 3 dirt, 7 bedrock, 41 gold
block, 54 chest … `blocks::NAMES[id]` has the 1.12 names.

## `WorldConfig`

```rust
pub struct WorldConfig {
    pub max_columns: u16,    // 16x16 columns held at once (view distance d → (2d+1)²)
    pub store_light: bool,   // keep block/sky light (+4 KB per section)
    pub store_biomes: bool,  // keep the 256-byte biome map per column
}
```

## `World`

```rust
let cfg = WorldConfig { max_columns: 49, store_light: false, store_biomes: false };
let bytes = World::column_table_bytes(&cfg) + 256 * 1024;       // column table + arena
let mut mem = vec![0u8; bytes];                                  // any alignment: World aligns internally
let mut world = World::new(&mut mem, cfg);
```

(With `Session` you do not create a `World` yourself; you give it `world_mem` and read `session.world`.)

| Method | Description |
|--------|-------------|
| `World::column_table_bytes(&cfg) -> usize` | bookkeeping bytes: `max_columns × COLUMN_BYTES` (80); one `Column` record per chunk column |
| `World::new(memory, cfg)` | create; memory beyond the column table becomes the arena. If the memory is too small, `max_columns` is reduced |
| `clear()` | forget everything |
| `handle(&Packet) -> bool` | feed a packet; `true` if it was a world packet (`MapChunk`, `UnloadChunk`, `BlockChange`, `MultiBlockChange`, `Explosion`, `TileEntityData`). `Login`/`Respawn` reset the world but return `false` |
| `handle_with(&Packet, Option<&mut dyn WorldSink>)` | same, also reporting block entity NBT to `WorldSink::tile_entity(pos, nbt)` |
| `loaded_columns() -> u32` | number of columns currently stored |
| `is_loaded(x, y, z)`, `column_loaded(cx, cz)` | is that block's / chunk column's data present (`cx = x >> 4`) |
| `block_state(x, y, z) -> u16` | the state; **unloaded space reads as air (0)** |
| `block_id(x, y, z) -> u8` | `block_state >> 4` |
| `solid(x, y, z)` | full cube for every meta |
| `collides(x, y, z)` | has any collision box (slabs, stairs, fences, ladders …) |
| `liquid(x, y, z)`, `climbable(x, y, z)` | water/lava; ladders and vines |
| `highest_block(x, z) -> i32` | y of the top non-air block, −1 if none |
| `biome(x, z) -> u8` | biome id; `0xFF` if unknown or not stored |
| `block_light(x, y, z)`, `sky_light(x, y, z) -> u8` | 0 – 15 (needs `store_light`; 0 otherwise) |
| `set_block_state(x, y, z, state) -> bool` | local edit; `false` if the column is not loaded or memory ran out |
| `arena_used()`, `arena_capacity()` | arena usage in bytes |
| `dimension() -> i32` | −1 nether, 0 overworld, 1 end |
| `config() -> WorldConfig` | the effective configuration |

`WorldSink` is the one-method trait used by `handle_with`:

```rust
pub trait WorldSink { fn tile_entity(&mut self, pos: BlockPos, nbt: Bytes<'_>); }   // NBT valid only in the call
```

It receives the block entities contained in `MapChunk` and every `TileEntityData` (signs, chests, spawners, banners …).
With a `Session` this is `SessionSink::tile_entity`.

Coordinates are world block coordinates (x/z any `i32`, y 0 – 255). `y` outside 0 – 255 reads as air and is never "loaded".

## `blocks`

Generated tables used by `World` and the physics:

| Item | Description |
|------|-------------|
| `SOLID`, `LIQUID`, `TRANSPARENT`, `CLIMBABLE`, `PARTIAL_SHAPE` | flag bits in `FLAGS[id]` |
| `FLAGS: [u8; 256]` | flags per block id |
| `shape_of(state: u16) -> u8` | collision shape id of a block state; index into `SHAPES` (0 = no collision, 1 = full cube) |
| `SHAPES: [ShapeRef]`, `BOXES: [Box32]` | shape table: shape `s` is `BOXES[SHAPES[s].off ..][.. SHAPES[s].n]`; a `Box32 { x0, y0, z0, x1, y1, z1 }` is in 1/32 block units relative to the block corner (a full cube is `0,0,0 – 32,32,32`) |
| `NAMES: [&str; 256]` | block names by id (without `minecraft:`) |

```rust
use mcscon::blocks;
let state = world.block_state(x, y, z);
let id = (state >> 4) as usize;
println!("{} solid={}", blocks::NAMES[id], blocks::FLAGS[id] & blocks::SOLID != 0);
let s = blocks::SHAPES[blocks::shape_of(state) as usize];
for b in &blocks::BOXES[s.off as usize..][..s.n as usize] { println!("box {:?}", b); }
```
