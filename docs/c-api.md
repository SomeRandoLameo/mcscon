# C API

Header: [`include/mcscon.h`](../include/mcscon.h) (plain C99, also usable from C++). Implementation:
[`src/capi.rs`](../src/capi.rs), compiled into `libmcscon.a` with the cargo feature `capi`.

The C API wraps the high-level Rust [`Session`](rust/session.md) in one opaque object, `McBot`: join a server, receive
chat, send chat and commands, walk, read position and health. It allocates and owns its buffers. Typed packets, blocks,
entities and the inventory are not exposed by default; [Extending the C API](#extending-the-c-api) shows how to add
what you need in a few lines.

## Building and linking

### 1. Build the static library

```bash
cargo rustc --release --lib --features capi --crate-type staticlib
```

This produces `target/release/libmcscon.a` (about 6 MB; the linker keeps only what is used – the example bot is about
1.8 MB, 1.3 MB stripped, on macOS). Build it once per target; for cross-compiling add
`--target <triple>`.

### 2. Compile and link your program

```bash
cc -std=c99 -Iinclude my_bot.c target/release/libmcscon.a -lm -lpthread -ldl -o my_bot
```

The extra libraries are what the Rust standard library needs on the target. Ask the compiler for the exact list:

```bash
cargo rustc --release --lib --features capi --crate-type staticlib -- --print native-static-libs
```

| Platform | Extra link flags |
|----------|------------------|
| Linux (glibc) | `-lm -lpthread -ldl` (newer glibc: `-lm` is enough, the others are harmless) |
| macOS | none needed (`-lm` is harmless) |
| other | use the `--print native-static-libs` output |

### With make

[`examples/Makefile`](../examples/Makefile) builds the library and both examples: `make -C examples`.

### With CMake

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_bot C)

set(MCSCON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mcscon)     # where the repository is
set(MCSCON_LIB ${MCSCON_DIR}/target/release/${CMAKE_STATIC_LIBRARY_PREFIX}mcscon${CMAKE_STATIC_LIBRARY_SUFFIX})

add_custom_command(OUTPUT ${MCSCON_LIB}
  COMMAND cargo rustc --release --lib --features capi --crate-type staticlib
  WORKING_DIRECTORY ${MCSCON_DIR})
add_custom_target(mcscon_build DEPENDS ${MCSCON_LIB})

