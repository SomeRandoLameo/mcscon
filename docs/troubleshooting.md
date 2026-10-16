# Troubleshooting

Problems and their usual causes, for all three languages. In Rust, the first step is always to implement
`SessionSink::error` and `SessionSink::event` and print what arrives – most questions answer themselves.

## Connecting and login

| Symptom | Cause | Fix |
|---------|-------|-----|
| `connect` returns `false` | no TCP connection: wrong host/port, server not running, firewall | check with `nc -vz host port` |
| `connect` succeeds, then `poll` returns −1 (C: `-1`, C++: `false`) right away | the server closed the connection during login | see the next rows |
| … and the server log says "Outdated client" / "Outdated server" | protocol version does not match the server | use the matching `MC_V1_*` / `Version` ([table](concepts.md#versions)) |
| … Rust reports `Error::EncryptionRequired` | the server runs in online mode | set `online-mode=false` in `server.properties`; online mode is not supported |
| … Rust reports `Error::Disconnected` | kicked during login: banned, whitelist, name already online, invalid name (max. 16 characters, letters, digits and `_`) | check the server log |
| connected, but kicked after ~30 s ("Timed out") | `poll` is not called often enough | call it every 5 – 50 ms, never block the loop for long |
| `connect` hangs for a long time | the TCP connect blocks until the OS gives up (unreachable host) | run the bot on its own thread, or check reachability first |

## In the world

| Symptom | Cause | Fix |
|---------|-------|-----|
| `spawned` stays `false` | login not finished yet, or the server is still preparing the spawn area | keep polling; it usually takes 0.1 – 2 s after connect |
| walking does nothing | not spawned yet; or the chunk under the bot has not arrived (physics waits for it); or the bot is dead | wait for `spawned`; in Rust check `bot.world.is_loaded(x, y, z)` |
| Rust: walking does nothing although spawned | `update()` is not called | call `bot.update(now_ms)` every loop iteration |
| the bot walks, but the server pulls it back ("moved wrongly") | the client-side world differs from the server (missing chunks), or `body` was changed directly | move only through `controls`; give the world enough memory (Rust: watch `arena_used()` / `loaded_columns()`) |
| blocks read as air (Rust) | that column is not loaded or was dropped because the table/arena was full | larger `max_columns` / arena, see [Memory sizing](memory.md#world-memory) |
| entities missing (Rust) | entity table full | larger `entities` slice; `EntityTracker::dropped()` counts the losses |
| chat text cut off | text longer than 511 bytes is truncated in the flattened form | Rust: `SessionSink::chat` also gets the full JSON |
| a command has no effect | the bot lacks permission | `op mcscon` in the server console |

## Building and linking (C / C++)

| Symptom | Cause | Fix |
|---------|-------|-----|
| `undefined reference to mc_bot_new` | `libmcscon.a` missing on the link line, or built without `--features capi` | rebuild with `cargo rustc --release --lib --features capi --crate-type staticlib`; put the `.a` **after** your source/object files |
| `undefined reference to dlsym / pthread_* / sin` | the native libraries the Rust standard library needs | add `-lm -lpthread -ldl` (Linux); exact list: `cargo rustc … -- --print native-static-libs` |
| `mcscon.h: No such file` | include path | `-I<repo>/include` |
| changes to `src/` have no effect | the static library was not rebuilt | rerun the `cargo rustc` line (`make -C examples` does it automatically) |
| C++: `use of deleted function mcscon::Bot::Bot(const Bot&)` | `Bot` cannot be copied or moved | keep it in place or use `std::unique_ptr<mcscon::Bot>` ([C++ API](cpp-api.md#ownership-and-copying)) |
| C++: crash / `terminate` from inside `poll()` | an exception escaped the chat callback | catch inside the lambda ([C++ API](cpp-api.md#exceptions)) |
| C: garbage in a saved chat string | `text` was stored as a pointer and used after the callback returned | copy it ([Concepts](concepts.md#zero-copy-and-lifetimes)) |

## Rust specifics

| Symptom | Cause | Fix |
|---------|-------|-----|
| "borrowed value does not live long enough" with `Str` / `Bytes` | keeping a view beyond the callback | copy: `String::from_utf8_lossy(s.as_bytes()).into_owned()` |
| `packets::read::<cb::X>` always `None` | it is a different packet, or it was malformed; or it is not a Play packet | check `p.play()`; for login/status use `p.login()` / `p.status()` and read with `p.r` |
| `packets::send` returns `false` | the packet does not exist in this version, `tx` is full, or the connection is closed | check `link.take_error()`; increase `tx` |
| `Error::PacketTooLarge` | `rx` or `scratch` too small for a packet (usually chunks) | harmless if you do not need that packet; otherwise enlarge, see [Memory sizing](memory.md) |
| `TcpTransport` not found | built with `default-features = false` | enable the `std` feature or bring your own [`Transport`](rust/transport.md) |
