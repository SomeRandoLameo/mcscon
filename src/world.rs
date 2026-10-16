//! Chunk/world model for 1.10-1.12.2. Sections stay in their compact wire form (palette + bit-packed indices)
//! inside a caller-provided arena, so a 16-section column costs a few KB instead of 128 KB.
use crate::arena::Arena;
use crate::blocks;
use crate::client::Packet;
use crate::ids::PlayCb;
use crate::nbt;
use crate::packets::{self, cb};
use crate::types::State;
use crate::wire::{BlockPos, Bytes, Reader};
use core::cell::Cell;
use core::marker::PhantomData;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WorldConfig {
    /// Simultaneously loaded 16x16 columns (view distance 4 -> 81, 8 -> 289).
    pub max_columns: u16,
    /// Keep block/sky light nibbles (+4 KB per section).
    pub store_light: bool,
    /// Keep the 256-byte biome map per column.
    pub store_biomes: bool,
}

/// Section block layout: header, palette (u16[], padded to 8 bytes; absent when bpb > 8), u64 data[64*bpb], optional 4 KB light.
#[repr(C)]
struct SecHdr {
    bpb: u8,
    light: u8,
    pal_len: u16,
    pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Column {
    x: i32,
    z: i32,
    sec: [u32; 16],
    biomes: u32,
    mask: u16,
    used: u8,
    pad: u8,
}
/// Bytes of bookkeeping per column.
pub const COLUMN_BYTES: usize = core::mem::size_of::<Column>();

/// A block state: `blockId << 4 | meta`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct BlockInfo {
    pub state: u16,
}
impl BlockInfo {
    pub fn id(&self) -> u8 {
        (self.state >> 4) as u8
    }
    pub fn meta(&self) -> u8 {
        (self.state & 15) as u8
    }
}

/// Receives tile entity NBT (MapChunk block entities + TileEntityData). The bytes are valid only inside the call.
pub trait WorldSink {
    fn tile_entity(&mut self, pos: BlockPos, nbt: Bytes<'_>);
}

fn pal_bytes(pal_len: u32) -> u32 {
    (pal_len * 2 + 7) & !7
}
fn data_bytes(bpb: u32) -> u32 {
    512 * bpb
}
fn sec_bytes(bpb: u32, pal_len: u32, light: bool) -> u32 {
    core::mem::size_of::<SecHdr>() as u32 + if bpb <= 8 { pal_bytes(pal_len) } else { 0 } + data_bytes(bpb) + if light { 4096 } else { 0 }
}
unsafe fn pal_of(h: *mut SecHdr) -> *mut u16 {
    h.add(1) as *mut u16
}
unsafe fn data_of(sec: *mut u8, bpb: u32, pal_len: u32) -> *mut u64 {
    sec.add(8 + if bpb <= 8 { pal_bytes(pal_len) as usize } else { 0 }) as *mut u64
}
unsafe fn put_packed(d: *mut u64, bpb: u32, idx: u32, v: u32) {
    let bit = idx * bpb;
    let (w, off) = ((bit >> 6) as usize, bit & 63);
    let mask = (1u64 << bpb) - 1;
    *d.add(w) = (*d.add(w) & !(mask << off)) | ((v as u64) << off);
    if off + bpb > 64 {
        let sh = 64 - off;
        *d.add(w + 1) = (*d.add(w + 1) & !(mask >> sh)) | ((v as u64) >> sh);
    }
}
unsafe fn get_packed(d: *const u64, bpb: u32, idx: u32) -> u64 {
    let bit = idx * bpb;
    let (w, off) = ((bit >> 6) as usize, bit & 63);
    let mut v = *d.add(w) >> off;
    if off + bpb > 64 {
        v |= *d.add(w + 1) << (64 - off);
    }
    v & ((1u64 << bpb) - 1)
}

pub struct World<'a> {
    cols: &'a mut [Column],
    arena: Arena,
    cfg: WorldConfig,
    dim: i32,
    last_hit: Cell<usize>,
    _mem: PhantomData<&'a mut [u8]>,
}

impl<'a> World<'a> {
    /// Bytes needed for the bookkeeping table: pass `memory` of at least `column_table_bytes(cfg)` + arena size.
    pub fn column_table_bytes(cfg: &WorldConfig) -> usize {
        cfg.max_columns as usize * COLUMN_BYTES
    }

