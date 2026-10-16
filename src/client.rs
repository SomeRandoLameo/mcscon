//! Protocol client: framing, compression, handshake/status/offline login, keep-alive and packet dispatch.
//! Nothing is ever allocated: the caller provides the receive, scratch and transmit buffers.
use crate::ids::{self, LoginCb, PlayCb, PlaySb};
use crate::inflate::zlib_inflate;
use crate::transport::Transport;
use crate::types::{Error, State, Version};
use crate::wire::{varint_size, Reader, Writer};

/// Caller-provided memory.
/// * `rx`: receive window. Must hold the largest *wire* frame you want to see.
/// * `scratch`: inflate target; only used when the server enables compression. Must hold the largest
///   *decompressed* packet you want to see (may be empty for uncompressed-only use).
/// * `tx`: outgoing queue.
///
/// Frames that do not fit are skipped (streamed away) and reported as [`Error::PacketTooLarge`].
pub struct Buffers<'a> {
    pub rx: &'a mut [u8],
    pub scratch: &'a mut [u8],
    pub tx: &'a mut [u8],
}

/// One received packet. `id` is the version-agnostic id; convert it with `ids::*::from_u8` according to `state`:
/// Status -> [`ids::StatusCb`], Login -> [`ids::LoginCb`], Play -> [`ids::PlayCb`].
/// `r` is positioned at the payload and only valid inside the handler.
#[derive(Clone, Copy)]
pub struct Packet<'a> {
    pub state: State,
    pub id: u8,
    pub wire_id: i32,
    pub version: Version,
    pub r: Reader<'a>,
}
impl<'a> Packet<'a> {
    pub fn play(&self) -> Option<PlayCb> {
        if self.state == State::Play {
            PlayCb::from_u8(self.id)
        } else {
            None
        }
    }
    pub fn login(&self) -> Option<LoginCb> {
        if self.state == State::Login {
            LoginCb::from_u8(self.id)
        } else {
            None
        }
    }
    pub fn status(&self) -> Option<ids::StatusCb> {
        if self.state == State::Status {
            ids::StatusCb::from_u8(self.id)
        } else {
            None
        }
    }
}

/// What [`Client::poll`] reports to the handler.
pub enum Event<'a> {
    Packet(Packet<'a>),
    /// A protocol/transport error with extra info (e.g. the oversized frame length).
    Error(Error, i32),
}

/// Anything packets can be sent through: the real connection ([`Link`]) or a test double.
pub trait Sender {
    fn version(&self) -> Version;
    fn state(&self) -> State;
    /// Builds and sends one frame: `f` writes the payload (without the packet id). `false` on overflow or if closed.
    fn send_wire(&mut self, wire_id: u8, f: &mut dyn FnMut(&mut Writer)) -> bool;
    /// Like [`Sender::send_wire`] with a version-agnostic serverbound id. `false` if the packet does not exist in this version.
    fn send_play(&mut self, id: PlaySb, f: &mut dyn FnMut(&mut Writer)) -> bool {
        let t = &ids::PLAY_SB_TABLES[self.version().table_index()];
        let wire = t.a2w.get(id as usize).copied().unwrap_or(0xFF);
        if wire == 0xFF {
            return false;
        }
        self.send_wire(wire, f)
    }
}

const HDR: usize = 6; // max frame header we prepend: 5 (length) + 1 (zero data-length)
const ERR_QUEUE: usize = 4;

/// The sending half of a connection: transport, transmit queue and connection state.
pub struct Link<'a, T: Transport> {
    pub t: T,
    tx: &'a mut [u8],
    th: usize,
    tt: usize,
    state: State,
    version: Version,
    threshold: i32,
    building: Option<usize>,
    errors: [(Error, i32); ERR_QUEUE],
    nerr: usize,
}

impl<'a, T: Transport> Link<'a, T> {
    fn new(t: T, tx: &'a mut [u8], version: Version) -> Self {
        Link { t, tx, th: 0, tt: 0, state: State::Closed, version, threshold: -1, building: None, errors: [(Error::None, 0); ERR_QUEUE], nerr: 0 }
    }

