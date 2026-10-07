#!/usr/bin/env python3
"""Generates include/mcscon/packets_cb.h (clientbound decoders) and packets_sb.h (serverbound encoders)
for the Play state from PrismarineJS/minecraft-data protocol.json.   usage: gen_packets.py <data/pc> <outdir>
Layout differences between versions are merged into one struct; decode/encode select the layout at runtime by Version."""
import json, sys, copy

ROOT, OUT = sys.argv[1], sys.argv[2]
# Version enum order in types.h: V1_10, V1_11, V1_11_2, V1_12, V1_12_1, V1_12_2
VERS = [("1.10", None), ("1.11", None), ("1.11", None), ("1.12", None), ("1.12.2", "1.12"), ("1.12.2", None)]
KEYWORDS = set("alignas alignof and asm auto bool break case catch char class const continue default delete do double else enum explicit export extern false float for friend goto if inline int long mutable namespace new not operator or private protected public register return short signed sizeof static struct switch template this throw true try typedef typeid typename union unsigned using virtual void volatile while xor".split())
PRIMS = {"i8": "i8", "u8": "u8", "i16": "i16", "u16": "u16", "i32": "i32", "u32": "u32", "i64": "i64", "u64": "u64",
         "f32": "float", "f64": "double", "bool": "bool", "varint": "i32", "varlong": "i64"}
RW = {"i8": ("i8_", "u8_((u8)%s)"), "u8": ("u8_", "u8_(%s)"), "i16": ("i16_", "u16_((u16)%s)"), "u16": ("u16_", "u16_(%s)"),
      "i32": ("i32_", "i32_(%s)"), "u32": ("u32_", "u32_(%s)"), "i64": ("i64_", "i64_(%s)"), "u64": ("u64_", "u64_(%s)"),
      "f32": ("f32_", "f32_(%s)"), "f64": ("f64_", "f64_(%s)"), "bool": ("bool_", "bool_(%s)"),
      "varint": ("varint", "varint(%s)"), "varlong": ("varlong", "varlong(%s)")}
RANK = {"bool": 0, "i8": 1, "u8": 2, "i16": 3, "u16": 4, "i32": 5, "u32": 6, "i64": 7, "u64": 8, "float": 9, "double": 10}

def camel(n): return "".join(p[:1].upper() + p[1:] for p in n.split("_"))
def ident(n): return n + "_" if n in KEYWORDS else n

# ------------------------------------------------------------------ type normalisation
def norm(t, types):
    if isinstance(t, str):
        if t in PRIMS: return ("prim", t)
        if t == "void": return ("void",)
        if t in ("string", "pstring"): return ("str",)
        if t == "UUID": return ("uuid",)
        if t == "slot": return ("slot",)
        if t == "position": return ("pos",)
        if t == "nbt": return ("nbt", False)
        if t == "optionalNbt": return ("nbt", True)
        if t == "restBuffer": return ("buf", None)
        if t == "ByteArray": return ("buf", "varint")
        if t == "entityMetadata": return ("meta",)
        if t in types and types[t] != "native": return norm(types[t], types)
        raise Exception("unknown type " + t)
    k = t[0]
    if k == "container": return ("container", [(f.get("name"), norm(f["type"], types), bool(f.get("anon"))) for f in t[1]])
    if k == "array":
        o = t[1]; e = norm(o["type"], types)
        return ("array", o.get("countType"), o.get("count"), e)
    if k == "option": return ("option", norm(t[1], types))
    if k == "mapper":
        inv = {}
        for key, name in t[1]["mappings"].items(): inv[name] = int(key, 0)
        return ("mapper", t[1]["type"], inv)
    if k == "buffer": return ("buf", t[1]["countType"])
    if k == "pstring": return ("str",)
    if k == "bitfield": return ("bits", t[1])
    if k == "switch":
        o = t[1]
        return ("switch", o["compareTo"], {key: norm(v, types) for key, v in o["fields"].items()}, norm(o["default"], types) if "default" in o else None)
    raise Exception("unsupported " + k)

def load(side):
    """returns {packet: [json per Version index]}"""
    out = {}
    per = []
    for d, kaFrom in VERS:
        p = json.load(open(f"{ROOT}/{d}/protocol.json"))
        pk = p["play"][side]["types"]; per.append((pk, p["types"]))
        if kaFrom:   # 1.12.1: 1.12.2 layout, but 1.12 keep-alive
            old = json.load(open(f"{ROOT}/{kaFrom}/protocol.json"))["play"][side]["types"]
            pk = dict(pk); pk["packet_keep_alive"] = old["packet_keep_alive"]; per[-1] = (pk, p["types"])
    names = set()
    for pk, _ in per: names |= {k for k in pk if k.startswith("packet_") and k != "packet"}
    for n in sorted(names):
        out[n] = [(pk.get(n), types) for pk, types in per]
    return out

