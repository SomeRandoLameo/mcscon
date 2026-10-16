//! Bounds-checked big-endian Reader/Writer plus the wire-level helper types used by the generated packets.
//! Everything is zero-copy and allocation-free: variable-length data is exposed as views into the packet buffer.
use crate::types::Version;
use core::marker::PhantomData;

/// UTF-8 string view (not validated, not NUL-terminated) into the packet buffer.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Str<'a>(pub &'a [u8]);
impl<'a> Str<'a> {
    pub fn as_bytes(&self) -> &'a [u8] {
        self.0
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn to_str(&self) -> Option<&'a str> {
        core::str::from_utf8(self.0).ok()
    }
}
impl<'a> PartialEq<&str> for Str<'a> {
    fn eq(&self, o: &&str) -> bool {
        self.0 == o.as_bytes()
    }
}

/// Raw byte view (NBT blobs, "rest of packet" buffers).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Bytes<'a>(pub &'a [u8]);

#[repr(transparent)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Uuid(pub [u8; 16]);

/// Scalar wrappers so arrays of variable-length integers can use the generic [`Array`] machinery.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct VarInt(pub i32);
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct VarLong(pub i64);

/// 1.10-1.12.2 block position: x:26 | y:12 | z:26
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct BlockPos {
    pub x: i32,
    pub y: i32,
    pub z: i32,
}
pub fn unpack_pos(v: u64) -> BlockPos {
    let sv = v as i64;
    BlockPos { x: (sv >> 38) as i32, y: ((v >> 26) & 0xFFF) as i32, z: ((v << 38) as i64 >> 38) as i32 }
}
pub fn pack_pos(x: i32, y: i32, z: i32) -> u64 {
    (((x & 0x3FF_FFFF) as u64) << 38) | (((y & 0xFFF) as u64) << 26) | ((z & 0x3FF_FFFF) as u64)
}

// ------------------------------------------------------------------------------------------------ Reader

/// Bounds-checked big-endian reader. Any overrun sets `err`; reads then return 0.
#[derive(Clone, Copy)]
pub struct Reader<'a> {
    pub p: &'a [u8],
    pub err: bool,
}

macro_rules! rd {
    ($name:ident, $t:ty, $n:expr) => {
        pub fn $name(&mut self) -> $t {
            if !self.need($n) {
                return 0 as $t;
            }
            let mut b = [0u8; $n];
            b.copy_from_slice(&self.p[..$n]);
            self.p = &self.p[$n..];
            <$t>::from_be_bytes(b)
        }
    };
}

impl<'a> Reader<'a> {
    pub const fn new(b: &'a [u8]) -> Self {
        Reader { p: b, err: false }
    }
    /// A reader that is already in the error state (mirrors the C++ default constructor).
    pub const fn failed() -> Self {
        Reader { p: &[], err: true }
    }
    pub fn left(&self) -> usize {
        self.p.len()
    }
    pub fn ok(&self) -> bool {
        !self.err
    }
    pub fn need(&mut self, n: usize) -> bool {
        if self.err || self.p.len() < n {
            self.err = true;
            return false;
        }
        true
    }
    rd!(u8, u8, 1);
    rd!(i8, i8, 1);
    rd!(u16, u16, 2);
    rd!(i16, i16, 2);
    rd!(u32, u32, 4);
    rd!(i32, i32, 4);
    rd!(u64, u64, 8);
    rd!(i64, i64, 8);
    rd!(f32, f32, 4);
    rd!(f64, f64, 8);
    pub fn bool(&mut self) -> bool {
        self.u8() != 0
    }
    pub fn varint(&mut self) -> i32 {
        let mut r: u32 = 0;
        let mut s = 0;
        while s < 35 {
            let b = self.u8();
            if self.err {
                return 0;
            }
            r |= ((b & 0x7F) as u32) << s;
            if b & 0x80 == 0 {
                return r as i32;
            }
            s += 7;
        }
        self.err = true;
        0
    }
    pub fn varlong(&mut self) -> i64 {
        let mut r: u64 = 0;
        let mut s = 0;
        while s < 70 {
            let b = self.u8();
            if self.err {
                return 0;
            }
            r |= ((b & 0x7F) as u64) << s;
            if b & 0x80 == 0 {
                return r as i64;
            }
            s += 7;
        }
        self.err = true;
        0
    }
    pub fn str(&mut self) -> Str<'a> {
        self.str_max(32767 * 4)
    }
    pub fn str_max(&mut self, max_len: u32) -> Str<'a> {
        let n = self.varint();
        if self.err || n < 0 || n as u32 > max_len || !self.need(n as usize) {
            self.err = true;
            return Str(&[]);
        }
        Str(self.take(n as usize))
    }
    /// Returns the next `n` bytes (error state and `None` if there are fewer).
    pub fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        if !self.need(n) {
            return None;
        }
        Some(self.take(n))
    }
    pub fn skip(&mut self, n: usize) {
        if self.need(n) {
            self.p = &self.p[n..];
        }
    }
    fn take(&mut self, n: usize) -> &'a [u8] {
        let (a, b) = self.p.split_at(n);
        self.p = b;
        a
    }
}

