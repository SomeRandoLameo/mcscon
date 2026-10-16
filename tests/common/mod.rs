//! Helpers shared by the Rust integration tests: wire builders, a scripted in-memory transport.
#![allow(dead_code)]
use mcscon::client::Packet;
use mcscon::ids::PlayCb;
use mcscon::transport::{Closed, Transport};
use mcscon::wire::{pack_pos, Reader, Writer};
use mcscon::{State, Version};

pub fn varint(b: &mut Vec<u8>, v: i32) {
    let mut t = [0u8; 5];
    let mut w = Writer::new(&mut t);
    w.varint(v);
    let n = w.pos;
    b.extend_from_slice(&t[..n]);
}
pub fn be64(b: &mut Vec<u8>, v: u64) {
    b.extend_from_slice(&v.to_be_bytes());
}

/// One wire section: bits-per-block, palette, packed indices, block light and optionally sky light.
pub fn section(b: &mut Vec<u8>, bpb: usize, pal: &[u16], value: impl Fn(usize) -> u64, sky: bool) {
    b.push(bpb as u8);
    varint(b, pal.len() as i32);
    for &p in pal {
        varint(b, p as i32);
    }
    let mut d = vec![0u64; 64 * bpb];
    for i in 0..4096 {
        let v = value(i);
        let bit = i * bpb;
        let (w, off) = (bit / 64, bit % 64);
        d[w] |= v << off;
        if off + bpb > 64 {
            d[w + 1] |= v >> (64 - off);
        }
    }
    varint(b, d.len() as i32);
    for x in d {
        be64(b, x);
    }
    b.extend(std::iter::repeat(0x55).take(2048)); // block light
    if sky {
        b.extend(std::iter::repeat(0xFF).take(2048)); // sky light
    }
}
pub fn ref_state(sec: usize, i: usize) -> u16 {
    (((sec * 7 + i * 31) % 90) << 4 | (i % 16)) as u16
}

pub fn map_chunk(cx: i32, cz: i32, ground_up: bool, mask: u32, data: &[u8]) -> Vec<u8> {
    let mut p = Vec::new();
    varint(&mut p, 0x20);
    p.extend_from_slice(&cx.to_be_bytes());
    p.extend_from_slice(&cz.to_be_bytes());
    p.push(ground_up as u8);
    varint(&mut p, mask as i32);
    varint(&mut p, data.len() as i32);
    p.extend_from_slice(data);
    varint(&mut p, 0);
    p
}

/// A packet body (one-byte wire id followed by the payload) as a decoded-packet handle.
pub fn mk(pkt: &[u8], id: PlayCb) -> Packet<'_> {
    Packet { state: State::Play, id: id as u8, wire_id: pkt[0] as i32, version: Version::V1_12_2, r: Reader::new(&pkt[1..]) }
}
/// Builds `varint(wire id) + payload`.
pub fn be(id: i32, f: impl FnOnce(&mut Writer)) -> Vec<u8> {
    let mut b = Vec::new();
    varint(&mut b, id);
    let mut t = [0u8; 1024];
    let mut w = Writer::new(&mut t);
    f(&mut w);
    let n = w.pos;
    b.extend_from_slice(&t[..n]);
    b
}
pub fn framed(payload: &[u8]) -> Vec<u8> {
    let mut f = Vec::new();
    varint(&mut f, payload.len() as i32);
    f.extend_from_slice(payload);
    f
}
/// Serverbound wire ids in an uncompressed byte stream.
pub fn sent_ids(out: &[u8]) -> Vec<i32> {
    let mut r = Vec::new();
    let mut rd = Reader::new(out);
    while rd.left() > 0 {
        let len = rd.varint() as usize;
        let mut p = Reader::new(&rd.p[..len]);
        r.push(p.varint());
        rd.skip(len);
    }
    r
}

#[derive(Default)]
pub struct Pipe {
    pub inp: Vec<u8>,
    pub out: Vec<u8>,
    pub pos: usize,
}
impl Transport for Pipe {
    fn connect(&mut self, _h: &str, _p: u16) -> bool {
        true
    }
    fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed> {
        let n = (self.inp.len() - self.pos).min(buf.len());
        buf[..n].copy_from_slice(&self.inp[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
    fn send(&mut self, buf: &[u8]) -> Result<usize, Closed> {
        self.out.extend_from_slice(buf);
        Ok(buf.len())
    }
    fn close(&mut self) {}
}

pub fn pos_bytes(x: i32, y: i32, z: i32) -> [u8; 8] {
    pack_pos(x, y, z).to_be_bytes()
}
