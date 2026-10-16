//! Zero-copy NBT reader over the raw bytes handed out by the packet decoders (`Slot::nbt`, tile entities, ...).
//! The bytes were already validated when the packet was decoded, so iteration cannot run out of bounds.
use crate::wire::{skip_nbt_payload, Bytes, Reader, Str};

pub const END: u8 = 0;
pub const BYTE: u8 = 1;
pub const SHORT: u8 = 2;
pub const INT: u8 = 3;
pub const LONG: u8 = 4;
pub const FLOAT: u8 = 5;
pub const DOUBLE: u8 = 6;
pub const BYTE_ARRAY: u8 = 7;
pub const STRING: u8 = 8;
pub const LIST: u8 = 9;
pub const COMPOUND: u8 = 10;
pub const INT_ARRAY: u8 = 11;
pub const LONG_ARRAY: u8 = 12;

#[derive(Clone, Copy)]
pub struct Compound<'a> {
    pub r: Reader<'a>,
}
#[derive(Clone, Copy)]
pub struct List<'a> {
    pub r: Reader<'a>,
    pub elem_type: u8,
    pub count: i32,
}
/// One tag. `value` is positioned at the payload.
#[derive(Clone, Copy)]
pub struct Entry<'a> {
    pub kind: u8,
    pub name: Str<'a>,
    pub value: Reader<'a>,
}

impl<'a> Entry<'a> {
    pub fn is_number(&self) -> bool {
        (BYTE..=DOUBLE).contains(&self.kind)
    }
    pub fn as_int(&self) -> i64 {
        let mut c = self.value;
        match self.kind {
            BYTE => c.i8() as i64,
            SHORT => c.i16() as i64,
            INT => c.i32() as i64,
            LONG => c.i64(),
            FLOAT => c.f32() as i64,
            DOUBLE => c.f64() as i64,
            _ => 0,
        }
    }
    pub fn as_float(&self) -> f64 {
        let mut c = self.value;
        match self.kind {
            FLOAT => c.f32() as f64,
            DOUBLE => c.f64(),
            _ => self.as_int() as f64,
        }
    }
    pub fn as_string(&self) -> Str<'a> {
        let mut c = self.value;
        if self.kind == STRING {
            let n = c.u16() as usize;
            if let Some(b) = c.bytes(n) {
                return Str(b);
            }
        }
        Str(&[])
    }
    pub fn as_compound(&self) -> Compound<'a> {
        Compound { r: Reader { p: self.value.p, err: false } }
    }
    pub fn as_list(&self) -> List<'a> {
        let mut r = Reader { p: self.value.p, err: false };
        let elem_type = r.u8();
        let count = r.i32();
        List { r, elem_type, count }
    }
    pub fn as_bytes(&self) -> Bytes<'a> {
        let mut c = self.value;
        if self.kind == BYTE_ARRAY {
            let n = c.i32();
            if n >= 0 {
                if let Some(b) = c.bytes(n as usize) {
                    return Bytes(b);
                }
            }
        }
        Bytes(&[])
    }
    pub fn array_length(&self) -> i32 {
        let mut c = self.value;
        if matches!(self.kind, BYTE_ARRAY | INT_ARRAY | LONG_ARRAY) {
            c.i32()
        } else {
            0
        }
    }
}

impl<'a> Compound<'a> {
    pub fn next_entry(&mut self) -> Option<Entry<'a>> {
        if self.r.err || self.r.left() == 0 {
            return None;
        }
        let t = self.r.u8();
        if t == 0 || self.r.err {
            return None;
        }
        let n = self.r.u16() as usize;
        let name = Str(self.r.bytes(n)?);
        let start = self.r.p;
        if !skip_nbt_payload(&mut self.r, t, 0) {
            return None;
        }
        let used = start.len() - self.r.p.len();
        Some(Entry { kind: t, name, value: Reader { p: &start[..used], err: false } })
    }
    pub fn find(&self, name: &str) -> Option<Entry<'a>> {
        let mut c = *self;
        while let Some(e) = c.next_entry() {
            if e.name == name {
                return Some(e);
            }
        }
        None
    }
}
impl<'a> Iterator for Compound<'a> {
    type Item = Entry<'a>;
    fn next(&mut self) -> Option<Entry<'a>> {
        self.next_entry()
    }
}

