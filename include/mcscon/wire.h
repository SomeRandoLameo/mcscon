// Wire-level helper types used by the generated packet structs (packets_cb.h / packets_sb.h).
// Everything is zero-copy and allocation-free: variable-length data is exposed as views into the packet buffer.
#pragma once
#include "buffer.h"

namespace mc {

struct Uuid { u8 b[16]; };
struct Bytes { const u8* data; u32 len; };
inline bool streq(StrView s, const char* lit) { size_t n = strlen(lit); return s.len == n && memcmp(s.data, lit, n) == 0; }

// Scalar wrappers so arrays of primitives can reuse the generic Array/Span machinery.
template<class T> struct Num { T v; operator T() const { return v; } };
struct VarInt { i32 v; operator i32() const { return v; } };
struct VarLong { i64 v; operator i64() const { return v; } };

// ---- NBT: we only validate/skip and hand out the raw bytes (a parser lives in the optional nbt module)
inline bool skipNbtPayload(Reader& r, u8 tag, int depth) {
    if (depth > 512) { r.err = true; return false; }
    switch (tag) {
    case 1: r.skip(1); break; case 2: r.skip(2); break; case 3: case 5: r.skip(4); break; case 4: case 6: r.skip(8); break;
    case 7: { i32 n = r.i32_(); if (n < 0) { r.err = true; break; } r.skip((size_t)n); break; }
    case 8: { u16 n = r.u16_(); r.skip(n); break; }
    case 9: {
        u8 t = r.u8_(); i32 n = r.i32_();
        if (n < 0 || (t == 0 && n > 0)) { r.err = true; break; }
        if (t == 1) r.skip((size_t)n); else
        for (i32 i = 0; i < n && r.ok(); ++i) skipNbtPayload(r, t, depth + 1);
        break;
    }
    case 10: for (;;) { u8 t = r.u8_(); if (!r.ok() || t == 0) break; r.skip(r.u16_()); skipNbtPayload(r, t, depth + 1); if (!r.ok()) break; } break;
    case 11: { i32 n = r.i32_(); if (n < 0 || n > (1 << 28)) { r.err = true; break; } r.skip((size_t)n * 4); break; }
    case 12: { i32 n = r.i32_(); if (n < 0 || n > (1 << 28)) { r.err = true; break; } r.skip((size_t)n * 8); break; }
    default: r.err = true;
    }
    return r.ok();
}
// Reads a root NBT tag (named, as sent in 1.10-1.12.2). If `optional`, a lone TAG_End yields an empty view.
inline bool readNbt(Reader& r, Bytes& out, bool optional) {
    const u8* start = r.p; u8 tag = r.u8_();
    if (!r.ok()) return false;
    if (tag == 0) { if (!optional) { r.err = true; return false; } out.data = start; out.len = 0; return true; }
    r.skip(r.u16_());
    if (!skipNbtPayload(r, tag, 0)) return false;
    out.data = start; out.len = (u32)(r.p - start); return true;
}

// ---- Slot (item stack)
struct Slot { i16 id; i8 count; i16 damage; Bytes nbt; };   // id == -1: empty; nbt.len == 0: no tag
inline bool decodeSlot(Reader& r, Slot& s) {
    s.id = r.i16_(); s.count = 0; s.damage = 0; s.nbt.data = 0; s.nbt.len = 0;
    if (s.id == -1) return r.ok();
    s.count = r.i8_(); s.damage = r.i16_();
    return readNbt(r, s.nbt, true);
}
inline void encodeSlot(Writer& w, const Slot& s) {
    w.u16_((u16)s.id); if (s.id == -1) return;
    w.u8_((u8)s.count); w.u16_((u16)s.damage);
    if (s.nbt.len) w.bytes(s.nbt.data, s.nbt.len); else w.u8_(0);
}

// primitive element decoders
inline bool decodePrim(Reader& r, u8& v) { v = r.u8_(); return r.ok(); }
inline bool decodePrim(Reader& r, i8& v) { v = r.i8_(); return r.ok(); }
inline bool decodePrim(Reader& r, i16& v) { v = r.i16_(); return r.ok(); }
inline bool decodePrim(Reader& r, u16& v) { v = r.u16_(); return r.ok(); }
inline bool decodePrim(Reader& r, i32& v) { v = r.i32_(); return r.ok(); }
inline bool decodePrim(Reader& r, i64& v) { v = r.i64_(); return r.ok(); }
inline bool decodePrim(Reader& r, float& v) { v = r.f32_(); return r.ok(); }
inline bool decodePrim(Reader& r, double& v) { v = r.f64_(); return r.ok(); }

template<class T> inline bool decodeElem(Reader& r, Num<T>& o, Version, i32) { return decodePrim(r, o.v); }

// ---- Array views: validated once at decode time, then iterate lazily with next().
template<class T> struct Array {
    Reader r; u32 n; Version v; i32 ctx;
    Array() : r(), n(0), v(Version::V1_12_2), ctx(0) { r.err = false; }
    u32 size() const { return n; }
    bool next(T& out) { if (!n) return false; --n; return decodeElem(r, out, v, ctx); }
};
template<class T> struct Span { const T* data; u32 n; Span() : data(0), n(0) {} Span(const T* d, u32 c) : data(d), n(c) {} };

inline bool decodeElem(Reader& r, VarInt& o, Version, i32) { o.v = r.varint(); return r.ok(); }
inline bool decodeElem(Reader& r, VarLong& o, Version, i32) { o.v = r.varlong(); return r.ok(); }
inline bool decodeElem(Reader& r, StrView& o, Version, i32) { o = r.str(); return r.ok(); }
inline bool decodeElem(Reader& r, Slot& o, Version, i32) { return decodeSlot(r, o); }
inline bool decodeElem(Reader& r, Uuid& o, Version, i32) { const u8* p = r.bytes(16); if (p) memcpy(o.b, p, 16); return r.ok(); }
inline bool decodeElem(Reader& r, Bytes& o, Version, i32) { return readNbt(r, o, false); }   // arrays of raw bytes are always NBT in our protocols
template<class T> inline bool decodeElem(Reader& r, Array<T>& o, Version v, i32 ctx) {
    i32 n = r.varint(); if (!r.ok() || n < 0 || (size_t)n > r.left()) { r.err = true; return false; }
    o.r = r; o.r.err = false; o.n = (u32)n; o.v = v; o.ctx = ctx;
    T tmp; for (i32 i = 0; i < n; ++i) if (!decodeElem(r, tmp, v, ctx)) return false;
    o.r.end = r.p; return true;
}
// primitive element encoders
inline void encodeElem(Writer& w, const Num<u8>& o, Version, i32) { w.u8_(o.v); }
inline void encodeElem(Writer& w, const Num<i8>& o, Version, i32) { w.u8_((u8)o.v); }
inline void encodeElem(Writer& w, const Num<i16>& o, Version, i32) { w.u16_((u16)o.v); }
inline void encodeElem(Writer& w, const Num<u16>& o, Version, i32) { w.u16_(o.v); }
inline void encodeElem(Writer& w, const Num<i32>& o, Version, i32) { w.i32_(o.v); }
inline void encodeElem(Writer& w, const Num<i64>& o, Version, i32) { w.i64_(o.v); }
inline void encodeElem(Writer& w, const Num<float>& o, Version, i32) { w.f32_(o.v); }
inline void encodeElem(Writer& w, const Num<double>& o, Version, i32) { w.f64_(o.v); }
inline void encodeElem(Writer& w, const VarInt& o, Version, i32) { w.varint(o.v); }
inline void encodeElem(Writer& w, const VarLong& o, Version, i32) { w.varlong(o.v); }
inline void encodeElem(Writer& w, const StrView& o, Version, i32) { w.str(o); }
inline void encodeElem(Writer& w, const Slot& o, Version, i32) { encodeSlot(w, o); }
inline void encodeElem(Writer& w, const Uuid& o, Version, i32) { w.bytes(o.b, 16); }
inline void encodeElem(Writer& w, const Bytes& o, Version, i32) { w.bytes(o.data, o.len); }
template<class T> inline void encodeElem(Writer& w, const Span<T>& o, Version v, i32 ctx) {
    w.varint((i32)o.n); for (u32 i = 0; i < o.n; ++i) encodeElem(w, o.data[i], v, ctx);
}

// ---- Entity metadata: iterate entries until the 0xFF terminator.
struct MetaEntry {
    u8 key, type;          // type: 0 byte,1 varint,2 float,3 string,4 chat,5 slot,6 bool,7 rotation,8 position,9 optPosition,10 direction,11 optUuid,12 blockId,13 nbt(1.12+)
    i32 i;                 // byte/varint/bool/direction/blockId
    float f[3];            // float / rotation
    StrView s;             // string/chat
    Slot slot; BlockPos pos; bool present; Uuid uuid; Bytes nbt;
};
struct MetaView {
    Reader r; Version v;
    MetaView() : r(), v(Version::V1_12_2) { r.err = false; }
    // false at the end marker (or on a malformed stream, already ruled out by decode-time validation)
    bool next(MetaEntry& e) {
        if (r.left() == 0) return false;
        u8 k = r.u8_(); if (k == 0xFF) return false;
        e.key = k; e.type = r.u8_(); e.i = 0; e.f[0] = e.f[1] = e.f[2] = 0; e.s.data = 0; e.s.len = 0; e.present = true;
        return metaValue(r, e, v);
    }
    static bool metaValue(Reader& r, MetaEntry& e, Version v) {
        switch (e.type) {
        case 0: e.i = r.i8_(); break;
        case 1: case 10: case 12: e.i = r.varint(); break;
        case 2: e.f[0] = r.f32_(); break;
        case 3: case 4: e.s = r.str(); break;
        case 5: decodeSlot(r, e.slot); break;
        case 6: e.i = r.bool_(); break;
        case 7: e.f[0] = r.f32_(); e.f[1] = r.f32_(); e.f[2] = r.f32_(); break;
        case 8: e.pos = unpackPos(r.u64_()); break;
        case 9: e.present = r.bool_(); if (e.present) e.pos = unpackPos(r.u64_()); break;
        case 11: e.present = r.bool_(); if (e.present) { const u8* p = r.bytes(16); if (p) memcpy(e.uuid.b, p, 16); } break;
        case 13: if (v < Version::V1_12) { r.err = true; break; } readNbt(r, e.nbt, false); break;
        default: r.err = true;
        }
        return r.ok();
    }
};
inline bool decodeMeta(Reader& r, MetaView& m, Version v) {
    m.r = r; m.r.err = false; m.v = v;
    for (;;) {
        u8 k = r.u8_(); if (!r.ok()) return false;
        if (k == 0xFF) break;
        MetaEntry e; e.type = r.u8_(); e.present = true; e.i = 0; e.s.data = 0; e.s.len = 0;
        if (!MetaView::metaValue(r, e, v)) return false;
    }
    m.r.end = r.p; return true;
}

} // namespace mc
