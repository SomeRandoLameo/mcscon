//! High-level bot session: Client + World + Entities + Inventory + player state + vanilla-like movement.
//! Everything lives in memory you provide ([`SessionStorage`]); nothing allocates.
use crate::chat::chat_to_text;
use crate::client::{Buffers, Client, Event, Link, Packet, Sender};
use crate::entities::{Entity, EntityEvent, EntitySink, EntityTracker, PlayerEntry, PlayerList, PlayerListEvent, PlayerListSink};
use crate::ids::PlayCb;
use crate::inventory::{Inventory, InventorySink, Window};
use crate::packets::{self, cb, sb};
use crate::physics::{physics_tick, Body, Controls};
use crate::transport::Transport;
use crate::types::{Error, State, Version};
use crate::world::{World, WorldConfig, WorldSink};
use crate::wire::{BlockPos, Bytes, MetaView, Slot, Str, Writer};

/// All memory a session needs besides the network buffers.
pub struct SessionStorage<'a> {
    /// 8-byte aligned memory for the column table + section arena; see `World::column_table_bytes`.
    pub world_mem: &'a mut [u8],
    pub world: WorldConfig,
    pub entities: &'a mut [Entity],
    pub players: &'a mut [PlayerEntry],
}

pub const ABILITY_INVULNERABLE: u8 = 1;
pub const ABILITY_FLYING: u8 = 2;
pub const ABILITY_ALLOW_FLYING: u8 = 4;
pub const ABILITY_CREATIVE: u8 = 8;

