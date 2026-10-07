// Zero-copy NBT reader over the raw bytes handed out by the packet decoders (Slot::nbt, tile entities, ...).
// The bytes were already validated when the packet was decoded, so iteration cannot run out of bounds.
#pragma once
#include "wire.h"

namespace mc {
enum NbtType : u8 { NbtEnd, NbtByte, NbtShort, NbtInt, NbtLong, NbtFloat, NbtDouble, NbtByteArray, NbtString, NbtList_, NbtCompound_, NbtIntArray, NbtLongArray };

struct NbtEntry; struct NbtCompound; struct NbtList;
struct NbtCompound { Reader r; bool next(NbtEntry& e); bool find(const char* name, NbtEntry& e) const; };
struct NbtList { Reader r; u8 elemType; i32 count; bool next(NbtEntry& e); };
struct NbtEntry {
    u8 type; StrView name; Reader value;      // value is positioned at the payload
    bool isNumber() const { return type >= NbtByte && type <= NbtDouble; }
    i64 asInt() const { Reader c = value; switch (type) { case NbtByte: return c.i8_(); case NbtShort: return c.i16_(); case NbtInt: return c.i32_(); case NbtLong: return c.i64_(); case NbtFloat: return (i64)c.f32_(); case NbtDouble: return (i64)c.f64_(); } return 0; }
    double asFloat() const { Reader c = value; switch (type) { case NbtFloat: return c.f32_(); case NbtDouble: return c.f64_(); } return (double)asInt(); }
    StrView asString() const { Reader c = value; StrView s = {0, 0}; if (type == NbtString) { u16 n = c.u16_(); s.data = (const char*)c.bytes(n); s.len = s.data ? n : 0; } return s; }
    NbtCompound asCompound() const { NbtCompound c; c.r = value; c.r.err = false; return c; }
    NbtList asList() const { NbtList l; l.r = value; l.r.err = false; l.elemType = l.r.u8_(); l.count = l.r.i32_(); return l; }
    Bytes asBytes() const { Reader c = value; Bytes b = {0, 0}; if (type == NbtByteArray) { i32 n = c.i32_(); b.data = c.bytes((size_t)n); b.len = b.data ? (u32)n : 0; } return b; }
    i32 arrayLength() const { Reader c = value; return (type == NbtByteArray || type == NbtIntArray || type == NbtLongArray) ? c.i32_() : 0; }
};
inline bool NbtCompound::next(NbtEntry& e) {
    if (r.err || r.left() == 0) return false;
    u8 t = r.u8_(); if (t == 0 || r.err) return false;
    e.type = t; u16 n = r.u16_(); e.name.data = (const char*)r.bytes(n); e.name.len = n;
    e.value = r; e.value.err = false;
    if (!skipNbtPayload(r, t, 0)) return false;
    e.value.end = r.p;
    return true;
}
inline bool NbtCompound::find(const char* name, NbtEntry& e) const {
    NbtCompound c = *this;
    while (c.next(e)) if (streq(e.name, name)) return true;
    return false;
}
inline bool NbtList::next(NbtEntry& e) {
    if (count <= 0 || r.err) return false;
    --count; e.type = elemType; e.name.data = 0; e.name.len = 0; e.value = r; e.value.err = false;
    if (!skipNbtPayload(r, elemType, 0)) return false;
    e.value.end = r.p; return true;
}
// Root tag of a Bytes blob (as found in Slot::nbt).
inline bool nbtRoot(Bytes b, NbtCompound& out, StrView* name = 0) {
    if (!b.len) return false;
    Reader r(b.data, b.len); u8 t = r.u8_(); if (t != NbtCompound_) return false;
    u16 n = r.u16_(); const u8* nm = r.bytes(n); if (name) { name->data = (const char*)nm; name->len = n; }
    out.r = r; out.r.err = false; return r.ok();
}
// Look up "a/b/c" through nested compounds.
inline bool nbtPath(Bytes b, const char* path, NbtEntry& out) {
    NbtCompound c; if (!nbtRoot(b, c)) return false;
    for (;;) {
        const char* slash = strchr(path, '/'); size_t n = slash ? (size_t)(slash - path) : strlen(path);
        NbtEntry e; bool found = false; NbtCompound it = c;
        while (it.next(e)) if (e.name.len == n && memcmp(e.name.data, path, n) == 0) { found = true; break; }
        if (!found) return false;
        if (!slash) { out = e; return true; }
        if (e.type != NbtCompound_) return false;
        c = e.asCompound(); path = slash + 1;
    }
}
} // namespace mc
