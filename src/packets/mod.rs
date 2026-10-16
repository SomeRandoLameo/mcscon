//! Typed packet layer (Play state, 1.10 - 1.12.2). Zero-copy: `Str`/`Bytes`/`Array`/`MetaView` fields point into the
//! receive buffer and are only valid inside the packet handler. Every decode returns `None` on a malformed packet.
//!
//! Decoding: `if let Some(m) = packets::read::<cb::Chat>(&packet) { ... }`
//! Sending:  `packets::send(&mut link, &sb::Chat { message: Str(b"hi") });`
use crate::client::{Packet, Sender};
use crate::ids::{PlayCb, PlaySb};
use crate::types::{State, Version};
use crate::wire::{pack_pos, BlockPos, Reader, Writer};

pub mod cb;
pub mod sb;

/// A clientbound Play packet that can be decoded.
pub trait ClientPacket<'a>: Sized {
    const ID: PlayCb;
    fn decode(r: &mut Reader<'a>, v: Version) -> Option<Self>;
}
/// A serverbound Play packet that can be encoded.
pub trait ServerPacket {
    const ID: PlaySb;
    fn encode(&self, w: &mut Writer, v: Version) -> bool;
}

/// Decodes `p` as `P`: `None` if it is another packet or malformed.
pub fn read<'a, P: ClientPacket<'a>>(p: &Packet<'a>) -> Option<P> {
    if p.state != State::Play || p.id != P::ID as u8 {
        return None;
    }
    let mut r = p.r;
    P::decode(&mut r, p.version)
}
/// Is `p` a `P` (by id only)?
pub fn is<'a, P: ClientPacket<'a>>(p: &Packet<'a>) -> bool {
    p.state == State::Play && p.id == P::ID as u8
}
pub fn send<S: Sender + ?Sized, P: ServerPacket>(tx: &mut S, pk: &P) -> bool {
    let v = tx.version();
    tx.send_play(P::ID, &mut |w| {
        if !pk.encode(w, v) {
            w.err = true;
        }
    })
}

// ------------------------------------------------------------------ serverbound helpers

