# mcscon

A minimal, embeddable **Minecraft: Java Edition client** protocol library (1.10 – 1.12.2, offline mode), written in
**Rust** and usable from **Rust**, **C** and **C++**.

- **Tiny and portable** – the protocol core is `#![no_std]`, never allocates (you hand in the buffers) and has one
  dependency (`libm`). The whole network layer is four functions: `connect / recv / send / close`.
- **Zero-copy** – strings, item NBT, arrays and entity metadata are views into the receive buffer.
- **Layered** – use only the packet pump and typed packets, or add world, entities, inventory, physics and a ready-made
  bot `Session`.
- **Generated, not hand-typed** – packet ids and all 80 clientbound / 34 serverbound layouts come from
  [PrismarineJS/minecraft-data](https://github.com/PrismarineJS/minecraft-data) via `tools/`.
- **Safe on bad input** – malformed packets are rejected, never panicked on; the decoders are checked against
  thousands of reference packets.

> Status: offline-mode login only. Online mode (encryption + Microsoft auth) and more protocol versions are planned,
> see [PLAN.md](PLAN.md).

## Documentation

Full documentation is in [`docs/`](docs/README.md):

| | |
|-|-|
| Start | [Getting started](docs/getting-started.md) (Rust, C, C++) · [Concepts](docs/concepts.md) · [Memory sizing](docs/memory.md) · [Troubleshooting](docs/troubleshooting.md) |
| Language guides | [C API](docs/c-api.md) · [C++ API](docs/cpp-api.md) · [Rust API](docs/rust-api.md) |
| Rust module reference | [client](docs/rust/client.md) · [transport](docs/rust/transport.md) · [packets](docs/rust/packets.md) · [wire](docs/rust/wire.md) · [session](docs/rust/session.md) · [world](docs/rust/world.md) · [entities](docs/rust/entities.md) · [inventory](docs/rust/inventory.md) · [physics](docs/rust/physics.md) · [chat & NBT](docs/rust/chat-nbt.md) · [internals](docs/rust/internals.md) |
| Reference | [Packet reference](docs/packet-reference.md) (generated) · [Testing and code generation](docs/testing-and-codegen.md) |

## Examples

One bot, three languages, the same steps: join, print chat, say hello, walk forward for 3 seconds, print the position.

| Language | File | Run |
|----------|------|-----|
| Rust | [`examples/bot.rs`](examples/bot.rs) | `cargo run --release --example bot -- 127.0.0.1 25565` |
| C | [`examples/c/bot.c`](examples/c/bot.c) | `make -C examples && examples/build/bot_c 127.0.0.1 25565` |
| C++ | [`examples/cpp/bot.cpp`](examples/cpp/bot.cpp) | `make -C examples && examples/build/bot_cpp 127.0.0.1 25565` |

The C and C++ examples use the small C API in [`include/mcscon.h`](include/mcscon.h) (about a dozen functions) and the
C++ wrapper [`include/mcscon.hpp`](include/mcscon.hpp). The Rust example uses the native API with fully caller-owned
memory. Start any offline-mode vanilla 1.12.2 server (`online-mode=false`) to try them.

The C API (`--features capi`, see `src/capi.rs`) allocates its buffers for convenience. For fixed, static memory use the
Rust API directly.

## Layers (Rust API)

| Module | What you get |
|--------|--------------|
| `client`, `inflate`, `transport`, `wire`, `ids` | handshake, status ping, offline login, framing, compression, keep-alive, packet dispatch |
| `packets::{cb,sb}` (generated) | typed decoders for all clientbound play packets, typed encoders for all serverbound ones, version-aware |
| `world`, `entities`, `inventory`, `physics`, `chat`, `nbt` | chunk model, entity tracker + tab list, inventory/windows/clicks, vanilla physics with exact collision shapes, chat JSON to text, NBT reader |
| `session` | a high-level bot: player state, teleport/respawn/resource-pack handling, 20 Hz movement |
| `posix` | TCP transport on `std::net` |

Only need the protocol? Use `Client` and decode what you care about:

```rust
use mcscon::{client::Event, packets::{self, cb, sb}, wire::Str};

client.poll(|link, event| {
    if let Event::Packet(p) = event {
        if let Some(chat) = packets::read::<cb::Chat>(&p) { /* chat.message is a view into the receive buffer */ }
    }
});
packets::send(&mut client.link, &sb::Chat { message: Str(b"hi") });
```

Buffers: `rx` must hold your largest wire packet, `scratch` your largest *decompressed* packet (unused without
compression). Packets that do not fit are skipped and reported as `Error::PacketTooLarge`; the connection stays healthy.

## Porting

Implement `mcscon::transport::Transport`: a blocking `connect`, non-blocking `recv`/`send` returning byte counts
(`0` = would block, `Err` = closed) and `close`. That is the entire platform dependency; nothing in the library reads
a clock, a file or a socket on its own.

## Tests

```
cargo test                    # unit tests, wire-vector tests, world/inventory/physics/session scenarios; no network
```

`tests/data/*.bin` are language-neutral wire vectors made by an independent Python reference encoder (random valid
clientbound packets for every version, plus zlib streams); `tests/vectors.rs` requires every decoder to consume them
exactly and to survive truncation and corruption.

## Regenerating the generated code

Needs a checkout of [minecraft-data](https://github.com/PrismarineJS/minecraft-data) (`MD=<checkout>/data/pc`):

```
python3 tools/gen_ids.py     $MD > src/ids.rs
(cd tools && python3 gen_packets_rs.py $MD ../src/packets)
python3 tools/gen_blocks.py  $MD > src/blocks.rs
python3 tools/gen_vectors.py $MD tests/data/packet_vectors.bin
python3 tools/gen_testvec.py tests/data/zlib_vectors.bin
```

## License

MIT, see [LICENSE](LICENSE).
