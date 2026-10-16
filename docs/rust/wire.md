# `wire`: Reader, Writer and wire types

Everything here is bounds-checked, allocation-free and panic-free.

## Reader

```rust
pub struct Reader<'a> { pub p: &'a [u8], pub err: bool }
```

A cursor over bytes (big-endian, like the protocol). **Overruns set `err`** and return 0 instead of failing, so you can
read a whole structure and check once at the end with `ok()`.

| Method | Description |
|--------|-------------|
| `Reader::new(bytes)` | new reader |
| `Reader::failed()` | a reader already in the error state |
| `left() -> usize`, `ok() -> bool` | bytes remaining; `true` while no overrun happened |
| `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 bool` | fixed-size big-endian reads |
| `varint() -> i32`, `varlong() -> i64` | variable-length integers (an over-long encoding sets `err`) |
| `str() -> Str`, `str_max(max_len) -> Str` | length-prefixed UTF-8 string view (error if longer than `max_len` / the data) |
| `bytes(n) -> Option<&[u8]>` | the next `n` bytes |
| `skip(n)` | skip `n` bytes |
| `need(n) -> bool` | check `n` bytes are available (sets `err` if not) |

```rust
let mut r = packet.r;                // Packet::r is a Reader copy
let text = r.str();                  // e.g. the status JSON
let x = r.f64();
if !r.ok() { /* truncated */ }
```

## Writer

```rust
pub struct Writer<'a> { pub buf: &'a mut [u8], pub pos: usize, pub err: bool }
```

The mirror of `Reader`, writing into your slice. **Overflow sets `err`** and further writes do nothing.

| Method | Description |
|--------|-------------|
| `Writer::new(buf)` | write into `buf` |
| `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 bool` | fixed-size big-endian writes |
| `varint(v: i32)`, `varlong(v: i64)` | variable-length integers |
| `str(&[u8])` | length-prefixed string |
| `bytes(&[u8])` | raw bytes |
| `left()`, `written() -> &[u8]`, `need(n)` | remaining space, bytes written so far, space check |
| `varint_size(v: u32) -> usize` (free function) | encoded size of a VarInt |

`Sender::send_wire` hands you a `Writer` for the packet payload:

```rust
use mcscon::client::Sender;          // the trait that provides send_wire / send_play
link.send_wire(0x00, &mut |w| { w.varint(340); w.str(b"host"); w.u16(25565); w.varint(2); });
```

## Views

| Type | Meaning |
|------|---------|
| `Str<'a>(pub &'a [u8])` | UTF-8 text view (unvalidated). `as_bytes()`, `len()`, `is_empty()`, `to_str() -> Option<&str>`; `str == "literal"` works |
| `Bytes<'a>(pub &'a [u8])` | raw bytes (NBT blobs) |
| `Uuid(pub [u8; 16])` | a UUID |
| `VarInt(i32)`, `VarLong(i64)` | wrappers used inside generic arrays |
| `BlockPos { x, y, z: i32 }` | block coordinates. Packed on the wire as `x:26 \| y:12 \| z:26`; `pack_pos(x, y, z) -> u64`, `unpack_pos(u64) -> BlockPos` |

All views borrow the receive buffer; copy the data if you need it after the callback returns.

## `Slot`: an item stack

```rust
pub struct Slot<'a> { pub id: i16, pub count: i8, pub damage: i16, pub nbt: Bytes<'a> }
```

`id == -1` is an empty slot. `nbt.0.is_empty()` means no NBT tag; otherwise read it with [`nbt`](chat-nbt.md#nbt).
`decode_slot(&mut Reader) -> Option<Slot>` and `encode_slot(&mut Writer, &Slot)` read/write one. The trackers keep only
the id/count/damage part (`entities::ItemStack`), because NBT cannot be stored without a heap.

## `Array<'a, T>`

A list that was **validated once** when the packet was decoded and is decoded lazily when you iterate:

```rust
for entry in array { /* T */ }       // Array implements Iterator; it is Copy, so iterate as often as you like
let n = array.size();
```

| Method | Description |
|--------|-------------|
| `size() -> u32`, `is_empty()` | number of elements (remaining, for a partly consumed copy) |
| `next_elem() -> Option<T>` | the next element (what `Iterator::next` calls) |
| `Array::parse(&mut Reader, n, version, ctx) -> Option<Array>` | validate `n` elements at the reader and return a view over exactly those bytes – what the generated decoders use; `ctx` is the value of an enclosing field that element layouts may depend on |

Element types implement `Elem<'a>` (decode one value) and, for serverbound arrays, `EncElem` (encode one value):

```rust
pub trait Elem<'a>: Sized { fn decode(r: &mut Reader<'a>, v: Version, ctx: i32) -> Option<Self>; }
pub trait EncElem { fn encode(&self, w: &mut Writer, v: Version, ctx: i32); }
```

They are implemented for `u8 i8 i16 u16 i32 i64 f32 f64`, `VarInt`, `VarLong`, `Str`, `Bytes`, `Uuid`, `Slot`, nested
`Array`s (decoding) and `&[T]` (encoding), plus the generated element structs in `packets::cb` / `packets::sb`.

## Entity metadata

`MetaView<'a>` is a validated view of an entity's metadata list. `next_entry() -> Option<MetaEntry>` walks it (`MetaView` also implements `Iterator`, so `for e in meta` works).
`MetaView::parse(&mut Reader, version)` validates a list up to the `0xFF` terminator; `raw() -> (&[u8], Version)`
returns the validated bytes, e.g. to hand them across an FFI boundary.

```rust
pub struct MetaEntry<'a> {
    pub key: u8,       // index
    pub kind: u8,      // 0 byte, 1 varint, 2 float, 3 string, 4 chat, 5 slot, 6 bool, 7 rotation,
                       // 8 position, 9 optional position, 10 direction, 11 optional UUID, 12 block id, 13 nbt (1.12+)
    pub i: i32,        // byte / varint / bool / direction / block id
    pub f: [f32; 3],   // float (f[0]) or rotation
    pub s: Str<'a>,    // string / chat
    pub slot: Slot<'a>,
    pub pos: BlockPos, // position / optional position
    pub present: bool, // optional fields: is the value there
    pub uuid: Uuid,
    pub nbt: Bytes<'a>,
}
```

Only the fields that match `kind` are meaningful. The common indices for living entities: `0` flags (bit 0x01 on fire,
0x02 sneaking, 0x08 sprinting, 0x10 using item, 0x20 invisible, 0x40 glowing, 0x80 elytra), `2` custom name, `7` health.
`EntityTracker` already decodes flags and health into [`Entity`](entities.md#entity).

## NBT and skipping

```rust
pub fn read_nbt<'a>(r: &mut Reader<'a>, optional: bool) -> Option<Bytes<'a>>
pub fn skip_nbt_payload(r: &mut Reader, tag: u8, depth: i32) -> bool
```

`read_nbt` validates one named root tag and returns its raw bytes (with `optional = true` a single `TAG_End` byte
means "no NBT" and yields empty `Bytes`). `skip_nbt_payload` skips the payload of a tag of type `tag`; `depth` starts
at 0 and nesting deeper than 512 levels is rejected. Use [`nbt`](chat-nbt.md#nbt) to look inside.