impl<'a> List<'a> {
    pub fn next_entry(&mut self) -> Option<Entry<'a>> {
        if self.count <= 0 || self.r.err {
            return None;
        }
        self.count -= 1;
        let start = self.r.p;
        if !skip_nbt_payload(&mut self.r, self.elem_type, 0) {
            return None;
        }
        let used = start.len() - self.r.p.len();
        Some(Entry { kind: self.elem_type, name: Str(&[]), value: Reader { p: &start[..used], err: false } })
    }
}
impl<'a> Iterator for List<'a> {
    type Item = Entry<'a>;
    fn next(&mut self) -> Option<Entry<'a>> {
        self.next_entry()
    }
}

/// Root tag of a [`Bytes`] blob (as found in `Slot::nbt`): the root compound and its name.
pub fn root<'a>(b: Bytes<'a>) -> Option<(Compound<'a>, Str<'a>)> {
    if b.0.is_empty() {
        return None;
    }
    let mut r = Reader::new(b.0);
    let t = r.u8();
    if t != COMPOUND {
        return None;
    }
    let n = r.u16() as usize;
    let name = Str(r.bytes(n)?);
    Some((Compound { r: Reader { p: r.p, err: false } }, name))
}

/// Look up "a/b/c" through nested compounds.
pub fn path<'a>(b: Bytes<'a>, path: &str) -> Option<Entry<'a>> {
    let (mut c, _) = root(b)?;
    let mut path = path;
    loop {
        let (head, rest) = match path.find('/') {
            Some(i) => (&path[..i], Some(&path[i + 1..])),
            None => (path, None),
        };
        let mut it = c;
        let mut found = None;
        while let Some(e) = it.next_entry() {
            if e.name == head {
                found = Some(e);
                break;
            }
        }
        let e = found?;
        match rest {
            None => return Some(e),
            Some(rest) => {
                if e.kind != COMPOUND {
                    return None;
                }
                c = e.as_compound();
                path = rest;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> std::vec::Vec<u8> {
        // {"": { display: { Name: "Hi" }, Damage: 5s, list: [1i, 2i] }}
        let mut v = std::vec![COMPOUND, 0, 0];
        v.extend_from_slice(&[COMPOUND, 0, 7]);
        v.extend_from_slice(b"display");
        v.extend_from_slice(&[STRING, 0, 4]);
        v.extend_from_slice(b"Name");
        v.extend_from_slice(&[0, 2, b'H', b'i']);
        v.push(0);
        v.extend_from_slice(&[SHORT, 0, 6]);
        v.extend_from_slice(b"Damage");
        v.extend_from_slice(&[0, 5]);
        v.extend_from_slice(&[LIST, 0, 4]);
        v.extend_from_slice(b"list");
        v.extend_from_slice(&[INT, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2]);
        v.push(0);
        v
    }

    #[test]
    fn lookup_and_paths() {
        let data = sample();
        let b = Bytes(&data);
        let (root, name) = root(b).unwrap();
        assert!(name.is_empty());
        assert_eq!(root.find("Damage").unwrap().as_int(), 5);
        assert_eq!(path(b, "display/Name").unwrap().as_string(), "Hi");
        assert!(path(b, "display/Nope").is_none());
        assert!(path(b, "Damage/x").is_none());
        let mut l = root.find("list").unwrap().as_list();
        assert_eq!(l.elem_type, INT);
        assert_eq!(l.next_entry().unwrap().as_int(), 1);
        assert_eq!(l.next_entry().unwrap().as_int(), 2);
        assert!(l.next_entry().is_none());
        assert_eq!(root.count(), 3);
    }
}
