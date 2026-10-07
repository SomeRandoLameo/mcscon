// Inventory & window tracking (1.10-1.12.2). Server-authoritative: state changes only through server packets;
// the click helpers just send the corresponding WindowClick and the server's SetSlot/WindowItems corrects us.
#pragma once
#include "entities.h"   // ItemStack

namespace mc {

#ifndef MCSCON_MAX_WINDOW_SLOTS
#define MCSCON_MAX_WINDOW_SLOTS 96     // large chest 54 + 36 player slots = 90
#endif

// Player inventory (window 0) layout
enum PlayerSlot { SlotCraftOut = 0, SlotCraftGrid = 1, SlotArmorFirst = 5 /*boots*/, SlotMainFirst = 9, SlotHotbarFirst = 36, SlotOffhand = 45, PlayerSlots = 46 };

struct Window {
    i8 id; bool open; u8 count;                 // count: number of slots (expected size right after OpenWindow, exact after WindowItems)
    bool loaded;                                 // true once the server sent the contents (WindowItems); wait for this before reading slots
    char type[28]; char title[64]; i32 entityId; // entityId only for horses
    i16 props[8];                                // CraftProgressBar values (furnace progress, enchant levels, ...)
    ItemStack slots[MCSCON_MAX_WINDOW_SLOTS];
    bool typeIs(const char* t) const { return !strcmp(type, t); }
};

// mode (WindowClick): 0 click, 1 shift-click, 2 number key, 3 middle (creative clone), 4 drop, 5 drag, 6 double click
enum ClickMode { ClickNormal = 0, ClickShift = 1, ClickHotbarSwap = 2, ClickMiddle = 3, ClickDrop = 4, ClickDrag = 5, ClickDouble = 6 };

typedef void (*SlotFn)(void* user, i8 windowId, i16 slot, const Slot& item);   // item.nbt valid only inside the callback
typedef void (*WindowFn)(void* user, const Window& w, bool opened);

class Inventory {
public:
    Inventory() : held_(0), actionNo_(0), slotFn_(0), winFn_(0), user_(0) { reset(); }
    void onSlot(SlotFn fn, void* user) { slotFn_ = fn; user_ = user; }
    void onWindow(WindowFn fn) { winFn_ = fn; }
    void reset();
    bool handle(const Packet& p, Client& c);        // true if consumed; replies to unaccepted transactions itself

    const Window& player() const { return player_; }
    const Window* openWindow() const { return other_.open ? &other_ : 0; }
    const ItemStack& cursor() const { return cursor_; }
    u8 heldSlot() const { return held_; }                         // hotbar index 0..8
    const ItemStack& heldItem() const { return player_.slots[SlotHotbarFirst + held_]; }
    const ItemStack& itemAt(i8 windowId, i16 slot) const;         // empty stack for unknown slots
    // first player-inventory slot (hotbar first) holding `id` (damage < 0: any); -1 if none
    int find(i16 id, i16 damage = -1) const;
    int countOf(i16 id, i16 damage = -1) const;
    int firstEmptySlot() const;                                   // main inventory/hotbar slot index, -1 if full

    // --- actions
    bool selectHotbar(Client& c, u8 index);                       // updates local state immediately (server never echoes it)
    bool click(Client& c, i16 slot, u8 button = 0, ClickMode mode = ClickNormal);   // in the open window (or player inventory)
    bool shiftClick(Client& c, i16 slot) { return click(c, slot, 0, ClickShift); }
    bool dropFromSlot(Client& c, i16 slot, bool wholeStack = false) { return click(c, slot, wholeStack ? 1 : 0, ClickDrop); }
    bool swapWithHotbar(Client& c, i16 slot, u8 hotbarIndex) { return click(c, slot, hotbarIndex, ClickHotbarSwap); }
    bool creativeSet(Client& c, i16 slot, i16 itemId, i8 count = 1, i16 damage = 0);   // creative mode only
    bool creativeClear(Client& c, i16 slot) { return creativeSet(c, slot, -1, 0, 0); }
    bool closeWindow(Client& c);

private:
    Window& win(i8 id) { return id == 0 ? player_ : other_; }
    void store(Window& w, i16 slot, const Slot& s);
    Window player_, other_; ItemStack cursor_; u8 held_; u16 actionNo_; SlotFn slotFn_; WindowFn winFn_; void* user_;
};

} // namespace mc
