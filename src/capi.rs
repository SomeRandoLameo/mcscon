//! A small C API (`include/mcscon.h`) over [`Session`]: one opaque bot object, no buffers or callbacks to manage.
//! It allocates (std); the protocol core itself never does. Use the Rust API directly for fully static memory.
use crate::client::Buffers;
use crate::entities::{Entity, PlayerEntry};
use crate::posix::TcpTransport;
use crate::session::{Session, SessionSink, SessionStorage};
use crate::wire::Str;
use crate::world::{World, WorldConfig};
use crate::Version;
use core::ffi::{c_char, c_int, c_void, CStr};
use std::boxed::Box;
use std::time::Instant;
use std::vec;
use std::vec::Vec;

type ChatFn = extern "C" fn(user: *mut c_void, text: *const c_char, position: c_int);

struct Chat {
    f: Option<ChatFn>,
    user: *mut c_void,
}
impl SessionSink for Chat {
    fn chat(&mut self, text: &[u8], position: u8, _json: Str<'_>) {
        if let Some(f) = self.f {
            let mut buf = [0u8; 1024];
            let n = text.len().min(buf.len() - 1);
            buf[..n].copy_from_slice(&text[..n]);
            f(self.user, buf.as_ptr() as *const c_char, position as c_int);
        }
    }
}

pub struct McBot {
    session: Session<'static, TcpTransport>, // declared first: dropped before the memory it borrows
    chat: Chat,
    start: Instant,
    _rx: Vec<u8>,
    _scratch: Vec<u8>,
    _tx: Vec<u8>,
    _world: Vec<u64>, // u64 for the 8-byte alignment the world arena needs
    _entities: Vec<Entity>,
    _players: Vec<PlayerEntry>,
}

/// The session borrows the vectors' heap memory, which stays put when the `Vec`s move into `McBot`.
unsafe fn lend<T>(v: &mut Vec<T>) -> &'static mut [T] {
    core::slice::from_raw_parts_mut(v.as_mut_ptr(), v.len())
}

/// `version`: 0 = 1.10, 1 = 1.11, 2 = 1.11.2, 3 = 1.12, 4 = 1.12.1, 5 = 1.12.2. Returns null for an unknown version.
#[no_mangle]
pub extern "C" fn mc_bot_new(version: c_int) -> *mut McBot {
    let Some(v) = u8::try_from(version).ok().and_then(Version::from_u8) else { return core::ptr::null_mut() };
    let cfg = WorldConfig { max_columns: 49, store_light: false, store_biomes: false };
    let mut rx = vec![0u8; 128 * 1024];
    let mut scratch = vec![0u8; 512 * 1024];
    let mut tx = vec![0u8; 4096];
    let mut world = vec![0u64; (World::column_table_bytes(&cfg) + 256 * 1024) / 8 + 1];
    let mut entities = vec![Entity::default(); 64];
    let mut players = vec![PlayerEntry::default(); 16];
    let session = unsafe {
        let world_mem = core::slice::from_raw_parts_mut(world.as_mut_ptr() as *mut u8, world.len() * 8);
        let storage = SessionStorage { world_mem: &mut *(world_mem as *mut [u8]), world: cfg, entities: lend(&mut entities), players: lend(&mut players) };
        Session::new(TcpTransport::new(), Buffers { rx: lend(&mut rx), scratch: lend(&mut scratch), tx: lend(&mut tx) }, v, storage)
    };
    Box::into_raw(Box::new(McBot {
        session,
        chat: Chat { f: None, user: core::ptr::null_mut() },
        start: Instant::now(),
        _rx: rx,
        _scratch: scratch,
        _tx: tx,
        _world: world,
        _entities: entities,
        _players: players,
    }))
}

/// Disconnects and frees the bot.
#[no_mangle]
pub unsafe extern "C" fn mc_bot_free(bot: *mut McBot) {
    if !bot.is_null() {
        drop(Box::from_raw(bot));
    }
}

#[no_mangle]
pub unsafe extern "C" fn mc_bot_on_chat(bot: *mut McBot, f: Option<ChatFn>, user: *mut c_void) {
    let b = &mut *bot;
    b.chat = Chat { f, user };
}

/// Offline-mode login. Returns false if the TCP connection could not be made.
#[no_mangle]
pub unsafe extern "C" fn mc_bot_connect(bot: *mut McBot, host: *const c_char, port: u16, name: *const c_char) -> bool {
    let (Ok(h), Ok(n)) = (CStr::from_ptr(host).to_str(), CStr::from_ptr(name).to_str()) else { return false };
    (*bot).session.connect(h, port, n)
}

/// Pumps the network and runs the 20 Hz physics/position ticks. Returns -1 once the connection is closed.
#[no_mangle]
pub unsafe extern "C" fn mc_bot_poll(bot: *mut McBot) -> c_int {
    let b = &mut *bot;
    let n = b.session.poll(&mut b.chat);
    b.session.update(b.start.elapsed().as_millis() as u32);
    n
}

#[no_mangle]
pub unsafe extern "C" fn mc_bot_disconnect(bot: *mut McBot) {
    (*bot).session.disconnect();
}

/// True once the server has placed the bot in the world.
#[no_mangle]
pub unsafe extern "C" fn mc_bot_spawned(bot: *const McBot) -> bool {
    (*bot).session.state.spawned
}

#[no_mangle]
pub unsafe extern "C" fn mc_bot_health(bot: *const McBot) -> f32 {
    (*bot).session.state.health
}

/// Writes x, y, z (feet position) to `xyz[3]`.
#[no_mangle]
pub unsafe extern "C" fn mc_bot_position(bot: *const McBot, xyz: *mut f64) {
    let b = &(*bot).session.body;
    *xyz = b.x;
    *xyz.add(1) = b.y;
    *xyz.add(2) = b.z;
}

/// Sends a chat message (or a command if it starts with '/').
#[no_mangle]
pub unsafe extern "C" fn mc_bot_say(bot: *mut McBot, message: *const c_char) -> bool {
    (*bot).session.say(CStr::from_ptr(message).to_bytes())
}

/// Walks forward (true) or stands still (false).
#[no_mangle]
pub unsafe extern "C" fn mc_bot_walk(bot: *mut McBot, forward: bool) {
    (*bot).session.controls.forward = forward;
}