# ------------------------------------------------------------------ generator state
class Gen:
    def __init__(self, mode):
        self.mode = mode            # 'dec' or 'enc'
        self.structs = []           # (name, [(member, ctype)]) in dependency order
        self.funcs = []             # code text of nested functions
        self.sig = {}               # nested path -> json signature (must agree across versions)
        self.lines = None

    def arr_t(self, e): return ("Array<%s>" if self.mode == "dec" else "Span<%s>") % e

class Scope:
    def __init__(self, up=None):
        self.f = {}                 # name -> (expr, kind, extra)
        self.up = up                # (name->info) of enclosing container for '../'

def ctype_of(g, node, path, S):
    k = node[0]
    if k == "prim": return PRIMS[node[1]]
    if k == "str": return "StrView"
    if k == "uuid": return "Uuid"
    if k == "slot": return "Slot"
    if k == "pos": return "BlockPos"
    if k == "nbt": return "Bytes"
    if k == "buf": return "Bytes"
    if k == "meta": return "MetaView"
    if k == "mapper": return PRIMS[node[1]]
    if k == "bits": return "u32"
    if k == "container": return path
    if k == "array": return g.arr_t(elem_t(g, node[3], path))
    if k == "option": return "Opt<%s>" % ctype_of(g, node[1], path + "_v", S)
    raise Exception("ctype " + k)

def elem_t(g, e, path):
    k = e[0]
    if k == "prim":
        return {"varint": "VarInt", "varlong": "VarLong"}.get(e[1]) or "Num<%s>" % PRIMS[e[1]]
    if k == "array":
        if e[1] != "varint" or e[2] is not None: raise Exception("nested array count")
        return g.arr_t(elem_t(g, e[3], path + "_e"))
    if k == "option": raise Exception("option elem")
    return ctype_of(g, e, path, None)

def merge_member(S, name, ct, ver_merge):
    for i, (n, t) in enumerate(S):
        if n == name:
            if t == ct: return
            if t in RANK and ct in RANK and ver_merge:
                if RANK[ct] > RANK[t]: S[i] = (n, ct)
                return
            raise Exception("conflicting member %s: %s vs %s" % (name, t, ct))
    S.append((name, ct))

# field reference resolution for switch compareTo
def resolve(sc, cmp):
    if cmp.startswith("../"):
        n = cmp[3:]; info = sc.up[n] if sc.up and n in sc.up else None
        if info is None: raise Exception("unresolved " + cmp)
        return ("ctx", info)
    if "/" in cmp:
        a, b = cmp.split("/"); expr, kind, extra = sc.f[a]
        assert kind == "bits"
        shift = 0; size = 0; found = False
        for fld in reversed(extra):
            if fld["name"] == b: size = fld["size"]; found = True; break
            shift += fld["size"]
        assert found
        return ("bits", expr, shift, (1 << size) - 1)
    if cmp not in sc.f: raise Exception("unresolved compareTo " + cmp + " in " + str(list(sc.f)))
    return ("field",) + sc.f[cmp]

def cond_for(ref, keys):
    parts = []
    for key in keys:
        if ref[0] == "bits": parts.append("(((%s) >> %d) & %d) == %s" % (ref[1], ref[2], ref[3], key))
        else:
            if ref[0] == "ctx": expr, kind, extra = ref[1]
            else: _, expr, kind, extra = ref
            if kind == "str": parts.append('streq(%s, "%s")' % (expr, key))
            elif kind == "mapper" and key in extra: parts.append("%s == %d" % (expr, extra[key]))
            else: parts.append("%s == %d" % (expr, int(key, 0)))
    return " || ".join(parts) if parts else "false"

# ------------------------------------------------------------------ code emission
def ind(n): return "    " * n

def gen_container(g, fields, path, sc, S, L, lvl, vm, top):
    """emit code for container fields into L, registering members in S; sc is the (flat) scope."""
    for name, node, anon in fields:
        if anon:
            # flatten: node is a switch of containers, or a container
            if node[0] == "container": gen_container(g, node[1], path, sc, S, L, lvl, vm, top)
            elif node[0] == "switch": gen_switch(g, None, node, path, sc, S, L, lvl, vm, top)
            else: raise Exception("anon " + node[0])
        else:
            gen_value(g, name, node, path, sc, S, L, lvl, vm, top)