    /// `memory` is split into the column table and the section arena (it is aligned to 8 bytes internally).
    pub fn new(memory: &'a mut [u8], cfg: WorldConfig) -> World<'a> {
        let skip = memory.as_ptr().align_offset(8).min(memory.len());
        let memory = &mut memory[skip..];
        let tbl = (Self::column_table_bytes(&cfg) + 7) & !7;
        let max_cols = if tbl <= memory.len() { cfg.max_columns as usize } else { memory.len() / COLUMN_BYTES };
        let tbl = (if tbl <= memory.len() { tbl } else { (max_cols * COLUMN_BYTES + 7) & !7 }).min(memory.len());
        let (head, tail) = memory.split_at_mut(tbl);
        // SAFETY: `head` is 8-byte aligned (Column needs 4), at least max_cols * size_of::<Column>() bytes long, and
        // every bit pattern (zero) is a valid Column.
        let cols: &'a mut [Column] = if max_cols == 0 {
            &mut []
        } else {
            unsafe {
                core::ptr::write_bytes(head.as_mut_ptr(), 0, head.len());
                core::slice::from_raw_parts_mut(head.as_mut_ptr() as *mut Column, max_cols)
            }
        };
        let mut arena = Arena::empty();
        // SAFETY: `tail` starts 8-byte aligned (tbl is a multiple of 8) and is exclusively borrowed for 'a.
        unsafe { arena.init(tail.as_mut_ptr(), tail.len()) };
        World { cols, arena, cfg: WorldConfig { max_columns: max_cols as u16, ..cfg }, dim: 0, last_hit: Cell::new(0), _mem: PhantomData }
    }

    pub fn clear(&mut self) {
        for i in 0..self.cols.len() {
            if self.cols[i].used != 0 {
                self.free_col(i);
            }
        }
    }

    // ------------------------------------------------------------------ column bookkeeping
    fn find_col(&self, cx: i32, cz: i32) -> Option<usize> {
        if self.cols.is_empty() {
            return None;
        }
        let h = &self.cols[self.last_hit.get().min(self.cols.len() - 1)];
        if h.used != 0 && h.x == cx && h.z == cz {
            return Some(self.last_hit.get());
        }
        for (i, c) in self.cols.iter().enumerate() {
            if c.used != 0 && c.x == cx && c.z == cz {
                self.last_hit.set(i);
                return Some(i);
            }
        }
        None
    }
    fn new_col(&mut self, cx: i32, cz: i32) -> Option<usize> {
        for (i, c) in self.cols.iter_mut().enumerate() {
            if c.used == 0 {
                *c = Column { x: cx, z: cz, used: 1, ..Column::default() };
                return Some(i);
            }
        }
        None
    }
    fn free_sections(&mut self, ci: usize) {
        for s in 0..16 {
            let o = self.cols[ci].sec[s];
            self.arena.release(o);
            self.cols[ci].sec[s] = 0;
        }
        let b = self.cols[ci].biomes;
        self.arena.release(b);
        self.cols[ci].biomes = 0;
        self.cols[ci].mask = 0;
    }
    fn free_col(&mut self, ci: usize) {
        self.free_sections(ci);
        self.cols[ci].used = 0;
    }
    pub fn loaded_columns(&self) -> u32 {
        self.cols.iter().filter(|c| c.used != 0).count() as u32
    }

    fn alloc_section(&mut self, bpb: u32, pal_len: u32, light: bool) -> u32 {
        let bytes = sec_bytes(bpb, pal_len, light);
        let off = self.arena.alloc(bytes);
        if off == 0 {
            return 0;
        }
        // SAFETY: the arena returned a block of `bytes` bytes at `off`.
        unsafe {
            core::ptr::write_bytes(self.arena.ptr(off), 0, bytes as usize);
            let h = self.arena.ptr(off) as *mut SecHdr;
            (*h).bpb = bpb as u8;
            (*h).pal_len = pal_len as u16;
            (*h).light = light as u8;
        }
        off
    }

    unsafe fn get_in_section(&self, h: *mut SecHdr, idx: u32) -> u16 {
        let bpb = (*h).bpb as u32;
        let d = data_of(h as *mut u8, bpb, (*h).pal_len as u32);
        let v = get_packed(d, bpb, idx);
        if bpb <= 8 {
            if v < (*h).pal_len as u64 {
                *pal_of(h).add(v as usize)
            } else {
                0
            }
        } else {
            v as u16
        }
    }

