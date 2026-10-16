//! Serverbound encoders vs. hand-computed wire bytes (version differences included).
use mcscon::packets::sb;
use mcscon::packets::ServerPacket;
use mcscon::wire::{Bytes, Str, Writer};
use mcscon::Version;

fn enc<P: ServerPacket>(p: &P, v: Version) -> (Vec<u8>, bool) {
    let mut buf = [0u8; 512];
    let mut w = Writer::new(&mut buf);
    let ok = p.encode(&mut w, v);
    let n = w.pos;
    (buf[..n].to_vec(), ok)
}
fn e<P: ServerPacket>(p: &P, v: Version) -> Vec<u8> {
    enc(p, v).0
}
fn s(x: &str) -> Str<'_> {
    Str(x.as_bytes())
}

#[test]
fn basic() {
    assert_eq!(e(&sb::Chat { message: s("hi") }, Version::V1_12_2), [2, b'h', b'i']);
    let st = sb::Settings { locale: s("en"), view_distance: 8, chat_flags: 0, chat_colors: true, skin_parts: 0x7F, main_hand: 1, ..Default::default() };
    assert_eq!(e(&st, Version::V1_10), [2, b'e', b'n', 8, 0, 1, 0x7F, 1]);
    assert_eq!(e(&sb::ClientCommand { action_id: 0 }, Version::V1_12), [0]);
    assert_eq!(e(&sb::TeleportConfirm { teleport_id: 300 }, Version::V1_12), [0xAC, 2]);
    let pl = sb::PositionLook { x: 1.0, yaw: 90.0, on_ground: true, ..Default::default() };
    let b = e(&pl, Version::V1_12_2);
    assert!(b.len() == 33 && b[0] == 0x3F && b[1] == 0xF0 && b[32] == 1);
}

#[test]
fn versions() {
    let k = sb::KeepAlive { keep_alive_id: 0x0102 };
    assert_eq!(e(&k, Version::V1_10), [0x82, 0x02]);
    assert_eq!(e(&k, Version::V1_12), [0x82, 0x02]);
    assert_eq!(e(&k, Version::V1_12_1), [0x82, 0x02]); // 1.12.1 still VarInt
    assert_eq!(e(&k, Version::V1_12_2), [0, 0, 0, 0, 0, 0, 1, 2]); // 1.12.2: long
    let mut bp = sb::BlockPlace { direction: 1, hand: 0, cursor_x: 8.0, cursor_y: 4.0, cursor_z: 0.0, ..Default::default() };
    bp.location.x = 1;
    bp.location.y = 2;
    bp.location.z = 3;
    let a = e(&bp, Version::V1_10);
    assert!(a.len() == 8 + 1 + 1 + 3 && a[10] == 8 && a[11] == 4);
    bp.cursor_x = 0.5;
    let c = e(&bp, Version::V1_11);
    assert!(c.len() == 8 + 1 + 1 + 12 && c[10] == 0x3F && c[11] == 0);
    assert!(c[0] == 0 && c[7] == 3); // position packing: x<<38|y<<26|z
}

#[test]
fn switches() {
    let mut u = sb::UseEntity { target: 7, mouse: 1, ..Default::default() }; // attack: no extra fields
    assert_eq!(e(&u, Version::V1_12_2), [7, 1]);
    u.mouse = 0;
    u.hand = 1;
    assert_eq!(e(&u, Version::V1_12_2), [7, 0, 1]); // interact: hand
    u.mouse = 2;
    u.x = 1.0;
    u.hand = 0;
    let b = e(&u, Version::V1_12_2);
    assert!(b.len() == 2 + 12 + 1 && b[2] == 0x3F); // interact_at: xyz + hand
    let mut at = sb::AdvancementTab { action: 0, tab_id: s("a:b") };
    assert_eq!(e(&at, Version::V1_12_2), [0, 3, b'a', b':', b'b']);
    at.action = 1;
    assert_eq!(e(&at, Version::V1_12_2), [1]);
    let mut cb = sb::CraftingBookData { type_: 1, crafting_book_open: true, crafting_filter: false, ..Default::default() };
    assert_eq!(e(&cb, Version::V1_12_2), [1, 1, 0]);
    cb.type_ = 0;
    cb.displayed_recipe = 5;
    assert_eq!(e(&cb, Version::V1_12_2), [0, 0, 0, 0, 5]);
}

#[test]
fn slots_and_arrays() {
    let mut sc = sb::SetCreativeSlot { slot: 36, ..Default::default() };
    sc.item.id = 1;
    sc.item.count = 64;
    sc.item.damage = 0;
    assert_eq!(e(&sc, Version::V1_12_2), [0, 36, 0, 1, 64, 0, 0, 0]); // id, count, damage, TAG_End
    sc.item.id = -1;
    assert_eq!(e(&sc, Version::V1_12_2), [0, 36, 0xFF, 0xFF]);
    let mut wc = sb::WindowClick { window_id: 1, slot: 5, mouse_button: 0, action: 3, mode: 0, ..Default::default() };
    wc.item.id = -1;
    assert_eq!(e(&wc, Version::V1_10), [1, 0, 5, 0, 0, 3, 0, 0xFF, 0xFF]);
    // 1.12.2 dropped PrepareCraftingGrid (replaced by CraftRecipeRequest); 1.12 has it
    let (_, ok) = enc(&sb::PrepareCraftingGrid::default(), Version::V1_12_2);
    assert!(!ok);
    let _ = Bytes(&[]);
}
