// The same bot in Rust (examples/bot.rs), C (examples/c/bot.c) and C++ (this file).
//
// What it does:
//   1. connects to an offline-mode server (online-mode=false) as "mcscon",
//   2. prints every chat message it receives,
//   3. once it is in the world: says hello and walks forward for 3 seconds,
//   4. prints its position and health, then disconnects.
//
// Build and run:   make -C examples && examples/build/bot_cpp [host] [port]     (defaults: 127.0.0.1 25565)
//
// mcscon.hpp is a thin RAII wrapper over the C API (include/mcscon.h). See docs/cpp-api.md.
#include "mcscon.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    // Command line: host and port of the server.
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? std::atoi(argv[2]) : 25565;
    using clock = std::chrono::steady_clock;

    // Create the bot for protocol 1.12.2. It allocates its own buffers and frees them (and disconnects) in the
    // destructor. The chat callback is any callable; `text` is plain text, valid only during the call.
    mcscon::Bot bot(MC_V1_12_2);
    bot.onChat([](const char* text) { std::printf("chat: %s\n", text); });

    // Offline-mode login: opens the TCP connection and sends handshake + login. The server's answer is processed
    // by poll() below. Returns false only if the TCP connection could not be made.
    if (!bot.connect(host, (uint16_t)port, "mcscon")) {
        std::fprintf(stderr, "could not connect to %s:%d\n", host, port);
        return 1;
    }

    // Main loop. The library has no threads and no clock; you call poll() regularly.
    bool spawned = false;             // have we said hello yet?
    clock::time_point spawned_at;     // when we said hello
    // poll() reads the network, handles every packet (chunks, entities, chat, keep-alive, teleports ...) and runs
    // the 20 Hz physics tick. It returns false when the connection is closed, which ends the loop.
    while (bot.poll()) {
        if (!bot.spawned()) {
            // not in the world yet, keep polling
        } else if (!spawned) {
            // The server has put us in the world: act once.
            bot.say("hello from C++");    // a chat message ("/command" would run a command)
            bot.walk(true);               // hold "W"; the physics engine moves us every tick
            spawned = true;
            spawned_at = clock::now();
        } else if (clock::now() - spawned_at > std::chrono::seconds(3)) {
            break;                        // 3 seconds of walking are over
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // don't spin the CPU
    }

    // Read the bot's state: position (feet) and health (20 = full).
    double p[3];
    bot.position(p);
    std::printf("position: %.1f %.1f %.1f  health: %.0f\n", p[0], p[1], p[2], bot.health());
}   // ~Bot() disconnects and frees everything