    // ------------------------------------------------------------------ queries (state = blockId << 4 | meta)
    pub fn is_loaded(&self, x: i32, y: i32, z: i32) -> bool {
        (0..256).contains(&y) && self.find_col(x >> 4, z >> 4).is_some()
    }
    pub fn column_loaded(&self, cx: i32, cz: i32) -> bool {
        self.find_col(cx, cz).is_some()
    }
    /// Unloaded space reads as air (0); use [`World::is_loaded`] to tell.
    pub fn block_state(&self, x: i32, y: i32, z: i32) -> u16 {
        if !(0..=255).contains(&y) {
            return 0;
        }
        let ci = match self.find_col(x >> 4, z >> 4) {
            Some(c) => c,
            None => return 0,
        };
        let so = self.cols[ci].sec[(y >> 4) as usize];
        if so == 0 {
            return 0;
        }
        // SAFETY: `so` is a live section block owned by this world.
        unsafe { self.get_in_section(self.arena.ptr(so) as *mut SecHdr, (((y & 15) << 8) | ((z & 15) << 4) | (x & 15)) as u32) }
    }
    pub fn block_id(&self, x: i32, y: i32, z: i32) -> u8 {
        (self.block_state(x, y, z) >> 4) as u8
    }
    /// True if the block state is a full cube (exact per-meta shapes: see `blocks::shape_of` / physics).
    pub fn solid(&self, x: i32, y: i32, z: i32) -> bool {
        blocks::shape_of(self.block_state(x, y, z)) == 1
    }
    /// Has any collision box (slabs, stairs, fences, ladders, ...).
    pub fn collides(&self, x: i32, y: i32, z: i32) -> bool {
        blocks::shape_of(self.block_state(x, y, z)) != 0
    }
    pub fn liquid(&self, x: i32, y: i32, z: i32) -> bool {
        blocks::FLAGS[self.block_id(x, y, z) as usize] & blocks::LIQUID != 0
    }
    pub fn climbable(&self, x: i32, y: i32, z: i32) -> bool {
        blocks::FLAGS[self.block_id(x, y, z) as usize] & blocks::CLIMBABLE != 0
    }
    /// y of the top non-air block, -1 if none.
    pub fn highest_block(&self, x: i32, z: i32) -> i32 {
        let ci = match self.find_col(x >> 4, z >> 4) {
            Some(c) => c,
            None => return -1,
        };
        for sy in (0..16).rev() {
            let so = self.cols[ci].sec[sy];
            if so == 0 {
                continue;
            }
            for ly in (0..16).rev() {
                // SAFETY: live section block.
                let st = unsafe { self.get_in_section(self.arena.ptr(so) as *mut SecHdr, ((ly << 8) | ((z & 15) << 4) | (x & 15)) as u32) };
                if st >> 4 != 0 {
                    return (sy * 16 + ly as usize) as i32;
                }
            }
        }
        -1
    }
    /// 0xFF if unknown.
    pub fn biome(&self, x: i32, z: i32) -> u8 {
        match self.find_col(x >> 4, z >> 4) {
            Some(ci) if self.cols[ci].biomes != 0 => unsafe { *self.arena.ptr(self.cols[ci].biomes).add((((z & 15) << 4) | (x & 15)) as usize) },
            _ => 0xFF,
        }
    }
    pub fn block_light(&self, x: i32, y: i32, z: i32) -> u8 {
        self.light(x, y, z, false)
    }
    pub fn sky_light(&self, x: i32, y: i32, z: i32) -> u8 {
        self.light(x, y, z, true)
    }
    fn light(&self, x: i32, y: i32, z: i32, sky: bool) -> u8 {
        if !(0..=255).contains(&y) {
            return if sky { 15 } else { 0 };
        }
        let ci = match self.find_col(x >> 4, z >> 4) {
            Some(c) => c,
            None => return 0,
        };
        let so = self.cols[ci].sec[(y >> 4) as usize];
        if so == 0 {
            return if sky { 15 } else { 0 }; // missing section: open sky above / no block light
        }
        // SAFETY: live section block; light nibbles follow the data when `light` is set.
        unsafe {
            let h = self.arena.ptr(so) as *mut SecHdr;
            if (*h).light == 0 {
                return 0;
            }
            let bpb = (*h).bpb as u32;
            let l = (data_of(h as *mut u8, bpb, (*h).pal_len as u32) as *mut u8).add(data_bytes(bpb) as usize + if sky { 2048 } else { 0 });
            let idx = (((y & 15) << 8) | ((z & 15) << 4) | (x & 15)) as usize;
            (*l.add(idx >> 1) >> ((idx & 1) * 4)) & 15
        }
    }

