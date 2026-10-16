//! Allocation-free zlib inflate (RFC 1950/1951). Canonical Huffman decoding in the style of zlib's puff.c,
//! accelerated with a 10-bit lookup table and a 64-bit bit buffer. We never need to deflate (uncompressed
//! frames are always legal). Needs about 7 KB of stack, no heap.

const FAST: usize = 10;
const FAST_MASK: u64 = (1 << FAST) - 1;

struct Huff {
    fast: [u16; 1 << FAST], // (symbol << 4) | len for codes up to FAST bits, 0 = not in the table
    count: [u16; 16],
    symbol: [u16; 288],
}

impl Huff {
    const fn new() -> Self {
        Huff { fast: [0; 1 << FAST], count: [0; 16], symbol: [0; 288] }
    }

    /// Returns 0 for a complete code, <0 if over-subscribed, >0 if incomplete.
    fn construct(&mut self, length: &[u8]) -> i32 {
        self.count = [0; 16];
        self.fast = [0; 1 << FAST];
        for &l in length {
            self.count[l as usize] += 1;
        }
        if self.count[0] as usize == length.len() {
            return 0;
        }
        let mut left: i32 = 1;
        for len in 1..=15 {
            left <<= 1;
            left -= self.count[len] as i32;
            if left < 0 {
                return left;
            }
        }
        let mut offs = [0u16; 16];
        for len in 1..15 {
            offs[len + 1] = offs[len] + self.count[len];
        }
        for (i, &l) in length.iter().enumerate() {
            if l != 0 {
                self.symbol[offs[l as usize] as usize] = i as u16;
                offs[l as usize] += 1;
            }
        }
        // fast table: deflate packs Huffman codes MSB-first into an LSB-first bit stream, so index by the reversed code
        let mut code: u32 = 0;
        let mut idx = 0usize;
        for len in 1..=15usize {
            for _ in 0..self.count[len] {
                let sym = self.symbol[idx];
                idx += 1;
                if len <= FAST {
                    let rev = (code.reverse_bits() >> (32 - len)) as usize;
                    let entry = (sym << 4) | len as u16;
                    let mut i = rev;
                    while i < (1 << FAST) {
                        self.fast[i] = entry;
                        i += 1 << len;
                    }
                }
                code += 1;
            }
            code <<= 1;
        }
        left
    }
}

struct Inf<'a, 'b> {
    inp: &'a [u8],
    pos: usize,
    out: &'b mut [u8],
    opos: usize,
    bitbuf: u64,
    bitcnt: u32,
}

const LEN_BASE: [u16; 29] = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEN_EXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

impl<'a, 'b> Inf<'a, 'b> {
    #[inline]
    fn refill(&mut self) {
        while self.bitcnt <= 56 && self.pos < self.inp.len() {
            self.bitbuf |= (self.inp[self.pos] as u64) << self.bitcnt;
            self.pos += 1;
            self.bitcnt += 8;
        }
    }

    #[inline]
    fn bits(&mut self, need: u32) -> Option<u32> {
        if self.bitcnt < need {
            self.refill();
            if self.bitcnt < need {
                return None;
            }
        }
        let v = (self.bitbuf & ((1u64 << need) - 1)) as u32;
        self.bitbuf >>= need;
        self.bitcnt -= need;
        Some(v)
    }