// ------------------------------------------------------------------------------------------------ Writer

/// Bounds-checked big-endian writer over caller memory. Overrun sets `err`.
pub struct Writer<'a> {
    pub buf: &'a mut [u8],
    pub pos: usize,
    pub err: bool,
}

macro_rules! wr {
    ($name:ident, $t:ty) => {
        pub fn $name(&mut self, v: $t) {
            self.bytes(&v.to_be_bytes());
        }
    };
}

impl<'a> Writer<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Writer { buf, pos: 0, err: false }
    }
    pub fn left(&self) -> usize {
        self.buf.len() - self.pos
    }
    pub fn written(&self) -> &[u8] {
        &self.buf[..self.pos]
    }
    pub fn need(&mut self, n: usize) -> bool {
        if self.err || self.left() < n {
            self.err = true;
            return false;
        }
        true
    }
    pub fn u8(&mut self, v: u8) {
        if self.need(1) {
            self.buf[self.pos] = v;
            self.pos += 1;
        }
    }
    pub fn bool(&mut self, v: bool) {
        self.u8(v as u8);
    }
    wr!(i8, i8);
    wr!(u16, u16);
    wr!(i16, i16);
    wr!(u32, u32);
    wr!(i32, i32);
    wr!(u64, u64);
    wr!(i64, i64);
    wr!(f32, f32);
    wr!(f64, f64);
    pub fn varint(&mut self, v: i32) {
        let mut u = v as u32;
        while u >= 0x80 {
            self.u8((u as u8) | 0x80);
            u >>= 7;
        }
        self.u8(u as u8);
    }
    pub fn varlong(&mut self, v: i64) {
        let mut u = v as u64;
        while u >= 0x80 {
            self.u8((u as u8) | 0x80);
            u >>= 7;
        }
        self.u8(u as u8);
    }
    pub fn bytes(&mut self, d: &[u8]) {
        if self.need(d.len()) {
            self.buf[self.pos..self.pos + d.len()].copy_from_slice(d);
            self.pos += d.len();
        }
    }
    pub fn str(&mut self, s: &[u8]) {
        self.varint(s.len() as i32);
        self.bytes(s);
    }
}

pub fn varint_size(mut v: u32) -> usize {
    let mut n = 1;
    while v >= 0x80 {
        v >>= 7;
        n += 1;
    }
    n
}

// ------------------------------------------------------------------------------------------------ element traits

/// Decodes one value (array element / nested struct) from a reader. `ctx` carries the value of a
/// field of the enclosing container that a nested `switch` refers to (`../field` in minecraft-data).
pub trait Elem<'a>: Sized {
    fn decode(r: &mut Reader<'a>, v: Version, ctx: i32) -> Option<Self>;
}
pub trait EncElem {
    fn encode(&self, w: &mut Writer, v: Version, ctx: i32);
}

macro_rules! prim_elem {
    ($t:ty, $rd:ident, $wr:ident) => {
        impl<'a> Elem<'a> for $t {
            fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
                let x = r.$rd();
                if r.err {
                    None
                } else {
                    Some(x)
                }
            }
        }
        impl EncElem for $t {
            fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
                w.$wr(*self);
            }
        }
    };
}
prim_elem!(u8, u8, u8);
prim_elem!(i8, i8, i8);
prim_elem!(i16, i16, i16);
prim_elem!(u16, u16, u16);
prim_elem!(i32, i32, i32);
prim_elem!(i64, i64, i64);
prim_elem!(f32, f32, f32);
prim_elem!(f64, f64, f64);