    pub fn state(&self) -> State {
        self.state
    }
    pub fn version(&self) -> Version {
        self.version
    }
    pub fn compression_threshold(&self) -> i32 {
        self.threshold
    }

    pub(crate) fn fail(&mut self, e: Error, info: i32) {
        if self.nerr == ERR_QUEUE {
            self.errors.copy_within(1.., 0);
            self.nerr -= 1;
        }
        self.errors[self.nerr] = (e, info);
        self.nerr += 1;
    }
    /// Pops the oldest pending error (errors raised outside `poll`, e.g. by a failed send).
    pub fn take_error(&mut self) -> Option<(Error, i32)> {
        if self.nerr == 0 {
            return None;
        }
        let e = self.errors[0];
        self.errors.copy_within(1.., 0);
        self.nerr -= 1;
        Some(e)
    }

    fn flush(&mut self) -> bool {
        while self.th < self.tt {
            match self.t.send(&self.tx[self.th..self.tt]) {
                Err(_) => {
                    self.fail(Error::Transport, 0);
                    self.disconnect();
                    return false;
                }
                Ok(0) => break,
                Ok(n) => self.th += n,
            }
        }
        if self.th == self.tt {
            self.th = 0;
            self.tt = 0;
        }
        true
    }

    /// Low-level frame building (used by the C ABI): reserves a frame and returns the offset of the payload area.
    /// The payload area is `payload_area()`; finish with [`Link::commit_raw`].
    pub fn begin_raw(&mut self) -> bool {
        if self.th == self.tt {
            self.th = 0;
            self.tt = 0;
        } else if self.tt + HDR + 8 > self.tx.len() && self.th > 0 {
            self.tx.copy_within(self.th..self.tt, 0);
            self.tt -= self.th;
            self.th = 0;
        }
        if self.tt + HDR >= self.tx.len() {
            self.building = None;
            return false;
        }
        self.building = Some(self.tt);
        true
    }
    /// The writable region of the frame started by [`Link::begin_raw`] (empty if none is open).
    pub fn payload_area(&mut self) -> &mut [u8] {
        match self.building {
            Some(s) => &mut self.tx[s + HDR..],
            None => &mut [],
        }
    }
    /// Finishes the frame: `inner` payload bytes were written (including the packet id), `err` if the writer overflowed.
    pub fn commit_raw(&mut self, inner: usize, err: bool) -> bool {
        let start = match self.building.take() {
            Some(s) => s,
            None => return false,
        };
        if err || self.state == State::Closed {
            self.fail(Error::Overflow, 0);
            return false;
        }
        let comp = self.threshold >= 0;
        let len_field = inner + comp as usize;
        let hlen = varint_size(len_field as u32) + comp as usize;
        {
            let mut h = Writer::new(&mut self.tx[start + HDR - hlen..start + HDR]);
            h.varint(len_field as i32);
            if comp {
                h.u8(0);
            }
        }
        self.tx.copy_within(start + HDR - hlen..start + HDR + inner, start);
        self.tt += hlen + inner;
        self.flush()
    }

    pub fn send_raw(&mut self, wire_id: u8, payload: &[u8]) -> bool {
        self.send_wire(wire_id, &mut |w| w.bytes(payload))
    }

    pub fn disconnect(&mut self) {
        if self.state != State::Closed {
            self.state = State::Closed;
            self.t.close();
        }
    }

    fn handshake(&mut self, host: &str, port: u16, next: i32) -> bool {
        if !self.t.connect(host, port) {
            self.fail(Error::Transport, 0);
            return false;
        }
        self.th = 0;
        self.tt = 0;
        self.threshold = -1;
        self.state = State::Handshake;
        let proto = self.version.protocol_number();
        if !self.send_wire(0x00, &mut |w| {
            w.varint(proto);
            w.str(host.as_bytes());
            w.u16(port);
            w.varint(next);
        }) {
            return false;
        }
        self.state = if next == 1 { State::Status } else { State::Login };
        true
    }
}

