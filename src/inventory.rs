//! Inventory & window tracking (1.10-1.12.2). Server-authoritative: state changes only through server packets;
//! the click helpers just send the corresponding WindowClick and the server's SetSlot/WindowItems corrects us.
use crate::client::{Packet, Sender};
use crate::entities::ItemStack;
use crate::ids::PlayCb;
use crate::packets::{self, cb, sb};
use crate::types::State;
use crate::wire::{Bytes, Slot, Str};

/// Large chest 54 + 36 player slots = 90.
pub const MAX_WINDOW_SLOTS: usize = 96;

// Player inventory (window 0) layout
pub const SLOT_CRAFT_OUT: usize = 0;
pub const SLOT_CRAFT_GRID: usize = 1;
/// Boots first.
pub const SLOT_ARMOR_FIRST: usize = 5;
pub const SLOT_MAIN_FIRST: usize = 9;
pub const SLOT_HOTBAR_FIRST: usize = 36;
pub const SLOT_OFFHAND: usize = 45;
pub const PLAYER_SLOTS: usize = 46;

const EMPTY: ItemStack = ItemStack { id: -1, count: 0, damage: 0 };

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Window {
    pub id: i8,
    pub open: bool,
    /// Number of slots (expected size right after OpenWindow, exact after WindowItems).
    pub count: u8,
    /// True once the server sent the contents (WindowItems); wait for this before reading slots.
    pub loaded: bool,
    /// NUL-terminated window type, e.g. "minecraft:chest".
    pub type_: [u8; 28],
    pub title: [u8; 64],
    /// Only for horses.
    pub entity_id: i32,
    /// CraftProgressBar values (furnace progress, enchant levels, ...).
    pub props: [i16; 8],
    pub slots: [ItemStack; MAX_WINDOW_SLOTS],
}
impl Window {
    pub const fn new() -> Window {
        Window { id: 0, open: false, count: 0, loaded: false, type_: [0; 28], title: [0; 64], entity_id: 0, props: [0; 8], slots: [EMPTY; MAX_WINDOW_SLOTS] }
    }
    pub fn type_is(&self, t: &str) -> bool {
        let n = self.type_.iter().position(|&c| c == 0).unwrap_or(self.type_.len());
        &self.type_[..n] == t.as_bytes()
    }
}
impl Default for Window {
    fn default() -> Self {
        Window::new()
    }
}

/// `WindowClick` mode.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ClickMode {
    Normal = 0,
    Shift = 1,
    HotbarSwap = 2,
    /// Creative clone.
    Middle = 3,
    Drop = 4,
    Drag = 5,
    Double = 6,
}

/// Receives slot and window changes. `item.nbt` is valid only inside the call.
pub trait InventorySink {
    /// Cursor changes are reported as `window_id == -1, slot == -1`.
    fn slot(&mut self, _window_id: i8, _slot: i16, _item: &Slot<'_>) {}
    fn window(&mut self, _w: &Window, _opened: bool) {}
}
impl InventorySink for () {}

pub struct Inventory {
    player: Window,
    other: Window,
    cursor: ItemStack,
    held: u8,
    action_no: u16,
}

impl Default for Inventory {
    fn default() -> Self {
        Self::new()
    }
}

fn copy_cstr(dst: &mut [u8], src: Str<'_>) {
    let n = src.0.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&src.0[..n]);
}

impl Inventory {
    pub fn new() -> Self {
        let mut i = Inventory { player: Window::new(), other: Window::new(), cursor: EMPTY, held: 0, action_no: 0 };
        i.reset();
        i
    }
    pub fn reset(&mut self) {
        self.player = Window::new();
        self.other = Window::new();
        self.cursor = EMPTY;
        self.player.id = 0;
        self.player.open = true;
        self.player.count = PLAYER_SLOTS as u8;
        self.other.id = -1;
        self.held = 0;
    }

