# `client`: the packet pump

`mcscon::client` is layer 2: it turns a byte pipe ([`Transport`](transport.md)) into packets. It handles framing,
compression, the handshake, offline login and keep-alive, and delivers everything else to you. Use it directly if you
only need the protocol; use [`Session`](session.md) for a ready-made bot.

```rust
use mcscon::client::{Buffers, Client, Event};
use mcscon::posix::TcpTransport;
use mcscon::Version;

let (mut rx, mut scratch, mut tx) = (vec![0u8; 64 * 1024], vec![0u8; 256 * 1024], vec![0u8; 4096]);
let mut client = Client::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx }, Version::V1_12_2);
client.connect_login("127.0.0.1", 25565, "mcscon");
while client.poll(|link, event| match event {
    Event::Packet(p) => { /* decode with packets::read::<cb::Chat>(&p) ... */ }
    Event::Error(e, info) => eprintln!("error {:?} ({})", e, info),
}) >= 0 {}
```

## `Version`

```rust
pub enum Version { V1_10, V1_11, V1_11_2, V1_12, V1_12_1, V1_12_2 }
```

| Item | Description |
|------|-------------|
| `Version::ALL` | all six versions, oldest first |
| `protocol_number(self) -> i32` | the protocol number sent in the handshake (210, 315, 316, 335, 338, 340) |
| `table_index(self) -> usize` | index into the generated id tables (1.10 → 0, 1.11/1.11.2 → 1, 1.12 → 2, 1.12.1/1.12.2 → 3) |
| `Version::from_u8(v)` | `0..=5` to a version (the order of the enum) |

`Version` derives `Ord`, so `version >= Version::V1_12` works for feature checks. It is `#[repr(u8)]` in this order,
which is also the numbering of the C constants `MC_V1_10 = 0 … MC_V1_12_2 = 5`.

## `State`

`State::{Handshake, Status, Login, Play, Closed}` – the connection state. A new client is `Closed`; `connect_*` moves it
to `Status` or `Login`; the server's *Login Success* moves it to `Play`; `disconnect`, a kick, or a fatal error moves it
to `Closed`. `State::from_u8(v)` converts `0..=3` to `Handshake … Play`; every other value gives `Closed`.

## `Error`

