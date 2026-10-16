# Getting started

This page gets a bot running against a local server in Rust, C and C++. All three example programs do the same thing:
join, print chat, say hello, walk forward for three seconds, print position and health, leave. The complete, commented
sources are in [`examples/`](../examples).

## 1. A server to talk to

mcscon only speaks the **offline-mode** login, so the server must run with `online-mode=false` in
`server.properties`. Any vanilla (or compatible) server of version **1.10, 1.11, 1.11.2, 1.12, 1.12.1 or 1.12.2**
works. Download the server jar from Mojang and accept the EULA yourself; this repository does not ship server files.

```bash
java -Xmx1G -jar server.jar nogui
```

> Only use offline mode on servers you own, on localhost or a private network. Never expose an offline-mode server to
> the internet.

Pick the protocol version in your code to match the server (the examples use 1.12.2). A mismatch makes the server
close the connection during login.

| Server | C / C++ constant | Rust |
|--------|------------------|------|
| 1.10 – 1.10.2 | `MC_V1_10` | `Version::V1_10` |
| 1.11 | `MC_V1_11` | `Version::V1_11` |
| 1.11.1, 1.11.2 | `MC_V1_11_2` | `Version::V1_11_2` |
| 1.12 | `MC_V1_12` | `Version::V1_12` |
| 1.12.1 | `MC_V1_12_1` | `Version::V1_12_1` |
| 1.12.2 | `MC_V1_12_2` | `Version::V1_12_2` |

## 2. Requirements

- A Rust toolchain, 1.82 or newer – <https://rustup.rs>. Needed for all three languages, because the C and C++ APIs
  are a static library built from the Rust code.
- For C / C++: a C99 / C++11 compiler, and `make` or CMake.

## 3. Rust

```bash
cargo run --release --example bot -- 127.0.0.1 25565
```

Use the library in your own crate:

```toml
[dependencies]
mcscon = { path = "../mcscon" }     # default feature "std" gives you TcpTransport
```

The shortest useful program:

```rust
use mcscon::{client::Buffers, posix::TcpTransport, session::{Session, SessionStorage}, world::WorldConfig, Version};

fn main() {
    let (mut rx, mut scratch, mut tx) = (vec![0u8; 128 << 10], vec![0u8; 512 << 10], vec![0u8; 4096]);
    let mut world = vec![0u8; 512 << 10];
    let (mut ents, mut players) = (vec![Default::default(); 64], vec![Default::default(); 16]);
    let storage = SessionStorage { world_mem: &mut world, world: WorldConfig { max_columns: 49, ..Default::default() },
                                   entities: &mut ents, players: &mut players };
    let mut bot = Session::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx },
                               Version::V1_12_2, storage);
    bot.connect("127.0.0.1", 25565, "mcscon");
    let t0 = std::time::Instant::now();
    while bot.poll(&mut ()) >= 0 {                       // network + packets
        bot.update(t0.elapsed().as_millis() as u32);     // 20 Hz physics/movement
        if bot.state.spawned { bot.say(b"hello"); break; }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
}
```

[`examples/bot.rs`](../examples/bot.rs) is the full version with chat output and walking. Next: the
[Rust API guide](rust-api.md).

## 4. C

```bash
make -C examples
examples/build/bot_c 127.0.0.1 25565
```

Build your own program against the static library:

```bash
cargo rustc --release --lib --features capi --crate-type staticlib
cc -std=c99 -Iinclude my_bot.c target/release/libmcscon.a -lm -lpthread -ldl -o my_bot
```

(`-lpthread -ldl` are needed by the Rust standard library on Linux; on macOS they are harmless and not required.)

```c
#include "mcscon.h"
#include <unistd.h>

int main(void) {
    McBot* bot = mc_bot_new(MC_V1_12_2);
    if (!mc_bot_connect(bot, "127.0.0.1", 25565, "mcscon")) return 1;
    while (mc_bot_poll(bot) >= 0) {                  /* call this in your main loop */
        if (mc_bot_spawned(bot)) { mc_bot_say(bot, "hello"); break; }
        usleep(10000);
    }
    mc_bot_free(bot);
    return 0;
}
```

Every function, CMake integration and how to add your own functions: [C API](c-api.md).

## 5. C++

```bash
make -C examples
examples/build/bot_cpp 127.0.0.1 25565
```

```cpp
#include "mcscon.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    mcscon::Bot bot(MC_V1_12_2);
    bot.onChat([](const char* text) { std::printf("chat: %s\n", text); });
    if (!bot.connect("127.0.0.1", 25565, "mcscon")) return 1;
    while (bot.poll()) {
        if (bot.spawned()) { bot.say("hello"); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}   // ~Bot() disconnects and frees
```

Compile with `c++ -std=c++11 -Iinclude my_bot.cpp target/release/libmcscon.a -lm -lpthread -ldl`. Details:
[C++ API](cpp-api.md).

## 6. Where to go next

- How the main loop, callbacks and buffers work: [Concepts](concepts.md).
- Typed packets, blocks, entities, inventory: the [Rust API](rust-api.md) – or [extend the C API](c-api.md#extending-the-c-api).
- Running on a microcontroller or console: [Memory sizing](memory.md) and [Porting a transport](rust/transport.md).
- Something does not work: [Troubleshooting](troubleshooting.md).
