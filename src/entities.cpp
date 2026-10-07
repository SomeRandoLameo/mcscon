#include <mcscon/entities.h>
#include <mcscon/packets.h>

namespace mc {

Entity* EntityTracker::alloc(i32 id) {
    Entity* e = get(id);
    if (!e) for (u16 i = 0; i < cap_; ++i) if (!tab_[i].used) { e = &tab_[i]; break; }
    if (!e) { ++dropped_; return 0; }
    memset(e, 0, sizeof *e); e->used = 1; e->id = id; e->vehicle = -1; return e;
}

void EntityTracker::applyMeta(Entity& e, MetaView m) {
    MetaEntry me;
    while (m.next(me)) {
        if (me.key == 0 && me.type == 0) e.flags = (u8)me.i;
        else if (me.key == 7 && me.type == 2 && e.kind != EntityKind::Object) { e.health = me.f[0]; e.hasHealth = true; }
    }
}

bool EntityTracker::handle(const Packet& p) {
    if (p.state != State::Play) return false;
    switch ((ids::PlayCb)p.id) {
    case ids::PlayCb::Login: { cb::Login l; if (pkt::read(p, l)) { clear(); self_ = l.entityId; } return false; }
    case ids::PlayCb::Respawn: { clear(); return false; }
    case ids::PlayCb::SpawnEntity: {
        cb::SpawnEntity s; if (!pkt::read(p, s)) return true;
        Entity* e = alloc(s.entityId); if (!e) return true;
        e->kind = EntityKind::Object; e->uuid = s.objectUUID; e->type = s.type; e->x = s.x; e->y = s.y; e->z = s.z;
        e->yaw = (u8)s.yaw; e->pitch = (u8)s.pitch; e->data = s.objectData; e->vx = s.velocity.x; e->vy = s.velocity.y; e->vz = s.velocity.z;
        fire(EntityEvent::Spawned, *e); return true;
    }
    case ids::PlayCb::SpawnEntityLiving: {
        cb::SpawnEntityLiving s; if (!pkt::read(p, s)) return true;
        Entity* e = alloc(s.entityId); if (!e) return true;
        e->kind = EntityKind::Mob; e->uuid = s.entityUUID; e->type = s.type; e->x = s.x; e->y = s.y; e->z = s.z;
        e->yaw = (u8)s.yaw; e->pitch = (u8)s.pitch; e->headYaw = (u8)s.headPitch; e->vx = s.velocity.x; e->vy = s.velocity.y; e->vz = s.velocity.z;
        applyMeta(*e, s.metadata); if (metaFn_) metaFn_(user_, *e, s.metadata);
        fire(EntityEvent::Spawned, *e); return true;
    }
    case ids::PlayCb::NamedEntitySpawn: {
        cb::NamedEntitySpawn s; if (!pkt::read(p, s)) return true;
        Entity* e = alloc(s.entityId); if (!e) return true;
        e->kind = EntityKind::Player; e->uuid = s.playerUUID; e->x = s.x; e->y = s.y; e->z = s.z; e->yaw = (u8)s.yaw; e->pitch = (u8)s.pitch;
        applyMeta(*e, s.metadata); if (metaFn_) metaFn_(user_, *e, s.metadata);
        fire(EntityEvent::Spawned, *e); return true;
    }
    case ids::PlayCb::SpawnEntityExperienceOrb: {
        cb::SpawnEntityExperienceOrb s; if (!pkt::read(p, s)) return true;
        Entity* e = alloc(s.entityId); if (!e) return true;
        e->kind = EntityKind::XpOrb; e->x = s.x; e->y = s.y; e->z = s.z; e->data = s.count; fire(EntityEvent::Spawned, *e); return true;
    }
    case ids::PlayCb::SpawnEntityWeather: {
        cb::SpawnEntityWeather s; if (!pkt::read(p, s)) return true;
        Entity* e = alloc(s.entityId); if (!e) return true;
        e->kind = EntityKind::Weather; e->type = s.type; e->x = s.x; e->y = s.y; e->z = s.z; fire(EntityEvent::Spawned, *e); return true;
    }
    case ids::PlayCb::SpawnEntityPainting: {
        cb::SpawnEntityPainting s; if (!pkt::read(p, s)) return true;
        Entity* e = alloc(s.entityId); if (!e) return true;
        e->kind = EntityKind::Painting; e->uuid = s.entityUUID; e->x = s.location.x; e->y = s.location.y; e->z = s.location.z; e->data = s.direction;
        fire(EntityEvent::Spawned, *e); return true;
    }
    case ids::PlayCb::RelEntityMove: {
        cb::RelEntityMove m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId); if (!e) return true;
        e->x += m.dX / 4096.0; e->y += m.dY / 4096.0; e->z += m.dZ / 4096.0; e->onGround = m.onGround; fire(EntityEvent::Moved, *e); return true;
    }
    case ids::PlayCb::EntityMoveLook: {
        cb::EntityMoveLook m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId); if (!e) return true;
        e->x += m.dX / 4096.0; e->y += m.dY / 4096.0; e->z += m.dZ / 4096.0; e->yaw = (u8)m.yaw; e->pitch = (u8)m.pitch; e->onGround = m.onGround;
        fire(EntityEvent::Moved, *e); return true;
    }
    case ids::PlayCb::EntityLook: {
        cb::EntityLook m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId); if (!e) return true;
        e->yaw = (u8)m.yaw; e->pitch = (u8)m.pitch; e->onGround = m.onGround; fire(EntityEvent::Moved, *e); return true;
    }
    case ids::PlayCb::EntityTeleport: {
        cb::EntityTeleport m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId); if (!e) return true;
        e->x = m.x; e->y = m.y; e->z = m.z; e->yaw = (u8)m.yaw; e->pitch = (u8)m.pitch; e->onGround = m.onGround; fire(EntityEvent::Moved, *e); return true;
    }
    case ids::PlayCb::EntityHeadRotation: {
        cb::EntityHeadRotation m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId); if (e) { e->headYaw = (u8)m.headYaw; fire(EntityEvent::Updated, *e); } return true;
    }
    case ids::PlayCb::EntityVelocity: {
        cb::EntityVelocity m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId); if (e) { e->vx = m.velocity.x; e->vy = m.velocity.y; e->vz = m.velocity.z; fire(EntityEvent::Updated, *e); } return true;
    }
    case ids::PlayCb::EntityDestroy: {
        cb::EntityDestroy d; if (!pkt::read(p, d)) return true; VarInt id;
        while (d.entityIds.next(id)) { Entity* e = get(id.v); if (e) { fire(EntityEvent::Destroying, *e); e->used = 0; } }
        return true;
    }
    case ids::PlayCb::EntityStatus: { cb::EntityStatus s; if (pkt::read(p, s)) { Entity* e = get(s.entityId); if (e) fire(EntityEvent::Status, *e, s.entityStatus); } return true; }
    case ids::PlayCb::Animation: { cb::Animation a; if (pkt::read(p, a)) { Entity* e = get(a.entityId); if (e) fire(EntityEvent::Animation, *e, a.animation); } return true; }
    case ids::PlayCb::EntityEquipment: {
        cb::EntityEquipment q; if (!pkt::read(p, q)) return true; Entity* e = get(q.entityId);
        if (e && q.slot >= 0 && q.slot < 6) { e->equipment[q.slot].id = q.item.id; e->equipment[q.slot].count = q.item.count; e->equipment[q.slot].damage = q.item.damage; fire(EntityEvent::Updated, *e); }
        return true;
    }
    case ids::PlayCb::EntityMetadata: {
        cb::EntityMetadata m; if (!pkt::read(p, m)) return true; Entity* e = get(m.entityId);
        if (e) { applyMeta(*e, m.metadata); if (metaFn_) metaFn_(user_, *e, m.metadata); fire(EntityEvent::Updated, *e); }
        return true;
    }
    case ids::PlayCb::AttachEntity: {
        cb::AttachEntity a; if (!pkt::read(p, a)) return true; Entity* e = get(a.entityId); if (e) { e->vehicle = a.vehicleId; fire(EntityEvent::Updated, *e); } return true;
    }
    case ids::PlayCb::SetPassengers: {
        cb::SetPassengers s; if (!pkt::read(p, s)) return true; VarInt id;
        for (u16 i = 0; i < cap_; ++i) if (tab_[i].used && tab_[i].vehicle == s.entityId) tab_[i].vehicle = -1;
        while (s.passengers.next(id)) { Entity* e = get(id.v); if (e) { e->vehicle = s.entityId; fire(EntityEvent::Updated, *e); } }
        return true;
    }
    case ids::PlayCb::Collect: { cb::Collect c; if (pkt::read(p, c)) { Entity* e = get(c.collectedEntityId); if (e) fire(EntityEvent::Collected, *e, c.collectorEntityId); } return true; }
    default: return false;
    }
}

