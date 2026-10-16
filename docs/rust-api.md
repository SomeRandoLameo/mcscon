# Rust API

The Rust crate is the library itself; the [C](c-api.md) and [C++](cpp-api.md) APIs are a thin layer on top of it.
From Rust you get everything: the packet pump, typed packets for every Play packet, the world/entity/inventory models,
vanilla physics, the ready-made bot `Session`, your own transports, and full control over memory (`no_std`, zero
allocation).

This page is the guide. The [module reference](README.md#rust-module-reference) documents every type and function.

## Adding the crate

```toml
[dependencies]
mcscon = { path = "../mcscon" }                                   # with std: TcpTransport included
# mcscon = { path = "../mcscon", default-features = false }      # no_std: bring your own Transport
```

| Feature | Default | Effect |
|---------|---------|--------|
| `std` | on | `mcscon::posix::TcpTransport` (TCP via `std::net`). Without it the crate is `#![no_std]` and needs no allocator. |
| `capi` | off | the C ABI in `mcscon::capi` (implies `std`); only needed to build `libmcscon.a` for C/C++. |

Minimum Rust version: 1.82. The only dependency is `libm` (deterministic maths for the physics).

## Choosing a layer

```text
 Session            ready-made bot: models + automatic behaviour + 20 Hz movement     → rust/session.md
   ├─ World, EntityTracker, PlayerList, Inventory, physics   (usable on their own)    → rust/world.md …
 packets::{cb, sb}  typed structs for every Play packet, read / send                  → rust/packets.md
 Client / Link      framing, compression, handshake, login, keep-alive, raw packets   → rust/client.md
 Transport          connect / recv / send / close – the only platform dependency      → rust/transport.md
```

- **"I want a bot"** → `Session`. It is what the C and C++ APIs use.
- **"I want the protocol, my own game logic"** → `Client` + `packets`, add models as needed.
- **"I only want to ping servers"** → `Client::connect_status` (see [client](rust/client.md#status-ping-example)).

## A `Session` bot

```rust
use mcscon::client::Buffers;
use mcscon::entities::{Entity, PlayerEntry};
use mcscon::posix::TcpTransport;
use mcscon::session::{Session, SessionEvent, SessionSink, SessionStorage};
use mcscon::wire::Str;
use mcscon::world::WorldConfig;
use mcscon::{Error, Version};

struct Bot { greeted: bool }

impl SessionSink for Bot {
    fn chat(&mut self, text: &[u8], _position: u8, _json: Str<'_>) {
        println!("chat: {}", String::from_utf8_lossy(text));
    }
    fn event(&mut self, ev: SessionEvent, arg: i32) {
        println!("event {:?} ({})", ev, arg);
    }
    fn error(&mut self, e: Error, info: i32) {
        eprintln!("error {:?} ({})", e, info);
    }
}

fn main() {
    // 1. memory – the library never allocates
    let (mut rx, mut scratch, mut tx) = (vec![0u8; 128 * 1024], vec![0u8; 512 * 1024], vec![0u8; 4096]);
    let cfg = WorldConfig { max_columns: 49, store_light: false, store_biomes: false };
    let mut world_mem = vec![0u8; 512 * 1024];
    let mut entities = vec![Entity::default(); 64];
    let mut players = vec![PlayerEntry::default(); 16];
    let storage = SessionStorage { world_mem: &mut world_mem, world: cfg, entities: &mut entities, players: &mut players };

    // 2. the bot
    let mut bot = Session::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx },
                               Version::V1_12_2, storage);
    if !bot.connect("127.0.0.1", 25565, "mcscon") { return; }

    // 3. the loop
    let mut sink = Bot { greeted: false };
    let t0 = std::time::Instant::now();
    while bot.poll(&mut sink) >= 0 {
        bot.update(t0.elapsed().as_millis() as u32);
        if bot.state.spawned && !sink.greeted {
            bot.say(b"hello");
            bot.controls.forward = true;
            sink.greeted = true;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
}
```

What is going on:

- **Memory** is passed in as slices. Sizes: [Memory sizing](memory.md). `Vec`, arrays on the stack or `static mut`
  buffers all work; the world memory does not need any particular alignment.
- **`poll(&mut sink)`** reads the network, updates all models and calls your [`SessionSink`](rust/session.md#sessionsink).
  Every sink method has an empty default; `&mut ()` is a sink that ignores everything.
- **`update(now_ms)`** runs the 20 Hz ticks with *your* clock (any monotonic millisecond counter, wrapping is fine).
- **State is plain fields**: `bot.state` (health, food, game mode …), `bot.body` (position, rotation),
  `bot.world`, `bot.entities`, `bot.players`, `bot.inventory`. Write `bot.controls` to move.
- **Actions** are methods: `say`, `look_at`, `set_rotation`, `attack`, `place_block`, `break_block_instant`,
  `select_hotbar`, `use_held_item`, `respawn`, and inventory clicks through `bot.inventory.*(&mut bot.client.link, …)`.
- **Anything else** can be sent with [`packets::send(&mut bot.client.link, &sb::…)`](rust/packets.md#sending) and
  received with [`packets::read::<cb::…>(p)`](rust/packets.md#reading) inside `SessionSink::packet`.

Full program: [`examples/bot.rs`](../examples/bot.rs) (`cargo run --release --example bot`).

## Protocol only: `Client` + typed packets

```rust
use mcscon::client::{Buffers, Client, Event};
use mcscon::packets::{self, cb, sb};
use mcscon::posix::TcpTransport;
use mcscon::Version;

let (mut rx, mut scratch, mut tx) = (vec![0u8; 64 * 1024], vec![0u8; 256 * 1024], vec![0u8; 4096]);
let mut client = Client::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx }, Version::V1_12_2);
client.connect_login("127.0.0.1", 25565, "mcscon");
while client.poll(|link, event| match event {
    Event::Packet(p) => {
        if let Some(pos) = packets::read::<cb::Position>(&p) {
            packets::teleport_confirm(link, pos.teleport_id);       // a Session would do this for you
        } else if let Some(chat) = packets::read::<cb::Chat>(&p) {
            println!("{}", String::from_utf8_lossy(chat.message.as_bytes()));   // raw JSON
        }
    }
    Event::Error(e, info) => eprintln!("{:?} {}", e, info),
}) >= 0 {
    std::thread::sleep(std::time::Duration::from_millis(10));
}
```

`Client` answers keep-alives and handles the login itself; everything else is up to you. You can add the models one by
one – each has a `handle(&Packet)` method:

```rust
let mut world = mcscon::world::World::new(&mut world_mem, cfg);
// inside the poll handler:
world.handle(&p);                          // keeps chunks and block changes
```

## Rust idioms in this library

**Views and lifetimes.** Decoded packets borrow the receive buffer: `Str<'a>`, `Bytes<'a>`, `Array<'a, T>`,
`MetaView<'a>`, `Slot<'a>`. The borrow checker stops you from keeping them past the callback. Copy what you need:

```rust
struct Keep { last_chat: String }

impl SessionSink for Keep {
    fn chat(&mut self, text: &[u8], _position: u8, _json: Str<'_>) {
        self.last_chat = String::from_utf8_lossy(text).into_owned();   // copied out of the receive buffer
    }
}
```

**Sinks instead of closures.** `Session::poll` takes a trait object with many optional methods
(`SessionSink`); `Client::poll` takes a closure `FnMut(&mut Link, Event)`. In both, the sending half (`link`) is
available, so you can answer packets immediately.

**`Sender`.** Every function that sends is generic over the [`Sender`](rust/client.md#sender) trait, implemented by
`Link` and `Client`. In a session use `&mut bot.client.link`; inside a `Client::poll` handler use the `link` argument;
in tests use your own implementation.

**No panics, no `Result` for network input.** Decoders return `Option` (`None` = malformed or other packet). Senders
return `bool` (`false` = not queued). Asynchronous errors arrive as `Event::Error` / `SessionSink::error`.

**`no_std`.** With `default-features = false` everything except `TcpTransport` and the C ABI is available. Supply a
[`Transport`](rust/transport.md) and a millisecond clock; that is the whole platform layer.

## Equivalents across the three languages

| Task | C | C++ | Rust (`Session`) |
|------|---|-----|------------------|
| create | `mc_bot_new(MC_V1_12_2)` | `mcscon::Bot bot(MC_V1_12_2)` | `Session::new(transport, buffers, Version::V1_12_2, storage)` |
| connect | `mc_bot_connect(b, host, port, name)` | `bot.connect(host, port, name)` | `bot.connect(host, port, name)` |
| main loop | `mc_bot_poll(b) >= 0` | `bot.poll()` | `bot.poll(&mut sink) >= 0` **and** `bot.update(now_ms)` |
| chat in | `mc_bot_on_chat(b, fn, user)` | `bot.onChat(fn)` | `SessionSink::chat` |
| chat out | `mc_bot_say(b, "hi")` | `bot.say("hi")` | `bot.say(b"hi")` |
| in world? | `mc_bot_spawned(b)` | `bot.spawned()` | `bot.state.spawned` |
| health | `mc_bot_health(b)` | `bot.health()` | `bot.state.health` |
| position | `mc_bot_position(b, xyz)` | `bot.position(xyz)` | `bot.body.x / y / z` |
| walk | `mc_bot_walk(b, true)` | `bot.walk(true)` | `bot.controls.forward = true` |
| disconnect | `mc_bot_disconnect(b)` | `bot.disconnect()` | `bot.disconnect()` |
| free | `mc_bot_free(b)` | destructor | drop (memory is yours) |

The C/C++ `poll` calls Rust's `poll` and `update` together, with an internal `std::time::Instant` clock.
