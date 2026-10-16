//! Tiny boundary-tag allocator over caller memory (8-byte aligned blocks, first fit, coalescing on free).
//! Offsets (`u32`) are used instead of pointers; 0 means "null". No heap.
use core::ptr;

pub struct Arena {
    base: *mut u8,
    size: u32,
    used: u32,
}

// The arena only ever touches the memory it was initialised with, which the owner keeps exclusively borrowed.
unsafe impl Send for Arena {}

impl Default for Arena {
    fn default() -> Self {
        Arena::empty()
    }
}

impl Arena {
    pub const fn empty() -> Self {
        Arena { base: ptr::null_mut(), size: 0, used: 0 }
    }

    /// # Safety
    /// `mem` must be 8-byte aligned, valid for `size` bytes, and stay exclusively owned by this arena
    /// for as long as it is used.
    pub unsafe fn init(&mut self, mem: *mut u8, size: usize) {
        self.base = mem;
        self.size = (size & !7) as u32;
        self.used = 0;
        if self.size < 32 {
            self.size = 0;
            return;
        }
        let s = self.size;
        self.set_hdr(0, s - 8, false, 0);
        self.set_hdr(s - 8, 0, true, s - 8); // end sentinel
    }

    pub fn alloc(&mut self, bytes: u32) -> u32 {
        let need = ((bytes + 7) & !7) + 8;
        let mut o = 0u32;
        while self.size != 0 && self.size_of(o) != 0 {
            let sz = self.size_of(o);
            if self.is_used(o) || sz < need {
                o += sz;
                continue;
            }
            if sz - need >= 16 {
                // split
                let prev = self.prev_of(o);
                self.set_hdr(o, need, true, prev);
                self.set_hdr(o + need, sz - need, false, need);
                self.set_prev(o + need + (sz - need), sz - need);
            } else {
                let prev = self.prev_of(o);
                self.set_hdr(o, sz, true, prev);
            }
            self.used += self.size_of(o);
            return o + 8;
        }
        0
    }

    pub fn release(&mut self, off: u32) {
        if off == 0 {
            return;
        }
        let mut o = off - 8;
        let mut sz = self.size_of(o);
        self.used -= sz;
        let nx = o + sz;
        if self.size_of(nx) != 0 && !self.is_used(nx) {
            sz += self.size_of(nx);
        }
        let pv = self.prev_of(o);
        if o != 0 && pv != 0 && !self.is_used(o - pv) {
            o -= pv;
            sz += pv;
        }
        let prev = if o != 0 { self.prev_of(o) } else { 0 };
        self.set_hdr(o, sz, false, prev);
        self.set_prev(o + sz, sz);
    }

    /// Pointer to the payload of the block at `off`.
    pub fn ptr(&self, off: u32) -> *mut u8 {
        self.base.wrapping_add(off as usize)
    }
    pub fn capacity(&self) -> u32 {
        self.size
    }
    pub fn used(&self) -> u32 {
        self.used
    }
    /// Size of the largest free block (fragmentation indicator).
    pub fn largest_free(&self) -> u32 {
        let mut m = 0;
        let mut o = 0;
        while self.size != 0 && self.size_of(o) != 0 {
            if !self.is_used(o) && self.size_of(o) > m {
                m = self.size_of(o);
            }
            o += self.size_of(o);
        }
        m.saturating_sub(8)
    }

    fn h(&self, o: u32) -> *mut u32 {
        self.base.wrapping_add(o as usize) as *mut u32
    }
    fn size_of(&self, o: u32) -> u32 {
        unsafe { *self.h(o) & !1 }
    }
    fn is_used(&self, o: u32) -> bool {
        unsafe { *self.h(o) & 1 != 0 }
    }
    fn prev_of(&self, o: u32) -> u32 {
        unsafe { *self.h(o).add(1) }
    }
    fn set_hdr(&mut self, o: u32, sz: u32, used: bool, prev: u32) {
        unsafe {
            *self.h(o) = sz | used as u32;
            *self.h(o).add(1) = prev;
        }
    }
    fn set_prev(&mut self, o: u32, prev: u32) {
        unsafe { *self.h(o).add(1) = prev }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alloc_release_coalesce() {
        let mut mem = std::vec![0u64; 8192]; // 64 KB, 8-byte aligned
        let mut a = Arena::empty();
        unsafe { a.init(mem.as_mut_ptr() as *mut u8, 64 * 1024) };
        let total = a.largest_free();
        assert!(total > 60_000);
        let x = a.alloc(100);
        let y = a.alloc(5000);
        let z = a.alloc(1);
        assert!(x != 0 && y != 0 && z != 0 && x != y && y != z);
        assert!(a.used() > 5100);
        a.release(y);
        a.release(x);
        a.release(z);
        assert_eq!(a.used(), 0);
        assert_eq!(a.largest_free(), total); // everything coalesced back into one block
        assert_eq!(a.alloc(1 << 20), 0); // too big
        // many small allocations, freed in a scrambled order
        let mut offs = std::vec::Vec::new();
        for i in 0..200 {
            offs.push(a.alloc(16 + i));
        }
        assert!(offs.iter().all(|&o| o != 0));
        for k in 0..200 {
            let i = (k * 73) % 200;
            a.release(offs[i]);
            offs[i] = 0;
        }
        // 73 and 200 are coprime, so each block was released exactly once
        assert_eq!(a.used(), 0);
        assert_eq!(a.largest_free(), total);
    }
}
