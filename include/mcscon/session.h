// High-level bot session: Client + World + Entities + Inventory + player state + vanilla-like movement.
// Everything lives in memory you provide (see SessionStorage); nothing allocates.
#pragma once
#include "chat.h"
#include "entities.h"
#include "inventory.h"
#include "packets.h"
#include "physics.h"
#include "world.h"

namespace mc {

struct SessionStorage {          // all pointers must stay valid for the lifetime of the Session
    void* worldMem; size_t worldMemSize; WorldConfig world;     // worldMem 8-byte aligned; see World::columnTableBytes
    Entity* entities; u16 maxEntities;
    PlayerEntry* players; u16 maxPlayers;
};
// Convenient static layout: Storage<MaxColumns, ArenaBytes, MaxEntities, MaxPlayers>
template<unsigned Columns, unsigned ArenaBytes, unsigned Entities, unsigned Players> struct StaticStorage {
    alignas(8) u8 world[Columns * sizeof(u32) * 20 + ArenaBytes];   // 80 bytes per column + arena
    Entity entities[Entities]; PlayerEntry players[Players];
    SessionStorage get(bool light = false, bool biomes = false) {
        SessionStorage s; s.worldMem = world; s.worldMemSize = sizeof world; s.world.maxColumns = Columns; s.world.storeLight = light; s.world.storeBiomes = biomes;
        s.entities = entities; s.maxEntities = Entities; s.players = players; s.maxPlayers = Players; return s;
    }
};

enum Ability { AbilityInvulnerable = 1, AbilityFlying = 2, AbilityAllowFlying = 4, AbilityCreative = 8 };
enum GameMode : u8 { Survival = 0, Creative = 1, Adventure = 2, Spectator = 3 };

struct PlayerState {
    i32 entityId; u8 gameMode; bool hardcore; i32 dimension; u8 difficulty; u8 maxPlayers;
    bool joined;                 // Login received
    bool spawned;                // first position from the server received (movement active)
    float health, saturation; i32 food;
    float xpBar; i32 xpLevel, xpTotal;
    u8 abilities; float flySpeed, walkSpeed;
    BlockPos spawnPoint; i64 worldAge, timeOfDay;
    bool raining; float rainLevel;
    bool dead() const { return joined && health <= 0; }
};

enum class ResourcePackPolicy : u8 { Accept, Decline, Ignore };

typedef void (*ChatFn)(void* user, const char* text, u8 position, StrView json);   // text: flattened, valid in callback; position 0 chat,1 system,2 action bar
typedef void (*SessionPacketFn)(void* user, const Packet& pkt);                    // every packet, after internal handling
typedef void (*SessionEventFn)(void* user, int event, i32 arg);

enum SessionEvent { EvJoined, EvSpawned /*server-confirmed position*/, EvDied, EvRespawned, EvKicked, EvGameModeChanged, EvDimensionChanged, EvTeleported };

class Session {
public:
    Session(const Transport& t, const Buffers& b, Version v, const SessionStorage& s);

    bool connect(const char* host, u16 port, const char* username);   // offline mode
    int poll();                          // pump network + dispatch; -1 when disconnected
    int update(u32 nowMs);               // run due 20 Hz ticks (physics + position packets); returns ticks executed
    void disconnect() { client_.disconnect(); }

    // callbacks
    void onChat(ChatFn fn, void* user) { chatFn_ = fn; chatUser_ = user; }
    void onPacket(SessionPacketFn fn, void* user) { packetFn_ = fn; packetUser_ = user; }
    void onEvent(SessionEventFn fn, void* user) { eventFn_ = fn; eventUser_ = user; }
    void autoRespawn(bool on) { autoRespawn_ = on; }
    void resourcePackPolicy(ResourcePackPolicy p) { rpPolicy_ = p; }
    void viewDistance(i8 d) { viewDistance_ = d; }
    void brand(const char* b) { brand_ = b; }

    // components
    Client& client() { return client_; }
    World& world() { return world_; }
    EntityTracker& entities() { return entities_; }
    PlayerList& players() { return players_; }
    Inventory& inventory() { return inv_; }
    const PlayerState& state() const { return st_; }
    Body& body() { return body_; }
    const Body& body() const { return body_; }
    Controls controls;                   // set these; applied on the next tick

    // actions
    bool say(const char* message);                                   // splits at 256 bytes; messages beginning with '/' are commands
    bool respawn() { return pkt::respawn(client_); }
    void lookAt(double x, double y, double z);
    void setRotation(float yaw, float pitch) { body_.yaw = yaw; body_.pitch = pitch; }
    bool breakBlockInstant(BlockPos p, i8 face = 1);                 // start+finish digging (creative mode / instantly mineable)
    bool placeBlock(BlockPos against, i8 face, i32 hand = 0);        // right-click `against` on `face`
    bool attack(i32 entityId) { return pkt::attackEntity(client_, entityId) && pkt::swingArm(client_); }
    bool selectHotbar(u8 i) { return inv_.selectHotbar(client_, i); }
    bool useHeldItem() { return pkt::useItem(client_); }

private:
    bool handlePacket(const Packet& p);
    static void trampoline(void* self, const Packet& p) { ((Session*)self)->handlePacket(p); }
    void tick();
    void sendMovement();
    void fire(int ev, i32 arg = 0) { if (eventFn_) eventFn_(eventUser_, ev, arg); }

    Client client_; World world_; EntityTracker entities_; PlayerList players_; Inventory inv_;
    PlayerState st_; Body body_;
    ChatFn chatFn_; void* chatUser_; SessionPacketFn packetFn_; void* packetUser_; SessionEventFn eventFn_; void* eventUser_;
    bool autoRespawn_; ResourcePackPolicy rpPolicy_; i8 viewDistance_; const char* brand_;
    u32 lastMs_; u32 accMs_; bool haveTime_;
    // last sent state (vanilla onUpdateWalkingPlayer logic)
    double sentX_, sentY_, sentZ_; float sentYaw_, sentPitch_; bool sentGround_, sentSneak_, sentSprint_; u32 posTicks_;
    bool wasDead_;
};

} // namespace mc
