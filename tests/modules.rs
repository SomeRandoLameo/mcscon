//! Physics, entities, tab list, inventory and the session logic with synthetic packets (Rust-native API).
mod common;
use common::*;
use mcscon::client::Buffers;
use mcscon::entities::{Entity, EntityEvent, EntityKind, EntitySink, EntityTracker, PlayerEntry, PlayerList};
use mcscon::ids::{PlayCb, PlaySb};
use mcscon::inventory::{Inventory, SLOT_HOTBAR_FIRST};
use mcscon::physics::{physics_tick, Body, Controls};
use mcscon::session::{Session, SessionEvent, SessionSink, SessionStorage};
use mcscon::wire::Str;
use mcscon::world::{World, WorldConfig};
use mcscon::{Error, Version};

// ------------------------------------------------------------------ physics

struct Flat<'m> {
    w: World<'m>,
}
fn flat(mem: &mut [u8]) -> Flat<'_> {
    let mut w = World::new(mem, WorldConfig { max_columns: 16, store_light: false, store_biomes: false });
    for cx in -1..=1 {
        for cz in -1..=1 {
            let mut d = Vec::new();
            section(&mut d, 4, &[0, 7 << 4, 3 << 4, 2 << 4], |i| {
                let y = i >> 8;
                (if y == 0 { 1 } else if y < 3 { 2 } else if y == 3 { 3 } else { 0 }) as u64
            }, true);
            d.extend(std::iter::repeat(1).take(256));
            w.handle(&mk(&map_chunk(cx, cz, true, 1, &d), PlayCb::MapChunk));
        }
    }
    Flat { w }
}
fn body(x: f64, y: f64, z: f64) -> Body {
    Body { x, y, z, ..Default::default() }
}

#[test]
fn physics() {
    let mut mem = vec![0u8; 512 * 1024 + 8];
    let mut f = flat(&mut mem);
    let mut b = body(0.5, 8.0, 0.5);
    let none = Controls::default();
    let mut ticks = 0;
    while !b.on_ground && ticks < 100 {
        physics_tick(&mut b, &none, &f.w, false, 0.1, 0.05);
        ticks += 1;
    }
    assert!(b.on_ground && (b.y - 4.0).abs() < 1e-9); // falls onto the grass surface (y = 4)
    assert!(ticks > 6 && ticks < 14);
    // chunks not loaded -> no simulation
    let mut far = body(500.0, 10.0, 500.0);
    assert!(!physics_tick(&mut far, &none, &f.w, false, 0.1, 0.05));
    // vanilla jump apex: 1.2491870787 blocks
    let mut jump = Controls { jump: true, ..none };
    let mut peak = 0f64;
    physics_tick(&mut b, &jump, &f.w, false, 0.1, 0.05);
    jump.jump = false;
    for _ in 0..40 {
        physics_tick(&mut b, &jump, &f.w, false, 0.1, 0.05);
        peak = peak.max(b.y - 4.0);
    }
    assert!((peak - 1.2491870787).abs() < 1e-3 && b.on_ground);
    // walking speed: 4.317 m/s, sprint 5.612 m/s
    let mut w1 = Body { on_ground: true, ..body(0.5, 4.0, 0.5) };
    let mut fwd = Controls { forward: true, ..none };
    for _ in 0..40 {
        physics_tick(&mut w1, &fwd, &f.w, false, 0.1, 0.05);
    }
    let v1 = w1.z - 0.5;
    for _ in 0..20 {
        physics_tick(&mut w1, &fwd, &f.w, false, 0.1, 0.05);
    }
    assert!((((w1.z - 0.5 - v1) / 20.0) * 20.0 - 4.317).abs() < 0.05);
    fwd.sprint = true;
    let mut w2 = Body { on_ground: true, ..body(0.5, 4.0, 0.5) };
    for _ in 0..40 {
        physics_tick(&mut w2, &fwd, &f.w, false, 0.1, 0.05);
    }
    let s1 = w2.z;
    for _ in 0..20 {
        physics_tick(&mut w2, &fwd, &f.w, false, 0.1, 0.05);
    }
    assert!(((w2.z - s1) - 5.612).abs() < 0.08);
    // wall: stops at the face
    f.w.set_block_state(0, 4, 5, 1 << 4);
    f.w.set_block_state(0, 5, 5, 1 << 4);
    let run = Controls { forward: true, ..none };
    let mut w3 = Body { on_ground: true, ..body(0.5, 4.0, 0.5) };
    for _ in 0..60 {
        physics_tick(&mut w3, &run, &f.w, false, 0.1, 0.05);
    }
    assert!((w3.z - (5.0 - 0.3)).abs() < 1e-9 && w3.collided_h);
    // creative flight: no gravity
    let mut fl = body(0.5, 10.0, 0.5);
    for _ in 0..20 {
        physics_tick(&mut fl, &none, &f.w, true, 0.1, 0.05);
    }
    assert!((fl.y - 10.0).abs() < 1e-6);
}

