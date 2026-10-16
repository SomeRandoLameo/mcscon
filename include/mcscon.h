/* mcscon C API: a Minecraft Java (1.10 - 1.12.2) bot in a handful of calls.
 * Link with libmcscon.a (cargo rustc --release --lib --features capi --crate-type staticlib) and, on Linux, -lpthread -ldl -lm. */
#ifndef MCSCON_H
#define MCSCON_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct McBot McBot;

enum { MC_V1_10, MC_V1_11, MC_V1_11_2, MC_V1_12, MC_V1_12_1, MC_V1_12_2 };

/* Called for every chat message. `text` is valid only during the call. position: 0 chat, 1 system, 2 action bar. */
typedef void (*McChatFn)(void* user, const char* text, int position);

McBot* mc_bot_new(int version);               /* NULL for an unknown version */
void   mc_bot_free(McBot* bot);               /* disconnects and frees */
void   mc_bot_on_chat(McBot* bot, McChatFn fn, void* user);

bool   mc_bot_connect(McBot* bot, const char* host, uint16_t port, const char* name);   /* offline mode */
int    mc_bot_poll(McBot* bot);               /* call often: network + 20 Hz ticks. Returns -1 once disconnected */
void   mc_bot_disconnect(McBot* bot);

bool   mc_bot_spawned(const McBot* bot);      /* true once the server placed the bot in the world */
float  mc_bot_health(const McBot* bot);
void   mc_bot_position(const McBot* bot, double xyz[3]);

bool   mc_bot_say(McBot* bot, const char* message);   /* text, or a command if it starts with '/' */
void   mc_bot_walk(McBot* bot, bool forward);

#ifdef __cplusplus
}
#endif
#endif