    // ------------------------------------------------------------------ edits
    /// Local edit (also used by the packet handlers). False if the column is not loaded or memory ran out.
    pub fn set_block_state(&mut self, x: i32, y: i32, z: i32, state: u16) -> bool {
        if !(0..=255).contains(&y) {
            return false;
        }
        let ci = match self.find_col(x >> 4, z >> 4) {
            Some(c) => c,
            None => return false,
        };
        self.set_in_section(ci, (y >> 4) as usize, (((y & 15) << 8) | ((z & 15) << 4) | (x & 15)) as u32, state)
    }

    fn set_in_section(&mut self, ci: usize, sy: usize, idx: u32, state: u16) -> bool {
        if self.cols[ci].sec[sy] == 0 {
            if state == 0 {
                return true;
            }
            let off = self.alloc_section(4, 1, self.cfg.store_light);
            if off == 0 {
                return false;
            }
            self.cols[ci].sec[sy] = off;
            self.cols[ci].mask |= 1 << sy; // palette[0] = air, all indices 0
        }
        // SAFETY: all pointers below stay inside live arena blocks; sections are only replaced through `alloc`/`release`.
        unsafe {
            let mut h = self.arena.ptr(self.cols[ci].sec[sy]) as *mut SecHdr;
            let mut bpb = (*h).bpb as u32;
            if bpb > 8 {
                put_packed(data_of(h as *mut u8, bpb, 0), bpb, idx, state as u32);
                return true;
            }
            let mut pal = pal_of(h);
            let mut pi = 0u32;
            while pi < (*h).pal_len as u32 {
                if *pal.add(pi as usize) == state {
                    break;
                }
                pi += 1;
            }
            if pi == (*h).pal_len as u32 {
                if ((*h).pal_len as u32) < (1u32 << bpb) {
                    // room in the palette: append (padding bytes may need to grow)
                    let new_len = (*h).pal_len as u32 + 1;
                    if pal_bytes(new_len) != pal_bytes((*h).pal_len as u32) {
                        // palette crosses an 8-byte boundary -> move data
                        let nb = sec_bytes(bpb, new_len, (*h).light != 0);
                        let off = self.arena.alloc(nb);
                        if off == 0 {
                            return false;
                        }
                        let np = self.arena.ptr(off);
                        core::ptr::write_bytes(np, 0, nb as usize);
                        let nh = np as *mut SecHdr;
                        (*nh).bpb = (*h).bpb;
                        (*nh).light = (*h).light;
                        (*nh).pad = (*h).pad;
                        (*nh).pal_len = new_len as u16;
                        core::ptr::copy_nonoverlapping(pal, pal_of(nh), (*h).pal_len as usize);
                        core::ptr::copy_nonoverlapping(
                            data_of(h as *mut u8, bpb, (*h).pal_len as u32) as *const u8,
                            data_of(np, bpb, new_len) as *mut u8,
                            (data_bytes(bpb) + if (*h).light != 0 { 4096 } else { 0 }) as usize,
                        );
                        self.arena.release(self.cols[ci].sec[sy]);
                        self.cols[ci].sec[sy] = off;
                        h = nh;
                        pal = pal_of(h);
                    } else {
                        (*h).pal_len = new_len as u16;
                    }
                    *pal.add(pi as usize) = state;
                } else {
                    // grow bits per block
                    let nbpb = if bpb < 8 { bpb + 1 } else { 13 };
                    let nlen = if nbpb <= 8 { (*h).pal_len as u32 + 1 } else { 0 };
                    let nb = sec_bytes(nbpb, nlen, (*h).light != 0);
                    let off = self.arena.alloc(nb);
                    if off == 0 {
                        return false;
                    }
                    let np = self.arena.ptr(off);
                    core::ptr::write_bytes(np, 0, nb as usize);
                    let nh = np as *mut SecHdr;
                    (*nh).bpb = nbpb as u8;
                    (*nh).pal_len = nlen as u16;
                    (*nh).light = (*h).light;
                    let nd = data_of(np, nbpb, nlen);
                    let od = data_of(h as *mut u8, bpb, (*h).pal_len as u32) as *const u64;
                    let old_len = (*h).pal_len as u32;
                    if nbpb <= 8 {
                        core::ptr::copy_nonoverlapping(pal, pal_of(nh), old_len as usize);
                        *pal_of(nh).add(old_len as usize) = state;
                    }
                    for i in 0..4096u32 {
                        let v = get_packed(od, bpb, i);
                        let nv = if nbpb <= 8 {
                            v as u32
                        } else if v < old_len as u64 {
                            *pal.add(v as usize) as u32
                        } else {
                            0
                        };
                        put_packed(nd, nbpb, i, nv);
                    }
                    if (*h).light != 0 {
                        core::ptr::copy_nonoverlapping((od as *const u8).add(data_bytes(bpb) as usize), (nd as *mut u8).add(data_bytes(nbpb) as usize), 4096);
                    }
                    self.arena.release(self.cols[ci].sec[sy]);
                    self.cols[ci].sec[sy] = off;
                    h = nh;
                    bpb = nbpb;
                    if bpb > 8 {
                        put_packed(data_of(np, bpb, 0), bpb, idx, state as u32);
                        return true;
                    }
                    pi = (*h).pal_len as u32 - 1;
                }
                bpb = (*h).bpb as u32;
            }
            put_packed(data_of(h as *mut u8, bpb, (*h).pal_len as u32), bpb, idx, pi);
        }
        true
    }

