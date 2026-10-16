# `entities`: entity tracker and tab list

`EntityTracker` follows every entity the server tells the client about (mobs, players, items, projectiles, vehicles,
paintings, XP orbs, lightning); `PlayerList` follows the tab list. Both use fixed tables you provide. With a
[`Session`](session.md) they are `session.entities` and `session.players`.

## `Entity`

```rust
pub struct Entity {
    pub id: i32, pub uuid: Uuid, pub kind: EntityKind, pub used: u8,
    pub type_: i32,
    pub x: f64, pub y: f64, pub z: f64,
    pub yaw: u8, pub pitch: u8, pub head_yaw: u8,
    pub vx: i16, pub vy: i16, pub vz: i16,
    pub on_ground: bool, pub vehicle: i32, pub flags: u8,
    pub health: f32, pub has_health: bool,
    pub data: i32, pub equipment: [ItemStack; 6],
}
```

| Field | Meaning |
|-------|---------|
| `id` | entity id (unique per connection) |
| `uuid` | entity UUID |
| `kind` | `EntityKind::{Object, Mob, Player, XpOrb, Weather, Painting}` – which spawn packet created it |
| `used` | slot is occupied (`iter()` already filters) |
| `type_` | the numeric type from the spawn packet: *mob type* for `Mob` (e.g. 90 pig, 100 horse, 120 villager), *object type* for `Object` (e.g. 1 boat, 2 item), weather type for `Weather`. Numbers are the 1.10 – 1.12.2 protocol ids |
| `x, y, z` | position in blocks (kept up to date from movement/teleport packets) |
| `yaw, pitch, head_yaw` | rotation in 1/256 turn; `yaw_deg()` / `pitch_deg()` convert to degrees |
| `vx, vy, vz` | velocity in 1/8000 block per tick |
| `on_ground` | on-ground flag from movement packets |
| `vehicle` | id of the entity this one rides, −1 if none |
| `flags` | metadata byte 0: `0x01` on fire, `0x02` sneaking, `0x08` sprinting, `0x10` using item, `0x20` invisible, `0x40` glowing, `0x80` elytra |
| `health`, `has_health` | metadata index 7 for living entities (`has_health` is false until the server sent it) |
| `data` | object data / XP count / painting direction |
| `equipment` | `[main hand, off hand, boots, leggings, chestplate, helmet]` as `ItemStack { id, count, damage }`; `id == -1` is empty |

NBT of equipment is not stored (no heap); read it from the packet in `SessionSink::packet` if you need it.
`Entity` is `#[repr(C)]`, `Copy` and `Default` (an unused slot) – create tables with `[Entity::default(); N]`.

`ItemStack` (also used by the [inventory](inventory.md)) is `#[repr(C)] struct ItemStack { id: i16, count: i8, damage: i16 }`.

## `EntityTracker`

| Method | Description |
|--------|-------------|
| `EntityTracker::new(storage: &mut [Entity])` | use `storage` as the table (capacity = its length) |
| `clear()` | forget all entities |
| `find(id) -> Option<&Entity>` | by entity id |
| `self_id() -> i32` | the bot's own entity id (−1 before login) |
| `count() -> u32` | entities currently tracked |
| `dropped() -> u32` | spawns that were lost because the table was full |
| `iter()` | all tracked entities |
| `table() -> &[Entity]` | the raw table (includes unused slots) |
| `nearest(x, y, z, max_dist, kind: Option<EntityKind>)` | the closest entity within `max_dist` blocks (excludes the bot itself), optionally of one kind |
| `handle(&Packet) -> bool` / `handle_with(&Packet, &mut dyn EntitySink)` | feed a packet (done for you by `Session`); `true` if consumed |

```rust
// nearest hostile-or-not mob within 10 blocks
if let Some(e) = session.entities.nearest(session.body.x, session.body.y, session.body.z, 10.0, Some(EntityKind::Mob)) {
    session.look_at(e.x, e.y + 1.0, e.z);
    let id = e.id;                    // copy the id, the borrow of `entities` ends here
    session.attack(id);
}
```

## `EntitySink`

```rust
pub trait EntitySink {
    fn event(&mut self, ev: EntityEvent, e: &Entity, arg: i32) {}
    fn metadata(&mut self, e: &Entity, meta: MetaView<'_>) {}
}
```

With a `Session` these arrive as `SessionSink::entity_event` / `entity_metadata`. `()` implements `EntitySink` (and
`PlayerListSink`, `InventorySink`) as a no-op sink; that is what `handle(&p)` uses.

| `EntityEvent` | `arg` | When |
|---------------|-------|------|
| `Spawned` | 0 | a new entity (already in the table) |
| `Moved` | 0 | position or rotation changed (frequent) |
| `Updated` | 0 | metadata, equipment, passengers, velocity … changed |
| `Destroying` | 0 | about to be removed (still readable in the callback) |
| `Status` | status id | *Entity Status* (hurt = 2, died = 3, …) |
| `Animation` | animation id | swing, damage, … |
| `Collected` | collector entity id | an item/orb was picked up |

`metadata` receives the raw [`MetaView`](wire.md#entity-metadata) every time it changes, for anything beyond flags and health.

## Tab list

```rust
pub struct PlayerEntry { pub uuid: Uuid, pub name: [u8; 17], pub used: u8, pub has_display_name: u8, pub game_mode: i32, pub ping: i32 }
```

| Field | Meaning |
|-------|---------|
| `uuid` | the player's UUID (key of the list) |
| `name` / `name()` | NUL-terminated name; `name()` returns the bytes without the NUL (at most 16) |
| `used` | slot occupied (`iter()` filters) |
| `has_display_name` | the server set a custom display name (the name itself is not stored) |
| `game_mode` | 0 survival, 1 creative, 2 adventure, 3 spectator |
| `ping` | latency in ms as reported by the server |

`PlayerEntry` is `#[repr(C)]`, `Copy` and `Default`. `PlayerList` has `new(storage)`, `find(&Uuid)`,
`find_by_name(&[u8])`, `count()`, `iter()`, `table()`, `handle(&Packet)` / `handle_with(&Packet, &mut dyn PlayerListSink)`.
`PlayerListEvent::{Added, Removed, Updated}` is delivered through `PlayerListSink::event` (or
`SessionSink::player_list_event`).

```rust
if let Some(p) = session.players.find_by_name(b"Notch") { println!("ping {}", p.ping); }
```
