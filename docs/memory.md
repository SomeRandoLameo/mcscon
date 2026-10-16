# Memory sizing

The Rust API makes you choose every buffer; the C and C++ APIs pick fixed defaults. This page explains what each
buffer costs and how to size it.

## Network buffers (`Buffers`)

```rust
pub struct Buffers<'a> { pub rx: &'a mut [u8], pub scratch: &'a mut [u8], pub tx: &'a mut [u8] }
```

| Buffer | Must hold | Typical |
|--------|-----------|---------|
| `rx` | the largest frame on the wire you want to receive. Chunk packets are the big ones: ~10 – 60 KB compressed on real terrain, a flat world ~1 KB. | 128 KB |
| `scratch` | the largest *decompressed* packet you want to receive (chunks: up to ~200 KB). Empty is fine if the server does not compress. | 512 KB |
| `tx` | the biggest burst of outgoing packets between two `poll` calls. A chat message is ~260 bytes, a movement packet ~40. | 4 KB |

A frame that does not fit is skipped and reported as `Error::PacketTooLarge`; everything else keeps working. That is
also how you run on tiny devices: give `rx` 4 KB and you simply do not see chunks (the world model stays empty).
Servers normally enable compression with a 256-byte threshold, so the common case is `rx` smaller than `scratch`.

## World memory

```rust
let cfg = WorldConfig { max_columns: 49, store_light: false, store_biomes: false };
let bytes = World::column_table_bytes(&cfg) + arena_bytes;   // column_table_bytes = max_columns * 80
```

- `max_columns` – how many 16×16 chunk columns are held at once. The server sends a square around the player:
  view distance `d` ⇒ `(2d+1)²` columns (d=3 ⇒ 49, d=4 ⇒ 81, d=8 ⇒ 289). `Session::view_distance` (default 8)
  is what the *client* asks for; the server may send fewer. If the table is full, further columns are dropped and
  `block_*` reads in them return air.
- Arena – block data. Each 16×16×16 section is stored compactly (palette + `bits_per_block` bit-packed indices):
  about `512 × bits_per_block` bytes plus a small palette – ~2 KB for a mostly-uniform section (4 bits), up to ~6.7 KB
  (13 bits). Light data adds 4 KB per section (`store_light`). Real terrain columns are typically 8 – 40 KB; a
  flat world column about 2 KB. Measured: 81 flat-world columns use ~170 KB. Use `World::arena_used()` and
  `arena_capacity()` to tune; when the arena is full new sections are dropped (the world degrades, nothing crashes).
- Alignment: `World::new` aligns the memory to 8 bytes itself (losing at most 7 bytes), so a plain `vec![0u8; n]` or
  `static mut MEM: [u8; N]` works.
- Want no world at all? `max_columns: 0` and an empty slice.

## Tables

| Table | Element size | Notes |
|-------|--------------|-------|
| `entities: &mut [Entity]` | 128 B | one slot per tracked entity; when full, spawns are dropped and counted in `EntityTracker::dropped()` |
| `players: &mut [PlayerEntry]` | 44 B | tab-list entries; names are limited to 16 bytes |

## Fixed-size state inside the session

`Inventory` ≈ 1.4 KB (player window + one open window of up to 96 slots), `PlayerState` 88 B, `Body` 64 B. All are
plain structs inside `Session`; a whole `Session<TcpTransport>` value is about 1.9 KB plus the buffers it borrows.

## The C and C++ API

`mc_bot_new` (and so `mcscon::Bot`) allocates once on the heap and frees it all in `mc_bot_free` / the destructor:

| Buffer | Size |
|--------|------|
| `rx` | 128 KB |
| `scratch` | 512 KB |
| `tx` | 4 KB |
| world | 49 columns (view distance 3) + 256 KB arena ≈ 260 KB |
| entities | 64 × 128 B = 8 KB |
| tab list | 16 × 44 B ≈ 0.7 KB |
| **total** | **≈ 0.9 MB** |

The sizes are constants in `mc_bot_new` ([`src/capi.rs`](../src/capi.rs)). To change them, edit that function and
rebuild the library – or, for fully static memory, use the Rust API.

Consequences of the defaults: the bot asks for view distance 8 but stores at most 49 chunk columns (columns beyond
that are dropped until others are unloaded), tracks at most 64 entities and 16 tab-list players. None of that affects chat, walking, position or health.

## Example budgets

| Target | `rx` | `scratch` | `tx` | World | Entities |
|--------|------|-----------|------|-------|----------|
| Desktop bot | 128 KB | 512 KB | 4 KB | 49 columns + 256 KB | 64 |
| Small console | 32 KB | 128 KB | 2 KB | 9 columns + 64 KB | 32 |
| Chat-only | 4 KB | 0 | 1 KB | none (`max_columns: 0`) | 8 |