`Error::{None, Transport, Protocol, Overflow, PacketTooLarge, Inflate, Disconnected, EncryptionRequired, Backpressure}`.
See [Concepts → Errors](../concepts.md#errors) for the meaning of each value. `Version`, `State` and `Error` are
re-exported at the crate root (`mcscon::Version` …); they are defined in `mcscon::types`.

## `Buffers`

```rust
pub struct Buffers<'a> { pub rx: &'a mut [u8], pub scratch: &'a mut [u8], pub tx: &'a mut [u8] }
```

Caller-owned memory. Sizing guide: [Memory sizing](../memory.md).

## `Client`

```rust
pub struct Client<'a, T: Transport> { pub link: Link<'a, T>, /* private */ }
```

| Method | Description |
|--------|-------------|
| `Client::new(t, buffers, version)` | create a client (state `Closed`, nothing sent yet) |
| `auto_keepalive(on: bool)` | answer the server's keep-alive packets inside `poll` (default **on**) |
| `connect_login(host, port, username) -> bool` | open the connection and send handshake + login start. `false` if the transport could not connect. Progress is reported by `poll`. |
| `connect_status(host, port) -> bool` | open a *server list ping*: the answer arrives as `StatusCb::ServerInfo` (JSON string); then call `send_status_ping(token)`, the reply is `StatusCb::Ping` |
| `send_status_ping(token: i64)` | send the status ping |
| `poll(f) -> i32` | pump the network (see below) |
| `disconnect()` | close the transport and set the state to `Closed` |
| `state()`, `version()`, `compression_threshold()` | current state, version, and the server's compression threshold (`-1` = compression off) |

### `poll`

```rust
pub fn poll<F>(&mut self, f: F) -> i32
where F: for<'p> FnMut(&mut Link<'a, T>, Event<'p>)
```

One pump step: flush the send queue, read what the transport has, and call `f` once for every complete packet (and for
every pending error). Returns the number of packets dispatched, or **-1 once the connection is closed**. It never blocks.

Details:

- Frames are reassembled from partial reads; the transport may deliver one byte at a time.
- During login the client handles *Set Compression*, *Login Success* (→ `Play`), *Disconnect* and *Encryption Request*
  (→ `Error::EncryptionRequired`, closed) itself; these packets are still passed to `f`.
- Keep-alives are answered automatically (if enabled) and still passed to `f`.
- A play-state *Kick/Disconnect* is passed to `f`, then the connection is closed (`Error::Disconnected`).
- A frame larger than `rx` (or a decompressed packet larger than `scratch`) is skipped and reported as
  `Error::PacketTooLarge`.
- Inside `f` you may send through `link` (`packets::send(link, …)`) or call `link.disconnect()`.

### `Event`

```rust
pub enum Event<'a> { Packet(Packet<'a>), Error(Error, i32) }
```

`Error(e, info)`: `info` carries extra data (the length of an oversized frame for `PacketTooLarge`, otherwise 0).

### `Packet`

```rust
pub struct Packet<'a> { pub state: State, pub id: u8, pub wire_id: i32, pub version: Version, pub r: Reader<'a> }
```

| Field / method | Description |
|----------------|-------------|
| `state` | the connection state when the packet arrived |
| `id` | the **version-agnostic** packet id (index into `PlayCb`, `LoginCb` or `StatusCb`); `0xFF` if the wire id is unknown in this version |
| `wire_id` | the id as sent on the wire (differs between versions) |
| `version` | the connection's version |
| `r` | a [`Reader`](wire.md#reader) positioned at the payload; only valid inside the handler |
| `play()`, `login()`, `status()` | `Some(PlayCb)` / `Some(LoginCb)` / `Some(StatusCb)` if the packet belongs to that state and the id is known |

Match on the enum instead of numbers:

```rust
use mcscon::ids::PlayCb;
if p.play() == Some(PlayCb::Chat) { /* ... */ }
```

For typed access use [`packets::read`](packets.md#reading).

## `Link`

The sending half of a connection (transport, send queue, state). It is `client.link`, and it is what your poll handler
receives, so you can reply from inside the handler.

| Method | Description |
|--------|-------------|
| `state()`, `version()`, `compression_threshold()` | as on `Client` |
| `send_raw(wire_id: u8, payload: &[u8]) -> bool` | send one packet with the given **wire** id and raw payload (without the id) |
| `take_error() -> Option<(Error, i32)>` | pops errors raised outside `poll` (for example a failed send) – up to 4 are queued; older ones are dropped. `poll` drains this queue into `Event::Error` |
| `disconnect()` | close the transport and set the state to `Closed` (idempotent) |
| `begin_raw()`, `payload_area()`, `commit_raw(inner, err)` | low-level in-place frame building, see below |
| `t` (public field) | the transport, e.g. to query socket options |

Prefer the typed helpers in [`packets`](packets.md) over `send_raw`.

### Building a frame in place

`send_wire` covers almost every need. For code that cannot pass a closure (an FFI layer, for example) `Link` also
exposes the three steps it uses internally; the payload is written directly into `tx`, without a copy:

```rust
if link.begin_raw() {                                  // reserve a frame; false if tx is full
    let area = link.payload_area();                    // writable space after the frame header
    let mut w = mcscon::wire::Writer::new(area);
    w.u8(wire_id);                                     // the packet id is part of the payload here
    w.str(b"hello");
    let (n, err) = (w.pos, w.err);
    link.commit_raw(n, err);                           // writes the header, queues and flushes; false on overflow
}
```

`commit_raw(inner, err)`: `inner` is the number of bytes written *including* the packet id; with `err = true` (writer
overflow) or a closed connection the frame is discarded and `Error::Overflow` is queued.

## `Sender`

```rust
pub trait Sender {
    fn version(&self) -> Version;
    fn state(&self) -> State;
    fn send_wire(&mut self, wire_id: u8, f: &mut dyn FnMut(&mut Writer)) -> bool;
    fn send_play(&mut self, id: PlaySb, f: &mut dyn FnMut(&mut Writer)) -> bool;   // provided
}
```

Everything that can have packets sent through it: `Link`, `Client`, or your test double. All functions in
[`packets`](packets.md), [`inventory`](inventory.md) and `session::say` are generic over `Sender`.

- `send_wire` builds one frame: `f` writes the payload (without the id) into a [`Writer`](wire.md#writer). Returns
  `false` on overflow of `tx` or if the connection is closed.
- `send_play` takes a version-agnostic [`PlaySb`](../packet-reference.md#serverbound) id and maps it to the wire id of
  the connection's version; `false` if that version has no such packet.

## Status ping example

```rust
let mut c = Client::new(TcpTransport::new(), Buffers { rx: &mut rx, scratch: &mut scratch, tx: &mut tx }, Version::V1_12_2);
c.connect_status("127.0.0.1", 25565);
while c.poll(|_link, ev| {
    if let Event::Packet(p) = ev {
        if p.status() == Some(mcscon::ids::StatusCb::ServerInfo) {
            let mut r = p.r;
            println!("{}", String::from_utf8_lossy(r.str().as_bytes()));   // the server list JSON
        }
    }
}) >= 0 { std::thread::sleep(std::time::Duration::from_millis(10)); }
```