#[test]
fn physics_shapes_water_ladder() {
    let mut mem = vec![0u8; 512 * 1024 + 8];
    let mut f = flat(&mut mem);
    let none = Controls::default();
    let run = Controls { forward: true, ..none };
    // half slab (bottom, meta 0) is stepped onto without jumping
    f.w.set_block_state(0, 4, 5, 44 << 4);
    let mut b = Body { on_ground: true, ..body(0.5, 4.0, 0.5) };
    for _ in 0..60 {
        physics_tick(&mut b, &run, &f.w, false, 0.1, 0.05);
    }
    assert!(b.z > 5.5 && (b.y - 4.0).abs() < 0.6);
    // fence line stops at the 3/8 post face
    for x in -1..=1 {
        f.w.set_block_state(x, 4, 8, 85 << 4);
    }
    let mut c = Body { on_ground: true, ..body(0.5, 4.0, 6.5) };
    for _ in 0..80 {
        physics_tick(&mut c, &run, &f.w, false, 0.1, 0.05);
    }
    assert!(c.z > 8.0 && c.z < 8.1 && c.collided_h, "z = {}", c.z);
    // water: sinks slowly
    for y in 4..8 {
        f.w.set_block_state(0, y, -8, 9 << 4);
    }
    let mut sw = body(0.5, 7.9, -7.5);
    for _ in 0..20 {
        physics_tick(&mut sw, &none, &f.w, false, 0.1, 0.05);
    }
    assert!(sw.in_water && sw.y < 7.9 && sw.y > 4.0);
    // ladder: climbing
    for y in 4..12 {
        f.w.set_block_state(-8, y, 0, (65 << 4) | 4);
        f.w.set_block_state(-9, y, 0, 1 << 4);
    }
    let mut cl = Body { yaw: 270.0, on_ground: true, ..body(-8.7, 4.0, 0.5) };
    let up = Controls { forward: true, ..none };
    for _ in 0..40 {
        physics_tick(&mut cl, &up, &f.w, false, 0.1, 0.05);
    }
    assert!(cl.on_ladder && cl.y > 8.0);
}

// ------------------------------------------------------------------ entities / tab list

#[derive(Default)]
struct Ev {
    n: u32,
    last: Option<EntityEvent>,
    arg: i32,
}
impl EntitySink for Ev {
    fn event(&mut self, ev: EntityEvent, _e: &Entity, arg: i32) {
        self.n += 1;
        self.last = Some(ev);
        self.arg = arg;
    }
}