def gen_switch(g, name, node, path, sc, S, L, lvl, vm, top):
    _, cmp, branches, default = node
    ref = resolve(sc, cmp)
    groups = {}   # json(branch) -> (node, keys)
    for key, b in branches.items():
        sig = json.dumps(b, sort_keys=True, default=str)
        groups.setdefault(sig, [b, []])[1].append(key)
    allkeys = [k for _, ks in groups.values() for k in ks]
    first = True
    for b, keys in groups.values():
        if b == ("void",): continue
        L.append(ind(lvl) + "if (%s) {" % cond_for(ref, keys))
        emit_branch(g, name, b, path, sc, S, L, lvl + 1, vm, top)
        L.append(ind(lvl) + "}")
    if default is not None and default != ("void",):
        L.append(ind(lvl) + "if (!(%s)) {" % cond_for(ref, allkeys))
        emit_branch(g, name, default, path, sc, S, L, lvl + 1, vm, top)
        L.append(ind(lvl) + "}")

def emit_branch(g, name, b, path, sc, S, L, lvl, vm, top):
    if name is None:   # anon: flatten
        if b[0] == "container": gen_container(g, b[1], path, sc, S, L, lvl, vm, top)
        else: raise Exception("anon branch " + b[0])
    else:
        gen_value(g, name, b, path, sc, S, L, lvl, vm, top, cond_member=True)

def gen_value(g, name, node, path, sc, S, L, lvl, vm, top, cond_member=False):
    k = node[0]
    if k == "void": return
    if k == "switch":
        gen_switch(g, name, node, path, sc, S, L, lvl, vm, top); return
    m = ident(name); o = "o." + m; dec = g.mode == "dec"
    sub = path + "_" + name
    ct = ctype_of(g, node, sub, S)
    if k == "array" and node[3][0] == "container" or (k == "option" and node[1][0] == "container"):
        pass
    if k == "container":
        make_struct(g, node, sub, sc)
    if k == "array" and node[3][0] == "container": make_struct(g, node[3], sub, sc)
    if k == "option" and node[1][0] == "container": make_struct(g, node[1], sub + "_v", sc)
    merge_member(S, m, ct, vm)
    # register in scope for later compareTo
    if k == "prim": sc.f[name] = (o, "num", None)
    elif k == "mapper": sc.f[name] = (o, "mapper", node[2])
    elif k == "str": sc.f[name] = (o, "str", None)
    elif k == "bits": sc.f[name] = (o, "bits", node[1])
    if dec: L.extend(dec_code(g, o, node, sub, sc, lvl))
    else: L.extend(enc_code(g, o, node, sub, sc, lvl))

def ctx_arg(sc, inner_node):
    """if an element container refers to '../x', return the expression passing x's value."""
    refs = set()
    def scan(n):
        if n[0] == "container":
            for _, t, _ in n[1]: scan(t)
        elif n[0] == "switch":
            if n[1].startswith("../"): refs.add(n[1][3:])
            for b in n[2].values(): scan(b)
            if n[3]: scan(n[3])
        elif n[0] in ("array",): scan(n[3])
        elif n[0] == "option": scan(n[1])
    scan(inner_node)
    if not refs: return "0", None
    assert len(refs) == 1, refs
    r = list(refs)[0]
    return "(i32)" + sc.f[r][0], r

def make_struct(g, node, path, parent_sc):
    """create struct + (de|en)code function for a nested container."""
    sig = json.dumps(node, sort_keys=True, default=str)
    if path in g.sig:
        if g.sig[path] != sig: raise Exception("nested layout differs between versions: " + path)
        return
    g.sig[path] = sig
    sc = Scope()
    # collect '../' targets
    refs = {}
    def scan(n):
        if n[0] == "container":
            for _, t, _ in n[1]: scan(t)
        elif n[0] == "switch":
            if n[1].startswith("../"): refs[n[1][3:]] = parent_sc.f.get(n[1][3:])
            for b in n[2].values(): scan(b)
            if n[3]: scan(n[3])
        elif n[0] == "array": scan(n[3])
        elif n[0] == "option": scan(n[1])
    scan(node)
    sc.up = {n: ("ctx", i[1], i[2]) for n, i in refs.items() if i}
    S = []; L = []
    gen_container(g, node[1], path, sc, S, L, 1, False, False)
    g.structs.append((path, S))
    if g.mode == "dec":
        g.funcs.append("inline bool decodeElem(Reader& r, %s& o, Version v, i32 ctx) {\n    (void)v; (void)ctx; o = %s();\n%s\n    return r.ok();\n}\n" % (path, path, "\n".join(L)))
    else:
        g.funcs.append("inline void encodeElem(Writer& w, const %s& o, Version v, i32 ctx) {\n    (void)v; (void)ctx;\n%s\n}\n" % (path, "\n".join(L)))

