//! Entity tracker + tab-list for 1.10-1.12.2. Fixed-capacity tables in caller memory, no heap.
use crate::client::Packet;
use crate::ids::PlayCb;
use crate::packets::{self, cb};
use crate::types::State;
use crate::wire::{MetaView, Uuid};

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum EntityKind {
    #[default]
    Object,
    Mob,
    Player,
    XpOrb,
    Weather,
    Painting,
}

/// NBT is not retained (no heap); read it from the packet handler if needed.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct ItemStack {
    pub id: i16,
    pub count: i8,
    pub damage: i16,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Entity {
    pub id: i32,
    pub uuid: Uuid,
    pub kind: EntityKind,
    pub used: u8,
    /// Object type / mob type / weather type (see wiki.vg "Entity IDs").
    pub type_: i32,
    pub x: f64,
    pub y: f64,
    pub z: f64,
    /// 256 steps per turn.
    pub yaw: u8,
    pub pitch: u8,
    pub head_yaw: u8,
    /// 1/8000 block per tick.
    pub vx: i16,
    pub vy: i16,
    pub vz: i16,
    pub on_ground: bool,
    /// -1 if not riding.
    pub vehicle: i32,
    /// Metadata index 0: 0x01 fire, 0x02 sneaking, 0x08 sprinting, 0x10 using item, 0x20 invisible, 0x40 glowing, 0x80 elytra.
    pub flags: u8,
    /// Metadata index 7 for living entities.
    pub health: f32,
    pub has_health: bool,
    /// Object data / xp count / painting direction.
    pub data: i32,
    /// 0 main hand, 1 off hand, 2..5 boots..helmet.
    pub equipment: [ItemStack; 6],
}
impl Entity {
    pub fn yaw_deg(&self) -> f32 {
        self.yaw as f32 * 360.0 / 256.0
    }
    pub fn pitch_deg(&self) -> f32 {
        self.pitch as f32 * 360.0 / 256.0
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum EntityEvent {
    Spawned,
    Moved,
    Updated,
    Destroying,
    Status,
    Animation,
    Collected,
}

/// Receives entity events. `arg`: status / animation id / collector id.
pub trait EntitySink {
    fn event(&mut self, _ev: EntityEvent, _e: &Entity, _arg: i32) {}
    fn metadata(&mut self, _e: &Entity, _meta: MetaView<'_>) {}
}
impl EntitySink for () {}

pub struct EntityTracker<'a> {
    tab: &'a mut [Entity],
    self_id: i32,
    dropped: u32,
}

impl<'a> EntityTracker<'a> {
    pub fn new(storage: &'a mut [Entity]) -> Self {
        for e in storage.iter_mut() {
            *e = Entity::default();
        }
        EntityTracker { tab: storage, self_id: -1, dropped: 0 }
    }
    pub fn clear(&mut self) {
        for e in self.tab.iter_mut() {
            e.used = 0;
        }
    }
    pub fn find(&self, id: i32) -> Option<&Entity> {
        self.tab.iter().find(|e| e.used != 0 && e.id == id)
    }
    pub fn self_id(&self) -> i32 {
        self.self_id
    }
    pub fn count(&self) -> u32 {
        self.tab.iter().filter(|e| e.used != 0).count() as u32
    }
    /// Spawns lost because the table was full.
    pub fn dropped(&self) -> u32 {
        self.dropped
    }
    pub fn iter(&self) -> impl Iterator<Item = &Entity> {
        self.tab.iter().filter(|e| e.used != 0)
    }
    pub fn table(&self) -> &[Entity] {
        self.tab
    }
    pub fn nearest(&self, x: f64, y: f64, z: f64, max_dist: f64, kind: Option<EntityKind>) -> Option<&Entity> {
        let mut best = None;
        let mut bd = max_dist * max_dist;
        for e in self.tab.iter() {
            if e.used != 0 && e.id != self.self_id && kind.is_none_or(|k| e.kind == k) {
                let (dx, dy, dz) = (e.x - x, e.y - y, e.z - z);
                let d = dx * dx + dy * dy + dz * dz;
                if d <= bd {
                    bd = d;
                    best = Some(e);
                }
            }
        }
        best
    }

    fn get(&mut self, id: i32) -> Option<&mut Entity> {
        self.tab.iter_mut().find(|e| e.used != 0 && e.id == id)
    }
    fn alloc(&mut self, id: i32) -> Option<&mut Entity> {
        let idx = match self.tab.iter().position(|e| e.used != 0 && e.id == id) {
            Some(i) => Some(i),
            None => self.tab.iter().position(|e| e.used == 0),
        };
        match idx {
            None => {
                self.dropped += 1;
                None
            }
            Some(i) => {
                let e = &mut self.tab[i];
                *e = Entity { used: 1, id, vehicle: -1, ..Entity::default() };
                Some(e)
            }
        }
    }

    fn apply_meta(e: &mut Entity, mut m: MetaView<'_>) {
        while let Some(me) = m.next_entry() {
            if me.key == 0 && me.kind == 0 {
                e.flags = me.i as u8;
            } else if me.key == 7 && me.kind == 2 && e.kind != EntityKind::Object {
                e.health = me.f[0];
                e.has_health = true;
            }
        }
    }

    /// Feed packets; returns true if the packet was consumed.
    pub fn handle(&mut self, p: &Packet) -> bool {
        self.handle_with(p, &mut ())
    }

    pub fn handle_with(&mut self, p: &Packet, sink: &mut dyn EntitySink) -> bool {
        if p.state != State::Play {
            return false;
        }
        crate::escape(&mut *self); // callbacks may read the tracker through other pointers
        let id = match PlayCb::from_u8(p.id) {
            Some(i) => i,
            None => return false,
        };
        macro_rules! pk {
            ($t:ty) => {
                match packets::read::<$t>(p) {
                    Some(x) => x,
                    None => return true,
                }
            };
        }
        match id {
            PlayCb::Login => {
                if let Some(l) = packets::read::<cb::Login>(p) {
                    self.clear();
                    self.self_id = l.entity_id;
                }
                false
            }
            PlayCb::Respawn => {
                self.clear();
                false
            }
            PlayCb::SpawnEntity => {
                let s = pk!(cb::SpawnEntity);
                if let Some(e) = self.alloc(s.entity_id) {
                    e.kind = EntityKind::Object;
                    e.uuid = s.object_uuid;
                    e.type_ = s.type_ as i32;
                    e.x = s.x;
                    e.y = s.y;
                    e.z = s.z;
                    e.yaw = s.yaw as u8;
                    e.pitch = s.pitch as u8;
                    e.data = s.object_data;
                    e.vx = s.velocity.x;
                    e.vy = s.velocity.y;
                    e.vz = s.velocity.z;
                    sink.event(EntityEvent::Spawned, e, 0);
                }
                true
            }
            PlayCb::SpawnEntityLiving => {
                let s = pk!(cb::SpawnEntityLiving);
                if let Some(e) = self.alloc(s.entity_id) {
                    e.kind = EntityKind::Mob;
                    e.uuid = s.entity_uuid;
                    e.type_ = s.type_;
                    e.x = s.x;
                    e.y = s.y;
                    e.z = s.z;
                    e.yaw = s.yaw as u8;
                    e.pitch = s.pitch as u8;
                    e.head_yaw = s.head_pitch as u8;
                    e.vx = s.velocity.x;
                    e.vy = s.velocity.y;
                    e.vz = s.velocity.z;
                    Self::apply_meta(e, s.metadata);
                    sink.metadata(e, s.metadata);
                    sink.event(EntityEvent::Spawned, e, 0);
                }
                true
            }
            PlayCb::NamedEntitySpawn => {
                let s = pk!(cb::NamedEntitySpawn);
                if let Some(e) = self.alloc(s.entity_id) {
                    e.kind = EntityKind::Player;
                    e.uuid = s.player_uuid;
                    e.x = s.x;
                    e.y = s.y;
                    e.z = s.z;
                    e.yaw = s.yaw as u8;
                    e.pitch = s.pitch as u8;
                    Self::apply_meta(e, s.metadata);
                    sink.metadata(e, s.metadata);
                    sink.event(EntityEvent::Spawned, e, 0);
                }
                true
            }
            PlayCb::SpawnEntityExperienceOrb => {
                let s = pk!(cb::SpawnEntityExperienceOrb);
                if let Some(e) = self.alloc(s.entity_id) {
                    e.kind = EntityKind::XpOrb;
                    e.x = s.x;
                    e.y = s.y;
                    e.z = s.z;
                    e.data = s.count as i32;
                    sink.event(EntityEvent::Spawned, e, 0);
                }
                true
            }
            PlayCb::SpawnEntityWeather => {
                let s = pk!(cb::SpawnEntityWeather);
                if let Some(e) = self.alloc(s.entity_id) {
                    e.kind = EntityKind::Weather;
                    e.type_ = s.type_ as i32;
                    e.x = s.x;
                    e.y = s.y;
                    e.z = s.z;
                    sink.event(EntityEvent::Spawned, e, 0);
                }
                true
            }
            PlayCb::SpawnEntityPainting => {
                let s = pk!(cb::SpawnEntityPainting);
                if let Some(e) = self.alloc(s.entity_id) {
                    e.kind = EntityKind::Painting;
                    e.uuid = s.entity_uuid;
                    e.x = s.location.x as f64;
                    e.y = s.location.y as f64;
                    e.z = s.location.z as f64;
                    e.data = s.direction as i32;
                    sink.event(EntityEvent::Spawned, e, 0);
                }
                true
            }
            PlayCb::RelEntityMove => {
                let m = pk!(cb::RelEntityMove);
                if let Some(e) = self.get(m.entity_id) {
                    e.x += m.d_x as f64 / 4096.0;
                    e.y += m.d_y as f64 / 4096.0;
                    e.z += m.d_z as f64 / 4096.0;
                    e.on_ground = m.on_ground;
                    sink.event(EntityEvent::Moved, e, 0);
                }
                true
            }
            PlayCb::EntityMoveLook => {
                let m = pk!(cb::EntityMoveLook);
                if let Some(e) = self.get(m.entity_id) {
                    e.x += m.d_x as f64 / 4096.0;
                    e.y += m.d_y as f64 / 4096.0;
                    e.z += m.d_z as f64 / 4096.0;
                    e.yaw = m.yaw as u8;
                    e.pitch = m.pitch as u8;
                    e.on_ground = m.on_ground;
                    sink.event(EntityEvent::Moved, e, 0);
                }
                true
            }
            PlayCb::EntityLook => {
                let m = pk!(cb::EntityLook);
                if let Some(e) = self.get(m.entity_id) {
                    e.yaw = m.yaw as u8;
                    e.pitch = m.pitch as u8;
                    e.on_ground = m.on_ground;
                    sink.event(EntityEvent::Moved, e, 0);
                }
                true
            }
            PlayCb::EntityTeleport => {
                let m = pk!(cb::EntityTeleport);
                if let Some(e) = self.get(m.entity_id) {
                    e.x = m.x;
                    e.y = m.y;
                    e.z = m.z;
                    e.yaw = m.yaw as u8;
                    e.pitch = m.pitch as u8;
                    e.on_ground = m.on_ground;
                    sink.event(EntityEvent::Moved, e, 0);
                }
                true
            }
            PlayCb::EntityHeadRotation => {
                let m = pk!(cb::EntityHeadRotation);
                if let Some(e) = self.get(m.entity_id) {
                    e.head_yaw = m.head_yaw as u8;
                    sink.event(EntityEvent::Updated, e, 0);
                }
                true
            }
            PlayCb::EntityVelocity => {
                let m = pk!(cb::EntityVelocity);
                if let Some(e) = self.get(m.entity_id) {
                    e.vx = m.velocity.x;
                    e.vy = m.velocity.y;
                    e.vz = m.velocity.z;
                    sink.event(EntityEvent::Updated, e, 0);
                }
                true
            }
            PlayCb::EntityDestroy => {
                let mut d = pk!(cb::EntityDestroy);
                while let Some(id) = d.entity_ids.next_elem() {
                    if let Some(e) = self.get(id.0) {
                        sink.event(EntityEvent::Destroying, e, 0);
                        e.used = 0;
                    }
                }
                true
            }
            PlayCb::EntityStatus => {
                if let Some(s) = packets::read::<cb::EntityStatus>(p) {
                    if let Some(e) = self.get(s.entity_id) {
                        sink.event(EntityEvent::Status, e, s.entity_status as i32);
                    }
                }
                true
            }
            PlayCb::Animation => {
                if let Some(a) = packets::read::<cb::Animation>(p) {
                    if let Some(e) = self.get(a.entity_id) {
                        sink.event(EntityEvent::Animation, e, a.animation as i32);
                    }
                }
                true
            }
            PlayCb::EntityEquipment => {
                let q = pk!(cb::EntityEquipment);
                if let Some(e) = self.get(q.entity_id) {
                    if (0..6).contains(&q.slot) {
                        e.equipment[q.slot as usize] = ItemStack { id: q.item.id, count: q.item.count, damage: q.item.damage };
                        sink.event(EntityEvent::Updated, e, 0);
                    }
                }
                true
            }
            PlayCb::EntityMetadata => {
                let m = pk!(cb::EntityMetadata);
                if let Some(e) = self.get(m.entity_id) {
                    Self::apply_meta(e, m.metadata);
                    sink.metadata(e, m.metadata);
                    sink.event(EntityEvent::Updated, e, 0);
                }
                true
            }
            PlayCb::AttachEntity => {
                let a = pk!(cb::AttachEntity);
                if let Some(e) = self.get(a.entity_id) {
                    e.vehicle = a.vehicle_id;
                    sink.event(EntityEvent::Updated, e, 0);
                }
                true
            }
            PlayCb::SetPassengers => {
                let mut s = pk!(cb::SetPassengers);
                for e in self.tab.iter_mut() {
                    if e.used != 0 && e.vehicle == s.entity_id {
                        e.vehicle = -1;
                    }
                }
                while let Some(id) = s.passengers.next_elem() {
                    if let Some(e) = self.get(id.0) {
                        e.vehicle = s.entity_id;
                        sink.event(EntityEvent::Updated, e, 0);
                    }
                }
                true
            }
            PlayCb::Collect => {
                if let Some(c) = packets::read::<cb::Collect>(p) {
                    if let Some(e) = self.get(c.collected_entity_id) {
                        sink.event(EntityEvent::Collected, e, c.collector_entity_id);
                    }
                }
                true
            }
            _ => false,
        }
    }
}

// ------------------------------------------------------------------------------------------------ tab list

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PlayerEntry {
    pub uuid: Uuid,
    /// NUL-terminated.
    pub name: [u8; 17],
    pub used: u8,
    pub has_display_name: u8,
    pub game_mode: i32,
    pub ping: i32,
}
impl PlayerEntry {
    pub fn name(&self) -> &[u8] {
        let n = self.name.iter().position(|&c| c == 0).unwrap_or(self.name.len());
        &self.name[..n]
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PlayerListEvent {
    Added,
    Removed,
    Updated,
}
pub trait PlayerListSink {
    fn event(&mut self, _ev: PlayerListEvent, _e: &PlayerEntry) {}
}
impl PlayerListSink for () {}

pub struct PlayerList<'a> {
    tab: &'a mut [PlayerEntry],
}
impl<'a> PlayerList<'a> {
    pub fn new(storage: &'a mut [PlayerEntry]) -> Self {
        for e in storage.iter_mut() {
            *e = PlayerEntry::default();
        }
        PlayerList { tab: storage }
    }
    pub fn find(&self, u: &Uuid) -> Option<&PlayerEntry> {
        self.tab.iter().find(|e| e.used != 0 && e.uuid == *u)
    }
    pub fn find_by_name(&self, name: &[u8]) -> Option<&PlayerEntry> {
        self.tab.iter().find(|e| e.used != 0 && e.name() == name)
    }
    pub fn count(&self) -> u32 {
        self.tab.iter().filter(|e| e.used != 0).count() as u32
    }
    pub fn iter(&self) -> impl Iterator<Item = &PlayerEntry> {
        self.tab.iter().filter(|e| e.used != 0)
    }
    pub fn table(&self) -> &[PlayerEntry] {
        self.tab
    }

    pub fn handle(&mut self, p: &Packet) -> bool {
        self.handle_with(p, &mut ())
    }

    pub fn handle_with(&mut self, p: &Packet, sink: &mut dyn PlayerListSink) -> bool {
        if p.state != State::Play {
            return false;
        }
        // the tab list survives dimension changes
        if p.id != PlayCb::PlayerInfo as u8 {
            return false;
        }
        crate::escape(&mut *self);
        let mut pi = match packets::read::<cb::PlayerInfo>(p) {
            Some(x) => x,
            None => return true,
        };
        while let Some(d) = pi.data.next_elem() {
            let idx = self.tab.iter().position(|e| e.used != 0 && e.uuid == d.uuid);
            match pi.action {
                0 => {
                    let idx = match idx.or_else(|| self.tab.iter().position(|e| e.used == 0)) {
                        Some(i) => i,
                        None => continue,
                    };
                    let e = &mut self.tab[idx];
                    let is_new = e.used == 0;
                    *e = PlayerEntry::default();
                    e.used = 1;
                    e.uuid = d.uuid;
                    let n = d.name.0.len().min(16);
                    e.name[..n].copy_from_slice(&d.name.0[..n]);
                    e.name[n] = 0;
                    e.game_mode = d.gamemode;
                    e.ping = d.ping;
                    e.has_display_name = d.display_name.is_some() as u8;
                    sink.event(if is_new { PlayerListEvent::Added } else { PlayerListEvent::Updated }, e);
                }
                1 => {
                    if let Some(i) = idx {
                        self.tab[i].game_mode = d.gamemode;
                        sink.event(PlayerListEvent::Updated, &self.tab[i]);
                    }
                }
                2 => {
                    if let Some(i) = idx {
                        self.tab[i].ping = d.ping;
                        sink.event(PlayerListEvent::Updated, &self.tab[i]);
                    }
                }
                3 => {
                    if let Some(i) = idx {
                        self.tab[i].has_display_name = d.display_name.is_some() as u8;
                        sink.event(PlayerListEvent::Updated, &self.tab[i]);
                    }
                }
                4 => {
                    if let Some(i) = idx {
                        sink.event(PlayerListEvent::Removed, &self.tab[i]);
                        self.tab[i].used = 0;
                    }
                }
                _ => {}
            }
        }
        true
    }
}