pub const SURVIVAL: u8 = 0;
pub const CREATIVE: u8 = 1;
pub const ADVENTURE: u8 = 2;
pub const SPECTATOR: u8 = 3;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PlayerState {
    pub entity_id: i32,
    pub game_mode: u8,
    pub hardcore: bool,
    pub dimension: i32,
    pub difficulty: u8,
    pub max_players: u8,
    /// Login received.
    pub joined: bool,
    /// First position from the server received (movement active).
    pub spawned: bool,
    pub health: f32,
    pub saturation: f32,
    pub food: i32,
    pub xp_bar: f32,
    pub xp_level: i32,
    pub xp_total: i32,
    pub abilities: u8,
    pub fly_speed: f32,
    pub walk_speed: f32,
    pub spawn_point: BlockPos,
    pub world_age: i64,
    pub time_of_day: i64,
    pub raining: bool,
    pub rain_level: f32,
}
impl PlayerState {
    pub fn dead(&self) -> bool {
        self.joined && self.health <= 0.0
    }
    pub(crate) fn fresh() -> Self {
        PlayerState { health: 20.0, food: 20, walk_speed: 0.1, fly_speed: 0.05, ..Default::default() }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ResourcePackPolicy {
    Accept,
    Decline,
    Ignore,
}

#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SessionEvent {
    Joined,
    /// Server-confirmed position.
    Spawned,
    Died,
    Respawned,
    Kicked,
    GameModeChanged,
    DimensionChanged,
    Teleported,
}
impl SessionEvent {
    pub fn from_i32(v: i32) -> Option<SessionEvent> {
        use SessionEvent::*;
        Some(match v {
            0 => Joined,
            1 => Spawned,
            2 => Died,
            3 => Respawned,
            4 => Kicked,
            5 => GameModeChanged,
            6 => DimensionChanged,
            7 => Teleported,
            _ => return None,
        })
    }
}

/// Everything a session reports. All methods default to no-ops.
pub trait SessionSink {
    /// `text`: flattened UTF-8, valid in the call; `position`: 0 chat, 1 system, 2 action bar.
    fn chat(&mut self, _text: &[u8], _position: u8, _json: Str<'_>) {}
    /// Every packet, after the session's own handling.
    fn packet(&mut self, _p: &Packet<'_>) {}
    fn event(&mut self, _ev: SessionEvent, _arg: i32) {}
    fn error(&mut self, _e: Error, _info: i32) {}
    fn entity_event(&mut self, _ev: EntityEvent, _e: &Entity, _arg: i32) {}
    fn entity_metadata(&mut self, _e: &Entity, _meta: MetaView<'_>) {}
    fn player_list_event(&mut self, _ev: PlayerListEvent, _e: &PlayerEntry) {}
    fn slot(&mut self, _window_id: i8, _slot: i16, _item: &Slot<'_>) {}
    fn window(&mut self, _w: &Window, _opened: bool) {}
    fn tile_entity(&mut self, _pos: BlockPos, _nbt: Bytes<'_>) {}
}
impl SessionSink for () {}

/// Forwards the component sinks to one [`SessionSink`].
pub(crate) struct Fwd<'s, S: SessionSink + ?Sized>(pub(crate) &'s mut S);
impl<'s, S: SessionSink + ?Sized> EntitySink for Fwd<'s, S> {
    fn event(&mut self, ev: EntityEvent, e: &Entity, arg: i32) {
        self.0.entity_event(ev, e, arg)
    }
    fn metadata(&mut self, e: &Entity, m: MetaView<'_>) {
        self.0.entity_metadata(e, m)
    }
}
impl<'s, S: SessionSink + ?Sized> PlayerListSink for Fwd<'s, S> {
    fn event(&mut self, ev: PlayerListEvent, e: &PlayerEntry) {
        self.0.player_list_event(ev, e)
    }
}
impl<'s, S: SessionSink + ?Sized> InventorySink for Fwd<'s, S> {
    fn slot(&mut self, w: i8, s: i16, item: &Slot<'_>) {
        self.0.slot(w, s, item)
    }
    fn window(&mut self, w: &Window, opened: bool) {
        self.0.window(w, opened)
    }
}
impl<'s, S: SessionSink + ?Sized> WorldSink for Fwd<'s, S> {
    fn tile_entity(&mut self, pos: BlockPos, nbt: Bytes<'_>) {
        self.0.tile_entity(pos, nbt)
    }
}

/// Settings and the "last sent" state of the movement logic.
pub(crate) struct Cfg<'a> {
    pub(crate) auto_respawn: bool,
    pub(crate) rp_policy: ResourcePackPolicy,
    pub(crate) view_distance: i8,
    pub(crate) brand: &'a str,
    pub(crate) last_ms: u32,
    pub(crate) acc_ms: u32,
    pub(crate) have_time: bool,
    // last sent state (vanilla onUpdateWalkingPlayer logic)
    pub(crate) sent_x: f64,
    pub(crate) sent_y: f64,
    pub(crate) sent_z: f64,
    pub(crate) sent_yaw: f32,
    pub(crate) sent_pitch: f32,
    pub(crate) sent_ground: bool,
    pub(crate) sent_sneak: bool,
    pub(crate) sent_sprint: bool,
    pub(crate) pos_ticks: u32,
    pub(crate) was_dead: bool,
}

impl<'a> Cfg<'a> {
    pub(crate) fn new() -> Self {
        Cfg {
            auto_respawn: true,
            rp_policy: ResourcePackPolicy::Accept,
            view_distance: 8,
            brand: "mcscon",
            last_ms: 0,
            acc_ms: 0,
            have_time: false,
            sent_x: 0.0,
            sent_y: 0.0,
            sent_z: 0.0,
            sent_yaw: 0.0,
            sent_pitch: 0.0,
            sent_ground: false,
            sent_sneak: false,
            sent_sprint: false,
            pos_ticks: 0,
            was_dead: false,
        }
    }
}

pub struct Session<'a, T: Transport> {
    pub client: Client<'a, T>,
    pub world: World<'a>,
    pub entities: EntityTracker<'a>,
    pub players: PlayerList<'a>,
    pub inventory: Inventory,
    pub state: PlayerState,
    pub body: Body,
    /// Set these; applied on the next tick.
    pub controls: Controls,
    cfg: Cfg<'a>,
}

/// Borrowed view of everything except the client, so the packet handler can run inside `Client::poll`.
/// Shared by the Rust [`Session`] and the C ABI (where the C++ wrapper owns the individual components).
pub(crate) struct Parts<'s, 'a> {
    pub(crate) world: &'s mut World<'a>,
    pub(crate) entities: &'s mut EntityTracker<'a>,
    pub(crate) players: &'s mut PlayerList<'a>,
    pub(crate) inventory: &'s mut Inventory,
    pub(crate) st: &'s mut PlayerState,
    pub(crate) body: &'s mut Body,
    pub(crate) controls: &'s Controls,
    pub(crate) cfg: &'s mut Cfg<'a>,
}

impl<'s, 'a> Parts<'s, 'a> {
    pub(crate) fn handle_packet<T: Transport, S: SessionSink + ?Sized>(&mut self, link: &mut Link<'_, T>, p: &Packet, sink: &mut S) {
        // Callbacks (the C++ front end) may read or write all of this through other pointers: see `crate::escape`.
        crate::escape(&mut *link);
        crate::escape(&mut *self.world);
        crate::escape(&mut *self.entities);
        crate::escape(&mut *self.players);
        crate::escape(&mut *self.inventory);
        crate::escape(&mut *self.st);
        crate::escape(&mut *self.body);
        crate::escape(&mut *self.cfg);
        if p.state == State::Play {
            // component models first (they see Login/Respawn too)
            self.world.handle_with(p, Some(&mut Fwd(&mut *sink)));
            self.entities.handle_with(p, &mut Fwd(&mut *sink));
            self.players.handle_with(p, &mut Fwd(&mut *sink));
            self.inventory.handle_with(p, link, &mut Fwd(&mut *sink));
            self.handle_session(link, p, sink);
        }
        sink.packet(p);
    }

    fn handle_session<T: Transport, S: SessionSink + ?Sized>(&mut self, link: &mut Link<'_, T>, p: &Packet, sink: &mut S) {
        let id = match PlayCb::from_u8(p.id) {
            Some(i) => i,
            None => return,
        };
        let (st, body, cfg) = (&mut *self.st, &mut *self.body, &mut *self.cfg);
        match id {
            PlayCb::Login => {
                if let Some(lg) = packets::read::<cb::Login>(p) {
                    st.joined = true;
                    st.entity_id = lg.entity_id;
                    st.game_mode = lg.game_mode & 7;
                    st.hardcore = lg.game_mode & 8 != 0;
                    st.dimension = lg.dimension;
                    st.difficulty = lg.difficulty;
                    st.max_players = lg.max_players;
                    *body = Body::default();
                    st.spawned = false;
                    packets::client_settings(link, "en_US", cfg.view_distance, 0, true, 0x7F, 1);
                    let mut data = [0u8; 32];
                    let mut w = Writer::new(&mut data);
                    w.str(cfg.brand.as_bytes());
                    let n = w.pos;
                    packets::plugin_message(link, "MC|Brand", &data[..n]);
                    sink.event(SessionEvent::Joined, lg.entity_id);
                }
            }
            PlayCb::Respawn => {
                if let Some(rs) = packets::read::<cb::Respawn>(p) {
                    st.dimension = rs.dimension;
                    st.difficulty = rs.difficulty;
                    st.game_mode = rs.gamemode & 7;
                    st.health = 20.0;
                    st.spawned = false;
                    cfg.was_dead = false;
                    sink.event(SessionEvent::DimensionChanged, rs.dimension);
                    sink.event(SessionEvent::Respawned, 0);
                }
            }
            PlayCb::Position => {
                if let Some(pos) = packets::read::<cb::Position>(p) {
                    let f = pos.flags as u8;
                    body.x = if f & 1 != 0 { body.x + pos.x } else { pos.x };
                    body.y = if f & 2 != 0 { body.y + pos.y } else { pos.y };
                    body.z = if f & 4 != 0 { body.z + pos.z } else { pos.z };
                    body.pitch = if f & 8 != 0 { body.pitch + pos.pitch } else { pos.pitch };
                    body.yaw = if f & 16 != 0 { body.yaw + pos.yaw } else { pos.yaw };
                    body.vx = 0.0;
                    body.vy = 0.0;
                    body.vz = 0.0;
                    packets::teleport_confirm(link, pos.teleport_id);
                    // vanilla acknowledges with the accepted position
                    packets::position_look(link, body.x, body.y, body.z, body.yaw, body.pitch, false);
                    cfg.sent_x = body.x;
                    cfg.sent_y = body.y;
                    cfg.sent_z = body.z;
                    cfg.sent_yaw = body.yaw;
                    cfg.sent_pitch = body.pitch;
                    cfg.pos_ticks = 0;
                    let first = !st.spawned;
                    st.spawned = true;
                    sink.event(if first { SessionEvent::Spawned } else { SessionEvent::Teleported }, 0);
                }
            }
            PlayCb::UpdateHealth => {
                if let Some(hp) = packets::read::<cb::UpdateHealth>(p) {
                    st.health = hp.health;
                    st.food = hp.food;
                    st.saturation = hp.food_saturation;
                    if hp.health <= 0.0 && !cfg.was_dead {
                        cfg.was_dead = true;
                        sink.event(SessionEvent::Died, 0);
                        if cfg.auto_respawn {
                            packets::respawn(link);
                        }
                    } else if hp.health > 0.0 {
                        cfg.was_dead = false;
                    }
                }
            }
            PlayCb::Experience => {
                if let Some(xp) = packets::read::<cb::Experience>(p) {
                    st.xp_bar = xp.experience_bar;
                    st.xp_level = xp.level;
                    st.xp_total = xp.total_experience;
                }
            }
            PlayCb::Abilities => {
                if let Some(ab) = packets::read::<cb::Abilities>(p) {
                    st.abilities = ab.flags as u8;
                    st.fly_speed = ab.flying_speed;
                    st.walk_speed = ab.walking_speed;
                }
            }
            PlayCb::UpdateTime => {
                if let Some(tm) = packets::read::<cb::UpdateTime>(p) {
                    st.world_age = tm.age;
                    st.time_of_day = tm.time;
                }
            }
            PlayCb::SpawnPosition => {
                if let Some(sp) = packets::read::<cb::SpawnPosition>(p) {
                    st.spawn_point = sp.location;
                }
            }
            PlayCb::Difficulty => {
                if let Some(df) = packets::read::<cb::Difficulty>(p) {
                    st.difficulty = df.difficulty;
                }
            }
            PlayCb::GameStateChange => {
                if let Some(gs) = packets::read::<cb::GameStateChange>(p) {
                    match gs.reason {
                        3 => {
                            st.game_mode = gs.game_mode as u8;
                            sink.event(SessionEvent::GameModeChanged, st.game_mode as i32);
                        }
                        1 => st.raining = false,
                        2 => st.raining = true,
                        7 => st.rain_level = gs.game_mode,
                        _ => {}
                    }
                }
            }
            PlayCb::Chat => {
                if let Some(ch) = packets::read::<cb::Chat>(p) {
                    let mut text = [0u8; 512];
                    let n = chat_to_text(ch.message.0, &mut text, true);
                    sink.chat(&text[..n], ch.position as u8, ch.message);
                }
            }
            PlayCb::ResourcePackSend => {
                if packets::is::<cb::ResourcePackSend>(p) {
                    match cfg.rp_policy {
                        ResourcePackPolicy::Decline => {
                            packets::send(link, &sb::ResourcePackReceive { result: 1 });
                        }
                        ResourcePackPolicy::Accept => {
                            packets::send(link, &sb::ResourcePackReceive { result: 3 });
                            packets::send(link, &sb::ResourcePackReceive { result: 0 });
                        }
                        ResourcePackPolicy::Ignore => {}
                    }
                }
            }
            PlayCb::KickDisconnect if packets::is::<cb::KickDisconnect>(p) => sink.event(SessionEvent::Kicked, 0),
            _ => {}
        }
    }

    /// Mirrors EntityPlayerSP.onUpdateWalkingPlayer
    fn send_movement<T: Transport>(&mut self, link: &mut Link<'_, T>) {
        let (st, body, cfg) = (&*self.st, &*self.body, &mut *self.cfg);
        if body.sneaking != cfg.sent_sneak {
            packets::entity_action(link, st.entity_id, if body.sneaking { 0 } else { 1 }, 0);
            cfg.sent_sneak = body.sneaking;
        }
        if body.sprinting != cfg.sent_sprint {
            packets::entity_action(link, st.entity_id, if body.sprinting { 3 } else { 4 }, 0);
            cfg.sent_sprint = body.sprinting;
        }
        let (dx, dy, dz) = (body.x - cfg.sent_x, body.y - cfg.sent_y, body.z - cfg.sent_z);
        let moved = dx * dx + dy * dy + dz * dz > 9.0e-4 || {
            cfg.pos_ticks += 1;
            cfg.pos_ticks >= 20
        };
        let rotated = body.yaw != cfg.sent_yaw || body.pitch != cfg.sent_pitch;
        if moved && rotated {
            packets::position_look(link, body.x, body.y, body.z, body.yaw, body.pitch, body.on_ground);
        } else if moved {
            packets::position(link, body.x, body.y, body.z, body.on_ground);
        } else if rotated {
            packets::look(link, body.yaw, body.pitch, body.on_ground);
        } else if cfg.sent_ground != body.on_ground {
            packets::on_ground(link, body.on_ground);
        }
        if moved {
            cfg.sent_x = body.x;
            cfg.sent_y = body.y;
            cfg.sent_z = body.z;
            cfg.pos_ticks = 0;
        }
        if rotated {
            cfg.sent_yaw = body.yaw;
            cfg.sent_pitch = body.pitch;
        }
        cfg.sent_ground = body.on_ground;
    }

    fn tick<T: Transport>(&mut self, link: &mut Link<'_, T>) {
        if !self.st.spawned || self.st.dead() || link.state() != State::Play {
            return;
        }
        let mut flying = self.st.abilities & ABILITY_FLYING != 0;
        if self.st.game_mode == SPECTATOR {
            flying = true;
        }
        physics_tick(self.body, self.controls, self.world, flying, self.st.walk_speed, self.st.fly_speed);
        self.send_movement(link);
    }

    /// Run due 20 Hz ticks (physics + position packets); returns ticks executed.
    pub(crate) fn update<T: Transport>(&mut self, link: &mut Link<'_, T>, now_ms: u32) -> i32 {
        if !self.cfg.have_time {
            self.cfg.have_time = true;
            self.cfg.last_ms = now_ms;
            return 0;
        }
        self.cfg.acc_ms = self.cfg.acc_ms.wrapping_add(now_ms.wrapping_sub(self.cfg.last_ms));
        self.cfg.last_ms = now_ms;
        let mut n = 0;
        while self.cfg.acc_ms >= 50 && n < 5 {
            self.cfg.acc_ms -= 50;
            self.tick(link);
            n += 1;
        }
        if self.cfg.acc_ms >= 50 {
            self.cfg.acc_ms = 0; // do not spiral after long stalls
        }
        n
    }

    /// Everything `connect` resets besides the login itself.
    pub(crate) fn reset_for_connect(&mut self) {
        self.world.clear();
        self.entities.clear();
        self.inventory.reset();
        *self.st = PlayerState::fresh();
        *self.body = Body::default();
        self.cfg.have_time = false;
        self.cfg.acc_ms = 0;
        self.cfg.was_dead = false;
    }
}

/// Pump the network and dispatch everything to the session model and `sink`.
pub(crate) fn poll_parts<'s, 'a, T: Transport, S: SessionSink + ?Sized>(client: &mut Client<'a, T>, parts: &mut Parts<'s, 'a>, sink: &mut S) -> i32 {
    client.poll(|link, ev| match ev {
        Event::Packet(p) => parts.handle_packet(link, &p, sink),
        Event::Error(e, info) => sink.error(e, info),
    })
}

impl<'a, T: Transport> Session<'a, T> {
    pub fn new(t: T, b: Buffers<'a>, v: Version, s: SessionStorage<'a>) -> Self {
        Session {
            client: Client::new(t, b, v),
            world: World::new(s.world_mem, s.world),
            entities: EntityTracker::new(s.entities),
            players: PlayerList::new(s.players),
            inventory: Inventory::new(),
            state: PlayerState::fresh(),
            body: Body::default(),
            controls: Controls::default(),
            cfg: Cfg::new(),
        }
    }

    pub fn auto_respawn(&mut self, on: bool) {
        self.cfg.auto_respawn = on;
    }
    pub fn resource_pack_policy(&mut self, p: ResourcePackPolicy) {
        self.cfg.rp_policy = p;
    }
    pub fn view_distance(&mut self, d: i8) {
        self.cfg.view_distance = d;
    }
    pub fn brand(&mut self, b: &'a str) {
        self.cfg.brand = b;
    }
    pub fn disconnect(&mut self) {
        self.client.disconnect();
    }

    /// Offline-mode login.
    pub fn connect(&mut self, host: &str, port: u16, username: &str) -> bool {
        self.controls = Controls::default();
        let Session { world, entities, players, inventory, state, body, controls, cfg, .. } = self;
        Parts { world, entities, players, inventory, st: state, body, controls, cfg }.reset_for_connect();
        self.client.connect_login(host, port, username)
    }

    /// Pump the network and dispatch; returns the number of packets, or -1 when disconnected.
    pub fn poll<S: SessionSink + ?Sized>(&mut self, sink: &mut S) -> i32 {
        let Session { client, world, entities, players, inventory, state, body, controls, cfg } = self;
        let mut parts = Parts { world, entities, players, inventory, st: state, body, controls, cfg };
        poll_parts(client, &mut parts, sink)
    }

    /// Run due 20 Hz ticks (physics + position packets); returns ticks executed. `now_ms`: your clock.
    pub fn update(&mut self, now_ms: u32) -> i32 {
        let Session { client, world, entities, players, inventory, state, body, controls, cfg } = self;
        let mut parts = Parts { world, entities, players, inventory, st: state, body, controls, cfg };
        parts.update(&mut client.link, now_ms)
    }

    // ------------------------------------------------------------------ actions

    /// Splits at 256 bytes; messages beginning with '/' are commands.
    pub fn say(&mut self, message: &[u8]) -> bool {
        say(&mut self.client.link, message)
    }
    pub fn respawn(&mut self) -> bool {
        packets::respawn(&mut self.client.link)
    }
    pub fn look_at(&mut self, x: f64, y: f64, z: f64) {
        look_at(&mut self.body, x, y, z)
    }
    pub fn set_rotation(&mut self, yaw: f32, pitch: f32) {
        self.body.yaw = yaw;
        self.body.pitch = pitch;
    }
    /// Start + finish digging (creative mode / instantly mineable).
    pub fn break_block_instant(&mut self, p: BlockPos, face: i8) -> bool {
        let l = &mut self.client.link;
        packets::swing_arm(l, 0) && packets::block_dig(l, 0, p, face) && packets::block_dig(l, 2, p, face)
    }
    /// Right-click `against` on `face`.
    pub fn place_block(&mut self, against: BlockPos, face: i8, hand: i32) -> bool {
        packets::block_place(&mut self.client.link, against, face as i32, hand, 0.5, 0.5, 0.5)
    }
    pub fn attack(&mut self, entity_id: i32) -> bool {
        packets::attack_entity(&mut self.client.link, entity_id) && packets::swing_arm(&mut self.client.link, 0)
    }
    pub fn select_hotbar(&mut self, i: u8) -> bool {
        self.inventory.select_hotbar(&mut self.client.link, i)
    }
    pub fn use_held_item(&mut self) -> bool {
        packets::use_item(&mut self.client.link, 0)
    }
}

/// Sends a chat message, splitting at 256 bytes (never inside a UTF-8 sequence).
/// Messages beginning with '/' are commands.
pub fn say<S: Sender + ?Sized>(tx: &mut S, message: &[u8]) -> bool {
    let mut m = message;
    let mut ok = true;
    while !m.is_empty() {
        let mut k = m.len().min(256);
        if k < m.len() {
            while k > 0 && (m[k] & 0xC0) == 0x80 {
                k -= 1;
            }
            if k == 0 {
                k = 256;
            }
        }
        ok &= packets::chat(tx, &m[..k]);
        m = &m[k..];
    }
    ok
}

/// Turns the body towards a world position.
pub fn look_at(body: &mut Body, x: f64, y: f64, z: f64) {
    let (dx, dy, dz) = (x - body.x, y - body.eye_y(), z - body.z);
    let h = libm::sqrt(dx * dx + dz * dz);
    body.yaw = (libm::atan2(dz, dx) * 180.0 / core::f64::consts::PI - 90.0) as f32;
    body.pitch = (-libm::atan2(dy, h) * 180.0 / core::f64::consts::PI) as f32;
}