impl<'a> Elem<'a> for VarInt {
    fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
        let x = r.varint();
        if r.err {
            None
        } else {
            Some(VarInt(x))
        }
    }
}
impl<'a> Elem<'a> for VarLong {
    fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
        let x = r.varlong();
        if r.err {
            None
        } else {
            Some(VarLong(x))
        }
    }
}
impl<'a> Elem<'a> for Str<'a> {
    fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
        let x = r.str();
        if r.err {
            None
        } else {
            Some(x)
        }
    }
}
impl<'a> Elem<'a> for Uuid {
    fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
        let p = r.bytes(16)?;
        let mut u = [0u8; 16];
        u.copy_from_slice(p);
        Some(Uuid(u))
    }
}
/// Arrays of raw bytes are always NBT compounds in our protocols.
impl<'a> Elem<'a> for Bytes<'a> {
    fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
        read_nbt(r, false)
    }
}
impl EncElem for VarInt {
    fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
        w.varint(self.0);
    }
}
impl EncElem for VarLong {
    fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
        w.varlong(self.0);
    }
}
impl<'a> EncElem for Str<'a> {
    fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
        w.str(self.0);
    }
}
impl EncElem for Uuid {
    fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
        w.bytes(&self.0);
    }
}
impl<'a> EncElem for Bytes<'a> {
    fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
        w.bytes(self.0);
    }
}
impl<T: EncElem> EncElem for &[T] {
    fn encode(&self, w: &mut Writer, v: Version, c: i32) {
        w.varint(self.len() as i32);
        for e in self.iter() {
            e.encode(w, v, c);
        }
    }
}

// ------------------------------------------------------------------------------------------------ Array view

/// Array view: validated once at decode time, then iterated lazily.
pub struct Array<'a, T> {
    r: Reader<'a>,
    n: u32,
    v: Version,
    ctx: i32,
    _t: PhantomData<fn() -> T>,
}
impl<'a, T> Clone for Array<'a, T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, T> Copy for Array<'a, T> {}
impl<'a, T> Default for Array<'a, T> {
    fn default() -> Self {
        Array { r: Reader::new(&[]), n: 0, v: Version::V1_12_2, ctx: 0, _t: PhantomData }
    }
}
impl<'a, T> core::fmt::Debug for Array<'a, T> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Array(n={})", self.n)
    }
}
impl<'a, T: Elem<'a>> Array<'a, T> {
    /// Validates `n` consecutive elements at the reader position and returns a view over exactly those bytes.
    pub fn parse(r: &mut Reader<'a>, n: i32, v: Version, ctx: i32) -> Option<Self> {
        if r.err || n < 0 || n as usize > r.left() {
            r.err = true;
            return None;
        }
        let start = r.p;
        for _ in 0..n {
            T::decode(r, v, ctx)?;
        }
        let used = start.len() - r.p.len();
        Some(Array { r: Reader::new(&start[..used]), n: n as u32, v, ctx, _t: PhantomData })
    }
    pub fn size(&self) -> u32 {
        self.n
    }
    pub fn is_empty(&self) -> bool {
        self.n == 0
    }
    pub fn next_elem(&mut self) -> Option<T> {
        if self.n == 0 {
            return None;
        }
        self.n -= 1;
        T::decode(&mut self.r, self.v, self.ctx)
    }
}
impl<'a, T: Elem<'a>> Iterator for Array<'a, T> {
    type Item = T;
    fn next(&mut self) -> Option<T> {
        self.next_elem()
    }
}
/// A nested array (varint count) as an element of another array.
impl<'a, T: Elem<'a>> Elem<'a> for Array<'a, T> {
    fn decode(r: &mut Reader<'a>, v: Version, ctx: i32) -> Option<Self> {
        let n = r.varint();
        Array::parse(r, n, v, ctx)
    }
}

// ------------------------------------------------------------------------------------------------ NBT skipping

