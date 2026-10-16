# C++ API

Header: [`include/mcscon.hpp`](../include/mcscon.hpp) – C++11, header only. It is a thin RAII layer over the
[C API](c-api.md) and adds no behaviour of its own: the destructor frees the bot, the chat callback is a
`std::function` (so lambdas with captures work), and strings are `std::string`.

## Building and linking

Exactly like the C API – build `libmcscon.a` once, then compile as C++11 or newer:

```bash
cargo rustc --release --lib --features capi --crate-type staticlib
c++ -std=c++11 -Iinclude my_bot.cpp target/release/libmcscon.a -lm -lpthread -ldl -o my_bot
```

`make -C examples` builds [`examples/cpp/bot.cpp`](../examples/cpp/bot.cpp). For CMake use the imported target from
[C API → With CMake](c-api.md#with-cmake) and add `CXX` to `project(...)`:

```cmake
project(my_bot CXX)
# … mcscon target as in the C API page …
add_executable(my_bot main.cpp)
set_target_properties(my_bot PROPERTIES CXX_STANDARD 11)
target_link_libraries(my_bot PRIVATE mcscon)
```

## Example

```cpp
#include "mcscon.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main() {
    std::vector<std::string> log;                      // state the callback writes to

    mcscon::Bot bot(MC_V1_12_2);
    bot.onChat([&log](const char* text) {              // captures are fine
        log.emplace_back(text);                        // copy: `text` dies after the call
        std::printf("chat: %s\n", text);
    });
    if (!bot.connect("127.0.0.1", 25565, "mcscon")) return 1;

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    while (bot.poll() && clock::now() - start < std::chrono::seconds(10)) {   // poll() is false once disconnected
        if (bot.spawned()) bot.walk(clock::now() - start < std::chrono::seconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("received %zu chat messages\n", log.size());
}                                                      // ~Bot() disconnects and frees
```

## `mcscon::Bot`

```cpp
namespace mcscon {
class Bot {
public:
    explicit Bot(int version = MC_V1_12_2);
    ~Bot();
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;

    void  onChat(std::function<void(const char* text)> fn);
    bool  connect(const std::string& host, uint16_t port, const std::string& name);
    bool  poll();
    bool  spawned() const;
    float health() const;
    void  position(double xyz[3]) const;
    bool  say(const std::string& message);
    void  walk(bool forward);
    void  disconnect();
};
}
```

| Member | C equivalent | Description |
|--------|--------------|-------------|
| `explicit Bot(int version = MC_V1_12_2)` | `mc_bot_new` | Create a bot for `MC_V1_10 … MC_V1_12_2` (the constants from `mcscon.h`, see [versions](c-api.md#types-and-constants)). Allocates ~0.9 MB. Throws `std::runtime_error("unknown version")` for an unknown version. |
| `~Bot()` | `mc_bot_free` | Disconnects and frees everything. |
| `onChat(fn)` | `mc_bot_on_chat` | Set the chat callback, replacing an earlier one. Called from inside `poll()` with plain text (format codes stripped, ≤ 511 bytes) that is valid **only during the call** – copy it into a `std::string` to keep it. The message position (chat / system / action bar) is not forwarded; use the C function directly if you need it (see below). |
| `connect(host, port, name)` | `mc_bot_connect` | Reset all state and start an offline-mode login as `name`. **Blocks** until the TCP connection is made. `false` if it could not be made; a rejected login shows up later as `poll()` returning `false`. May be called again after a disconnect. |
| `poll()` | `mc_bot_poll` | Call regularly (every 5 – 50 ms): sends queued data, handles all received packets, calls the chat callback, runs the 20 Hz movement ticks. Never blocks. Returns `false` once the connection is closed (the packet count of the C function is dropped). |
| `spawned() const` | `mc_bot_spawned` | `true` once the server has placed the bot in the world; walking only works from then on. |
| `health() const` | `mc_bot_health` | 20 = full, 0 = dead (the bot respawns automatically). |
| `position(xyz) const` | `mc_bot_position` | Feet position: `xyz[0]` x, `xyz[1]` y (height), `xyz[2]` z. |
| `say(message)` | `mc_bot_say` | Chat message; text starting with `/` is a command; split at 256 bytes. `false` if not queued. |
| `walk(forward)` | `mc_bot_walk` | Hold / release the forward key. |
| `disconnect()` | `mc_bot_disconnect` | Close the connection (`poll()` then returns `false`). |

The full behaviour of every function is described in the [C API reference](c-api.md#reference).

## Ownership and copying

A `Bot` is **neither copyable nor movable**: the chat trampoline registered with the C API points at the `Bot` object
itself, so the object must stay at one address. To put bots in containers or pass ownership around, hold them through
a pointer:

```cpp
std::unique_ptr<mcscon::Bot> bot(new mcscon::Bot(MC_V1_12_2));   // C++11 (std::make_unique in C++14)
std::vector<std::unique_ptr<mcscon::Bot>> swarm;
swarm.push_back(std::move(bot));
```

## Exceptions

Only the constructor throws. **Exceptions must not escape the chat callback**: it is called from Rust code, and
unwinding through it is undefined behaviour (typically `std::terminate`). Catch inside the lambda:

```cpp
bot.onChat([](const char* text) {
    try { handle(text); } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); }
});
```

## Callbacks and re-entrancy

From inside `onChat` you may call `say`, `walk`, `disconnect` and the const getters. Do not destroy the `Bot` or call
`poll()` from inside the callback.

## Threads

Like the C API, one `Bot` must only be used from one thread at a time. A common layout is one thread per bot that runs
the `poll()` loop, with a `std::mutex` (or a command queue) if other threads want to `say()` something.

## Mixing with the C API

`mcscon.hpp` includes `mcscon.h`, so all `mc_bot_*` functions and `MC_V1_*` constants are available too. The wrapper
does not expose its `McBot*`; if you need C functions it does not wrap (for example the chat `position`, or functions
you [added yourself](c-api.md#extending-the-c-api)), either use the C API directly with your own RAII guard

```cpp
struct BotDeleter { void operator()(McBot* b) const { mc_bot_free(b); } };
std::unique_ptr<McBot, BotDeleter> bot(mc_bot_new(MC_V1_12_2));
mc_bot_on_chat(bot.get(), [](void*, const char* text, int position) { /* … */ }, nullptr);
```

or add the member to `mcscon::Bot` – each one is a one-line forward to the C function.

## Need more than the bot?

World, entities, inventory, typed packets and custom transports live in the Rust core. Either
[extend the C API](c-api.md#extending-the-c-api) with the few functions you need, or write the bot logic in Rust
([Rust API](rust-api.md)).