def dec_code(g, o, node, path, sc, lvl):
    k = node[0]; i = ind(lvl); L = []
    if k == "prim": L.append(i + "%s = r.%s();" % (o, RW[node[1]][0]))
    elif k == "mapper": L.append(i + "%s = r.%s();" % (o, RW[node[1]][0]))
    elif k == "str": L.append(i + "%s = r.str();" % o)
    elif k == "uuid": L.append(i + "{ const u8* p = r.bytes(16); if (p) memcpy(%s.b, p, 16); }" % o)
    elif k == "slot": L.append(i + "if (!decodeSlot(r, %s)) return false;" % o)
    elif k == "pos": L.append(i + "%s = unpackPos(r.u64_());" % o)
    elif k == "nbt": L.append(i + "if (!readNbt(r, %s, %s)) return false;" % (o, "true" if node[1] else "false"))
    elif k == "meta": L.append(i + "if (!decodeMeta(r, %s, v)) return false;" % o)
    elif k == "bits":
        size = sum(f["size"] for f in node[1]); assert size == 32
        L.append(i + "%s = r.u32_();" % o)
    elif k == "buf":
        if node[1] is None: L.append(i + "{ %s.len = (u32)r.left(); %s.data = r.bytes(r.left()); }" % (o, o))
        else:
            L.append(i + "{ i32 n = r.%s(); if (!r.ok() || n < 0) return false; %s.data = r.bytes((size_t)n); %s.len = (u32)n; }" % (RW[node[1]][0], o, o))
    elif k == "container":
        L.append(i + "if (!decodeElem(r, %s, v, %s)) return false;" % (o, ctx_for_container(node, sc)))
    elif k == "option":
        L.append(i + "%s.has = r.bool_();" % o)
        L.append(i + "if (%s.has) {" % o)
        inner = node[1]
        L.extend(dec_code(g, o + ".v", inner, path + "_v", sc, lvl + 1) if inner[0] != "container" else
                 [ind(lvl + 1) + "if (!decodeElem(r, %s.v, v, %s)) return false;" % (o, ctx_for_container(inner, sc))])
        L.append(i + "}")
    elif k == "array":
        _, ct, cnt, e = node
        ctx = "0"
        if e[0] == "container": ctx = ctx_for_container(e, sc)
        if cnt is not None: L.append(i + "{ i32 n = %d;" % cnt)
        else: L.append(i + "{ i32 n = r.%s();" % RW[ct][0])
        L.append(ind(lvl + 1) + "if (!r.ok() || n < 0 || (size_t)n > r.left()) return false;")
        L.append(ind(lvl + 1) + "%s.r = r; %s.r.err = false; %s.n = (u32)n; %s.v = v; %s.ctx = %s;" % (o, o, o, o, o, ctx))
        L.append(ind(lvl + 1) + "%s tmp; for (i32 j = 0; j < n; ++j) if (!decodeElem(r, tmp, v, %s)) return false;" % (elem_t(g, e, path), ctx))
        L.append(ind(lvl + 1) + "%s.r.end = r.p; }" % o)
    else: raise Exception("dec " + k)
    return L

def ctx_for_container(node, sc):
    c, _ = ctx_arg(sc, node); return c