impl<'a, T: Transport> Sender for Link<'a, T> {
    fn version(&self) -> Version {
        self.version
    }
    fn state(&self) -> State {
        self.state
    }
    fn send_wire(&mut self, wire_id: u8, f: &mut dyn FnMut(&mut Writer)) -> bool {
        if !self.begin_raw() {
            return false;
        }
        let (len, err) = {
            let mut w = Writer::new(self.payload_area());
            w.varint(wire_id as i32);
            f(&mut w);
            (w.pos, w.err)
        };
        self.commit_raw(len, err)
    }
}

pub struct Client<'a, T: Transport> {
    pub link: Link<'a, T>,
    rx: &'a mut [u8],
    scratch: &'a mut [u8],
    rh: usize,
    rt: usize,
    skip: usize,
    auto_keepalive: bool,
}

impl<'a, T: Transport> Client<'a, T> {
    pub fn new(t: T, b: Buffers<'a>, version: Version) -> Self {
        Client { link: Link::new(t, b.tx, version), rx: b.rx, scratch: b.scratch, rh: 0, rt: 0, skip: 0, auto_keepalive: true }
    }

    /// Reply to server keep-alives automatically (default on).
    pub fn auto_keepalive(&mut self, on: bool) {
        self.auto_keepalive = on;
    }
    /// Offline-mode login. Returns false if the transport could not connect.
    pub fn connect_login(&mut self, host: &str, port: u16, username: &str) -> bool {
        self.reset_rx();
        if !self.link.handshake(host, port, 2) {
            return false;
        }
        self.link.send_wire(0x00, &mut |w| w.str(username.as_bytes()))
    }
    /// Server list ping: after this, expect `StatusCb::ServerInfo`, then call [`Client::send_status_ping`].
    pub fn connect_status(&mut self, host: &str, port: u16) -> bool {
        self.reset_rx();
        if !self.link.handshake(host, port, 1) {
            return false;
        }
        self.link.send_raw(0x00, &[])
    }
    pub fn send_status_ping(&mut self, token: i64) {
        self.link.send_wire(0x01, &mut |w| w.i64(token));
    }
    pub fn disconnect(&mut self) {
        self.link.disconnect();
    }
    pub fn state(&self) -> State {
        self.link.state
    }
    pub fn version(&self) -> Version {
        self.link.version
    }
    pub fn compression_threshold(&self) -> i32 {
        self.link.threshold
    }

    fn reset_rx(&mut self) {
        self.rh = 0;
        self.rt = 0;
        self.skip = 0;
    }

