# mcscon documentation

mcscon is a small Minecraft: Java Edition **client** library (protocol 1.10 – 1.12.2, offline-mode login). The core is
written in Rust and is usable from **Rust**, **C** and **C++**.

## Start here

| I want to … | Read |
|-------------|------|
| run a bot in five minutes (any language) | [Getting started](getting-started.md) |
| understand how the library works: polling, callbacks, lifetimes, errors | [Concepts](concepts.md) |
| decide how much memory to give it | [Memory sizing](memory.md) |
| fix "it does not connect / does not move / does not link" | [Troubleshooting](troubleshooting.md) |

## Pick your language

| Language | Guide | Header / crate | What you get |
|----------|-------|----------------|--------------|
| C (C99) | [C API](c-api.md) | `include/mcscon.h` + `libmcscon.a` | an opaque bot object (`McBot`) and 11 functions: connect, poll, chat, walk, position, health |
| C++ (C++11) | [C++ API](cpp-api.md) | `include/mcscon.hpp` (header only) + `libmcscon.a` | the C bot wrapped in RAII (`mcscon::Bot`) with a `std::function` chat callback |
| Rust (1.82+) | [Rust API](rust-api.md) | crate `mcscon` | the whole library: packet pump, typed packets, world, entities, inventory, physics, bot `Session`, custom transports, `no_std`, fixed memory |

### What is available where

| Feature | C | C++ | Rust |
|---------|:-:|:---:|:----:|
| Offline-mode login, keep-alive, compression | ✓ | ✓ | ✓ |
| Receive chat as plain text | ✓ | ✓ | ✓ |
| Send chat and commands | ✓ | ✓ | ✓ |
| Walk forward, read position and health | ✓ | ✓ | ✓ |
| Full movement controls (strafe, jump, sneak, sprint), look direction | – | – | ✓ |
| Server list ping (status) | – | – | ✓ |
| Typed access to all 80 clientbound / 34 serverbound packets | – | – | ✓ |
| World blocks, entities, tab list, inventory and windows | – | – | ✓ |
| Events (spawned, died, kicked, …) and error codes | – | – | ✓ |
| Your own transport (other OS, console, TLS tunnel …) | – | – | ✓ |
| Caller-owned / static memory, `no_std` | – (allocates ~0.9 MB) | – | ✓ |

C and C++ cover the common "bot that chats and walks" case. Everything else is reachable from C/C++ by adding a few
lines to the C API – see [Extending the C API](c-api.md#extending-the-c-api).

## Rust module reference

| Page | Covers |
|------|--------|
| [client](rust/client.md) | `Client`, `Link`, `Buffers`, `Packet`, `Event`, `Sender`, `Version`, `State`, `Error` – the packet pump |
| [transport](rust/transport.md) | the `Transport` trait, `TcpTransport`, porting to a new platform |
| [packets](rust/packets.md) | the typed packet layer: `packets::read`, `packets::send`, `ClientPacket` / `ServerPacket`, helper functions |
| [wire](rust/wire.md) | `Reader`, `Writer`, `Str`, `Bytes`, `Slot`, `Array`, `MetaView`, `BlockPos`, `Uuid`, NBT skipping |
| [session](rust/session.md) | `Session`: the high-level bot, `PlayerState`, `SessionSink`, `SessionEvent` |
| [world](rust/world.md) | `World`, `WorldConfig`, block states, collision shapes (`blocks`) |
| [entities](rust/entities.md) | `EntityTracker`, `Entity`, `PlayerList`, sinks |
| [inventory](rust/inventory.md) | `Inventory`, `Window`, slot constants, clicks |
| [physics](rust/physics.md) | `Body`, `Controls`, `physics_tick`, `physics_move` |
| [chat & NBT](rust/chat-nbt.md) | `chat_to_text`, the zero-copy `nbt` reader |
| [internals](rust/internals.md) | `Arena`, zlib inflate, id tables, feature flags, the C ABI module |

`cargo doc --open` builds the same API as rustdoc pages.

## Reference

- [Packet reference](packet-reference.md) – every clientbound and serverbound packet: purpose, fields, wire id per version (generated).
- [Testing and code generation](testing-and-codegen.md) – how the library is verified (Rust tests and C/C++ examples) and how the generated files are produced.