    pub fn player(&self) -> &Window {
        &self.player
    }
    pub fn open_window(&self) -> Option<&Window> {
        if self.other.open {
            Some(&self.other)
        } else {
            None
        }
    }
    pub fn cursor(&self) -> &ItemStack {
        &self.cursor
    }
    /// Hotbar index 0..8.
    pub fn held_slot(&self) -> u8 {
        self.held
    }
    pub fn held_item(&self) -> &ItemStack {
        &self.player.slots[SLOT_HOTBAR_FIRST + self.held as usize]
    }
    /// Empty stack for unknown slots.
    pub fn item_at(&self, window_id: i8, slot: i16) -> &ItemStack {
        let w = if window_id == 0 { &self.player } else { &self.other };
        if (window_id != 0 && (!self.other.open || window_id != self.other.id)) || slot < 0 || slot as usize >= MAX_WINDOW_SLOTS {
            return &EMPTY;
        }
        &w.slots[slot as usize]
    }
    /// First player-inventory slot (hotbar first) holding `id` (`damage < 0`: any); `None` if none.
    pub fn find(&self, id: i16, damage: i16) -> Option<usize> {
        for pass in 0..2 {
            let (from, to) = if pass == 0 { (SLOT_HOTBAR_FIRST, SLOT_OFFHAND) } else { (SLOT_MAIN_FIRST, SLOT_HOTBAR_FIRST) };
            for i in from..to {
                let s = &self.player.slots[i];
                if s.id == id && (damage < 0 || s.damage == damage) {
                    return Some(i);
                }
            }
        }
        None
    }
    pub fn count_of(&self, id: i16, damage: i16) -> i32 {
        let mut n = 0;
        for i in SLOT_MAIN_FIRST..=SLOT_OFFHAND {
            let s = &self.player.slots[i];
            if s.id == id && (damage < 0 || s.damage == damage) {
                n += s.count as i32;
            }
        }
        n
    }
    /// Main inventory/hotbar slot index, `None` if full.
    pub fn first_empty_slot(&self) -> Option<usize> {
        (SLOT_HOTBAR_FIRST..SLOT_OFFHAND).chain(SLOT_MAIN_FIRST..SLOT_HOTBAR_FIRST).find(|&i| self.player.slots[i].id == -1)
    }

    fn store(&mut self, other: bool, slot: i16, s: &Slot, sink: &mut dyn InventorySink) {
        if slot < 0 || slot as usize >= MAX_WINDOW_SLOTS {
            return;
        }
        let item = ItemStack { id: s.id, count: s.count, damage: s.damage };
        let wid;
        {
            let w = if other { &mut self.other } else { &mut self.player };
            w.slots[slot as usize] = item;
            if slot as usize >= w.count as usize {
                w.count = (slot + 1) as u8;
            }
            wid = w.id;
        }
        // In an open window the last 36 slots are the player's main inventory + hotbar: mirror them into window 0.
        if other && self.other.count >= 36 && slot as usize >= self.other.count as usize - 36 {
            let ps = SLOT_MAIN_FIRST + (slot as usize - (self.other.count as usize - 36));
            if ps < SLOT_OFFHAND {
                self.player.slots[ps] = self.other.slots[slot as usize];
            }
        }
        sink.slot(wid, slot, s);
    }

    /// Feed packets; returns true if consumed. Replies to unaccepted transactions itself.
    pub fn handle<S: Sender + ?Sized>(&mut self, p: &Packet, tx: &mut S) -> bool {
        self.handle_with(p, tx, &mut ())
    }

    pub fn handle_with<S: Sender + ?Sized>(&mut self, p: &Packet, tx: &mut S, sink: &mut dyn InventorySink) -> bool {
        if p.state != State::Play {
            return false;
        }
        crate::escape(&mut *self); // callbacks may read the inventory through other pointers
        let id = match PlayCb::from_u8(p.id) {
            Some(i) => i,
            None => return false,
        };
        match id {
            PlayCb::Login => {
                self.reset();
                false
            }
            PlayCb::Respawn => false,
            PlayCb::HeldItemSlot => {
                if let Some(h) = packets::read::<cb::HeldItemSlot>(p) {
                    if (0..9).contains(&h.slot) {
                        self.held = h.slot as u8;
                    }
                }
                true
            }
            PlayCb::WindowItems => {
                let mut w = match packets::read::<cb::WindowItems>(p) {
                    Some(w) => w,
                    None => return true,
                };
                let is_other = w.window_id != 0;
                if is_other && !self.other.open {
                    // items may precede OpenWindow handling in odd servers
                    self.other.id = w.window_id as i8;
                    self.other.count = 0;
                }
                {
                    let t = if is_other { &mut self.other } else { &mut self.player };
                    for s in t.slots.iter_mut() {
                        s.id = -1;
                    }
                    t.loaded = true;
                    // known before storing, for the player-slot mirror
                    t.count = (w.items.size() as usize).min(MAX_WINDOW_SLOTS) as u8;
                }
                let mut i: i16 = 0;
                while let Some(s) = w.items.next_elem() {
                    self.store(is_other, i, &s, sink);
                    i += 1;
                }
                if !is_other {
                    self.player.count = PLAYER_SLOTS as u8;
                }
                true
            }
            PlayCb::SetSlot => {
                let s = match packets::read::<cb::SetSlot>(p) {
                    Some(s) => s,
                    None => return true,
                };
                if s.window_id == -1 && s.slot == -1 {
                    self.cursor = ItemStack { id: s.item.id, count: s.item.count, damage: s.item.damage };
                    sink.slot(-1, -1, &s.item);
                } else if s.window_id == -2 || s.window_id == 0 {
                    self.store(false, s.slot, &s.item, sink);
                } else if self.other.open && s.window_id == self.other.id {
                    self.store(true, s.slot, &s.item, sink);
                }
                true
            }
            PlayCb::OpenWindow => {
                let o = match packets::read::<cb::OpenWindow>(p) {
                    Some(o) => o,
                    None => return true,
                };
                self.other = Window::new();
                self.other.id = o.window_id as i8;
                self.other.open = true;
                self.other.count = ((o.slot_count as usize + 36).min(MAX_WINDOW_SLOTS)) as u8;
                copy_cstr(&mut self.other.type_, o.inventory_type);
                copy_cstr(&mut self.other.title, o.window_title);
                self.other.entity_id = o.entity_id;
                sink.window(&self.other, true);
                true
            }
            PlayCb::CloseWindow => {
                if self.other.open {
                    self.other.open = false;
                    sink.window(&self.other, false);
                }
                true
            }
            PlayCb::CraftProgressBar => {
                if let Some(b) = packets::read::<cb::CraftProgressBar>(p) {
                    if self.other.open && b.window_id == self.other.id as u8 && (0..8).contains(&b.property) {
                        self.other.props[b.property as usize] = b.value;
                    }
                }
                true
            }
            PlayCb::Transaction => {
                if let Some(t) = packets::read::<cb::Transaction>(p) {
                    if !t.accepted {
                        let r = sb::Transaction { window_id: t.window_id, action: t.action, accepted: true };
                        packets::send(tx, &r);
                    }
                }
                true
            }
            _ => false,
        }
    }

