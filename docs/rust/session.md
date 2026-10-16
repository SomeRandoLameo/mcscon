# `session`: the high-level bot

`Session` is layer 5. It owns a [`Client`](client.md) plus the models ([`World`](world.md),
[`EntityTracker`](entities.md), [`PlayerList`](entities.md), [`Inventory`](inventory.md), [`Body`](physics.md)) and wires
them together. After `connect` you only call `poll` and `update` and read or change plain fields.

```rust
use mcscon::session::{Session, SessionSink, SessionStorage};

let mut bot = Session::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx }, Version::V1_12_2, storage);
bot.connect("127.0.0.1", 25565, "mcscon");
let t0 = std::time::Instant::now();
while bot.poll(&mut my_sink) >= 0 {
    bot.update(t0.elapsed().as_millis() as u32);
    // your logic
    std::thread::sleep(std::time::Duration::from_millis(10));
}
```

A complete program: [`examples/bot.rs`](../../examples/bot.rs).

## `SessionStorage`

```rust
pub struct SessionStorage<'a> {
    pub world_mem: &'a mut [u8],        // 8-byte aligned; column table + chunk arena (see World::column_table_bytes)
    pub world: WorldConfig,
    pub entities: &'a mut [Entity],
    pub players: &'a mut [PlayerEntry],
}
```

Sizing: [Memory sizing](../memory.md).

## `Session` fields (all public)

| Field | Type | Meaning |
|-------|------|---------|
| `client` | `Client` | the packet pump; `client.link` is the sender |
| `world` | `World` | blocks around the bot |
| `entities` | `EntityTracker` | mobs, players, items, … |
| `players` | `PlayerList` | tab list |
| `inventory` | `Inventory` | the bot's inventory and the open window |
| `state` | `PlayerState` | health, game mode, … (below) |
| `body` | `Body` | position, velocity, rotation, on-ground flags |
| `controls` | `Controls` | **write these**: `forward`, `back`, `left`, `right`, `jump`, `sneak`, `sprint`; applied on the next tick |

## Methods

| Method | Description |
|--------|-------------|
| `Session::new(transport, buffers, version, storage)` | create (disconnected) |
| `connect(host, port, username) -> bool` | reset all state and start an offline-mode login. `false` if TCP failed |
| `poll(&mut sink) -> i32` | pump the network, update the models, call the sink. Returns the number of packets or `-1` when disconnected |
| `update(now_ms: u32) -> i32` | run the **20 Hz ticks** that are due (physics + movement packets) using *your* clock in milliseconds; returns ticks executed (at most 5 per call; after a long stall it skips ahead instead of catching up). Safe to call every loop iteration |
| `disconnect()` | close |
| `say(&[u8]) -> bool` | send chat; splits at 256 bytes (never inside a UTF-8 character); text starting with `/` is a command |
| `respawn() -> bool` | send a respawn request (only needed with `auto_respawn(false)`) |
| `look_at(x, y, z)` | turn `body` towards a world position (eyes to point) |
| `set_rotation(yaw, pitch)` | set the view direction in degrees (yaw 0 = +Z/south, 90 = −X/west, 180 = −Z/north, 270 = +X/east; pitch 90 = straight down) |
| `break_block_instant(pos, face) -> bool` | swing + start + finish digging (creative mode, or blocks that break instantly) |
| `place_block(against: BlockPos, face: i8, hand: i32) -> bool` | right-click block `against` on `face` (place a block, open a chest/furnace, press a button …). `hand` 0 main, 1 off |
| `attack(entity_id) -> bool` | attack an entity and swing |
| `select_hotbar(index: u8) -> bool` | select hotbar slot 0 – 8 |
| `use_held_item() -> bool` | right-click with the held item (eat, throw, shoot …) |
| `auto_respawn(on)` | respawn automatically after death (default **on**) |
| `resource_pack_policy(p)` | `ResourcePackPolicy::{Accept, Decline, Ignore}` – what to answer to a server resource pack (default **Accept**: reports "accepted" then "loaded"; nothing is downloaded) |
| `view_distance(d: i8)` | the view distance requested in the client settings (default 8; set **before** `connect`) |
| `brand(&'a str)` | client brand sent in `MC|Brand` (default `"mcscon"`; set before `connect`) |

`connect` resets the world, entities, inventory, `state`, `body`, `controls` and the tick clock, so one `Session` can
connect again and again. The tab list (`players`) is **not** cleared: entries from a previous connection stay until
the new server removes them (`PlayerList` has no `clear`; create a new `Session` if that matters).

### Free functions

The session's helpers are also usable without a `Session`:

| Function | Description |
|----------|-------------|
| `session::say<S: Sender>(tx, message: &[u8]) -> bool` | `Session::say` for any [`Sender`](client.md#sender) (e.g. inside `Client::poll`) |
| `session::look_at(body: &mut Body, x, y, z)` | turn a [`Body`](physics.md#body) so its eyes point at a world position |

## `PlayerState`

Updated from server packets; read it any time.

| Field | Meaning |
|-------|---------|
| `entity_id: i32` | the bot's entity id |
| `game_mode: u8` | `SURVIVAL` 0, `CREATIVE` 1, `ADVENTURE` 2, `SPECTATOR` 3 (constants in `mcscon::session`) |
| `hardcore: bool`, `dimension: i32`, `difficulty: u8`, `max_players: u8` | from *Join Game* / *Respawn* |
| `joined: bool` | *Join Game* received |
| `spawned: bool` | the server has placed the bot (first position received); movement and physics are active only now. Becomes `false` again on respawn / dimension change until the next position |
| `health: f32`, `food: i32`, `saturation: f32` | 20 = full health / food |
| `xp_bar: f32`, `xp_level: i32`, `xp_total: i32` | experience |
| `abilities: u8` | bit set of `ABILITY_INVULNERABLE` 1, `ABILITY_FLYING` 2, `ABILITY_ALLOW_FLYING` 4, `ABILITY_CREATIVE` 8 (constants in `mcscon::session`) |
| `fly_speed: f32`, `walk_speed: f32` | ability speeds (defaults 0.05 / 0.1) |
| `spawn_point: BlockPos` | world spawn |
| `world_age: i64`, `time_of_day: i64` | ticks |
| `raining: bool`, `rain_level: f32` | weather |
| `dead() -> bool` | `joined && health <= 0` |

Before the first server values arrive (and after each `connect`) health and food read 20, walk/fly speed 0.1/0.05,
everything else 0 / `false`. `PlayerState` is `#[repr(C)]`, `Copy` and `Default`.

## `SessionSink`

Everything the session reports. **All methods have empty defaults**; implement what you need, pass `&mut sink` to
`poll` (`()` implements it too: `bot.poll(&mut ())`).

```rust
pub trait SessionSink {
    fn chat(&mut self, text: &[u8], position: u8, json: Str<'_>) {}
    fn packet(&mut self, p: &Packet<'_>) {}
    fn event(&mut self, ev: SessionEvent, arg: i32) {}
    fn error(&mut self, e: Error, info: i32) {}
    fn entity_event(&mut self, ev: EntityEvent, e: &Entity, arg: i32) {}
    fn entity_metadata(&mut self, e: &Entity, meta: MetaView<'_>) {}
    fn player_list_event(&mut self, ev: PlayerListEvent, e: &PlayerEntry) {}
    fn slot(&mut self, window_id: i8, slot: i16, item: &Slot<'_>) {}
    fn window(&mut self, w: &Window, opened: bool) {}
    fn tile_entity(&mut self, pos: BlockPos, nbt: Bytes<'_>) {}
}
```

| Callback | When |
|----------|------|
| `chat` | a chat message. `text`: the component flattened to plain text with `§` codes stripped (at most 511 bytes); `position`: 0 chat, 1 system, 2 action bar; `json`: the original JSON |
| `packet` | **every** packet, after the session handled it – use this to decode what the session does not model, e.g. `if let Some(m) = packets::read::<cb::Title>(p)` |
| `event` | high-level events (below) |
| `error` | errors (see [Errors](../concepts.md#errors)) |
| `entity_event`, `entity_metadata` | see [entities](entities.md) |
| `player_list_event` | tab list changes |
| `slot` | an inventory slot changed (`window_id` −1 / `slot` −1 = the cursor) |
| `window` | a window opened (`opened == true`) or closed |
| `tile_entity` | block entity NBT (sign text, spawner, banner …) |

All borrowed arguments are valid only during the call.

### `SessionEvent`

| Event | `arg` | Meaning |
|-------|-------|---------|
| `Joined` | entity id | *Join Game* received (client settings and brand have been sent) |
| `Spawned` | 0 | the first server-confirmed position: the bot is in the world |
| `Died` | 0 | health reached 0 |
| `Respawned` | 0 | *Respawn* received (also sent after a dimension change) |
| `Kicked` | 0 | the server disconnected the bot (the connection closes after the event) |
| `GameModeChanged` | new game mode | |
| `DimensionChanged` | dimension id | |
| `Teleported` | 0 | the server moved the bot (`/tp`, anti-cheat correction); the session confirmed it |

`SessionEvent` is `#[repr(i32)]` with the values 0 – 7 in the order above; `SessionEvent::from_i32(v)` converts back
(`None` for other values) – handy when passing events through an FFI layer.

### `ResourcePackPolicy`

| Value | Answer sent to a *Resource Pack Send* |
|-------|----------------------------------------|
| `Accept` (default) | "accepted" (3), then "successfully loaded" (0). Nothing is downloaded. |
| `Decline` | "declined" (1). Servers with `require-resource-pack` may kick the bot. |
| `Ignore` | nothing; handle `cb::ResourcePackSend` yourself in `SessionSink::packet`. |

## What the session does for you

- **Login**: after *Join Game* it sends client settings (`en_US`, your view distance, all skin parts, right hand) and the brand plugin message.
- **Position**: applies absolute/relative teleports, sends the teleport confirmation and the accepted position, resets velocity.
- **Chunks, blocks, entities, tab list, inventory/windows**: fed to the models.
- **Health / death**: tracks health, raises `Died`, and with `auto_respawn` sends the respawn request.
- **Resource packs**: answers according to the policy.
- **Movement** (`update`): every 50 ms it runs [physics](physics.md) from `controls`, `body` and the loaded world, and
  sends position/look/on-ground packets exactly like the vanilla client (a position packet at least once per second,
  sneak/sprint `EntityAction` on change). No movement happens before `state.spawned`, while dead, or while the chunk
  under the bot is not loaded.
- **Keep-alive and compression**: by the underlying `Client`.

## Notes

- Mutating `body` directly teleports the bot locally; the server will usually correct it. Use `/tp` style commands or
  controls for legal movement.
- Reading `world` right after `Spawned` may find chunks still missing; wait until `world.loaded_columns()` is large enough.
- `Session` is generic over the transport: `Session<'a, T: Transport>`.