    #[inline]
    fn decode(&mut self, h: &Huff) -> Option<usize> {
        if self.bitcnt < 15 {
            self.refill();
        }
        let e = h.fast[(self.bitbuf & FAST_MASK) as usize];
        if e != 0 {
            let len = (e & 15) as u32;
            if len > self.bitcnt {
                return None; // truncated input
            }
            self.bitbuf >>= len;
            self.bitcnt -= len;
            return Some((e >> 4) as usize);
        }
        // long code (or invalid): canonical bit-serial walk
        let (mut code, mut first, mut index) = (0i32, 0i32, 0i32);
        for len in 1..=15u32 {
            if len > self.bitcnt {
                return None;
            }
            code |= ((self.bitbuf >> (len - 1)) & 1) as i32;
            let count = h.count[len as usize] as i32;
            if code - count < first {
                self.bitbuf >>= len;
                self.bitcnt -= len;
                return Some(h.symbol[(index + (code - first)) as usize] as usize);
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        None
    }

    fn codes(&mut self, lencode: &Huff, distcode: &Huff) -> bool {
        loop {
            let sym = match self.decode(lencode) {
                Some(s) => s,
                None => return false,
            };
            if sym < 256 {
                if self.opos >= self.out.len() {
                    return false;
                }
                self.out[self.opos] = sym as u8;
                self.opos += 1;
            } else if sym == 256 {
                return true;
            } else {
                let sym = sym - 257;
                if sym >= 29 {
                    return false;
                }
                let e = match self.bits(LEN_EXTRA[sym] as u32) {
                    Some(e) => e,
                    None => return false,
                };
                let len = LEN_BASE[sym] as usize + e as usize;
                let ds = match self.decode(distcode) {
                    Some(d) if d < 30 => d,
                    _ => return false,
                };
                let e = match self.bits(DIST_EXTRA[ds] as u32) {
                    Some(e) => e,
                    None => return false,
                };
                let dist = DIST_BASE[ds] as usize + e as usize;
                if dist > self.opos || self.opos + len > self.out.len() {
                    return false;
                }
                let o = self.opos;
                if dist >= len {
                    self.out.copy_within(o - dist..o - dist + len, o);
                } else {
                    for i in 0..len {
                        self.out[o + i] = self.out[o - dist + i];
                    }
                }
                self.opos += len;
            }
        }
    }

    fn stored(&mut self) -> bool {
        // discard the partial byte, then give the whole bytes still in the bit buffer back to the input
        let drop = self.bitcnt % 8;
        self.bitbuf >>= drop;
        self.bitcnt -= drop;
        self.pos -= (self.bitcnt / 8) as usize;
        self.bitbuf = 0;
        self.bitcnt = 0;
        if self.pos + 4 > self.inp.len() {
            return false;
        }
        let len = self.inp[self.pos] as usize | (self.inp[self.pos + 1] as usize) << 8;
        let nlen = self.inp[self.pos + 2] as usize | (self.inp[self.pos + 3] as usize) << 8;
        self.pos += 4;
        if len != (!nlen & 0xFFFF) || self.pos + len > self.inp.len() || self.opos + len > self.out.len() {
            return false;
        }
        self.out[self.opos..self.opos + len].copy_from_slice(&self.inp[self.pos..self.pos + len]);
        self.opos += len;
        self.pos += len;
        true
    }

    fn fixed_block(&mut self) -> bool {
        let mut lc = Huff::new();
        let mut dc = Huff::new();
        let mut l = [0u8; 288];
        for (i, x) in l.iter_mut().enumerate() {
            *x = if i < 144 {
                8
            } else if i < 256 {
                9
            } else if i < 280 {
                7
            } else {
                8
            };
        }
        lc.construct(&l);
        let d = [5u8; 30];
        dc.construct(&d);
        self.codes(&lc, &dc)
    }

    fn dynamic_block(&mut self) -> bool {
        const ORDER: [u8; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
        let (nlen, ndist, ncode) = match (self.bits(5), self.bits(5), self.bits(4)) {
            (Some(a), Some(b), Some(c)) => (a as usize + 257, b as usize + 1, c as usize + 4),
            _ => return false,
        };
        if nlen > 286 || ndist > 30 {
            return false;
        }
        let mut lengths = [0u8; 320];
        for i in 0..ncode {
            match self.bits(3) {
                Some(b) => lengths[ORDER[i] as usize] = b as u8,
                None => return false,
            }
        }
        let mut lencode = Huff::new();
        if lencode.construct(&lengths[..19]) != 0 {
            return false;
        }
        let total = nlen + ndist;
        let mut i = 0;
        while i < total {
            let sym = match self.decode(&lencode) {
                Some(s) => s,
                None => return false,
            };
            if sym < 16 {
                lengths[i] = sym as u8;
                i += 1;
                continue;
            }
            let (len, rep) = if sym == 16 {
                if i == 0 {
                    return false;
                }
                match self.bits(2) {
                    Some(r) => (lengths[i - 1], r as usize + 3),
                    None => return false,
                }
            } else if sym == 17 {
                match self.bits(3) {
                    Some(r) => (0, r as usize + 3),
                    None => return false,
                }
            } else {
                match self.bits(7) {
                    Some(r) => (0, r as usize + 11),
                    None => return false,
                }
            };
            if i + rep > total {
                return false;
            }
            for _ in 0..rep {
                lengths[i] = len;
                i += 1;
            }
        }
        if lengths[256] == 0 {
            return false;
        }
        let mut lc = Huff::new();
        let mut dc = Huff::new();
        let err = lc.construct(&lengths[..nlen]);
        if err != 0 && (err < 0 || nlen != (lc.count[0] + lc.count[1]) as usize) {
            return false;
        }
        let err = dc.construct(&lengths[nlen..nlen + ndist]);
        if err != 0 && (err < 0 || ndist != (dc.count[0] + dc.count[1]) as usize) {
            return false;
        }
        self.codes(&lc, &dc)
    }
}

fn adler32(d: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for chunk in d.chunks(5552) {
        for &x in chunk {
            a += x as u32;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    (b << 16) | a
}

/// Inflates a zlib stream (RFC 1950, no preset dictionary) into `out`. Returns the decompressed size, or
/// `None` on corrupt data or output overflow. The Adler-32 trailer is verified only if `verify` is set.
pub fn zlib_inflate(input: &[u8], out: &mut [u8], verify: bool) -> Option<usize> {
    if input.len() < 6 {
        return None;
    }
    #[allow(clippy::manual_is_multiple_of)]
    let bad_check = (((input[0] as u32) << 8) | input[1] as u32) % 31 != 0;
    if (input[0] & 0x0F) != 8 || bad_check || (input[1] & 0x20) != 0 {
        return None;
    }
    let mut s = Inf { inp: input, pos: 2, out, opos: 0, bitbuf: 0, bitcnt: 0 };
    loop {
        let last = s.bits(1)?;
        let ty = s.bits(2)?;
        let ok = match ty {
            0 => s.stored(),
            1 => s.fixed_block(),
            2 => s.dynamic_block(),
            _ => false,
        };
        if !ok {
            return None;
        }
        if last != 0 {
            break;
        }
    }
    if verify {
        // after the last block, unused whole bytes in the bit buffer belong to the trailer
        let pos = s.pos - (s.bitcnt / 8) as usize;
        if pos + 4 > input.len() {
            return None;
        }
        let want = u32::from_be_bytes([input[pos], input[pos + 1], input[pos + 2], input[pos + 3]]);
        if want != adler32(&s.out[..s.opos]) {
            return None;
        }
    }
    Some(s.opos)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stored_block() {
        // zlib header + one stored block "abc" + adler32
        let z = [0x78, 0x01, 0x01, 3, 0, 0xFC, 0xFF, b'a', b'b', b'c', 0x02, 0x4D, 0x01, 0x27];
        let mut out = [0u8; 8];
        assert_eq!(zlib_inflate(&z, &mut out, true), Some(3));
        assert_eq!(&out[..3], b"abc");
    }

    #[test]
    fn rejects_garbage() {
        let mut out = [0u8; 16];
        assert_eq!(zlib_inflate(&[0; 3], &mut out, false), None);
        assert_eq!(zlib_inflate(&[0x78, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF], &mut out, false), None);
    }

    #[test]
    fn output_overflow_is_an_error() {
        let z = [0x78, 0x01, 0x01, 3, 0, 0xFC, 0xFF, b'a', b'b', b'c', 0x02, 0x4D, 0x01, 0x27];
        let mut out = [0u8; 2];
        assert_eq!(zlib_inflate(&z, &mut out, false), None);
    }
}
