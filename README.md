# mcscon

A flexible and lightweight Minecraft protocol library.

mcscon is a minimal, embeddable C++ implementation of the Minecraft: Java Edition **client** protocol
(currently **1.10 – 1.12.2**, offline mode). It is designed for tiny footprints and easy porting: no
dependencies (no curl, no OpenSSL, no zlib), no heap allocation, no exceptions, no RTTI, C++11.

- **Portable by construction** – the whole network layer is four function pointers (`connect/recv/send/close`).
- **Zero allocation** – you hand in the buffers; everything else lives in fixed-size tables you size yourself.
- **Zero-copy decoding** – strings, item NBT, arrays and entity metadata are views into the receive buffer.
- **Layered** – use only the raw packet pump, or add world / entities / inventory / physics / a ready-made `Session`.
- **Generated, not hand-typed** – packet ids and all packet layouts come from
  [PrismarineJS/minecraft-data](https://github.com/PrismarineJS/minecraft-data) via the scripts in `tools/`.

> Status: offline-mode login only. Online mode (encryption + Microsoft auth), other protocol versions and
> platform ports beyond POSIX sockets are planned, see [PLAN.md](PLAN.md).

## Layers

| Layer | Files | What you get |
|-------|-------|--------------|
| Core (~9 KB code) | `client.h/.cpp`, `zlib.cpp`, `transport.h`, `buffer.h` | handshake, status ping, offline login, framing, compression (inflate), keep-alive, packet dispatch |
| Packets (header-only) | `packets_cb.h`, `packets_sb.h`, `packets.h`, `wire.h` | typed decoders for all 80 clientbound play packets, typed encoders for all 34 serverbound ones, version-aware |
| `libmcscon_modules` (optional) | `world.h`, `entities.h`, `inventory.h`, `physics.h`, `chat.h`, `nbt.h`, `session.h` | chunk/world model, entity tracker + tab list, inventory/windows/clicks, vanilla physics with exact collision shapes, chat JSON to text, NBT reader, a high-level bot `Session` |
| Platform (optional) | `platform/posix_transport.*` | BSD-socket transport |

## Quick start

```cpp
#include <mcscon/session.h>
#include "platform/posix_transport.h"
using namespace mc;

static u8 rx[128 * 1024], scratch[512 * 1024], tx[4096];
static StaticStorage<49, 256 * 1024, 64, 16> storage;   // 49 chunk columns, 256 KB chunk arena, 64 entities, 16 tab-list entries

int main() {
    PosixSocket sock;
    Buffers buffers = {rx, sizeof rx, scratch, sizeof scratch, tx, sizeof tx};
    Session bot(makePosixTransport(&sock), buffers, Version::V1_12_2, storage.get());

    bot.onChat([](void*, const char* text, u8, StrView) { printf("chat: %s\n", text); }, 0);
    if (!bot.connect("127.0.0.1", 25565, "mcscon")) return 1;

    while (bot.poll() >= 0) {              // pump the network
        bot.update(nowMs());               // 20 Hz physics + position packets (nowMs(): your clock)
        // bot.say("hello!"); bot.controls.forward = true; bot.world().blockId(x, y, z); ...
    }
}
```

Only need the protocol? Use `Client` directly and decode what you care about:

```cpp
void onPacket(void*, const Packet& p) {
    cb::Chat chat;
    if (pkt::read(p, chat)) { /* chat.message is a StrView into the receive buffer */ }
}
// sending:
sb::Chat msg = {}; msg.message = StrView{"hi", 2}; pkt::send(client, msg);
```

Buffers: `rx` must hold your largest wire packet, `scratch` your largest *decompressed* packet (not needed without
compression). Packets that do not fit are skipped and reported as `Error::PacketTooLarge`, the connection stays healthy.

## Porting

Implement `mc::Transport` (see `include/mcscon/transport.h`): a blocking `connect`, non-blocking `recv`/`send`
returning byte counts (`0` = would block, `<0` = closed) and `close`. That is the entire platform dependency.
Nothing in the library reads a clock, a file or a socket on its own.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build        # unit tests, no network needed
```

Or just copy `include/` and the `src/*.cpp` you need into your own build system.

### Integration tests against real servers

The integration tests drive real vanilla servers (1.10.2, 1.11, 1.11.2, 1.12, 1.12.1, 1.12.2) that you download
yourself from Mojang. Accepting the Mojang EULA is up to you. Put each `server.jar` into `testserver/<version>/`, set
`eula=true` there, and run them only on loopback (the provided `server.properties` bind to `127.0.0.1`,
`online-mode=false`).

```
python3 tools/verify_all.py            # build + unit tests + per version: scenario, packet coverage, edge cases
python3 tools/integ_session.py 1.12.2  # world, physics, entities, inventory, windows, chat, death/respawn
python3 tools/integ_coverage.py 1.12.2 # two bots, ~100 console commands, every clientbound packet is decoded
```

Last full run: 28/28 steps passed. The coverage runs decoded 74-78 of the 80 clientbound packet types from real
traffic without a single decode error; `Entity` and `PlayerlistHeader` are never sent by vanilla and are covered by
synthetic vectors only.

### Regenerating the generated headers

```
python3 tools/gen_ids.py     <minecraft-data>/data/pc > include/mcscon/ids_gen.h
python3 tools/gen_packets.py <minecraft-data>/data/pc include/mcscon
python3 tools/gen_blocks.py  <minecraft-data>/data/pc > include/mcscon/blocks_gen.h
python3 tools/gen_vectors.py <minecraft-data>/data/pc > tests/packet_vectors.h
```

## Limitations

- Offline mode only (no encryption, no Microsoft/Mojang authentication yet).
- Protocol 1.10 – 1.12.2 only. Tested against vanilla servers only (not Spigot/Paper).
- Only a POSIX transport is provided; `connect()` is blocking. Other platforms (Windows, 3DS, lwIP) need a transport.
- Built and tested with Clang on macOS (arm64) so far.
- Compressed packets must fit completely into the receive buffer (no streaming inflate).
- Several packet families are decoded but not modelled (scoreboard, boss bars, titles, maps, advancements, ...);
  they reach you through the packet callback. Item NBT is not stored, only exposed inside callbacks.

## License

MIT, see [LICENSE](LICENSE).