#[test]
fn entities() {
    let mut store = [Entity::default(); 8];
    let mut t = EntityTracker::new(&mut store);
    let mut ev = Ev::default();
    // Spawn mob 77: pig, metadata: flags(0)=0x01, health(7)=10.0
    let b = be(0x03, |w| {
        w.varint(77);
        for i in 0..16 {
            w.u8(i);
        }
        w.varint(90);
        w.f64(10.0);
        w.f64(64.0);
        w.f64(-5.0);
        w.u8(64);
        w.u8(32);
        w.u8(16);
        w.u16(100);
        w.u16(0);
        w.u16((-200i16) as u16);
        w.u8(0);
        w.u8(0);
        w.u8(1);
        w.u8(7);
        w.u8(2);
        w.f32(10.0);
        w.u8(0xFF);
    });
    assert!(t.handle_with(&mk(&b, PlayCb::SpawnEntityLiving), &mut ev));
    let e = *t.find(77).unwrap();
    assert!(e.kind == EntityKind::Mob && e.type_ == 90 && e.x == 10.0 && e.y == 64.0 && e.z == -5.0 && e.flags == 1);
    assert!(e.has_health && e.health == 10.0 && e.vz == -200 && e.uuid.0[5] == 5);
    assert!(ev.n == 1 && ev.last == Some(EntityEvent::Spawned));
    // relative move: 1 block in +x (4096 units), 0.5 in -z
    let b = be(0x26, |w| {
        w.varint(77);
        w.u16(4096);
        w.u16(0);
        w.u16((-2048i16) as u16);
        w.bool(true);
    });
    t.handle_with(&mk(&b, PlayCb::RelEntityMove), &mut ev);
    let e = t.find(77).unwrap();
    assert!(e.x == 11.0 && e.z == -5.5 && e.on_ground);
    let b = be(0x4C, |w| {
        w.varint(77);
        w.f64(1.0);
        w.f64(2.0);
        w.f64(3.0);
        w.u8(128);
        w.u8(0);
        w.bool(false);
    });
    t.handle_with(&mk(&b, PlayCb::EntityTeleport), &mut ev);
    let e = t.find(77).unwrap();
    assert!(e.x == 1.0 && e.y == 2.0 && e.z == 3.0 && (e.yaw_deg() - 180.0).abs() < 1e-3);
    // equipment (slot 5 = head: diamond helmet id 310)
    let b = be(0x3F, |w| {
        w.varint(77);
        w.varint(5);
        w.u16(310);
        w.u8(1);
        w.u16(0);
        w.u8(0);
    });
    t.handle_with(&mk(&b, PlayCb::EntityEquipment), &mut ev);
    assert!(t.find(77).unwrap().equipment[5].id == 310 && t.find(77).unwrap().equipment[5].count == 1);
    // status event (death animation 3)
    let b = be(0x1B, |w| {
        w.i32(77);
        w.u8(3);
    });
    t.handle_with(&mk(&b, PlayCb::EntityStatus), &mut ev);
    assert!(ev.last == Some(EntityEvent::Status) && ev.arg == 3);
    // vehicle: passengers
    let b = be(0x05, |w| {
        w.varint(80);
        for _ in 0..16 {
            w.u8(1);
        }
        w.f64(0.0);
        w.f64(0.0);
        w.f64(0.0);
        w.u8(0);
        w.u8(0);
        w.u8(0xFF);
    });
    t.handle_with(&mk(&b, PlayCb::NamedEntitySpawn), &mut ev);
    assert_eq!(t.find(80).unwrap().kind, EntityKind::Player);
    let b = be(0x43, |w| {
        w.varint(77);
        w.varint(1);
        w.varint(80);
    });
    t.handle_with(&mk(&b, PlayCb::SetPassengers), &mut ev);
    assert_eq!(t.find(80).unwrap().vehicle, 77);
    let b = be(0x43, |w| {
        w.varint(77);
        w.varint(0);
    });
    t.handle_with(&mk(&b, PlayCb::SetPassengers), &mut ev);
    assert_eq!(t.find(80).unwrap().vehicle, -1);
    assert_eq!(t.nearest(1.0, 2.0, 3.0, 5.0, Some(EntityKind::Mob)).map(|e| e.id), Some(77));
    assert!(t.nearest(100.0, 2.0, 3.0, 5.0, None).is_none());
    // destroy
    let b = be(0x32, |w| {
        w.varint(2);
        w.varint(77);
        w.varint(999);
    });
    t.handle_with(&mk(&b, PlayCb::EntityDestroy), &mut ev);
    assert!(t.find(77).is_none() && ev.last == Some(EntityEvent::Destroying) && t.count() == 1);
    // table full -> counted, no crash
    for i in 0..20 {
        let b = be(0x01, |w| {
            w.varint(1000 + i);
            w.f64(0.0);
            w.f64(0.0);
            w.f64(0.0);
            w.u16(1);
        });
        t.handle(&mk(&b, PlayCb::SpawnEntityExperienceOrb));
    }
    assert!(t.count() == 8 && t.dropped() == 13);
    // malformed packets are consumed without effect
    let bad = be(0x26, |w| w.varint(1));
    assert!(t.handle(&mk(&bad, PlayCb::RelEntityMove)));
}

