# Concepts

This page explains how mcscon works. The rules apply to all three languages; where they differ, the C, C++ and Rust
forms are shown side by side.

## What the library is (and is not)

mcscon implements the **client side** of the Minecraft: Java Edition network protocol for versions 1.10 – 1.12.2:
connecting, logging in (offline mode), compression, keep-alive, decoding every server packet, encoding every client
packet, and optional game models on top (world, entities, inventory, physics, a ready-made bot).

It is **not** a game client: no rendering, no sound, no authentication against Mojang/Microsoft (online-mode servers
are rejected with `Error::EncryptionRequired`), and only protocol versions 1.10 – 1.12.2.

## Layers

You can use as little or as much as you need. Each layer only depends on the ones above it in this list.

1. **Transport** ([`Transport`](rust/transport.md)) – four functions that move bytes: `connect`, `recv`, `send`, `close`.
2. **Client** ([`Client`](rust/client.md)) – framing, compression, handshake, login, keep-alive, and delivery of every
   received packet to your callback; also sending.
3. **Packets** ([`packets`](rust/packets.md)) – typed structs for every packet (`cb::Chat`, `sb::BlockPlace`, …) with
   `read` and `send`.
4. **Models** – [`World`](rust/world.md), [`EntityTracker`](rust/entities.md) / [`PlayerList`](rust/entities.md),
   [`Inventory`](rust/inventory.md), [physics](rust/physics.md): each consumes packets and keeps state.
5. **Session** ([`Session`](rust/session.md)) – wires all models together with the automatic behaviour a bot needs
   (teleport confirmations, respawn, client settings, resource-pack replies, 20 Hz movement).
6. **C API / C++ wrapper** ([C](c-api.md), [C++](cpp-api.md)) – one `Session` behind an opaque handle, with its memory
   allocated for you.

Rust can use any layer. C and C++ use layer 6 (and through it, everything below).

## The polling model

There are no threads, no callbacks from other threads and no hidden timers. Your program owns the main loop and calls
the library regularly:

```text
loop:
    poll        read the network, handle every packet that arrived, call your callbacks
    update      run the 20 Hz game ticks that are due (physics, movement packets)
    your logic  read state, say something, set controls, click the inventory …
    sleep a few milliseconds
```

<table>
<tr><th>C</th><th>C++</th><th>Rust</th></tr>
<tr><td>

```c
while (mc_bot_poll(bot) >= 0) {
    /* your logic */
    usleep(10000);
}
```

</td><td>

```cpp
while (bot.poll()) {
    // your logic
    std::this_thread::sleep_for(
        std::chrono::milliseconds(10));
}
```

</td><td>

```rust
while bot.poll(&mut sink) >= 0 {
    bot.update(t0.elapsed().as_millis() as u32);
    // your logic
    std::thread::sleep(Duration::from_millis(10));
}
```

</td></tr>
</table>

- In C and C++, `poll` does both steps (it has an internal clock). In Rust, `Session::poll` and `Session::update` are
  separate so that you supply the clock – important on platforms without `std`.
- `poll` **never blocks**: if nothing arrived it returns at once. `connect` is the only blocking call (it opens the
  TCP connection). Run the bot on its own thread if your UI must not freeze while connecting.
- Call `poll` at least every few hundred milliseconds. The server sends keep-alives and kicks clients that do not
  answer within 30 seconds; the library answers them inside `poll`.
- Every 5 – 50 ms is a good rate. Movement stays at the correct speed at any rate (missed ticks are caught up, at most
  5 per call).

## Threads

Everything runs on the thread that calls it. A bot (`McBot*`, `mcscon::Bot`, `Session`) is **not thread-safe**: use it
from one thread, or protect every call with your own lock. Many bots on many threads are fine – they share nothing.
In Rust the compiler enforces this: a `Session` can be moved to another thread (if its transport can), but not shared
between threads.

## Callbacks and events

Everything the library reports is delivered **during `poll`**, on your thread:

| | C | C++ | Rust |
|-|---|-----|------|
| chat | `mc_bot_on_chat(bot, fn, user)` | `bot.onChat(lambda)` | `SessionSink::chat` |
| every packet | – | – | `SessionSink::packet`, or the closure of `Client::poll` |
| events (joined, spawned, died, kicked …) | – | – | `SessionSink::event` |
| errors | – (`poll` returns −1) | – (`poll` returns `false`) | `SessionSink::error`, `Event::Error` |
| entities, tab list, inventory, block entities | – | – | the other `SessionSink` methods |

Inside a callback you may send (`say`, …) and read state. Do not free, move or `poll` the bot from inside its own
callback.

## Zero-copy and lifetimes

Decoded data is not copied out of the receive buffer: text, NBT, arrays and entity metadata are **views** into it.
The buffer is reused for the next packet, so a view is valid **only inside the callback that received it**.

