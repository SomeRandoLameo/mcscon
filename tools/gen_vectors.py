#!/usr/bin/env python3
"""Independent reference encoder: walks the raw minecraft-data JSON, builds random *valid* packets and writes
tests/packet_vectors.h. The C++ decoders must consume every byte of them (checked in tests/test_packets.cpp).
usage: gen_vectors.py <data/pc> > tests/packet_vectors.h"""
import json, random, struct, sys
ROOT = sys.argv[1]
VERS = [("V1_10", "1.10", None), ("V1_11", "1.11", None), ("V1_11_2", "1.11", None), ("V1_12", "1.12", None), ("V1_12_1", "1.12.2", "1.12"), ("V1_12_2", "1.12.2", None)]
rnd = random.Random(42)
def camel(n): return "".join(p[:1].upper() + p[1:] for p in n.split("_"))
def varint(v):
    v &= 0xFFFFFFFF; o = bytearray()
    while v >= 0x80: o.append((v & 0x7F) | 0x80); v >>= 7
    o.append(v); return bytes(o)
def varlong(v):
    v &= (1 << 64) - 1; o = bytearray()
    while v >= 0x80: o.append((v & 0x7F) | 0x80); v >>= 7
    o.append(v); return bytes(o)
def rstr(): return varint(0) if rnd.random() < .2 else (lambda s: varint(len(s)) + s)(bytes(rnd.randrange(97, 123) for _ in range(rnd.randrange(1, 12))))
def nbt_payload(tag, depth=0):
    if tag == 1: return bytes([rnd.randrange(256)])
    if tag == 2: return struct.pack(">h", rnd.randrange(-100, 100))
    if tag == 3: return struct.pack(">i", rnd.randrange(-1000, 1000))
    if tag == 4: return struct.pack(">q", rnd.randrange(-10**9, 10**9))
    if tag == 5: return struct.pack(">f", 1.5)
    if tag == 6: return struct.pack(">d", 2.5)
    if tag == 7: n = rnd.randrange(0, 5); return struct.pack(">i", n) + bytes(n)
    if tag == 8: s = b"abc"[:rnd.randrange(0, 4)]; return struct.pack(">H", len(s)) + s
    if tag == 9:
        t = rnd.choice([1, 3, 8, 10]) if depth < 3 else 1; n = rnd.randrange(0, 3)
        return bytes([t]) + struct.pack(">i", n) + b"".join(nbt_payload(t, depth + 1) for _ in range(n))
    if tag == 10:
        o = b""
        for _ in range(rnd.randrange(0, 4)):
            t = rnd.choice([1, 2, 3, 4, 5, 6, 7, 8, 9, 10] if depth < 3 else [1, 3, 8])
            o += bytes([t]) + struct.pack(">H", 2) + b"kk" + nbt_payload(t, depth + 1)
        return o + b"\x00"
    if tag == 11: n = rnd.randrange(0, 4); return struct.pack(">i", n) + bytes(4 * n)
    if tag == 12: n = rnd.randrange(0, 4); return struct.pack(">i", n) + bytes(8 * n)
def nbt(): return b"\x0a" + struct.pack(">H", 0) + nbt_payload(10)

PRIM = {"i8": ">b", "u8": ">B", "i16": ">h", "u16": ">H", "i32": ">i", "u32": ">I", "i64": ">q", "u64": ">Q", "f32": ">f", "f64": ">d", "bool": ">?"}
def rprim(t):
    if t in ("f32", "f64"): return rnd.choice([0.0, 1.5, -2.25, 100.0])
    if t == "bool": return rnd.random() < .5
    if t == "i8": return rnd.randrange(-128, 128)
    if t == "u8": return rnd.randrange(0, 256)
    if t == "i16": return rnd.randrange(-300, 300)
    if t == "u16": return rnd.randrange(0, 300)
    if t == "i32": return rnd.randrange(-300, 300)
    if t == "i64": return rnd.randrange(-300, 300)
    if t == "varint": return rnd.randrange(0, 40)
    if t == "varlong": return rnd.randrange(0, 40)
    return rnd.randrange(0, 100)

class Ctx:
    def __init__(self, parent=None): self.v = {}; self.parent = parent

