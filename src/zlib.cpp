// Compact inflate (canonical-Huffman, bit-serial decode in the style of zlib's puff.c).
// Small and allocation-free; we never need to deflate (uncompressed frames are always legal).
#include <mcscon/zlib.h>

namespace mc {
namespace {

struct Huff { u16 count[16]; u16 symbol[288]; };

struct Inf {
    const u8* in; size_t inLen, inPos;
    u8* out; size_t outCap, outPos;
    u32 bitBuf; int bitCnt;
};

inline int bits(Inf& s, int need) {
    u32 v = s.bitBuf;
    while (s.bitCnt < need) {
        if (s.inPos >= s.inLen) return -1;
        v |= (u32)s.in[s.inPos++] << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = v >> need; s.bitCnt -= need;
    return (int)(v & ((1u << need) - 1));
}

int decode(Inf& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        int b = bits(s, 1); if (b < 0) return -1;
        code |= b;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

// returns 0 complete/ok; <0 error (over-subscribed); >0 incomplete
int construct(Huff& h, const u8* length, int n) {
    for (int i = 0; i <= 15; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[length[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) { left <<= 1; left -= h.count[len]; if (left < 0) return left; }
    u16 offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i) if (length[i]) h.symbol[offs[length[i]]++] = (u16)i;
    return left;
}

const u16 kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const u8  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const u16 kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const u8  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool codes(Inf& s, const Huff& lencode, const Huff& distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s.outPos >= s.outCap) return false;
            s.out[s.outPos++] = (u8)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257; if (sym >= 29) return false;
            int e = bits(s, kLenExtra[sym]); if (e < 0) return false;
            u32 len = kLenBase[sym] + e;
            int ds = decode(s, distcode); if (ds < 0 || ds >= 30) return false;
            e = bits(s, kDistExtra[ds]); if (e < 0) return false;
            u32 dist = kDistBase[ds] + e;
            if (dist > s.outPos || s.outPos + len > s.outCap) return false;
            u8* o = s.out + s.outPos; const u8* src = o - dist;
            for (u32 i = 0; i < len; ++i) o[i] = src[i];
            s.outPos += len;
        }
    }
}

bool stored(Inf& s) {
    s.bitBuf = 0; s.bitCnt = 0;
    if (s.inPos + 4 > s.inLen) return false;
    u32 len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    u32 nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFF) || s.inPos + len > s.inLen || s.outPos + len > s.outCap) return false;
    for (u32 i = 0; i < len; ++i) s.out[s.outPos++] = s.in[s.inPos++];
    return true;
}

bool fixedBlock(Inf& s) {
    Huff lc, dc; u8 l[288];
    int i = 0;
    for (; i < 144; ++i) l[i] = 8;
    for (; i < 256; ++i) l[i] = 9;
    for (; i < 280; ++i) l[i] = 7;
    for (; i < 288; ++i) l[i] = 8;
    construct(lc, l, 288);
    for (i = 0; i < 30; ++i) l[i] = 5;
    construct(dc, l, 30);
    return codes(s, lc, dc);
}

bool dynamicBlock(Inf& s) {
    static const u8 order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return false;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    u8 lengths[320]; int i;
    for (i = 0; i < 19; ++i) lengths[i] = 0;
    for (i = 0; i < ncode; ++i) { int b = bits(s, 3); if (b < 0) return false; lengths[order[i]] = (u8)b; }
    Huff lencode;
    if (construct(lencode, lengths, 19) != 0) return false;
    for (i = 0; i < nlen + ndist;) {
        int sym = decode(s, lencode); if (sym < 0) return false;
        if (sym < 16) { lengths[i++] = (u8)sym; continue; }
        int len = 0, rep;
        if (sym == 16) { if (i == 0) return false; len = lengths[i - 1]; rep = bits(s, 2); if (rep < 0) return false; rep += 3; }
        else if (sym == 17) { rep = bits(s, 3); if (rep < 0) return false; rep += 3; }
        else { rep = bits(s, 7); if (rep < 0) return false; rep += 11; }
        if (i + rep > nlen + ndist) return false;
        while (rep--) lengths[i++] = (u8)len;
    }
    if (lengths[256] == 0) return false;
    Huff lc, dc;
    int err = construct(lc, lengths, nlen);
    if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return false;
    err = construct(dc, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return false;
    return codes(s, lc, dc);
}

u32 adler32(const u8* d, size_t n) {
    u32 a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552; n -= k;
        while (k--) { a += *d++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

} // namespace

i32 zlibInflate(const u8* in, size_t inLen, u8* out, size_t outCap, bool verify) {
    if (inLen < 6) return -1;
    if ((in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return -1;
    Inf s; s.in = in; s.inLen = inLen; s.inPos = 2; s.out = out; s.outCap = outCap; s.outPos = 0;
    s.bitBuf = 0; s.bitCnt = 0;
    int last;
    do {
        last = bits(s, 1); int type = bits(s, 2);
        if (last < 0 || type < 0) return -1;
        bool ok = type == 0 ? stored(s) : type == 1 ? fixedBlock(s) : type == 2 ? dynamicBlock(s) : false;
        if (!ok) return -1;
    } while (!last);
    if (verify) {
        // after the last block, unused whole bytes in the bit buffer belong to the trailer
        size_t pos = s.inPos - (size_t)(s.bitCnt / 8);
        if (pos + 4 > inLen) return -1;
        u32 want = ((u32)in[pos] << 24) | (in[pos + 1] << 16) | (in[pos + 2] << 8) | in[pos + 3];
        if (want != adler32(out, s.outPos)) return -1;
    }
    return (i32)s.outPos;
}

} // namespace mc
