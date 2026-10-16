# `inventory`: items, windows and clicks

`Inventory` tracks the bot's inventory and at most one open container window (chest, furnace, crafting table, villager,
horse …). It is **server-authoritative**: state only changes when the server sends it. The click helpers send the
corresponding `WindowClick`; the server then answers with the corrected state, which `Inventory` stores. With a
[`Session`](session.md) it is `session.inventory`.

## Slots

Items are `entities::ItemStack { id: i16, count: i8, damage: i16 }`; `id == -1` is empty. Item ids are the classic
numeric ids (264 diamond, 260 apple, 276 diamond sword, 323 sign, 368 ender pearl …). NBT is not stored.

**Player inventory window (id 0), 46 slots:**

| Slots | Constant | Content |
|-------|----------|---------|
| 0 | `SLOT_CRAFT_OUT` | crafting result |
| 1 – 4 | `SLOT_CRAFT_GRID` | 2×2 crafting grid |
| 5 – 8 | `SLOT_ARMOR_FIRST` | boots, leggings, chestplate, helmet |
| 9 – 35 | `SLOT_MAIN_FIRST` | main inventory |
| 36 – 44 | `SLOT_HOTBAR_FIRST` | hotbar (hotbar index = slot − 36) |
| 45 | `SLOT_OFFHAND` | off hand |

`PLAYER_SLOTS = 46`. In an **open container window** the slot numbers are the container's own slots first
(e.g. chest: 0 – 26 or 0 – 53), followed by the player's 36 main+hotbar slots. `MAX_WINDOW_SLOTS = 96`.

## `Window`

`Window::new()` / `Default` is an empty, closed window.

```rust
pub struct Window { pub id: i8, pub open: bool, pub count: u8, pub loaded: bool, pub type_: [u8; 28],
                    pub title: [u8; 64], pub entity_id: i32, pub props: [i16; 8], pub slots: [ItemStack; 96] }
```

| Field | Meaning |
|-------|---------|
| `id` | window id (0 = player inventory) |
| `open` | currently open |
| `count` | number of slots (exact after the contents arrived) |
| `loaded` | the server has sent the contents – **wait for this before reading `slots`** |
| `type_` / `type_is("minecraft:chest")` | NUL-terminated window type, e.g. `minecraft:chest`, `minecraft:crafting_table`, `minecraft:furnace`, `minecraft:villager` |
| `title` | NUL-terminated JSON title |
| `entity_id` | horse window: the horse |
| `props` | window properties (furnace progress/fuel, enchantment levels, …) |
| `slots` | the contents |

## `Inventory`

| Method | Description |
|--------|-------------|
| `Inventory::new()` / `Default` | an empty inventory (a `Session` creates its own) |
| `player() -> &Window` | the player inventory window (id 0) |
| `open_window() -> Option<&Window>` | the open container, if any |
| `cursor() -> &ItemStack` | the stack held on the mouse cursor |
| `held_slot() -> u8` | selected hotbar index 0 – 8 |
| `held_item() -> &ItemStack` | the item in the selected hotbar slot |
| `item_at(window_id: i8, slot: i16) -> &ItemStack` | any slot; an empty stack for unknown slots |
| `find(id, damage) -> Option<usize>` | first player-inventory slot (hotbar first) with item `id`; `damage < 0` = any damage |
| `count_of(id, damage) -> i32` | how many of that item the player has |
| `first_empty_slot() -> Option<usize>` | first free main/hotbar slot |
| `select_hotbar(tx, index) -> bool` | select a hotbar slot (updates `held_slot` immediately) |
| `click(tx, slot, button, mode) -> bool` | send a click in the open window (or the player inventory if none is open) |
| `shift_click(tx, slot)` | quick-move a stack |
| `drop_from_slot(tx, slot, whole_stack)` | drop one item or the whole stack |
| `swap_with_hotbar(tx, slot, hotbar_index)` | swap with a hotbar slot (number key) |
| `creative_set(tx, slot, item_id, count, damage)` | creative mode: put items into a slot |
| `creative_clear(tx, slot)` | creative mode: empty a slot |
| `close_window(tx) -> bool` | close the open window |
| `reset()` | clear (done on connect) |
| `handle(&Packet, tx)` / `handle_with(&Packet, tx, &mut dyn InventorySink)` | feed a packet (done for you by `Session`) |

`tx` is any [`Sender`](client.md#sender), e.g. `&mut session.client.link`. All click helpers return `false` if the packet
could not be queued.

`click(tx, slot, button, mode)` sends the raw `WindowClick`; `button` meaning depends on the mode (see the
[wiki](https://wiki.vg/index.php?title=Protocol&oldid=14204#Click_Window)):

| `ClickMode` | Value | `button` |
|-------------|-------|----------|
| `Normal` | 0 | 0 left click, 1 right click; slot −999 = drop outside the window |
| `Shift` | 1 | 0 / 1 – quick move (what `shift_click` sends) |
| `HotbarSwap` | 2 | hotbar index 0 – 8 (what `swap_with_hotbar` sends) |
| `Middle` | 3 | 2 – creative "pick block" clone |
| `Drop` | 4 | 0 drop one, 1 drop stack (what `drop_from_slot` sends) |
| `Drag` | 5 | painting: 0/4/8 start, 1/5/9 add slot, 2/6/10 end (left/right/middle) |
| `Double` | 6 | 0 – collect all matching items to the cursor |

### How clicks resolve

The helpers keep no client-side prediction. Each click reports an impossible "clicked item", which makes vanilla reply
with a rejection plus a full resync (`WindowItems` + cursor). `Inventory` acknowledges the rejection itself and then
holds the server's state. So after a click, **wait for the update** instead of assuming the result:

```rust
let slot = session.inventory.find(264, -1).unwrap();                 // a diamond stack
session.inventory.click(&mut session.client.link, slot as i16, 0, ClickMode::Normal);
// … keep calling poll …; the stack is on the cursor once:
session.inventory.cursor().id == 264
```

Opening and using a chest:

```rust
session.look_at(cx as f64 + 0.5, cy as f64 + 0.5, cz as f64 + 0.5);
session.place_block(BlockPos { x: cx, y: cy, z: cz }, 1, 0);          // right-click
// poll until:
let w = session.inventory.open_window();                               // Some(w) with w.loaded
session.inventory.shift_click(&mut session.client.link, 0);          // move chest slot 0 to the inventory
session.inventory.close_window(&mut session.client.link);
```

## `InventorySink`

```rust
pub trait InventorySink {
    fn slot(&mut self, window_id: i8, slot: i16, item: &Slot<'_>) {}
    fn window(&mut self, w: &Window, opened: bool) {}
}
```

`slot` fires for every slot update (including NBT, valid only during the call). A cursor change is reported with
`window_id == -1, slot == -1`. `window` fires when a container opens (`opened = true`, `w.loaded` still false) and
closes. With a `Session` these are `SessionSink::slot` / `window`.