    /// Pump: flush the transmit queue, read the transport, hand every complete packet (and error) to `f`.
    /// Returns the number of packets dispatched, or -1 when the connection is closed.
    pub fn poll<F>(&mut self, mut f: F) -> i32
    where
        F: for<'p> FnMut(&mut Link<'a, T>, Event<'p>),
    {
        let Client { link, rx, scratch, rh, rt, skip, auto_keepalive } = self;
        macro_rules! drain {
            () => {
                while let Some((e, info)) = link.take_error() {
                    crate::escape(&mut *link);
                    f(link, Event::Error(e, info));
                    crate::escape(&mut *link);
                }
            };
        }
        macro_rules! bail {
            () => {{
                drain!();
                return -1;
            }};
        }
        if link.state == State::Closed {
            return -1;
        }
        if !link.flush() {
            bail!();
        }

        // refill rx
        if *rh == *rt {
            *rh = 0;
            *rt = 0;
        } else if *rt == rx.len() && *rh > 0 {
            rx.copy_within(*rh..*rt, 0);
            *rt -= *rh;
            *rh = 0;
        }
        if *rt < rx.len() {
            match link.t.recv(&mut rx[*rt..]) {
                Err(_) => {
                    link.fail(Error::Transport, 0);
                    link.disconnect();
                    bail!();
                }
                Ok(n) => *rt += n,
            }
        }

        let mut count = 0;
        while link.state != State::Closed && *rh < *rt {
            if *skip > 0 {
                // discarding an oversized frame
                let n = (*rt - *rh).min(*skip);
                *rh += n;
                *skip -= n;
                continue;
            }
            let mut r = Reader::new(&rx[*rh..*rt]);
            let len = r.varint();
            if !r.ok() {
                if *rt - *rh >= 5 {
                    link.fail(Error::Protocol, 0);
                    link.disconnect();
                    bail!();
                }
                break; // length prefix incomplete
            }
            if len <= 0 {
                link.fail(Error::Protocol, 0);
                link.disconnect();
                bail!();
            }
            let hdr = *rt - *rh - r.left();
            let avail = *rt - *rh - hdr;
            let len = len as usize;
            if len > avail {
                if len + hdr > rx.len() {
                    // can never fit: stream it away
                    link.fail(Error::PacketTooLarge, len as i32);
                    *skip = len - avail;
                    *rh = *rt;
                    break;
                }
                // make room for the rest of this frame
                if *rh > 0 && *rh + hdr + len > rx.len() {
                    rx.copy_within(*rh..*rt, 0);
                    *rt -= *rh;
                    *rh = 0;
                }
                break;
            }
            let body_start = *rh + hdr;
            *rh += hdr + len; // consume before dispatch (the handler may send)
            let mut body: &[u8] = &rx[body_start..body_start + len];
            if link.threshold >= 0 {
                let mut br = Reader::new(body);
                let dlen = br.varint();
                if !br.ok() || dlen < 0 {
                    link.fail(Error::Protocol, 0);
                    link.disconnect();
                    bail!();
                }
                body = br.p;
                if dlen > 0 {
                    let dlen = dlen as usize;
                    if scratch.is_empty() || dlen > scratch.len() {
                        link.fail(Error::PacketTooLarge, dlen as i32);
                        drain!();
                        continue;
                    }
                    match zlib_inflate(body, &mut scratch[..dlen], false) {
                        Some(n) if n == dlen => {}
                        _ => {
                            link.fail(Error::Inflate, 0);
                            link.disconnect();
                            bail!();
                        }
                    }
                    body = &scratch[..dlen];
                }
            }
            dispatch(link, body, *auto_keepalive, &mut f);
            count += 1;
        }
        drain!();
        if link.state == State::Closed {
            -1
        } else {
            count
        }
    }
}

impl<'a, T: Transport> Sender for Client<'a, T> {
    fn version(&self) -> Version {
        self.link.version
    }
    fn state(&self) -> State {
        self.link.state
    }
    fn send_wire(&mut self, wire_id: u8, f: &mut dyn FnMut(&mut Writer)) -> bool {
        self.link.send_wire(wire_id, f)
    }
}