#[test]
fn player_list() {
    let mut store = [PlayerEntry::default(); 4];
    let mut pl = PlayerList::new(&mut store);
    let add = |pl: &mut PlayerList, id: u8, name: &str, gm: i32| {
        let b = be(0x2E, |w| {
            w.varint(0);
            w.varint(1);
            for _ in 0..16 {
                w.u8(id);
            }
            w.str(name.as_bytes());
            w.varint(1);
            w.str(b"textures");
            w.str(b"val");
            w.bool(false);
            w.varint(gm);
            w.varint(42);
            w.bool(false);
        });
        pl.handle(&mk(&b, PlayCb::PlayerInfo));
    };
    add(&mut pl, 1, "Alice", 0);
    add(&mut pl, 2, "Bob", 1);
    assert!(pl.count() == 2 && pl.find_by_name(b"Bob").unwrap().game_mode == 1 && pl.find_by_name(b"Alice").unwrap().ping == 42);
    let lat = be(0x2E, |w| {
        w.varint(2);
        w.varint(1);
        for _ in 0..16 {
            w.u8(2);
        }
        w.varint(150);
    });
    pl.handle(&mk(&lat, PlayCb::PlayerInfo));
    assert_eq!(pl.find_by_name(b"Bob").unwrap().ping, 150);
    let rm = be(0x2E, |w| {
        w.varint(4);
        w.varint(1);
        for _ in 0..16 {
            w.u8(1);
        }
    });
    pl.handle(&mk(&rm, PlayCb::PlayerInfo));
    assert!(pl.count() == 1 && pl.find_by_name(b"Alice").is_none());
}

// ------------------------------------------------------------------ inventory with a captured send sink