pub fn teleport_confirm<S: Sender + ?Sized>(c: &mut S, id: i32) -> bool {
    c.send_play(PlaySb::TeleportConfirm, &mut |w| w.varint(id))
}
/// Max 256 chars.
pub fn chat<S: Sender + ?Sized>(c: &mut S, msg: &[u8]) -> bool {
    c.send_play(PlaySb::Chat, &mut |w| w.str(msg))
}
pub fn respawn<S: Sender + ?Sized>(c: &mut S) -> bool {
    c.send_play(PlaySb::ClientCommand, &mut |w| w.varint(0))
}
pub fn request_stats<S: Sender + ?Sized>(c: &mut S) -> bool {
    c.send_play(PlaySb::ClientCommand, &mut |w| w.varint(1))
}
pub fn client_settings<S: Sender + ?Sized>(c: &mut S, locale: &str, view_distance: i8, chat_mode: i32, colors: bool, skin_parts: u8, main_hand: i32) -> bool {
    c.send_play(PlaySb::Settings, &mut |w| {
        w.str(locale.as_bytes());
        w.u8(view_distance as u8);
        w.varint(chat_mode);
        w.bool(colors);
        w.u8(skin_parts);
        w.varint(main_hand);
    })
}
/// `client_settings` with the C++ defaults ("en_GB", view distance 2, ...).
pub fn client_settings_default<S: Sender + ?Sized>(c: &mut S) -> bool {
    client_settings(c, "en_GB", 2, 0, true, 0x7F, 1)
}
pub fn on_ground<S: Sender + ?Sized>(c: &mut S, g: bool) -> bool {
    c.send_play(PlaySb::Flying, &mut |w| w.bool(g))
}
pub fn position<S: Sender + ?Sized>(c: &mut S, x: f64, y: f64, z: f64, g: bool) -> bool {
    c.send_play(PlaySb::Position, &mut |w| {
        w.f64(x);
        w.f64(y);
        w.f64(z);
        w.bool(g);
    })
}
pub fn look<S: Sender + ?Sized>(c: &mut S, yaw: f32, pitch: f32, g: bool) -> bool {
    c.send_play(PlaySb::Look, &mut |w| {
        w.f32(yaw);
        w.f32(pitch);
        w.bool(g);
    })
}
pub fn position_look<S: Sender + ?Sized>(c: &mut S, x: f64, y: f64, z: f64, yaw: f32, pitch: f32, g: bool) -> bool {
    c.send_play(PlaySb::PositionLook, &mut |w| {
        w.f64(x);
        w.f64(y);
        w.f64(z);
        w.f32(yaw);
        w.f32(pitch);
        w.bool(g);
    })
}
pub fn held_item<S: Sender + ?Sized>(c: &mut S, slot: i16) -> bool {
    c.send_play(PlaySb::HeldItemSlot, &mut |w| w.u16(slot as u16))
}
pub fn swing_arm<S: Sender + ?Sized>(c: &mut S, hand: i32) -> bool {
    c.send_play(PlaySb::ArmAnimation, &mut |w| w.varint(hand))
}
pub fn use_item<S: Sender + ?Sized>(c: &mut S, hand: i32) -> bool {
    c.send_play(PlaySb::UseItem, &mut |w| w.varint(hand))
}
/// status: 0 start, 1 cancel, 2 finish, 3 drop stack, 4 drop item, 5 release/shoot, 6 swap hands
pub fn block_dig<S: Sender + ?Sized>(c: &mut S, status: i32, p: BlockPos, face: i8) -> bool {
    c.send_play(PlaySb::BlockDig, &mut |w| {
        w.varint(status);
        w.u64(pack_pos(p.x, p.y, p.z));
        w.u8(face as u8);
    })
}
/// Cursor in 0..1 (sent as bytes 0..16 on 1.10, floats afterwards).
pub fn block_place<S: Sender + ?Sized>(c: &mut S, p: BlockPos, face: i32, hand: i32, cx: f32, cy: f32, cz: f32) -> bool {
    let old = c.version() == Version::V1_10;
    c.send_play(PlaySb::BlockPlace, &mut |w| {
        w.u64(pack_pos(p.x, p.y, p.z));
        w.varint(face);
        w.varint(hand);
        if old {
            w.u8((cx * 16.0) as u8);
            w.u8((cy * 16.0) as u8);
            w.u8((cz * 16.0) as u8);
        } else {
            w.f32(cx);
            w.f32(cy);
            w.f32(cz);
        }
    })
}
/// action: 0 sneak, 1 unsneak, 2 leave bed, 3 start sprint, 4 stop sprint, 5 start horse jump, 6 stop horse jump, 7 open horse inv, 8 elytra
pub fn entity_action<S: Sender + ?Sized>(c: &mut S, entity_id: i32, action: i32, jump_boost: i32) -> bool {
    c.send_play(PlaySb::EntityAction, &mut |w| {
        w.varint(entity_id);
        w.varint(action);
        w.varint(jump_boost);
    })
}
pub fn plugin_message<S: Sender + ?Sized>(c: &mut S, channel: &str, data: &[u8]) -> bool {
    c.send_play(PlaySb::CustomPayload, &mut |w| {
        w.str(channel.as_bytes());
        w.bytes(data);
    })
}
pub fn close_window<S: Sender + ?Sized>(c: &mut S, id: u8) -> bool {
    c.send_play(PlaySb::CloseWindow, &mut |w| w.u8(id))
}
pub fn attack_entity<S: Sender + ?Sized>(c: &mut S, target: i32) -> bool {
    c.send_play(PlaySb::UseEntity, &mut |w| {
        w.varint(target);
        w.varint(1);
    })
}
pub fn interact_entity<S: Sender + ?Sized>(c: &mut S, target: i32, hand: i32) -> bool {
    c.send_play(PlaySb::UseEntity, &mut |w| {
        w.varint(target);
        w.varint(0);
        w.varint(hand);
    })
}
