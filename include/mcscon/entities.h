// Entity tracker + tab-list for 1.10-1.12.2. Fixed-capacity tables in caller memory, no heap.
#pragma once
#include "client.h"
#include "wire.h"

namespace mc {

enum class EntityKind : u8 { Object, Mob, Player, XpOrb, Weather, Painting };
struct ItemStack { i16 id; i8 count; i16 damage; };       // NBT is not retained (no heap); read it from the packet callback if needed

struct Entity {
    i32 id; Uuid uuid; EntityKind kind; u8 used;
    i32 type;                        // object type / mob type / weather type (see wiki.vg "Entity IDs")
    double x, y, z;
    u8 yaw, pitch, headYaw;          // 256 steps per turn
    i16 vx, vy, vz;                  // 1/8000 block per tick
    bool onGround;
    i32 vehicle;                     // -1 if not riding
    u8 flags;                        // metadata[0]: 0x01 fire, 0x02 sneaking, 0x08 sprinting, 0x10 using item, 0x20 invisible, 0x40 glowing, 0x80 elytra
    float health; bool hasHealth;    // metadata[7] for living entities
    i32 data;                        // object data / xp count / painting direction
    ItemStack equipment[6];          // 0 main hand, 1 off hand, 2..5 boots..helmet
    float yawDeg() const { return yaw * 360.0f / 256.0f; }
    float pitchDeg() const { return pitch * 360.0f / 256.0f; }
};

enum class EntityEvent : u8 { Spawned, Moved, Updated, Destroying, Status, Animation, Collected };
typedef void (*EntityFn)(void* user, EntityEvent ev, const Entity& e, i32 arg);   // arg: status / animation id / collector id
typedef void (*MetaFn)(void* user, const Entity& e, MetaView meta);

class EntityTracker {
public:
    EntityTracker() : tab_(0), cap_(0), self_(-1), dropped_(0), fn_(0), user_(0), metaFn_(0) {}
    void init(Entity* storage, u16 capacity) { tab_ = storage; cap_ = capacity; memset(storage, 0, sizeof(Entity) * capacity); }
    void onEvent(EntityFn fn, void* user) { fn_ = fn; user_ = user; }
    void onMetadata(MetaFn fn) { metaFn_ = fn; }
    bool handle(const Packet& p);                  // true if consumed
    void clear() { for (u16 i = 0; i < cap_; ++i) tab_[i].used = 0; }

    const Entity* find(i32 id) const { for (u16 i = 0; i < cap_; ++i) if (tab_[i].used && tab_[i].id == id) return &tab_[i]; return 0; }
    i32 selfId() const { return self_; }
    u32 count() const { u32 n = 0; for (u16 i = 0; i < cap_; ++i) n += tab_[i].used; return n; }
    u32 dropped() const { return dropped_; }       // spawns lost because the table was full
    template<class F> void forEach(F f) const { for (u16 i = 0; i < cap_; ++i) if (tab_[i].used) f(tab_[i]); }
    const Entity* nearest(double x, double y, double z, double maxDist, int kind = -1) const {
        const Entity* best = 0; double bd = maxDist * maxDist;
        for (u16 i = 0; i < cap_; ++i) if (tab_[i].used && tab_[i].id != self_ && (kind < 0 || (int)tab_[i].kind == kind)) {
            double dx = tab_[i].x - x, dy = tab_[i].y - y, dz = tab_[i].z - z, d = dx * dx + dy * dy + dz * dz;
            if (d <= bd) { bd = d; best = &tab_[i]; }
        }
        return best;
    }
private:
    Entity* alloc(i32 id);
    Entity* get(i32 id) { return const_cast<Entity*>(find(id)); }
    void fire(EntityEvent ev, const Entity& e, i32 arg = 0) { if (fn_) fn_(user_, ev, e, arg); }
    void applyMeta(Entity& e, MetaView m);
    Entity* tab_; u16 cap_; i32 self_; u32 dropped_; EntityFn fn_; void* user_; MetaFn metaFn_;
};

// ---- tab list ----
struct PlayerEntry { Uuid uuid; char name[17]; u8 used; u8 hasDisplayName; i32 gameMode; i32 ping; };
enum class PlayerListEvent : u8 { Added, Removed, Updated };
typedef void (*PlayerFn)(void* user, PlayerListEvent ev, const PlayerEntry& e);

class PlayerList {
public:
    PlayerList() : tab_(0), cap_(0), fn_(0), user_(0) {}
    void init(PlayerEntry* storage, u16 capacity) { tab_ = storage; cap_ = capacity; memset(storage, 0, sizeof(PlayerEntry) * capacity); }
    void onEvent(PlayerFn fn, void* user) { fn_ = fn; user_ = user; }
    bool handle(const Packet& p);
    const PlayerEntry* find(const Uuid& u) const { for (u16 i = 0; i < cap_; ++i) if (tab_[i].used && !memcmp(tab_[i].uuid.b, u.b, 16)) return &tab_[i]; return 0; }
    const PlayerEntry* findByName(const char* name) const { for (u16 i = 0; i < cap_; ++i) if (tab_[i].used && !strcmp(tab_[i].name, name)) return &tab_[i]; return 0; }
    u32 count() const { u32 n = 0; for (u16 i = 0; i < cap_; ++i) n += tab_[i].used; return n; }
    template<class F> void forEach(F f) const { for (u16 i = 0; i < cap_; ++i) if (tab_[i].used) f(tab_[i]); }
private:
    PlayerEntry* get(const Uuid& u) { return const_cast<PlayerEntry*>(find(u)); }
    PlayerEntry* tab_; u16 cap_; PlayerFn fn_; void* user_;
};

} // namespace mc