| | What is a view | How to keep it |
|-|----------------|----------------|
| C | the `text` argument of the chat callback | `strdup(text)` or copy into your own buffer |
| C++ | the `text` argument of `onChat` | `std::string copy(text);` |
| Rust | `Str`, `Bytes`, `Array`, `MetaView`, `Slot::nbt`, `&[u8]` arguments of sinks | `String::from_utf8_lossy(s.as_bytes()).into_owned()`, `.to_vec()` |

In Rust the borrow checker enforces this; in C and C++ it is your responsibility.

## Memory

The protocol core never allocates. It works on buffers that are handed in:

| Buffer | Purpose |
|--------|---------|
| `rx` | receive window; must hold the biggest packet *as sent on the wire* you want to see |
| `scratch` | inflate target for compressed packets; must hold the biggest packet *after decompression* |
| `tx` | send queue |
| world memory | chunk columns + block data arena |
| entity / player tables | fixed arrays |

- **C / C++**: `mc_bot_new` / `mcscon::Bot` allocates all of this once (~0.9 MB) and frees it at the end.
- **Rust**: you allocate the buffers (heap, stack or static) and decide every size.

Packets that do not fit are **skipped** (streamed away without being buffered) and reported as
`Error::PacketTooLarge`; the connection stays healthy. A full world or entity table drops new data instead of failing.
Numbers: [Memory sizing](memory.md).

## Versions

| Minecraft | Protocol | C / C++ | Rust |
|-----------|----------|---------|------|
| 1.10 – 1.10.2 | 210 | `MC_V1_10` | `Version::V1_10` |
| 1.11 | 315 | `MC_V1_11` | `Version::V1_11` |
| 1.11.1 / 1.11.2 | 316 | `MC_V1_11_2` | `Version::V1_11_2` |
| 1.12 | 335 | `MC_V1_12` | `Version::V1_12` |
| 1.12.1 | 338 | `MC_V1_12_1` | `Version::V1_12_1` |
| 1.12.2 | 340 | `MC_V1_12_2` | `Version::V1_12_2` |

The version is fixed when the bot is created and must match the server. Packet ids and layouts differ between
versions; the library hides this. Typed packets are the union of all layouts, and `Version` selects the right one at
run time. Some packets exist only from a version on (for example `Advancements`, `UnlockRecipes`,
`CraftRecipeRequest` from 1.12); sending a packet the version does not know makes `send` return `false`. Receiving an
unknown id yields a `Packet` whose `id` is `0xFF` (see [client](rust/client.md#packet)).

## Errors

**Calls** that can fail return `bool` (`true` = success) or a count / `-1`:

| Situation | C | C++ | Rust |
|-----------|---|-----|------|
| TCP connection failed | `mc_bot_connect` → `false` | `connect` → `false` | `connect` → `false` |
| connection closed | `mc_bot_poll` → `-1` | `poll` → `false` | `poll` → `-1` |
| packet not queued | `mc_bot_say` → `false` | `say` → `false` | `say` / `packets::send` → `false` |
| unknown version | `mc_bot_new` → `NULL` | constructor throws | (cannot happen: `Version` is an enum) |
| malformed packet | ignored | ignored | `packets::read` → `None` |

**Asynchronous problems** are reported as `Error` values in Rust (through `SessionSink::error` or `Event::Error`). C and
C++ only see the effect (the connection closes or a packet is skipped).

| `Error` | Meaning | Connection |
|---------|---------|------------|
| `Transport` | the transport reported a failure | closed |
| `Protocol` | malformed frame, packet header or login data | usually closed |
| `Overflow` | an outgoing packet did not fit into `tx` (or the connection was already closed); that packet was not sent | stays open |
| `PacketTooLarge` | a packet did not fit `rx` / `scratch`; it was skipped (the info value is its length) | stays open |
| `Inflate` | corrupt compressed data | closed |
| `Disconnected` | the server kicked you (play) or refused the login | closed |
| `EncryptionRequired` | the server is in online mode; not supported | closed |
| `Backpressure` | reserved; not raised by the current version | – |
| `None` | placeholder value, never reported | – |

Malformed input never panics and never reads out of bounds.

## Automatic behaviour

Handled for you in all three languages (in Rust you can still observe the packets):

- **Keep-alive**: answered inside `poll` (Rust: switch off with `Client::auto_keepalive(false)`).
- **Compression**: `SetCompression` during login enables it; received compressed frames are inflated. Sending always
  uses the "uncompressed" form, which every server accepts.
- **Session** (so also C and C++): teleport confirmation after `Position`, client settings and the `MC|Brand` plugin
  message after `Login`, resource-pack replies, automatic respawn after death, 20 Hz position/look/on-ground packets.
  Details: [What the session does for you](rust/session.md#what-the-session-does-for-you).

## Offline mode and security

Offline mode sends only a username; the server believes whatever name you send. Do not use it against servers you do
not control. Online mode (AES encryption, Mojang/Microsoft authentication) is not implemented (see `PLAN.md`).
