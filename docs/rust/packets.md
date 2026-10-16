# `packets`: typed packets

`mcscon::packets` is layer 3: a Rust struct for **every** Play-state packet of 1.10 – 1.12.2, generated from
[minecraft-data](https://github.com/PrismarineJS/minecraft-data).

- `packets::cb::*` – clientbound (server → client): **decoded**.
- `packets::sb::*` – serverbound (client → server): **encoded**.
- Fields, ids and a one-line description of each: [Packet reference](../packet-reference.md).

## Reading

```rust
pub fn read<'a, P: ClientPacket<'a>>(p: &Packet<'a>) -> Option<P>
pub fn is<'a, P: ClientPacket<'a>>(p: &Packet<'a>) -> bool
```

`read` returns `Some(packet)` if `p` is a `P` **and** decodes cleanly; `None` if it is another packet or malformed
(truncated, bad lengths, …). `is` checks the id only.

```rust
use mcscon::packets::{self, cb};

client.poll(|link, event| {
    if let mcscon::client::Event::Packet(p) = event {
        if let Some(chat) = packets::read::<cb::Chat>(&p) {
            // chat.message : Str  (JSON text component) – a view, valid only in here
            // chat.position: i8   (0 chat, 1 system, 2 action bar)
            println!("{}", String::from_utf8_lossy(chat.message.as_bytes()));
        } else if let Some(hp) = packets::read::<cb::UpdateHealth>(&p) {
            if hp.health <= 0.0 { packets::respawn(link); }
        }
    }
});
```

`read` also returns `None` for packets that are not in the Play state – login and status packets are handled through
`Packet::login()` / `Packet::status()` and `p.r` (see [client](client.md#packet)).

Several packets can be tested in a row; each `read` decodes from the start of the payload (the packet's reader is
copied, not consumed). To dispatch by id first (cheaper if you handle many packet types) use
`match p.play() { Some(PlayCb::Chat) => … }`.

### What the structs contain

- Numbers are plain Rust types (`i32`, `f64`, `bool`, …); VarInts are `i32`.
- Text is `wire::Str` (UTF-8 view, **not validated** – use `to_str()` or `String::from_utf8_lossy`).
- `wire::Slot` is an item stack (id, count, damage, NBT view).
- `wire::Array<T>` is a validated list you iterate lazily (`for x in array`), copy-able, `size()` for the length.
- `wire::MetaView` is entity metadata (iterate with `next_entry()`).
- `wire::BlockPos` is a block coordinate; `wire::Uuid` is 16 bytes.
- `Option<T>` marks fields that are optional on the wire.
- Layout differences between versions are merged: a field a version does not send keeps its default (0 / empty).

See [wire types](wire.md).

## Sending

```rust
pub fn send<S: Sender + ?Sized, P: ServerPacket>(tx: &mut S, pk: &P) -> bool
```

Encodes `pk` for the connection's version and queues it. Returns `false` if the packet does not exist in that version,
does not fit the send buffer, or the connection is closed.

```rust
use mcscon::packets::{self, sb};
use mcscon::wire::Str;

packets::send(&mut client.link, &sb::Chat { message: Str(b"hello") });
packets::send(&mut client.link, &sb::HeldItemSlot { slot_id: 3 });
```

Serverbound structs derive `Default`, so `sb::WindowClick { window_id: 0, slot: 36, ..Default::default() }` works.
Strings you send are `Str(&[u8])`, arrays are `&[T]`-like views – check the field types in the
[reference](../packet-reference.md#serverbound).

## The packet traits

Every generated struct implements one of two traits; `read` and `send` are thin wrappers around them.

```rust
pub trait ClientPacket<'a>: Sized {            // every cb::* struct
    const ID: PlayCb;                          // version-agnostic id
    fn decode(r: &mut Reader<'a>, v: Version) -> Option<Self>;
}
pub trait ServerPacket {                       // every sb::* struct
    const ID: PlaySb;
    fn encode(&self, w: &mut Writer, v: Version) -> bool;   // false: packet/field not encodable in this version
}
```

Use them for generic code (`fn log<'a, P: ClientPacket<'a> + core::fmt::Debug>(p: &Packet<'a>)`), to decode a payload
you got from elsewhere (`cb::Chat::decode(&mut Reader::new(bytes), Version::V1_12_2)`), or to encode into your own
buffer (`pk.encode(&mut Writer::new(&mut buf), version)`). All generated structs derive `Clone`, `Copy`, `Default` and `Debug`.

## Helper functions

Shortcuts for the packets you send most. All take any [`Sender`](client.md#sender) (`&mut link`, `&mut client`, …) and
return `bool` (`true` = queued).

| Function | Sends |
|----------|-------|
| `chat(c, msg: &[u8])` | `Chat` (max 256 bytes; use `session::say` to split longer text) |
| `teleport_confirm(c, id)` | `TeleportConfirm` for a `cb::Position.teleport_id` |
| `respawn(c)` | `ClientCommand` action 0 (respawn after death) |
| `request_stats(c)` | `ClientCommand` action 1 (open the statistics screen / request stats) |
| `client_settings(c, locale, view_distance, chat_mode, colors, skin_parts, main_hand)` | `Settings` |
| `client_settings_default(c)` | `Settings` with `en_GB`, view distance 2, all skin parts, right hand (`Session` sends its own `en_US` settings with its configured view distance) |
| `on_ground(c, g)` | `Flying` (on-ground flag only) |
| `position(c, x, y, z, on_ground)` | `Position` |
| `look(c, yaw, pitch, on_ground)` | `Look` |
| `position_look(c, x, y, z, yaw, pitch, on_ground)` | `PositionLook` |
| `held_item(c, slot: i16)` | `HeldItemSlot` (hotbar 0 – 8) |
| `swing_arm(c, hand)` | `ArmAnimation` (`hand` 0 main, 1 off) |
| `use_item(c, hand)` | `UseItem` |
| `block_dig(c, status, pos, face)` | `BlockDig`: status 0 start, 1 cancel, 2 finish, 3 drop stack, 4 drop item, 5 release use, 6 swap hands |
| `block_place(c, pos, face, hand, cx, cy, cz)` | `BlockPlace`: right-click `pos` on `face` (cursor position `cx,cy,cz` in 0..1) |
| `entity_action(c, entity_id, action, jump_boost)` | `EntityAction`: 0 sneak, 1 unsneak, 2 leave bed, 3 start sprint, 4 stop sprint, 5 start horse jump, 6 stop horse jump, 7 open horse inventory, 8 elytra |
| `plugin_message(c, channel, data)` | `CustomPayload` |
| `close_window(c, id: u8)` | `CloseWindow` |
| `attack_entity(c, target)` | `UseEntity` action 1 (attack) |
| `interact_entity(c, target, hand)` | `UseEntity` action 0 (right-click an entity) |

Block faces: `0` bottom (−Y), `1` top (+Y), `2` north (−Z), `3` south (+Z), `4` west (−X), `5` east (+X).

## Validation

`cb::validate(PlayCb, Reader, Version) -> bool` decodes (and discards) any clientbound packet by id; use it to check
that a stream parses completely. The test-suite uses it on thousands of reference packets.

## Wire ids

Packet ids in the protocol change between versions. The library uses version-agnostic ids (`ids::PlayCb`,
`ids::PlaySb`, `ids::LoginCb`, …) and translates with generated tables (`ids::PLAY_CB_TABLES[version.table_index()]`:
`w2a` wire → agnostic, `a2w` agnostic → wire, `0xFF` = not present). Each enum has `COUNT`, `ALL`, `NAMES` and
`from_u8`.