fn dispatch<'a, T, F>(link: &mut Link<'a, T>, body: &[u8], auto_keepalive: bool, f: &mut F)
where
    T: Transport,
    F: for<'p> FnMut(&mut Link<'a, T>, Event<'p>),
{
    let mut r = Reader::new(body);
    let wire = r.varint();
    if !r.ok() {
        link.fail(Error::Protocol, 0);
        return;
    }
    let ti = link.version.table_index();
    let tab = match link.state {
        State::Status => &ids::STATUS_CB_TABLES[ti],
        State::Login => &ids::LOGIN_CB_TABLES[ti],
        State::Play => &ids::PLAY_CB_TABLES[ti],
        _ => return,
    };
    let id = if wire >= 0 { tab.w2a.get(wire as usize).copied().unwrap_or(0xFF) } else { 0xFF };
    let p = Packet { state: link.state, id, wire_id: wire, version: link.version, r };

    match link.state {
        State::Login => internal_login(link, &p),
        State::Play => {
            if id == PlayCb::KeepAlive as u8 && auto_keepalive {
                let n = r.left().min(10);
                let mut echo = [0u8; 10];
                echo[..n].copy_from_slice(&r.p[..n]);
                let st = &ids::PLAY_SB_TABLES[ti];
                link.send_raw(st.a2w[PlaySb::KeepAlive as usize], &echo[..n]);
            } else if id == PlayCb::KickDisconnect as u8 {
                link.fail(Error::Disconnected, 0);
            }
        }
        _ => {}
    }
    while let Some((e, info)) = link.take_error() {
        crate::escape(&mut *link);
        f(link, Event::Error(e, info));
        crate::escape(&mut *link);
    }
    crate::escape(&mut *link); // the handler may send, disconnect, ... through other pointers to the link
    f(link, Event::Packet(p));
    crate::escape(&mut *link);
    if p.state == State::Play && id == PlayCb::KickDisconnect as u8 {
        link.disconnect();
    }
    // Online-mode servers (encryption request) are not supported yet, and a login disconnect ends the connection: close cleanly.
    if p.state == State::Login && (id == LoginCb::EncryptionBegin as u8 || id == LoginCb::Disconnect as u8) {
        link.disconnect();
    }
}

fn internal_login<T: Transport>(link: &mut Link<'_, T>, p: &Packet) {
    let mut r = p.r;
    match LoginCb::from_u8(p.id) {
        Some(LoginCb::Compress) => {
            let th = r.varint();
            if r.ok() {
                link.threshold = th;
            } else {
                link.fail(Error::Protocol, 0);
            }
        }
        Some(LoginCb::Success) => link.state = State::Play,
        Some(LoginCb::EncryptionBegin) => link.fail(Error::EncryptionRequired, 0),
        Some(LoginCb::Disconnect) => link.fail(Error::Disconnected, 0),
        _ => {}
    }
}

#[cfg(test)]
pub(crate) mod testutil {
    use super::*;
    use crate::transport::Closed;
    use std::vec::Vec;

    /// Scripted in-memory "server": tests push bytes to `inp`, client output lands in `out`.
    #[derive(Default)]
    pub struct Pipe {
        pub inp: Vec<u8>,
        pub out: Vec<u8>,
        pub pos: usize,
        pub chunk: usize,
        pub connected: bool,
        pub closed: bool,
    }
    impl Pipe {
        pub fn new() -> Self {
            Pipe { chunk: 1 << 20, ..Default::default() }
        }
    }
    impl Transport for Pipe {
        fn connect(&mut self, _h: &str, _p: u16) -> bool {
            self.connected = true;
            true
        }
        fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed> {
            if self.closed {
                return Err(Closed);
            }
            let n = (self.inp.len() - self.pos).min(buf.len()).min(self.chunk);
            buf[..n].copy_from_slice(&self.inp[self.pos..self.pos + n]);
            self.pos += n;
            Ok(n)
        }
        fn send(&mut self, buf: &[u8]) -> Result<usize, Closed> {
            self.out.extend_from_slice(buf);
            Ok(buf.len())
        }
        fn close(&mut self) {
            self.closed = true;
        }
    }

