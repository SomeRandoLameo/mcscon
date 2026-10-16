/* The same bot in Rust (examples/bot.rs), C (this file) and C++ (examples/cpp/bot.cpp).
 *
 * What it does:
 *   1. connects to an offline-mode server (online-mode=false) as "mcscon",
 *   2. prints every chat message it receives,
 *   3. once it is in the world: says hello and walks forward for 3 seconds,
 *   4. prints its position and health, then disconnects.
 *
 * Build and run:   make -C examples && examples/build/bot_c [host] [port]     (defaults: 127.0.0.1 25565)
 *
 * The whole API is in include/mcscon.h. See docs/c-api.md for every function. */
#include "mcscon.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Chat callback: called from inside mc_bot_poll() for every chat message.
 * `text` is plain text (formatting stripped) and only valid during the call, copy it if you need to keep it.
 * `user` is the pointer you passed to mc_bot_on_chat (unused here). */
static void on_chat(void* user, const char* text, int position) {
    (void)user; (void)position;
    printf("chat: %s\n", text);
}

/* Seconds on a monotonic clock, to time the 3 seconds of walking. */
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    /* Command line: host and port of the server. */
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 25565;

    /* Create the bot for protocol 1.12.2 (it allocates its own buffers) and register the chat callback. */
    McBot* bot = mc_bot_new(MC_V1_12_2);
    mc_bot_on_chat(bot, on_chat, NULL);

    /* Offline-mode login: opens the TCP connection and sends handshake + login. The server's answer is processed
     * by mc_bot_poll() below. Returns false only if the TCP connection could not be made. */
    if (!mc_bot_connect(bot, host, (uint16_t)port, "mcscon")) {
        fprintf(stderr, "could not connect to %s:%d\n", host, port);
        return 1;
    }

    /* Main loop. The library has no threads and no clock; you call mc_bot_poll() regularly. */
    double spawned_at = 0; /* time when we said hello; 0 = not yet */
    /* mc_bot_poll reads the network, handles every packet (chunks, entities, chat, keep-alive, teleports ...) and
     * runs the 20 Hz physics tick. It returns -1 when the connection is closed, which ends the loop. */
    while (mc_bot_poll(bot) >= 0) {
        if (!spawned_at && mc_bot_spawned(bot)) {
            /* The server has put us in the world: act once. */
            mc_bot_say(bot, "hello from C");  /* a chat message ("/command" would run a command) */
            mc_bot_walk(bot, true);           /* hold "W"; the physics engine moves us every tick */
            spawned_at = now();
        } else if (spawned_at && now() - spawned_at > 3.0) {
            break; /* 3 seconds of walking are over */
        }
        usleep(10 * 1000); /* don't spin the CPU */
    }

    /* Read the bot's state: position (feet) and health (20 = full). */
    double p[3];
    mc_bot_position(bot, p);
    printf("position: %.1f %.1f %.1f  health: %.0f\n", p[0], p[1], p[2], mc_bot_health(bot));

    mc_bot_free(bot); /* disconnects and frees everything */
    return 0;
}