    // ------------------------------------------------------------------ chunk data
    fn dim_has_sky(&self) -> bool {
        self.dim == 0
    }

    fn read_column(&mut self, ci: usize, r: &mut Reader, mask: u32, ground_up: bool) -> bool {
        for sy in 0..16 {
            if mask & (1 << sy) == 0 {
                continue;
            }
            let bpb = r.u8() as u32;
            let pal_len = r.varint();
            if !r.ok() || !(1..=13).contains(&bpb) || !(0..=256).contains(&pal_len) {
                return false;
            }
            let mut pal = [0u16; 256];
            for i in 0..pal_len as usize {
                pal[i] = r.varint() as u16;
            }
            let longs = r.varint();
            if !r.ok() || longs != (64 * bpb) as i32 {
                return false;
            }
            let src = r.bytes(longs as usize * 8);
            let bl = r.bytes(2048);
            let sl = if self.dim_has_sky() { r.bytes(2048) } else { None };
            if !r.ok() {
                return false;
            }
            let (src, bl) = match (src, bl) {
                (Some(s), Some(b)) => (s, b),
                _ => return false,
            };
            let old = self.cols[ci].sec[sy];
            self.arena.release(old);
            self.cols[ci].sec[sy] = 0;
            let stored = if bpb <= 8 { pal_len as u32 } else { 0 };
            if bpb < 4 {
                return false; // vanilla never sends fewer than 4 bits per block
            }
            let off = self.alloc_section(bpb, stored, self.cfg.store_light);
            if off == 0 {
                continue; // out of memory: leave this section as air
            }
            self.cols[ci].sec[sy] = off;
            self.cols[ci].mask |= 1 << sy;
            // SAFETY: freshly allocated section of the exact size computed from (bpb, stored, light).
            unsafe {
                let h = self.arena.ptr(off) as *mut SecHdr;
                if bpb <= 8 {
                    core::ptr::copy_nonoverlapping(pal.as_ptr(), pal_of(h), pal_len as usize);
                }
                let d = data_of(h as *mut u8, bpb, stored);
                for i in 0..longs as usize {
                    let mut b = [0u8; 8];
                    b.copy_from_slice(&src[i * 8..i * 8 + 8]);
                    *d.add(i) = u64::from_be_bytes(b);
                }
                if self.cfg.store_light {
                    let l = (d as *mut u8).add(data_bytes(bpb) as usize);
                    core::ptr::copy_nonoverlapping(bl.as_ptr(), l, 2048);
                    if let Some(sl) = sl {
                        core::ptr::copy_nonoverlapping(sl.as_ptr(), l.add(2048), 2048);
                    }
                }
            }
        }
        if ground_up {
            if let Some(bio) = r.bytes(256) {
                if self.cfg.store_biomes {
                    let off = self.arena.alloc(256);
                    if off != 0 {
                        // SAFETY: 256-byte block just allocated.
                        unsafe { core::ptr::copy_nonoverlapping(bio.as_ptr(), self.arena.ptr(off), 256) };
                        self.cols[ci].biomes = off;
                    }
                }
            }
        }
        r.ok()
    }

