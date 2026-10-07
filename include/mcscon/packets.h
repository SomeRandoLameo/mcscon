// Typed packet layer (Play state, 1.10 - 1.12.2). Header-only, zero-copy: StrView fields point into the
// receive buffer and are only valid inside the packet callback. Every decode returns false on a malformed packet.
#pragma once
#include "client.h"
#include "packets_cb.h"
#include "packets_sb.h"

namespace mc {
namespace pkt {

// ------------------------------------------------------------------ generic typed access
// Decoding:  cb::Chat m; if (pkt::read(packet, m)) { ... }        (false: other packet id or malformed)
// Sending:   sb::Chat m = {}; m.message = ...; pkt::send(client, m);
template<class P> inline bool read(const Packet& p, P& out) {
    if (p.state != State::Play || p.id != (u8)P::packetId()) return false;
    Reader r = p.r; return cb::decode(r, out, p.version);
}
template<class P> inline bool send(Client& c, const P& pk) {
    Writer w = c.begin(P::packetId());
    if (!sb::encode(w, pk, c.version())) w.err = true;
    return c.commit(w);
}
// Dispatch helper: pkt::is<cb::Chat>(packet)
template<class P> inline bool is(const Packet& p) { return p.state == State::Play && p.id == (u8)P::packetId(); }

// ------------------------------------------------------------------ serverbound
inline bool teleportConfirm(Client& c, i32 id) { Writer w = c.begin(ids::PlaySb::TeleportConfirm); w.varint(id); return c.commit(w); }
inline bool chat(Client& c, const char* msg) { Writer w = c.begin(ids::PlaySb::Chat); w.str(msg); return c.commit(w); }   // max 256 chars
inline bool respawn(Client& c) { Writer w = c.begin(ids::PlaySb::ClientCommand); w.varint(0); return c.commit(w); }
inline bool requestStats(Client& c) { Writer w = c.begin(ids::PlaySb::ClientCommand); w.varint(1); return c.commit(w); }
inline bool clientSettings(Client& c, const char* locale = "en_GB", i8 viewDistance = 2, i32 chatMode = 0, bool colors = true, u8 skinParts = 0x7F, i32 mainHand = 1) {
    Writer w = c.begin(ids::PlaySb::Settings);
    w.str(locale); w.u8_((u8)viewDistance); w.varint(chatMode); w.bool_(colors); w.u8_(skinParts); w.varint(mainHand); return c.commit(w);
}
inline bool onGround(Client& c, bool g) { Writer w = c.begin(ids::PlaySb::Flying); w.bool_(g); return c.commit(w); }
inline bool position(Client& c, double x, double y, double z, bool g) {
    Writer w = c.begin(ids::PlaySb::Position); w.f64_(x); w.f64_(y); w.f64_(z); w.bool_(g); return c.commit(w);
}
inline bool look(Client& c, float yaw, float pitch, bool g) {
    Writer w = c.begin(ids::PlaySb::Look); w.f32_(yaw); w.f32_(pitch); w.bool_(g); return c.commit(w);
}
inline bool positionLook(Client& c, double x, double y, double z, float yaw, float pitch, bool g) {
    Writer w = c.begin(ids::PlaySb::PositionLook); w.f64_(x); w.f64_(y); w.f64_(z); w.f32_(yaw); w.f32_(pitch); w.bool_(g); return c.commit(w);
}
inline bool heldItem(Client& c, i16 slot) { Writer w = c.begin(ids::PlaySb::HeldItemSlot); w.u16_((u16)slot); return c.commit(w); }
inline bool swingArm(Client& c, i32 hand = 0) { Writer w = c.begin(ids::PlaySb::ArmAnimation); w.varint(hand); return c.commit(w); }
inline bool useItem(Client& c, i32 hand = 0) { Writer w = c.begin(ids::PlaySb::UseItem); w.varint(hand); return c.commit(w); }
// status: 0 start, 1 cancel, 2 finish, 3 drop stack, 4 drop item, 5 release/shoot, 6 swap hands
inline bool blockDig(Client& c, i32 status, BlockPos p, i8 face) {
    Writer w = c.begin(ids::PlaySb::BlockDig); w.varint(status); w.u64_(packPos(p.x, p.y, p.z)); w.u8_((u8)face); return c.commit(w);
}
// cursor in 0..1 (sent as bytes 0..16 on 1.10, floats afterwards)
inline bool blockPlace(Client& c, BlockPos p, i32 face, i32 hand, float cx, float cy, float cz) {
    Writer w = c.begin(ids::PlaySb::BlockPlace); w.u64_(packPos(p.x, p.y, p.z)); w.varint(face); w.varint(hand);
    if (c.version() == Version::V1_10) { w.u8_((u8)(cx * 16)); w.u8_((u8)(cy * 16)); w.u8_((u8)(cz * 16)); }
    else { w.f32_(cx); w.f32_(cy); w.f32_(cz); }
    return c.commit(w);
}
// action: 0 sneak, 1 unsneak, 2 leave bed, 3 start sprint, 4 stop sprint, 5 start horse jump, 6 stop horse jump, 7 open horse inv, 8 elytra
inline bool entityAction(Client& c, i32 entityId, i32 action, i32 jumpBoost = 0) {
    Writer w = c.begin(ids::PlaySb::EntityAction); w.varint(entityId); w.varint(action); w.varint(jumpBoost); return c.commit(w);
}
inline bool pluginMessage(Client& c, const char* channel, const void* data, size_t len) {
    Writer w = c.begin(ids::PlaySb::CustomPayload); w.str(channel); w.bytes(data, len); return c.commit(w);
}
inline bool closeWindow(Client& c, u8 id) { Writer w = c.begin(ids::PlaySb::CloseWindow); w.u8_(id); return c.commit(w); }
inline bool attackEntity(Client& c, i32 target) { Writer w = c.begin(ids::PlaySb::UseEntity); w.varint(target); w.varint(1); return c.commit(w); }
inline bool interactEntity(Client& c, i32 target, i32 hand = 0) { Writer w = c.begin(ids::PlaySb::UseEntity); w.varint(target); w.varint(0); w.varint(hand); return c.commit(w); }

} // namespace pkt
} // namespace mc