pub fn skip_nbt_payload(r: &mut Reader, tag: u8, depth: i32) -> bool {
    if depth > 512 {
        r.err = true;
        return false;
    }
    match tag {
        1 => r.skip(1),
        2 => r.skip(2),
        3 | 5 => r.skip(4),
        4 | 6 => r.skip(8),
        7 => {
            let n = r.i32();
            if n < 0 {
                r.err = true;
            } else {
                r.skip(n as usize);
            }
        }
        8 => {
            let n = r.u16();
            r.skip(n as usize);
        }
        9 => {
            let t = r.u8();
            let n = r.i32();
            if n < 0 || (t == 0 && n > 0) {
                r.err = true;
            } else if t == 1 {
                r.skip(n as usize);
            } else {
                let mut i = 0;
                while i < n && r.ok() {
                    skip_nbt_payload(r, t, depth + 1);
                    i += 1;
                }
            }
        }
        10 => loop {
            let t = r.u8();
            if !r.ok() || t == 0 {
                break;
            }
            let n = r.u16();
            r.skip(n as usize);
            skip_nbt_payload(r, t, depth + 1);
            if !r.ok() {
                break;
            }
        },
        11 => {
            let n = r.i32();
            if !(0..=(1 << 28)).contains(&n) {
                r.err = true;
            } else {
                r.skip(n as usize * 4);
            }
        }
        12 => {
            let n = r.i32();
            if !(0..=(1 << 28)).contains(&n) {
                r.err = true;
            } else {
                r.skip(n as usize * 8);
            }
        }
        _ => r.err = true,
    }
    r.ok()
}

/// Reads a root NBT tag (named, as sent in 1.10-1.12.2). If `optional`, a lone TAG_End yields an empty view.
pub fn read_nbt<'a>(r: &mut Reader<'a>, optional: bool) -> Option<Bytes<'a>> {
    let start = r.p;
    let tag = r.u8();
    if !r.ok() {
        return None;
    }
    if tag == 0 {
        if !optional {
            r.err = true;
            return None;
        }
        return Some(Bytes(&start[..0]));
    }
    let n = r.u16();
    r.skip(n as usize);
    if !skip_nbt_payload(r, tag, 0) {
        return None;
    }
    Some(Bytes(&start[..start.len() - r.p.len()]))
}

// ------------------------------------------------------------------------------------------------ Slot

/// Item stack. `id == -1`: empty; `nbt.0.is_empty()`: no tag.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Slot<'a> {
    pub id: i16,
    pub count: i8,
    pub damage: i16,
    pub nbt: Bytes<'a>,
}
pub fn decode_slot<'a>(r: &mut Reader<'a>) -> Option<Slot<'a>> {
    let mut s = Slot { id: r.i16(), ..Default::default() };
    if s.id == -1 {
        return if r.ok() { Some(s) } else { None };
    }
    s.count = r.i8();
    s.damage = r.i16();
    s.nbt = read_nbt(r, true)?;
    Some(s)
}
pub fn encode_slot(w: &mut Writer, s: &Slot) {
    w.u16(s.id as u16);
    if s.id == -1 {
        return;
    }
    w.u8(s.count as u8);
    w.u16(s.damage as u16);
    if !s.nbt.0.is_empty() {
        w.bytes(s.nbt.0);
    } else {
        w.u8(0);
    }
}
impl<'a> Elem<'a> for Slot<'a> {
    fn decode(r: &mut Reader<'a>, _v: Version, _c: i32) -> Option<Self> {
        decode_slot(r)
    }
}
impl<'a> EncElem for Slot<'a> {
    fn encode(&self, w: &mut Writer, _v: Version, _c: i32) {
        encode_slot(w, self);
    }
}

// ------------------------------------------------------------------------------------------------ entity metadata

/// One entity metadata entry. `kind`: 0 byte,1 varint,2 float,3 string,4 chat,5 slot,6 bool,7 rotation,
/// 8 position,9 optPosition,10 direction,11 optUuid,12 blockId,13 nbt (1.12+).
#[derive(Clone, Copy, Default, Debug)]
pub struct MetaEntry<'a> {
    pub key: u8,
    pub kind: u8,
    pub i: i32,
    pub f: [f32; 3],
    pub s: Str<'a>,
    pub slot: Slot<'a>,
    pub pos: BlockPos,
    pub present: bool,
    pub uuid: Uuid,
    pub nbt: Bytes<'a>,
}

#[derive(Clone, Copy)]
pub struct MetaView<'a> {
    r: Reader<'a>,
    v: Version,
}
impl<'a> Default for MetaView<'a> {
    fn default() -> Self {
        MetaView { r: Reader::new(&[]), v: Version::V1_12_2 }
    }
}
impl<'a> core::fmt::Debug for MetaView<'a> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "MetaView({} bytes)", self.r.left())
    }
}