#[test]
fn inventory() {
    let mut pipe = Pipe::default();
    let mut rx = [0u8; 1024];
    let mut tx = [0u8; 1024];
    let mut c = mcscon::client::Client::new(&mut pipe, Buffers { rx: &mut rx, scratch: &mut [], tx: &mut tx }, Version::V1_12_2);
    c.connect_login("h", 1, "b");
    let mut inv = Inventory::new();
    // WindowItems for window 0: 46 slots, diamonds x5 in hotbar slot 38
    let wi = be(0x14, |w| {
        w.u8(0);
        w.u16(46);
        for i in 0..46 {
            if i == 38 {
                w.u16(264);
                w.u8(5);
                w.u16(0);
                w.u8(0);
            } else {
                w.u16(0xFFFF);
            }
        }
    });
    assert!(inv.handle(&mk(&wi, PlayCb::WindowItems), &mut c));
    assert!(inv.count_of(264, -1) == 5 && inv.find(264, -1) == Some(38) && inv.first_empty_slot() == Some(36));
    // SetSlot cursor
    let cur = be(0x16, |w| {
        w.u8(0xFF);
        w.u16(0xFFFF);
        w.u16(1);
        w.u8(3);
        w.u16(0);
        w.u8(0);
    });
    inv.handle(&mk(&cur, PlayCb::SetSlot), &mut c);
    assert!(inv.cursor().id == 1 && inv.cursor().count == 3);
    // open a chest: 27 slots -> window of 63; the last 36 mirror into the player inventory
    let ow = be(0x13, |w| {
        w.u8(3);
        w.str(b"minecraft:chest");
        w.str(b"{\"text\":\"Chest\"}");
        w.u8(27);
    });
    inv.handle(&mk(&ow, PlayCb::OpenWindow), &mut c);
    {
        let w = inv.open_window().unwrap();
        assert!(w.type_is("minecraft:chest") && w.count == 63 && !w.loaded);
    }
    let ci = be(0x14, |w| {
        w.u8(3);
        w.u16(63);
        for i in 0..63 {
            if i == 0 {
                w.u16(260);
                w.u8(3);
                w.u16(0);
                w.u8(0);
            } else if i == 62 {
                w.u16(1);
                w.u8(1);
                w.u16(0);
                w.u8(0);
            } else {
                w.u16(0xFFFF);
            }
        }
    });
    inv.handle(&mk(&ci, PlayCb::WindowItems), &mut c);
    assert!(inv.open_window().unwrap().loaded);
    assert!(inv.open_window().unwrap().slots[0].id == 260 && inv.player().slots[44].id == 1); // window slot 62 = last hotbar slot
    assert!(inv.item_at(3, 0).id == 260 && inv.item_at(9, 0).id == -1);
    // unaccepted transaction is acknowledged automatically
    c.link.t.out.clear();
    let tr = be(0x11, |w| {
        w.u8(3);
        w.u16(7);
        w.bool(false);
    });
    inv.handle(&mk(&tr, PlayCb::Transaction), &mut c);
    assert_eq!(c.link.t.out, [5, 0x05, 3, 0, 7, 1]); // len 5, id 0x05, windowId, action, accepted
    // click sends WindowClick for window 3 with an "impossible" clicked item to force a resync
    c.link.t.out.clear();
    assert!(inv.shift_click(&mut c, 0));
    assert!(c.link.t.out.len() > 6 && c.link.t.out[1] == 0x07 && c.link.t.out[2] == 3);
    inv.handle(&mk(&be(0x12, |w| w.u8(3)), PlayCb::CloseWindow), &mut c);
    assert!(inv.open_window().is_none());
    // creative set updates local state immediately
    assert!(inv.creative_set(&mut c, 36, 276, 1, 0) && inv.player().slots[SLOT_HOTBAR_FIRST].id == 276);
    assert!(inv.select_hotbar(&mut c, 8) && !inv.select_hotbar(&mut c, 9) && inv.held_slot() == 8);
}

// ------------------------------------------------------------------ session

#[derive(Default)]
struct SEv {
    joined: u32,
    spawned: u32,
    died: u32,
    respawned: u32,
    tp: u32,
    chat: String,
    errors: Vec<Error>,
}
impl SessionSink for SEv {
    fn event(&mut self, ev: SessionEvent, _arg: i32) {
        match ev {
            SessionEvent::Joined => self.joined += 1,
            SessionEvent::Spawned => self.spawned += 1,
            SessionEvent::Died => self.died += 1,
            SessionEvent::Respawned => self.respawned += 1,
            SessionEvent::Teleported => self.tp += 1,
            _ => {}
        }
    }
    fn chat(&mut self, text: &[u8], _pos: u8, _json: Str<'_>) {
        self.chat = String::from_utf8_lossy(text).into_owned();
    }
    fn error(&mut self, e: Error, _info: i32) {
        self.errors.push(e);
    }
}