    // ------------------------------------------------------------------ actions

    /// Updates local state immediately (the server never echoes it).
    pub fn select_hotbar<S: Sender + ?Sized>(&mut self, tx: &mut S, index: u8) -> bool {
        if index > 8 {
            return false;
        }
        if !packets::send(tx, &sb::HeldItemSlot { slot_id: index as i16 }) {
            return false;
        }
        self.held = index;
        true
    }

    /// The vanilla server does not echo a click whose predicted outcome (the "clicked item" field) matches its own
    /// result - it assumes the client simulated the click itself. We keep no client-side prediction (state would need
    /// per-container transfer rules), so we deliberately report an impossible item: the server then answers with
    /// Transaction(accepted=false) plus a full resync (WindowItems + cursor), which `handle` acknowledges.
    pub fn click<S: Sender + ?Sized>(&mut self, tx: &mut S, slot: i16, button: u8, mode: ClickMode) -> bool {
        let mut k = sb::WindowClick {
            window_id: if self.other.open { self.other.id as u8 } else { 0 },
            slot,
            mouse_button: button as i8,
            action: self.action_no as i16,
            mode: mode as i8,
            ..Default::default()
        };
        self.action_no = self.action_no.wrapping_add(1);
        // A stack no real click can produce (stone, damage 32767): robust whatever the server returns for the click
        // (1.12.2 returns "empty" for a shift-click that empties the source slot, older versions the moved stack).
        k.item = Slot { id: 1, count: 1, damage: 32767, nbt: Bytes(&[]) };
        packets::send(tx, &k)
    }
    pub fn shift_click<S: Sender + ?Sized>(&mut self, tx: &mut S, slot: i16) -> bool {
        self.click(tx, slot, 0, ClickMode::Shift)
    }
    pub fn drop_from_slot<S: Sender + ?Sized>(&mut self, tx: &mut S, slot: i16, whole_stack: bool) -> bool {
        self.click(tx, slot, whole_stack as u8, ClickMode::Drop)
    }
    pub fn swap_with_hotbar<S: Sender + ?Sized>(&mut self, tx: &mut S, slot: i16, hotbar_index: u8) -> bool {
        self.click(tx, slot, hotbar_index, ClickMode::HotbarSwap)
    }
    /// Creative mode only.
    pub fn creative_set<S: Sender + ?Sized>(&mut self, tx: &mut S, slot: i16, item_id: i16, count: i8, damage: i16) -> bool {
        let s = sb::SetCreativeSlot { slot, item: Slot { id: item_id, count, damage, nbt: Bytes(&[]) } };
        if !packets::send(tx, &s) {
            return false;
        }
        if slot >= 0 && (slot as usize) < PLAYER_SLOTS {
            self.player.slots[slot as usize] = ItemStack { id: item_id, count, damage };
        }
        true
    }
    pub fn creative_clear<S: Sender + ?Sized>(&mut self, tx: &mut S, slot: i16) -> bool {
        self.creative_set(tx, slot, -1, 0, 0)
    }
    pub fn close_window<S: Sender + ?Sized>(&mut self, tx: &mut S) -> bool {
        let w = sb::CloseWindow { window_id: if self.other.open { self.other.id as u8 } else { 0 } };
        self.other.open = false;
        packets::send(tx, &w)
    }
}