bool PlayerList::handle(const Packet& p) {
    if (p.state != State::Play) return false;
    if (p.id == (u8)ids::PlayCb::Login || p.id == (u8)ids::PlayCb::Respawn) return false;   // tab list survives dimension changes
    if (p.id != (u8)ids::PlayCb::PlayerInfo) return false;
    cb::PlayerInfo pi; if (!pkt::read(p, pi)) return true;
    cb::PlayerInfo_data d;
    while (pi.data.next(d)) {
        PlayerEntry* e = get(d.uuid);
        switch (pi.action) {
        case 0: {
            if (!e) for (u16 i = 0; i < cap_; ++i) if (!tab_[i].used) { e = &tab_[i]; break; }
            if (!e) break;
            bool isNew = !e->used; memset(e, 0, sizeof *e); e->used = 1; e->uuid = d.uuid;
            u32 n = d.name.len < 16 ? d.name.len : 16; memcpy(e->name, d.name.data, n); e->name[n] = 0;
            e->gameMode = d.gamemode; e->ping = d.ping; e->hasDisplayName = d.displayName.has;
            if (fn_) fn_(user_, isNew ? PlayerListEvent::Added : PlayerListEvent::Updated, *e); break;
        }
        case 1: if (e) { e->gameMode = d.gamemode; if (fn_) fn_(user_, PlayerListEvent::Updated, *e); } break;
        case 2: if (e) { e->ping = d.ping; if (fn_) fn_(user_, PlayerListEvent::Updated, *e); } break;
        case 3: if (e) { e->hasDisplayName = d.displayName.has; if (fn_) fn_(user_, PlayerListEvent::Updated, *e); } break;
        case 4: if (e) { if (fn_) fn_(user_, PlayerListEvent::Removed, *e); e->used = 0; } break;
        }
    }
    return true;
}

} // namespace mc
