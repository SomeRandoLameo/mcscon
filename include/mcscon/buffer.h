#pragma once
#include "types.h"
#include <string.h>

namespace mc {

struct StrView { const char* data; u32 len; };   // not NUL-terminated, points into packet buffer

// Bounds-checked big-endian reader. Any overrun sets `err`; reads then return 0.
struct Reader {
    const u8* p; const u8* end; bool err;
    Reader() : p(0), end(0), err(true) {}
    Reader(const u8* b, size_t n) : p(b), end(b + n), err(false) {}
    size_t left() const { return (size_t)(end - p); }
    bool ok() const { return !err; }
    bool need(size_t n) { if (err || left() < n) { err = true; return false; } return true; }

    u8  u8_()  { return need(1) ? *p++ : 0; }
    i8  i8_()  { return (i8)u8_(); }
    bool bool_() { return u8_() != 0; }
    u16 u16_() { u16 v = (u16)(u8_() << 8); return v | u8_(); }
    i16 i16_() { return (i16)u16_(); }
    u32 u32_() { u32 v = (u32)u16_() << 16; return v | u16_(); }
    i32 i32_() { return (i32)u32_(); }
    u64 u64_() { u64 v = (u64)u32_() << 32; return v | u32_(); }
    i64 i64_() { return (i64)u64_(); }
    float f32_() { u32 v = u32_(); float f; memcpy(&f, &v, 4); return f; }
    double f64_() { u64 v = u64_(); double d; memcpy(&d, &v, 8); return d; }

    i32 varint() {
        u32 r = 0;
        for (int s = 0; s < 35; s += 7) {
            u8 b = u8_(); if (err) return 0;
            r |= (u32)(b & 0x7F) << s;
            if (!(b & 0x80)) return (i32)r;
        }
        err = true; return 0;
    }
    i64 varlong() {
        u64 r = 0;
        for (int s = 0; s < 70; s += 7) {
            u8 b = u8_(); if (err) return 0;
            r |= (u64)(b & 0x7F) << s;
            if (!(b & 0x80)) return (i64)r;
        }
        err = true; return 0;
    }
    StrView str(u32 maxLen = 32767 * 4) {
        i32 n = varint();
        StrView s = {0, 0};
        if (err || n < 0 || (u32)n > maxLen || !need((size_t)n)) { err = true; return s; }
        s.data = (const char*)p; s.len = (u32)n; p += n; return s;
    }
    const u8* bytes(size_t n) { if (!need(n)) return 0; const u8* r = p; p += n; return r; }
    void skip(size_t n) { if (need(n)) p += n; }
};

// 1.10-1.12.2 block position: x:26 | y:12 | z:26
struct BlockPos { i32 x, y, z; };
inline BlockPos unpackPos(u64 v) {
    i64 sv = (i64)v; BlockPos b;
    b.x = (i32)(sv >> 38); b.y = (i32)((v >> 26) & 0xFFF); b.z = (i32)((i64)(v << 38) >> 38);
    return b;
}
inline u64 packPos(i32 x, i32 y, i32 z) {
    return ((u64)(x & 0x3FFFFFF) << 38) | ((u64)(y & 0xFFF) << 26) | (u64)(z & 0x3FFFFFF);
}

// Bounds-checked big-endian writer over caller memory. Overrun sets `err`.
struct Writer {
    u8* p; u8* end; bool err;
    Writer() : p(0), end(0), err(true) {}
    Writer(u8* b, size_t n) : p(b), end(b + n), err(false) {}
    size_t left() const { return (size_t)(end - p); }
    bool need(size_t n) { if (err || left() < n) { err = true; return false; } return true; }

    void u8_(u8 v) { if (need(1)) *p++ = v; }
    void bool_(bool v) { u8_(v ? 1 : 0); }
    void u16_(u16 v) { u8_((u8)(v >> 8)); u8_((u8)v); }
    void u32_(u32 v) { u16_((u16)(v >> 16)); u16_((u16)v); }
    void u64_(u64 v) { u32_((u32)(v >> 32)); u32_((u32)v); }
    void i32_(i32 v) { u32_((u32)v); }
    void i64_(i64 v) { u64_((u64)v); }
    void f32_(float f) { u32 v; memcpy(&v, &f, 4); u32_(v); }
    void f64_(double d) { u64 v; memcpy(&v, &d, 8); u64_(v); }
    void varint(i32 v) {
        u32 u = (u32)v;
        while (u >= 0x80) { u8_((u8)(u | 0x80)); u >>= 7; }
        u8_((u8)u);
    }
    void varlong(i64 v) {
        u64 u = (u64)v;
        while (u >= 0x80) { u8_((u8)(u | 0x80)); u >>= 7; }
        u8_((u8)u);
    }
    void bytes(const void* d, size_t n) { if (need(n)) { memcpy(p, d, n); p += n; } }
    void str(const char* s, size_t n) { varint((i32)n); bytes(s, n); }
    void str(const char* s) { str(s, strlen(s)); }
    void str(StrView s) { str(s.data, s.len); }
};

inline size_t varintSize(u32 v) { size_t n = 1; while (v >= 0x80) { v >>= 7; ++n; } return n; }

} // namespace mc