fn meta_value<'a>(r: &mut Reader<'a>, e: &mut MetaEntry<'a>, v: Version) -> bool {
    match e.kind {
        0 => e.i = r.i8() as i32,
        1 | 10 | 12 => e.i = r.varint(),
        2 => e.f[0] = r.f32(),
        3 | 4 => e.s = r.str(),
        5 => match decode_slot(r) {
            Some(s) => e.slot = s,
            None => return false,
        },
        6 => e.i = r.bool() as i32,
        7 => {
            e.f[0] = r.f32();
            e.f[1] = r.f32();
            e.f[2] = r.f32();
        }
        8 => e.pos = unpack_pos(r.u64()),
        9 => {
            e.present = r.bool();
            if e.present {
                e.pos = unpack_pos(r.u64());
            }
        }
        11 => {
            e.present = r.bool();
            if e.present {
                if let Some(p) = r.bytes(16) {
                    e.uuid.0.copy_from_slice(p);
                }
            }
        }
        13 => {
            if v < Version::V1_12 {
                r.err = true;
            } else {
                match read_nbt(r, false) {
                    Some(b) => e.nbt = b,
                    None => return false,
                }
            }
        }
        _ => r.err = true,
    }
    r.ok()
}

impl<'a> MetaView<'a> {
    /// The validated raw bytes and the protocol version (for handing the view across the C ABI).
    pub fn raw(&self) -> (&'a [u8], Version) {
        (self.r.p, self.v)
    }
    /// Validates the whole metadata list (until the 0xFF terminator) and returns a view over it.
    pub fn parse(r: &mut Reader<'a>, v: Version) -> Option<Self> {
        let start = r.p;
        loop {
            let k = r.u8();
            if !r.ok() {
                return None;
            }
            if k == 0xFF {
                break;
            }
            let mut e = MetaEntry { key: k, kind: r.u8(), present: true, ..Default::default() };
            if !meta_value(r, &mut e, v) {
                return None;
            }
        }
        let used = start.len() - r.p.len();
        Some(MetaView { r: Reader::new(&start[..used]), v })
    }
    /// None at the end marker (or on a malformed stream, already ruled out by decode-time validation).
    pub fn next_entry(&mut self) -> Option<MetaEntry<'a>> {
        if self.r.left() == 0 {
            return None;
        }
        let k = self.r.u8();
        if k == 0xFF {
            return None;
        }
        let mut e = MetaEntry { key: k, kind: self.r.u8(), present: true, ..Default::default() };
        if meta_value(&mut self.r, &mut e, self.v) {
            Some(e)
        } else {
            None
        }
    }
}
impl<'a> Iterator for MetaView<'a> {
    type Item = MetaEntry<'a>;
    fn next(&mut self) -> Option<MetaEntry<'a>> {
        self.next_entry()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn varint_roundtrip() {
        for &v in &[0i32, 1, 127, 128, 300, 25565, i32::MAX, -1, i32::MIN] {
            let mut buf = [0u8; 8];
            let mut w = Writer::new(&mut buf);
            w.varint(v);
            let n = w.pos;
            assert_eq!(n, varint_size(v as u32));
            let mut r = Reader::new(&buf[..n]);
            assert_eq!(r.varint(), v);
            assert!(r.ok() && r.left() == 0);
        }
    }

    #[test]
    fn reader_overrun_sets_error() {
        let mut r = Reader::new(&[1, 2, 3]);
        assert_eq!(r.u32(), 0);
        assert!(!r.ok());
        assert_eq!(r.u8(), 0);
        let mut r = Reader::new(&[0xFF; 6]);
        r.varint();
        assert!(!r.ok());
    }

    #[test]
    fn pos_roundtrip() {
        for &(x, y, z) in &[(0, 0, 0), (-1, 255, -1), (33554431, 4095, -33554432), (-5, 64, 7)] {
            let p = unpack_pos(pack_pos(x, y, z));
            assert_eq!((p.x, p.y, p.z), (x, y, z));
        }
    }

    #[test]
    fn str_limits() {
        let mut r = Reader::new(&[3, b'a', b'b', b'c', 9]);
        assert_eq!(r.str(), "abc");
        let mut r = Reader::new(&[5, b'a']);
        r.str();
        assert!(!r.ok());
    }
}