add_library(mcscon STATIC IMPORTED)
set_target_properties(mcscon PROPERTIES
  IMPORTED_LOCATION ${MCSCON_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${MCSCON_DIR}/include)
add_dependencies(mcscon mcscon_build)
if(UNIX AND NOT APPLE)
  target_link_libraries(mcscon INTERFACE m pthread dl)
endif()

add_executable(my_bot main.c)
target_link_libraries(my_bot PRIVATE mcscon)
```

The custom command only runs when the library file is missing; run the `cargo rustc` line again (or delete the file)
after changing the Rust sources.

## Example

```c
#include "mcscon.h"
#include <stdio.h>
#include <unistd.h>

static void on_chat(void* user, const char* text, int position) {
    (void)user;
    printf("[%d] %s\n", position, text);
}

int main(void) {
    McBot* bot = mc_bot_new(MC_V1_12_2);
    if (!bot) return 1;
    mc_bot_on_chat(bot, on_chat, NULL);
    if (!mc_bot_connect(bot, "127.0.0.1", 25565, "mcscon")) { mc_bot_free(bot); return 1; }

    int greeted = 0;
    while (mc_bot_poll(bot) >= 0) {                 /* main loop: -1 = disconnected */
        if (!greeted && mc_bot_spawned(bot)) {
            mc_bot_say(bot, "hello");
            mc_bot_walk(bot, true);
            greeted = 1;
        }
        usleep(10000);                              /* 10 ms */
    }
    mc_bot_free(bot);
    return 0;
}
```

A fuller program: [`examples/c/bot.c`](../examples/c/bot.c).

## Reference

### Types and constants

```c
typedef struct McBot McBot;      /* opaque; created by mc_bot_new, destroyed by mc_bot_free */

enum { MC_V1_10, MC_V1_11, MC_V1_11_2, MC_V1_12, MC_V1_12_1, MC_V1_12_2 };

typedef void (*McChatFn)(void* user, const char* text, int position);
```

| Constant | Value | Server versions | Protocol number |
|----------|-------|-----------------|-----------------|
| `MC_V1_10` | 0 | 1.10 – 1.10.2 | 210 |
| `MC_V1_11` | 1 | 1.11 | 315 |
| `MC_V1_11_2` | 2 | 1.11.1, 1.11.2 | 316 |
| `MC_V1_12` | 3 | 1.12 | 335 |
| `MC_V1_12_1` | 4 | 1.12.1 | 338 |
| `MC_V1_12_2` | 5 | 1.12.2 | 340 |

### Lifetime

#### `McBot* mc_bot_new(int version)`

Creates a bot for a protocol version (`MC_V1_*`). Allocates all memory the bot will ever use, about 0.9 MB
(see [Memory sizing](memory.md#the-c-and-c-api)). Nothing is connected yet.
Returns `NULL` for an unknown version.

#### `void mc_bot_free(McBot* bot)`

Closes the connection (if open) and frees everything. `NULL` is allowed and does nothing. The pointer must not be used
afterwards, and must not be freed from inside the chat callback.

### Callbacks

#### `void mc_bot_on_chat(McBot* bot, McChatFn fn, void* user)`

Registers the chat callback, replacing an earlier one; `fn = NULL` removes it. `user` is passed back to every call
unchanged (use it for your own context struct).

The callback `fn(user, text, position)` is called **from inside `mc_bot_poll`**, on the thread that calls
`mc_bot_poll`, once per chat message:

| Argument | Meaning |
|----------|---------|
| `text` | the message flattened to plain UTF-8: JSON text components and common vanilla translations (`chat.type.text`, join/leave, deaths, `/say`, `/me` …) resolved, `§` formatting codes stripped. NUL-terminated, at most 511 bytes (longer text is cut). **Valid only during the call** – copy it if you need it later. |
| `position` | `0` player chat, `1` system message (command feedback, join/leave), `2` action bar (above the hotbar) |

From inside the callback you may call `mc_bot_say`, `mc_bot_walk`, `mc_bot_disconnect` and the state getters. Do not
call `mc_bot_free` or `mc_bot_poll` there.

### Connecting

#### `bool mc_bot_connect(McBot* bot, const char* host, uint16_t port, const char* name)`

Starts an offline-mode login as player `name`:

1. resets all state (world, entities, inventory, health, position, walking),
2. opens the TCP connection – this **blocks** until connected or failed (DNS names and IP literals are accepted),
3. sends the handshake and login packets.

The server's answer is processed by `mc_bot_poll`. Returns `false` if the TCP connection could not be made or `host` /
`name` are not valid UTF-8. A successful return does not mean the login succeeded: a rejected login (wrong version,
online-mode server, banned name, name longer than 16 characters) shows up as `mc_bot_poll` returning −1.

May be called again after a disconnect to reconnect with the same bot.

#### `void mc_bot_disconnect(McBot* bot)`

Closes the connection. The next `mc_bot_poll` returns −1. The bot can be reconnected with `mc_bot_connect`.

### Running

#### `int mc_bot_poll(McBot* bot)`

The heartbeat – **call it regularly**, every 5 – 50 ms. One call

1. sends what is queued (chat, movement),
2. reads everything the network has delivered and handles each packet: login progress, compression, keep-alive
   answers, chunks, entities, inventory, health, teleports (confirmed automatically), respawn after death,
   resource-pack requests (accepted), chat (→ your callback),
3. runs the 20 Hz movement ticks that are due (physics + position packets), using an internal monotonic clock.

It never blocks. Returns the number of packets handled (≥ 0), or **−1 once the connection is closed** (kicked, server
stopped, network error, login rejected, `mc_bot_disconnect`, or never connected).

If you call it less often than every 50 ms the bot still moves at the correct speed (up to 5 ticks are caught up per
call). If you stop calling it for more than ~30 s, the server times the bot out.

### State

#### `bool mc_bot_spawned(const McBot* bot)`

`true` once the server has placed the bot in the world (it sent the first position after login). Before that, chat
works but walking does nothing. Becomes `false` briefly after a respawn or a dimension change, until the server sends
the new position.

#### `float mc_bot_health(const McBot* bot)`

Health in half hearts: `20` = full, `0` = dead. The bot respawns automatically after death. Before the server sends
health it reads `20`.

#### `void mc_bot_position(const McBot* bot, double xyz[3])`

Writes the position of the bot's feet to `xyz[0]`, `xyz[1]`, `xyz[2]` (x, y, z in blocks; y = height). `(0, 0, 0)`
before spawning.

### Acting

#### `bool mc_bot_say(McBot* bot, const char* message)`

Sends a chat message (UTF-8). Text beginning with `/` is executed as a command (`"/time set day"`, if the bot has
permission). Text longer than 256 bytes is split into several messages, never inside a UTF-8 character. Returns `true`
if everything was queued; `false` if the connection is closed or the send buffer (4 KB) is full.

#### `void mc_bot_walk(McBot* bot, bool forward)`

Holds (`true`) or releases (`false`) the forward key. The bot walks in the direction it faces at walking speed, with
vanilla physics (gravity, step-up onto slabs and stairs, collisions, water). Movement happens inside `mc_bot_poll`.
The direction is the one the server last set (the spawn rotation); turning is not exposed in C – see
[Extending the C API](#extending-the-c-api).

## Rules

- **Threading**: a bot is not thread-safe. Use it from one thread, or protect every call with your own mutex. Several
  bots on several threads are fine.
- **Memory**: all buffers belong to the bot and are freed by `mc_bot_free`. Nothing is allocated after `mc_bot_new`.
- **Pointers**: all functions except `mc_bot_free` require a valid, non-`NULL` `McBot*`; strings must be NUL-terminated.
- **Errors**: there are no error codes. A connection that could not be opened is `mc_bot_connect` returning `false`;
  everything afterwards (rejected login, kick, network error) is `mc_bot_poll` returning −1.
- **Signals**: the library installs no signal handlers and does not raise `SIGPIPE` (the sockets are opened with the
  platform's no-SIGPIPE option).
- **Server version must match** the `MC_V1_*` constant, otherwise the server closes the connection during login.

## Extending the C API

The C API is intentionally small; the Rust core behind it has everything. Adding a function takes two steps:
write it in [`src/capi.rs`](../src/capi.rs) and declare it in [`include/mcscon.h`](../include/mcscon.h). Inside, the
bot is `(*bot).session`, a full [`Session`](rust/session.md).

Example – food level, turning, jumping and a block query:

```rust
// src/capi.rs
#[no_mangle]
pub unsafe extern "C" fn mc_bot_food(bot: *const McBot) -> c_int {
    (*bot).session.state.food
}

#[no_mangle]
pub unsafe extern "C" fn mc_bot_look(bot: *mut McBot, yaw: f32, pitch: f32) {
    (*bot).session.set_rotation(yaw, pitch);
}

#[no_mangle]
pub unsafe extern "C" fn mc_bot_jump(bot: *mut McBot, on: bool) {
    (*bot).session.controls.jump = on;
}

/// Block id at x, y, z (0 = air or not loaded).
#[no_mangle]
pub unsafe extern "C" fn mc_bot_block(bot: *const McBot, x: c_int, y: c_int, z: c_int) -> c_int {
    (*bot).session.world.block_id(x, y, z) as c_int
}
```

```c
/* include/mcscon.h */
int  mc_bot_food(const McBot* bot);
void mc_bot_look(McBot* bot, float yaw, float pitch);
void mc_bot_jump(McBot* bot, bool on);
int  mc_bot_block(const McBot* bot, int x, int y, int z);
```

Rebuild the library (`cargo rustc … --features capi --crate-type staticlib`) and the new functions are available to C
and C++. Guidelines:

- Use only C types in signatures: `c_int`, `f32`/`f64`, `bool`, `*const c_char`, raw pointers, `#[repr(C)]` structs.
- Never let a Rust panic or a C++ exception cross the boundary. The library does not panic on network input; avoid
  `unwrap()` in your additions.
- Views (`Str`, `Bytes`, chat `text`) point into the receive buffer: copy them before returning to C, or only hand
  them out inside a callback, as `mc_bot_on_chat` does.
- More events: add a field with an `extern "C" fn` pointer to `McBot`, call it from a `SessionSink` method
  (see the `Chat` sink in `capi.rs` for the pattern). The [`SessionSink`](rust/session.md#sessionsink) methods are
  the complete list of what can be reported.

## See also

- [C++ API](cpp-api.md) – the same bot with RAII.
- [Concepts](concepts.md) – the polling model, lifetimes and errors explained for all three languages.
- [Troubleshooting](troubleshooting.md).