    /// Feed packets; returns true if the packet was a world packet (MapChunk, UnloadChunk, BlockChange,
    /// MultiBlockChange, Explosion, TileEntityData). Login/Respawn reset the world but are reported as not consumed.
    pub fn handle(&mut self, p: &Packet) -> bool {
        self.handle_with(p, None)
    }

    /// Like [`World::handle`], additionally reporting tile entity NBT to `sink`.
    pub fn handle_with(&mut self, p: &Packet, mut sink: Option<&mut dyn WorldSink>) -> bool {
        if p.state != State::Play {
            return false;
        }
        crate::escape(&mut *self); // tile entity callbacks may read the world through other pointers
        let id = match PlayCb::from_u8(p.id) {
            Some(i) => i,
            None => return false,
        };
        match id {
            PlayCb::MapChunk => {
                let m = match packets::read::<cb::MapChunk>(p) {
                    Some(m) => m,
                    None => return true,
                };
                let mut ci = self.find_col(m.x, m.z);
                if m.ground_up {
                    if let Some(c) = ci {
                        self.free_col(c);
                    }
                    ci = self.new_col(m.x, m.z);
                }
                let ci = match ci {
                    Some(c) => c,
                    None => return true, // partial update for unknown column, or table full
                };
                let mut r = Reader::new(m.chunk_data.0);
                if !self.read_column(ci, &mut r, m.bit_map as u32, m.ground_up) {
                    self.free_col(ci);
                } else if let Some(sink) = sink.as_mut() {
                    let mut arr = m.block_entities;
                    while let Some(b) = arr.next_elem() {
                        if let Some((c, _)) = nbt::root(b) {
                            let (x, y, z) = (c.find("x"), c.find("y"), c.find("z"));
                            if let (Some(x), Some(y), Some(z)) = (x, y, z) {
                                sink.tile_entity(BlockPos { x: x.as_int() as i32, y: y.as_int() as i32, z: z.as_int() as i32 }, b);
                            }
                        }
                    }
                }
                true
            }
            PlayCb::UnloadChunk => {
                if let Some(u) = packets::read::<cb::UnloadChunk>(p) {
                    if let Some(ci) = self.find_col(u.chunk_x, u.chunk_z) {
                        self.free_col(ci);
                    }
                }
                true
            }
            PlayCb::BlockChange => {
                if let Some(b) = packets::read::<cb::BlockChange>(p) {
                    self.set_block_state(b.location.x, b.location.y, b.location.z, b.type_ as u16);
                }
                true
            }
            PlayCb::MultiBlockChange => {
                if let Some(mut m) = packets::read::<cb::MultiBlockChange>(p) {
                    while let Some(rec) = m.records.next_elem() {
                        self.set_block_state(m.chunk_x * 16 + (rec.horizontal_pos >> 4) as i32, rec.y as i32, m.chunk_z * 16 + (rec.horizontal_pos & 15) as i32, rec.block_id as u16);
                    }
                }
                true
            }
            PlayCb::Explosion => {
                if let Some(mut e) = packets::read::<cb::Explosion>(p) {
                    let f = |v: f32| (if v < 0.0 { v - 1.0 } else { v }) as i32;
                    let (ox, oy, oz) = (f(e.x), f(e.y), f(e.z));
                    while let Some(o) = e.affected_block_offsets.next_elem() {
                        self.set_block_state(ox + o.x as i32, oy + o.y as i32, oz + o.z as i32, 0);
                    }
                }
                true
            }
            PlayCb::TileEntityData => {
                if let Some(t) = packets::read::<cb::TileEntityData>(p) {
                    if let Some(sink) = sink.as_mut() {
                        if !t.nbt_data.0.is_empty() {
                            sink.tile_entity(t.location, t.nbt_data);
                        }
                    }
                }
                true
            }
            PlayCb::Login => {
                if let Some(l) = packets::read::<cb::Login>(p) {
                    self.clear();
                    self.dim = l.dimension;
                }
                false
            }
            PlayCb::Respawn => {
                if let Some(r) = packets::read::<cb::Respawn>(p) {
                    self.clear();
                    self.dim = r.dimension;
                }
                false
            }
            _ => false,
        }
    }

    // ------------------------------------------------------------------ stats
    pub fn arena_used(&self) -> u32 {
        self.arena.used()
    }
    pub fn arena_capacity(&self) -> u32 {
        self.arena.capacity()
    }
    pub fn dimension(&self) -> i32 {
        self.dim
    }
    pub fn config(&self) -> WorldConfig {
        self.cfg
    }
}