def enc_code(g, o, node, path, sc, lvl):
    k = node[0]; i = ind(lvl); L = []
    if k in ("prim", "mapper"):
        L.append(i + "w.%s;" % (RW[node[1]][1] % o))
    elif k == "str": L.append(i + "w.str(%s);" % o)
    elif k == "uuid": L.append(i + "w.bytes(%s.b, 16);" % o)
    elif k == "slot": L.append(i + "encodeSlot(w, %s);" % o)
    elif k == "pos": L.append(i + "w.u64_(packPos(%s.x, %s.y, %s.z));" % (o, o, o))
    elif k == "nbt":
        L.append(i + "if (%s.len) w.bytes(%s.data, %s.len);%s" % (o, o, o, " else w.u8_(0);" if node[1] else ""))
    elif k == "bits": L.append(i + "w.u32_(%s);" % o)
    elif k == "buf":
        if node[1] is not None: L.append(i + "w.%s;" % (RW[node[1]][1] % ("(i32)%s.len" % o) if node[1] == "varint" else RW[node[1]][1] % ("%s.len" % o)))
        L.append(i + "w.bytes(%s.data, %s.len);" % (o, o))
    elif k == "container":
        L.append(i + "encodeElem(w, %s, v, %s);" % (o, ctx_for_container(node, sc)))
    elif k == "option":
        L.append(i + "w.bool_(%s.has);" % o)
        L.append(i + "if (%s.has) {" % o)
        inner = node[1]
        if inner[0] == "container": L.append(ind(lvl + 1) + "encodeElem(w, %s.v, v, %s);" % (o, ctx_for_container(inner, sc)))
        else: L.extend(enc_code(g, o + ".v", inner, path + "_v", sc, lvl + 1))
        L.append(i + "}")
    elif k == "array":
        _, ct, cnt, e = node
        ctx = ctx_for_container(e, sc) if e[0] == "container" else "0"
        if cnt is None: L.append(i + "w.%s;" % (RW[ct][1] % ("(%s)%s.n" % (PRIMS[ct], o))))
        L.append(i + "for (u32 j = 0; j < %s.n; ++j) encodeElem(w, %s.data[j], v, %s);" % (o, o, ctx))
    else: raise Exception("enc " + k)
    return L

# ------------------------------------------------------------------ per packet
def emit_side(side, mode, ns, idenum):
    pk = load(side)
    g = Gen(mode)
    chunks = []
    for pname, vers in pk.items():
        name = camel(pname[len("packet_"):])
        # distinct layouts
        sigs = []; var = []
        for j, types in vers:
            if j is None: var.append(255); continue
            s = json.dumps(j, sort_keys=True)
            if s not in sigs: sigs.append(s)
            var.append(sigs.index(s))
        S = []; cases = []
        g.structs = []; g.funcs = []; g.sig = {}
        for idx, s in enumerate(sigs):
            j = json.loads(s); types = [t for jj, t in vers if jj is not None and json.dumps(jj, sort_keys=True) == s][0]
            node = norm(j, types); sc = Scope(); L = []
            gen_container(g, node[1], name, sc, S, L, 2, True, True)
            cases.append((idx, L))
        # emit
        out = []
        for sname, members in g.structs:
            out.append("struct %s {\n%s};\n" % (sname, "".join("    %s %s;\n" % (t, n) for n, t in members)))
        out.extend(g.funcs)
        top = "struct %s {\n    static %s packetId() { return %s::%s; }\n%s};\n" % (name, idenum, idenum, name, "".join("    %s %s;\n" % (t, n) for n, t in S))
        out.append(top)
        out.append("static const u8 kVar_%s[6] = {%s};" % (name, ",".join(map(str, var))))
        if mode == "dec":
            body = "".join("    case %d: {\n%s\n        break; }\n" % (idx, "\n".join(L)) for idx, L in cases)
            out.append("inline bool decode(Reader& r, %s& o, Version v) {\n    o = %s();\n    switch (kVar_%s[(int)v]) {\n%s    default: return false;\n    }\n    return r.ok();\n}\n" % (name, name, name, body))
        else:
            body = "".join("    case %d: {\n%s\n        break; }\n" % (idx, "\n".join(L)) for idx, L in cases)
            out.append("inline bool encode(Writer& w, const %s& o, Version v) {\n    switch (kVar_%s[(int)v]) {\n%s    default: return false;\n    }\n    return !w.err;\n}\n" % (name, name, body))
        chunks.append("\n".join(out))
    if mode == "dec":
        cases = "".join("    case %s::%s: { %s o; return decode(r, o, v); }\n" % (idenum, camel(p[7:]), camel(p[7:])) for p in pk)
        chunks.append("// Decodes (and discards) any clientbound packet by agnostic id - for validation/diagnostics.\ninline bool validate(%s id, Reader r, Version v) {\n    switch (id) {\n%s    default: return false;\n    }\n}\n" % (idenum, cases))
    hdr = ["// GENERATED by tools/gen_packets.py from minecraft-data - do not edit", "#pragma once", '#include "wire.h"', '#include "ids_gen.h"',
           "namespace mc { namespace %s {" % ns, "template<class T> struct Opt { bool has; T v; Opt() : has(false), v() {} };", ""]
    return "\n".join(hdr) + "\n" + "\n".join(chunks) + "\n}} // namespace mc::%s\n" % ns

open(OUT + "/packets_cb.h", "w").write(emit_side("toClient", "dec", "cb", "ids::PlayCb"))
open(OUT + "/packets_sb.h", "w").write(emit_side("toServer", "enc", "sb", "ids::PlaySb"))