#[test]
fn session() {
    let mut pipe = Pipe::default();
    let mut rx = vec![0u8; 8192];
    let mut tx = vec![0u8; 4096];
    let mut world_mem = vec![0u8; 9 * 80 + 64 * 1024 + 8];
    let mut ents = [Entity::default(); 16];
    let mut players = [PlayerEntry::default(); 8];
    let storage = SessionStorage {
        world_mem: &mut world_mem,
        world: WorldConfig { max_columns: 9, store_light: false, store_biomes: false },
        entities: &mut ents,
        players: &mut players,
    };
    let mut s = Session::new(&mut pipe, Buffers { rx: &mut rx, scratch: &mut [], tx: &mut tx }, Version::V1_12_2, storage);
    let mut ev = SEv::default();
    assert!(s.connect("h", 1, "bot"));
    s.client.link.t.out.clear();
    let succ = be(0x02, |w| {
        w.str(b"00000000-0000-0000-0000-000000000000");
        w.str(b"bot");
    });
    let join = be(0x23, |w| {
        w.i32(12);
        w.u8(0);
        w.i32(0);
        w.u8(0);
        w.u8(20);
        w.str(b"flat");
        w.bool(false);
    });
    let pos = be(0x2F, |w| {
        w.f64(1.5);
        w.f64(4.0);
        w.f64(2.5);
        w.f32(90.0);
        w.f32(10.0);
        w.u8(0);
        w.varint(7);
    });
    let mut data = framed(&succ);
    data.extend(framed(&join));
    data.extend(framed(&pos));
    s.client.link.t.inp = data;
    while s.poll(&mut ev) > 0 {}
    assert!(ev.joined == 1 && ev.spawned == 1 && s.state.joined && s.state.spawned && s.state.entity_id == 12 && s.state.game_mode == 0);
    assert!(s.body.x == 1.5 && s.body.yaw == 90.0 && s.body.pitch == 10.0);
    // settings, MC|Brand, teleport confirm 7, position+look
    assert_eq!(sent_ids(&s.client.link.t.out), [0x04, 0x09, 0x00, 0x0E]);
    // relative flags: x,y relative (bit0,1), yaw relative (0x10)
    s.client.link.t.out.clear();
    s.client.link.t.inp = framed(&be(0x2F, |w| {
        w.f64(1.0);
        w.f64(1.0);
        w.f64(9.0);
        w.f32(45.0);
        w.f32(0.0);
        w.u8(0x13);
        w.varint(8);
    }));
    s.client.link.t.pos = 0;
    while s.poll(&mut ev) > 0 {}
    assert!(s.body.x == 2.5 && s.body.y == 5.0 && s.body.z == 9.0 && s.body.yaw == 135.0 && s.body.pitch == 0.0 && ev.tp == 1);
    // chat
    s.client.link.t.inp = framed(&be(0x0F, |w| {
        w.str(b"{\"translate\":\"chat.type.text\",\"with\":[{\"text\":\"Al\"},\"yo\"]}");
        w.u8(0);
    }));
    s.client.link.t.pos = 0;
    while s.poll(&mut ev) > 0 {}
    assert_eq!(ev.chat, "<Al> yo");
    // resource pack: accepted then loaded
    s.client.link.t.out.clear();
    s.client.link.t.pos = 0;
    s.client.link.t.inp = framed(&be(0x34, |w| {
        w.str(b"http://x/p.zip");
        w.str(b"hash");
    }));
    while s.poll(&mut ev) > 0 {}
    let rp = mcscon::ids::PLAY_SB_TABLES[3].a2w[PlaySb::ResourcePackReceive as usize] as i32;
    assert_eq!(sent_ids(&s.client.link.t.out), [rp, rp]);
    // death -> auto respawn (ClientCommand 0x03)
    s.client.link.t.out.clear();
    s.client.link.t.pos = 0;
    s.client.link.t.inp = framed(&be(0x41, |w| {
        w.f32(0.0);
        w.varint(20);
        w.f32(5.0);
    }));
    while s.poll(&mut ev) > 0 {}
    assert!(ev.died == 1 && s.state.dead() && sent_ids(&s.client.link.t.out) == [0x03]);
    // ticking while dead / without loaded chunks is harmless
    s.update(1000);
    assert_eq!(s.update(1100), 2);
    // say() splits long messages at 256 bytes without cutting UTF-8
    s.client.link.t.out.clear();
    let mut long = "a".repeat(255);
    long.push('ä');
    long.push_str(&"b".repeat(10));
    assert!(s.say(long.as_bytes()) && sent_ids(&s.client.link.t.out).len() == 2);
}