class G:
    def __init__(self, types, ver): self.types = types; self.ver = ver
    def lookup(self, ctx, path):
        if path.startswith("../"): return self.lookup(ctx.parent, path[3:])
        if "/" in path:
            a, b = path.split("/"); return ctx.v[a + "/" + b]
        return ctx.v[path]
    def gen(self, t, ctx, name=None):
        if isinstance(t, str):
            if t in PRIM: v = rprim(t); ctx.v[name] = v; return struct.pack(PRIM[t], v)
            if t == "varint": v = rprim(t); ctx.v[name] = v; return varint(v)
            if t == "varlong": v = rprim(t); ctx.v[name] = v; return varlong(v)
            if t == "void": return b""
            if t in ("string", "pstring"):
                s = rstr(); ctx.v[name] = s; return s
            if t == "UUID": return bytes(rnd.randrange(256) for _ in range(16))
            if t == "position": return bytes(rnd.randrange(256) for _ in range(8))
            if t == "nbt": return nbt()
            if t == "optionalNbt": return b"\x00" if rnd.random() < .5 else nbt()
            if t == "restBuffer": return bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 20)))
            if t == "ByteArray": n = rnd.randrange(0, 20); return varint(n) + bytes(n)
            if t == "slot":
                if rnd.random() < .3: return struct.pack(">h", -1)
                return struct.pack(">hbh", rnd.randrange(1, 400), rnd.randrange(1, 64), rnd.randrange(0, 15)) + (b"\x00" if rnd.random() < .5 else nbt())
            if t == "entityMetadata": return self.meta()
            return self.gen(self.types[t], ctx, name)
        k = t[0]
        if k == "container":
            c = Ctx(ctx); out = b""
            for f in t[1]:
                if f.get("anon"):
                    out += self.gen(f["type"], c, None)
                else: out += self.gen(f["type"], c, f["name"])
            # anon members flatten into scope; expose to parent via name lookups in c; if named, store
            if name is not None: ctx.v[name] = c
            else: ctx.v.update(c.v)
            return out
        if k == "array":
            o = t[1]; n = o["count"] if "count" in o else rnd.randrange(0, 4)
            head = b"" if "count" in o else self.countbytes(o["countType"], n)
            return head + b"".join(self.gen(o["type"], self.view(ctx)) for _ in range(n))
        if k == "option":
            if rnd.random() < .5: return b"\x00"
            return b"\x01" + self.gen(t[1], ctx, None)
        if k == "buffer":
            n = rnd.randrange(0, 40); return self.countbytes(t[1]["countType"], n) + bytes(rnd.randrange(256) for _ in range(n))
        if k == "mapper":
            key = rnd.choice(list(t[1]["mappings"].keys())); iv = int(key, 0); ctx.v[name] = t[1]["mappings"][key]
            ctx.v[name + "#n"] = iv
            return self.gen(t[1]["type"], Ctx(), None)[:0] + self.enc_int(t[1]["type"], iv)
        if k == "bitfield":
            val = 0; out = 0
            for f in t[1]:
                x = rnd.randrange(0, 1 << f["size"]) if f["size"] < 8 else 0
                ctx.v[name + "/" + f["name"]] = x; out = (out << f["size"]) | x
            return struct.pack(">I", out)
        if k == "switch":
            o = t[1]; key = o["compareTo"]; val = self.lookup(ctx, key)
            if isinstance(val, Ctx): raise Exception
            cands = [str(val)]
            # numeric keys may be given as hex/decimal, mapper values by name
            br = o["fields"]; chosen = None
            for kk, tt in br.items():
                try: kn = int(kk, 0)
                except ValueError: kn = None
                if kk == str(val) or (kn is not None and isinstance(val, int) and kn == val) or (kn is not None and (key + "#n") in self.curnums(ctx, key) and kn == self.curnums(ctx, key)[key + "#n"]):
                    chosen = tt; break
            if chosen is None: chosen = o.get("default", "void")
            return self.gen(chosen, ctx, name)
        raise Exception("gen " + k)
    def view(self, ctx):
        c = Ctx(ctx.parent); c.v = dict(ctx.v); return c
    def curnums(self, ctx, key):
        while key.startswith("../"): ctx = ctx.parent; key = key[3:]
        return ctx.v
    def enc_int(self, t, v):
        if t == "varint": return varint(v)
        return struct.pack(PRIM[t], v)
    def countbytes(self, ct, n): return varint(n) if ct == "varint" else struct.pack(PRIM[ct], n)
    def meta(self):
        out = b""; types = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12] + ([13] if self.ver in ("1.12", "1.12.2") else [])
        for i in range(rnd.randrange(0, 5)):
            ty = rnd.choice(types); out += bytes([i, ty])
            if ty == 0: out += bytes([rnd.randrange(256)])
            elif ty in (1, 10, 12): out += varint(rnd.randrange(0, 300))
            elif ty == 2: out += struct.pack(">f", 1.0)
            elif ty in (3, 4): out += rstr()
            elif ty == 5: out += self.gen("slot", Ctx())
            elif ty == 6: out += bytes([rnd.randrange(2)])
            elif ty == 7: out += struct.pack(">fff", 1, 2, 3)
            elif ty == 8: out += bytes(8)
            elif ty == 9: out += (b"\x01" + bytes(8)) if rnd.random() < .5 else b"\x00"
            elif ty == 11: out += (b"\x01" + bytes(16)) if rnd.random() < .5 else b"\x00"
            elif ty == 13: out += nbt()
        return out + b"\xff"

def packets(d, side, kaFrom):
    p = json.load(open(f"{ROOT}/{d}/protocol.json")); pk = dict(p["play"][side]["types"])
    if kaFrom: pk["packet_keep_alive"] = json.load(open(f"{ROOT}/{kaFrom}/protocol.json"))["play"][side]["types"]["packet_keep_alive"]
    return {n: t for n, t in pk.items() if n.startswith("packet_") and n != "packet"}, p["types"]

SAMPLES = 24
out = ["// GENERATED by tools/gen_vectors.py - random valid clientbound packets from an independent reference encoder",
       "#pragma once", "#include <mcscon/packets_cb.h>", "#include <string.h>",
       "struct PVec { int ver; const char* name; const unsigned char* d; unsigned len; };"]
rows = []; names = set(); n = 0
for vi, (vl, d, ka) in enumerate(VERS):
    pk, types = packets(d, "toClient", ka)
    g = G(types, d if not ka else "1.12.2")
    for pn, t in sorted(pk.items()):
        cn = camel(pn[7:]); names.add(cn)
        for s in range(SAMPLES):
            ctx = Ctx(); b = g.gen(t, ctx, None)
            out.append("static const unsigned char v%d[] = {%s};" % (n, ",".join(map(str, b)) if b else "0"))
            rows.append("{%d,\"%s\",v%d,%d}" % (vi, cn, n, len(b))); n += 1
out.append("static const PVec pvecs[] = {%s};" % ",".join(rows))
out.append("static bool decodeByName(const char* name, mc::Reader& r, mc::Version v) {")
for cn in sorted(names):
    out.append('    if (!strcmp(name, "%s")) { mc::cb::%s o; return mc::cb::decode(r, o, v); }' % (cn, cn))
out.append("    return false;\n}")
print("\n".join(out))
