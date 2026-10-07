#include <mcscon/inventory.h>
#include <mcscon/packets.h>

namespace mc {

void Inventory::reset() {
    memset(&player_, 0, sizeof player_); memset(&other_, 0, sizeof other_); memset(&cursor_, 0, sizeof cursor_);
    player_.id = 0; player_.open = true; player_.count = PlayerSlots; other_.id = -1;
    for (int i = 0; i < MCSCON_MAX_WINDOW_SLOTS; ++i) { player_.slots[i].id = -1; other_.slots[i].id = -1; }
    cursor_.id = -1; held_ = 0;
}

void Inventory::store(Window& w, i16 slot, const Slot& s) {
    if (slot < 0 || slot >= MCSCON_MAX_WINDOW_SLOTS) return;
    w.slots[slot].id = s.id; w.slots[slot].count = s.count; w.slots[slot].damage = s.damage;
    if (slot >= w.count) w.count = (u8)(slot + 1);
    // In an open window the last 36 slots are the player's main inventory + hotbar: mirror them into window 0.
    if (&w == &other_ && w.count >= 36 && slot >= w.count - 36) {
        int ps = SlotMainFirst + (slot - (w.count - 36));
        if (ps < SlotOffhand) { player_.slots[ps] = w.slots[slot]; }
    }
    if (slotFn_) slotFn_(user_, w.id, slot, s);
}

const ItemStack& Inventory::itemAt(i8 windowId, i16 slot) const {
    static const ItemStack empty = {-1, 0, 0};
    const Window& w = windowId == 0 ? player_ : other_;
    if ((windowId != 0 && (!other_.open || windowId != other_.id)) || slot < 0 || slot >= MCSCON_MAX_WINDOW_SLOTS) return empty;
    return w.slots[slot];
}
int Inventory::find(i16 id, i16 damage) const {
    for (int pass = 0; pass < 2; ++pass) {
        int from = pass == 0 ? SlotHotbarFirst : SlotMainFirst, to = pass == 0 ? SlotOffhand : SlotHotbarFirst;
        for (int i = from; i < to; ++i) if (player_.slots[i].id == id && (damage < 0 || player_.slots[i].damage == damage)) return i;
    }
    return -1;
}
int Inventory::countOf(i16 id, i16 damage) const {
    int n = 0;
    for (int i = SlotMainFirst; i <= SlotOffhand; ++i) if (player_.slots[i].id == id && (damage < 0 || player_.slots[i].damage == damage)) n += player_.slots[i].count;
    return n;
}
int Inventory::firstEmptySlot() const {
    for (int i = SlotHotbarFirst; i < SlotOffhand; ++i) if (player_.slots[i].id == -1) return i;
    for (int i = SlotMainFirst; i < SlotHotbarFirst; ++i) if (player_.slots[i].id == -1) return i;
    return -1;
}

bool Inventory::handle(const Packet& p, Client& c) {
    if (p.state != State::Play) return false;
    switch ((ids::PlayCb)p.id) {
    case ids::PlayCb::Login: reset(); return false;
    case ids::PlayCb::Respawn: return false;
    case ids::PlayCb::HeldItemSlot: { cb::HeldItemSlot h; if (pkt::read(p, h) && h.slot >= 0 && h.slot < 9) held_ = (u8)h.slot; return true; }
    case ids::PlayCb::WindowItems: {
        cb::WindowItems w; if (!pkt::read(p, w)) return true;
        Window& t = w.windowId == 0 ? player_ : other_;
        if (w.windowId != 0 && !other_.open) { other_.id = (i8)w.windowId; other_.count = 0; }   // items may precede OpenWindow handling in odd servers
        Slot s; i16 i = 0;
        for (int k = 0; k < MCSCON_MAX_WINDOW_SLOTS; ++k) t.slots[k].id = -1;
        t.loaded = true;
        t.count = (u8)(w.items.size() > MCSCON_MAX_WINDOW_SLOTS ? MCSCON_MAX_WINDOW_SLOTS : w.items.size());   // known before storing, for the player-slot mirror
        while (w.items.next(s)) { store(t, i++, s); }
        if (w.windowId == 0) t.count = PlayerSlots;
        return true;
    }
    case ids::PlayCb::SetSlot: {
        cb::SetSlot s; if (!pkt::read(p, s)) return true;
        if (s.windowId == -1 && s.slot == -1) { cursor_.id = s.item.id; cursor_.count = s.item.count; cursor_.damage = s.item.damage; if (slotFn_) slotFn_(user_, -1, -1, s.item); }
        else if (s.windowId == -2) store(player_, s.slot, s.item);
        else if (s.windowId == 0) store(player_, s.slot, s.item);
        else if (other_.open && s.windowId == other_.id) store(other_, s.slot, s.item);
        return true;
    }
    case ids::PlayCb::OpenWindow: {
        cb::OpenWindow o; if (!pkt::read(p, o)) return true;
        memset(&other_, 0, sizeof other_);
        for (int i = 0; i < MCSCON_MAX_WINDOW_SLOTS; ++i) other_.slots[i].id = -1;
        other_.id = (i8)o.windowId; other_.open = true; other_.count = (u8)(o.slotCount + 36 > MCSCON_MAX_WINDOW_SLOTS ? MCSCON_MAX_WINDOW_SLOTS : o.slotCount + 36);
        u32 n = o.inventoryType.len < sizeof other_.type - 1 ? o.inventoryType.len : sizeof other_.type - 1; memcpy(other_.type, o.inventoryType.data, n);
        n = o.windowTitle.len < sizeof other_.title - 1 ? o.windowTitle.len : sizeof other_.title - 1; memcpy(other_.title, o.windowTitle.data, n);
        other_.entityId = o.entityId;
        if (winFn_) winFn_(user_, other_, true);
        return true;
    }
    case ids::PlayCb::CloseWindow: { if (other_.open) { other_.open = false; if (winFn_) winFn_(user_, other_, false); } return true; }
    case ids::PlayCb::CraftProgressBar: {
        cb::CraftProgressBar b; if (pkt::read(p, b) && other_.open && b.windowId == (u8)other_.id && b.property >= 0 && b.property < 8) other_.props[b.property] = b.value;
        return true;
    }
    case ids::PlayCb::Transaction: {
        cb::Transaction t; if (!pkt::read(p, t)) return true;
        if (!t.accepted) { sb::Transaction r = {}; r.windowId = t.windowId; r.action = t.action; r.accepted = true; pkt::send(c, r); }
        return true;
    }
    default: return false;
    }
}

bool Inventory::selectHotbar(Client& c, u8 index) {
    if (index > 8) return false;
    sb::HeldItemSlot h = {}; h.slotId = index; if (!pkt::send(c, h)) return false;
    held_ = index; return true;
}

// The vanilla server does not echo a click whose predicted outcome (the "clicked item" field) matches its own
// result - it assumes the client simulated the click itself. We keep no client-side prediction (state would need
// per-container transfer rules), so we deliberately report an impossible item: the server then answers with
// Transaction(accepted=false) plus a full resync (WindowItems + cursor), which handle() acknowledges.
bool Inventory::click(Client& c, i16 slot, u8 button, ClickMode mode) {
    sb::WindowClick k = {};
    k.windowId = (u8)(other_.open ? other_.id : 0); k.slot = slot; k.mouseButton = (i8)button; k.action = (i16)actionNo_++; k.mode = (i8)mode;
    // A stack no real click can produce (stone, damage 32767): robust whatever the server returns for the click
    // (1.12.2 returns "empty" for a shift-click that empties the source slot, older versions the moved stack).
    k.item.id = 1; k.item.count = 1; k.item.damage = 32767;
    return pkt::send(c, k);
}

bool Inventory::creativeSet(Client& c, i16 slot, i16 itemId, i8 count, i16 damage) {
    sb::SetCreativeSlot s = {}; s.slot = slot; s.item.id = itemId; s.item.count = count; s.item.damage = damage;
    if (!pkt::send(c, s)) return false;
    if (slot >= 0 && slot < PlayerSlots) { player_.slots[slot].id = itemId; player_.slots[slot].count = count; player_.slots[slot].damage = damage; }
    return true;
}

bool Inventory::closeWindow(Client& c) {
    sb::CloseWindow w = {}; w.windowId = (u8)(other_.open ? other_.id : 0);
    other_.open = false; return pkt::send(c, w);
}

} // namespace mc