    pub fn put_var(b: &mut Vec<u8>, v: i32) {
        let mut t = [0u8; 5];
        let mut w = Writer::new(&mut t);
        w.varint(v);
        let n = w.pos;
        b.extend_from_slice(&t[..n]);
    }
    /// frame = [len][payload]; with compression: [len][dataLen=0][payload]
    pub fn frame(payload: &[u8], threshold: i32) -> Vec<u8> {
        let mut f = Vec::new();
        if threshold < 0 {
            put_var(&mut f, payload.len() as i32);
            f.extend_from_slice(payload);
        } else {
            put_var(&mut f, payload.len() as i32 + 1);
            f.push(0);
            f.extend_from_slice(payload);
        }
        f
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use std::vec;
    use std::vec::Vec;

    #[test]
    fn login_flow_and_framing() {
        let mut pipe = Pipe::new();
        let mut rx = [0u8; 1024];
        let mut sc = [0u8; 1024];
        let mut tx = [0u8; 512];
        let mut c = Client::new(&mut pipe, Buffers { rx: &mut rx, scratch: &mut sc, tx: &mut tx }, Version::V1_12_2);
        assert!(c.connect_login("localhost", 25565, "bot"));
        assert_eq!(c.state(), State::Login);
        // handshake + login start were written
        let mut sent = Vec::new();
        // handshake: id 0, proto 340 (2 bytes), host "localhost", port, next=2
        sent.extend_from_slice(&[0x10, 0x00, 0xD4, 0x02, 9]);
        sent.extend_from_slice(b"localhost");
        sent.extend_from_slice(&[0x63, 0xDD, 0x02]);
        sent.extend_from_slice(&[5, 0x00, 3, b'b', b'o', b't']);
        let _ = c;
        assert_eq!(pipe.out, sent);
    }

    #[test]
    fn success_switches_to_play_and_dispatches() {
        let mut pipe = Pipe::new();
        // Login Success (id 2): uuid string, name string
        let mut p = vec![0x02];
        p.push(1);
        p.push(b'u');
        p.push(1);
        p.push(b'n');
        pipe.inp.extend(frame(&p, -1));
        let mut rx = [0u8; 256];
        let mut sc = [0u8; 0];
        let mut tx = [0u8; 128];
        let mut c = Client::new(&mut pipe, Buffers { rx: &mut rx, scratch: &mut sc, tx: &mut tx }, Version::V1_12_2);
        assert!(c.connect_login("h", 1, "n"));
        let mut seen = Vec::new();
        let n = c.poll(|_, ev| {
            if let Event::Packet(p) = ev {
                seen.push((p.state, p.id));
            }
        });
        assert_eq!(n, 1);
        assert_eq!(seen, vec![(State::Login, LoginCb::Success as u8)]);
        assert_eq!(c.state(), State::Play);
    }

    #[test]
    fn keepalive_is_echoed() {
        let mut pipe = Pipe::new();
        let mut rx = [0u8; 256];
        let mut sc = [0u8; 0];
        let mut tx = [0u8; 128];
        let mut c = Client::new(&mut pipe, Buffers { rx: &mut rx, scratch: &mut sc, tx: &mut tx }, Version::V1_12_2);
        c.link.state = State::Play;
        c.link.t.inp.extend(frame(&[0x1F, 0, 0, 0, 0, 0, 0, 0, 7], -1)); // 1.12.2 KeepAlive id 0x1F, long id
        let n = c.poll(|_, _| {});
        assert_eq!(n, 1);
        let out = &c.link.t.out;
        assert_eq!(out[1], 0x0B); // serverbound keep-alive in 1.12.2
        assert_eq!(&out[2..], &[0, 0, 0, 0, 0, 0, 0, 7]);
    }

    #[test]
    fn oversized_frame_is_skipped() {
        let mut pipe = Pipe::new();
        pipe.chunk = 16;
        let mut big = vec![0x20u8];
        big.extend(vec![0xAB; 600]);
        pipe.inp.extend(frame(&big, -1));
        pipe.inp.extend(frame(&[0x1F, 0, 0, 0, 0, 0, 0, 0, 1], -1));
        let mut rx = [0u8; 128];
        let mut sc = [0u8; 0];
        let mut tx = [0u8; 128];
        let mut c = Client::new(&mut pipe, Buffers { rx: &mut rx, scratch: &mut sc, tx: &mut tx }, Version::V1_12_2);
        c.link.state = State::Play;
        let (mut too_large, mut packets) = (0, 0);
        for _ in 0..200 {
            c.poll(|_, ev| match ev {
                Event::Error(Error::PacketTooLarge, _) => too_large += 1,
                Event::Packet(_) => packets += 1,
                _ => {}
            });
        }
        assert_eq!(too_large, 1);
        assert_eq!(packets, 1);
        assert_eq!(c.state(), State::Play);
    }
}
