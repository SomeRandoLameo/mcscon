//! The same bot in Rust (this file), C (examples/c/bot.c) and C++ (examples/cpp/bot.cpp).
//!
//! What it does:
//!   1. connects to an offline-mode server (`online-mode=false`) as "mcscon",
//!   2. prints every chat message it receives,
//!   3. once it is in the world: says hello and walks forward for 3 seconds,
//!   4. prints its position and health, then disconnects.
//!
//! Run:  cargo run --release --example bot -- [host] [port]      (defaults: 127.0.0.1 25565)
//!
//! This is the native Rust API: *you* own all memory (nothing is allocated by the library), so the setup is longer than
//! in the C/C++ examples. In exchange you decide exactly how much RAM the bot uses. See docs/memory.md.
use mcscon::client::Buffers;
use mcscon::entities::{Entity, PlayerEntry};
use mcscon::posix::TcpTransport;
use mcscon::session::{Session, SessionSink, SessionStorage};
use mcscon::wire::Str;
use mcscon::world::{World, WorldConfig};
use mcscon::Version;
use std::time::{Duration, Instant};

/// The session reports events by calling methods of a "sink". Every method has an empty default, so you only
/// implement what you care about. Here: chat only. (Others: `event`, `error`, `entity_event`, `slot`, `window`, ...)
struct Printer;
impl SessionSink for Printer {
    /// `text` is the message flattened to plain UTF-8 (formatting stripped). It is only valid during this call.
    fn chat(&mut self, text: &[u8], _position: u8, _json: Str<'_>) {
        println!("chat: {}", String::from_utf8_lossy(text));
    }
}

fn main() {
    // Command line: host and port of the server.
    let host = std::env::args().nth(1).unwrap_or_else(|| "127.0.0.1".into());
    let port: u16 = std::env::args().nth(2).and_then(|p| p.parse().ok()).unwrap_or(25565);

    // ---- Memory: the library never allocates, so we hand it every buffer it needs.
    // Network buffers: `rx` must fit the biggest packet *on the wire* you want to see, `scratch` the biggest packet
    // *after decompression* (only used when the server enables compression), `tx` is the send queue.
    let (mut rx, mut scratch, mut tx) = (vec![0u8; 128 * 1024], vec![0u8; 512 * 1024], vec![0u8; 4096]);
    // World storage: room for 49 chunk columns (7x7 chunks around the bot) plus an arena for their block data.
    // It is a Vec<u64> because the world needs 8-byte alignment.
    let cfg = WorldConfig { max_columns: 49, store_light: false, store_biomes: false };
    let mut world = vec![0u64; (World::column_table_bytes(&cfg) + 256 * 1024) / 8 + 1];
    let world_mem = unsafe { std::slice::from_raw_parts_mut(world.as_mut_ptr() as *mut u8, world.len() * 8) };
    // Tables for tracked entities (mobs, players, items, ...) and for the tab list.
    let (mut entities, mut players) = (vec![Entity::default(); 64], vec![PlayerEntry::default(); 16]);
    let storage = SessionStorage { world_mem, world: cfg, entities: &mut entities, players: &mut players };

    // ---- Create the bot. `TcpTransport` is the byte pipe (std TCP); `Version` picks the protocol (1.12.2 here).
    let mut bot = Session::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx }, Version::V1_12_2, storage);

    // Offline-mode login: opens the TCP connection and sends handshake + login. The server's answer is processed
    // by `poll` below. Returns false only if the TCP connection could not be made.
    if !bot.connect(&host, port, "mcscon") {
        eprintln!("could not connect to {host}:{port}");
        std::process::exit(1);
    }

    // ---- Main loop. The library has no threads and no clock; you call it regularly.
    let start = Instant::now();
    let mut spawned_at: Option<Instant> = None; // set when we have said hello and started walking
    while bot.poll(&mut Printer) >= 0 {
        // `poll` reads the network and handles every packet (chunks, entities, chat, keep-alive, teleports ...).
        // It returns -1 when the connection is closed, which ends the loop.

        // `update` runs the 20 Hz game tick: physics and the position packets the server expects.
        // You pass your own clock in milliseconds.
        bot.update(start.elapsed().as_millis() as u32);

        match spawned_at {
            // `state.spawned` becomes true once the server has put us in the world. Then act once:
            None if bot.state.spawned => {
                bot.say(b"hello from Rust"); // a chat message ("/command" would run a command)
                bot.controls.forward = true; // hold "W"; the physics engine moves us every tick
                spawned_at = Some(Instant::now());
            }
            // 3 seconds after that we are done.
            Some(t) if t.elapsed() > Duration::from_secs(3) => break,
            _ => {}
        }
        std::thread::sleep(Duration::from_millis(10)); // don't spin the CPU
    }

    // The bot's state is plain public fields: `body` is the position/physics, `state` is health, game mode, etc.
    println!("position: {:.1} {:.1} {:.1}  health: {:.0}", bot.body.x, bot.body.y, bot.body.z, bot.state.health);
    bot.disconnect();
}
