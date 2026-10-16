#!/usr/bin/env python3
"""Rust back-end of the packet generator: writes src/packets/cb.rs (clientbound decoders) and sb.rs
(serverbound encoders) for the Play state from PrismarineJS/minecraft-data protocol.json.
usage: gen_packets_rs.py <data/pc> <outdir>      (outdir = src/packets)
Shares the protocol normalisation with gen_packets.py (the C++ back-end), so both languages see the same layouts.
Layout differences between versions are merged into one struct; decode/encode select the layout at runtime by Version."""
import json, re, sys
import gen_packets as G

ROOT, OUT = sys.argv[1], sys.argv[2]
G.ROOT = ROOT
norm, load, camel, resolve = G.norm, G.load, G.camel, G.resolve

RUST_KW = set("as break const continue crate else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while async await dyn abstract become box do final macro override priv typeof unsized virtual yield try".split())
PRIMS = {k: k for k in "i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 bool".split()}
PRIMS.update({"varint": "i32", "varlong": "i64"})
RANK = {"bool": 0, "i8": 1, "u8": 2, "i16": 3, "u16": 4, "i32": 5, "u32": 6, "i64": 7, "u64": 8, "f32": 9, "f64": 10}

def snake(n):
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", n)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", s)
    s = s.lower()
    return s + "_" if s in RUST_KW else s

def ind(n): return "    " * n

class Gen:
    def __init__(self, mode):
        self.mode = mode
        self.structs = []     # (name, [(member, type)])
        self.funcs = []
        self.sig = {}
        self.lt = set()       # struct names that carry the 'a lifetime

    def lt_of(self, name): return "<'a>" if name in self.lt else ""
    def arr_t(self, e): return ("Array<'a, %s>" % e) if self.mode == "dec" else ("&'a [%s]" % e)

def ctype_of(g, node, path):
    k = node[0]
    if k == "prim": return PRIMS[node[1]]
    if k == "str": return "Str<'a>"
    if k == "uuid": return "Uuid"
    if k == "slot": return "Slot<'a>"
    if k == "pos": return "BlockPos"
    if k in ("nbt", "buf"): return "Bytes<'a>"
    if k == "meta": return "MetaView<'a>"
    if k == "mapper": return PRIMS[node[1]]
    if k == "bits": return "u32"
    if k == "container": return path + g.lt_of(path)
    if k == "array": return g.arr_t(elem_t(g, node[3], path))
    if k == "option": return "Option<%s>" % ctype_of(g, node[1], path + "_v")
    raise Exception("ctype " + k)

def elem_t(g, e, path):
    k = e[0]
    if k == "prim": return {"varint": "VarInt", "varlong": "VarLong"}.get(e[1]) or PRIMS[e[1]]
    if k == "array":
        if e[1] != "varint" or e[2] is not None: raise Exception("nested array count")
        return g.arr_t(elem_t(g, e[3], path + "_e"))
    if k == "option": raise Exception("option elem")
    return ctype_of(g, e, path)

def merge_member(S, name, ct, ver_merge):
    for i, (n, t) in enumerate(S):
        if n == name:
            if t == ct: return
            if t in RANK and ct in RANK and ver_merge:
                if RANK[ct] > RANK[t]: S[i] = (n, ct)
                return
            raise Exception("conflicting member %s: %s vs %s" % (name, t, ct))
    S.append((name, ct))

def cond_for(ref, keys):
    parts = []
    for key in keys:
        if ref[0] == "bits": parts.append("((((%s) >> %d) & %d) as i64) == %d" % (ref[1], ref[2], ref[3], int(key, 0)))
        else:
            if ref[0] == "ctx": expr, kind, extra = ref[1]
            else: _, expr, kind, extra = ref
            if kind == "str": parts.append('%s == "%s"' % (expr, key))
            elif kind == "mapper" and key in extra: parts.append("(%s as i64) == %d" % (expr, extra[key]))
            else: parts.append("(%s as i64) == %d" % (expr, int(key, 0)))
    return " || ".join(parts) if parts else "false"

# ------------------------------------------------------------------ structure (mirrors gen_packets.py)
def gen_container(g, fields, path, sc, S, L, lvl, vm):
    for name, node, anon in fields:
        if anon:
            if node[0] == "container": gen_container(g, node[1], path, sc, S, L, lvl, vm)
            elif node[0] == "switch": gen_switch(g, None, node, path, sc, S, L, lvl, vm)
            else: raise Exception("anon " + node[0])
        else:
            gen_value(g, name, node, path, sc, S, L, lvl, vm)

def gen_switch(g, name, node, path, sc, S, L, lvl, vm):
    _, cmp, branches, default = node
    ref = resolve(sc, cmp)
    groups = {}
    for key, b in branches.items():
        sig = json.dumps(b, sort_keys=True, default=str)
        groups.setdefault(sig, [b, []])[1].append(key)
    allkeys = [k for _, ks in groups.values() for k in ks]
    for b, keys in groups.values():
        if b == ("void",): continue
        L.append(ind(lvl) + "if %s {" % cond_for(ref, keys))
        emit_branch(g, name, b, path, sc, S, L, lvl + 1, vm)
        L.append(ind(lvl) + "}")
    if default is not None and default != ("void",):
        L.append(ind(lvl) + "if !(%s) {" % cond_for(ref, allkeys))
        emit_branch(g, name, default, path, sc, S, L, lvl + 1, vm)
        L.append(ind(lvl) + "}")

def emit_branch(g, name, b, path, sc, S, L, lvl, vm):
    if name is None:
        if b[0] == "container": gen_container(g, b[1], path, sc, S, L, lvl, vm)
        else: raise Exception("anon branch " + b[0])
    else:
        gen_value(g, name, b, path, sc, S, L, lvl, vm)

def gen_value(g, name, node, path, sc, S, L, lvl, vm):
    k = node[0]
    if k == "void": return
    if k == "switch":
        gen_switch(g, name, node, path, sc, S, L, lvl, vm); return
    m = snake(name); o = "o." + m
    sub = path + "_" + name
    # nested structs first so that their lifetime flag is known when the member type is built
    if k == "container": make_struct(g, node, sub, sc)
    if k == "array" and node[3][0] == "container": make_struct(g, node[3], sub, sc)
    if k == "option" and node[1][0] == "container": make_struct(g, node[1], sub + "_v", sc)
    ct = ctype_of(g, node, sub)
    merge_member(S, m, ct, vm)
    if k == "prim": sc.f[name] = (o, "num", None)
    elif k == "mapper": sc.f[name] = (o, "mapper", node[2])
    elif k == "str": sc.f[name] = (o, "str", None)
    elif k == "bits": sc.f[name] = (o, "bits", node[1])
    if g.mode == "dec": L.append(ind(lvl) + "%s = %s;" % (o, dec_expr(g, node, sub, sc, m)))
    else: L.extend(enc_code(g, o, node, sub, sc, lvl))

def ctx_arg(sc, inner):
    refs = set()
    def scan(n):
        if n[0] == "container":
            for _, t, _ in n[1]: scan(t)
        elif n[0] == "switch":
            if n[1].startswith("../"): refs.add(n[1][3:])
            for b in n[2].values(): scan(b)
            if n[3]: scan(n[3])
        elif n[0] == "array": scan(n[3])
        elif n[0] == "option": scan(n[1])
    scan(inner)
    if not refs: return "0"
    assert len(refs) == 1, refs
    return "(%s as i32)" % sc.f[list(refs)[0]][0]

def make_struct(g, node, path, parent_sc):
    sig = json.dumps(node, sort_keys=True, default=str)
    if path in g.sig:
        if g.sig[path] != sig: raise Exception("nested layout differs between versions: " + path)
        return
    g.sig[path] = sig
    sc = G.Scope()
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
    # a nested element struct reads its parent's value from the `ctx` parameter
    gen_container(g, node[1], path, sc, S, L, 2, False)
    emit_struct(g, path, S)
    finals = {n: t for n, t in S}
    body = "\n".join(resolve_casts(l, finals) for l in L)
    if g.mode == "dec":
        g.funcs.append("impl<'a> Elem<'a> for %s%s {\n    fn decode(r: &mut Reader<'a>, v: Version, ctx: i32) -> Option<Self> {\n        let mut o = Self::default();\n%s\n        if r.err { None } else { Some(o) }\n    }\n}\n" % (path, g.lt_of(path), body))
    else:
        g.funcs.append("impl%s EncElem for %s%s {\n    fn encode(&self, w: &mut Writer, v: Version, ctx: i32) {\n        let o = self;\n%s\n    }\n}\n" % (g.lt_of(path), path, g.lt_of(path), body))

def resolve_casts(line, finals):
    return re.sub(r"@@CAST:([^:]+):([^@]+)@@", lambda m: "" if finals.get(m.group(1)) in (None, m.group(2)) else " as " + finals[m.group(1)], line)

def emit_struct(g, name, members):
    uses_lt = any("'a" in t for _, t in members)
    if uses_lt: g.lt.add(name)
    g.structs.append((name, members, uses_lt))

# ------------------------------------------------------------------ decode
def dec_expr(g, node, path, sc, member=None):
    k = node[0]
    if k in ("prim", "mapper"):
        t = node[1]
        return "r.%s()@@CAST:%s:%s@@" % (t, member, PRIMS[t]) if member else "r.%s()" % t
    if k == "str": return "r.str()"
    if k == "uuid": return "{ let mut u = Uuid::default(); if let Some(p) = r.bytes(16) { u.0.copy_from_slice(p); } u }"
    if k == "slot": return "decode_slot(r)?"
    if k == "pos": return "unpack_pos(r.u64())"
    if k == "nbt": return "read_nbt(r, %s)?" % ("true" if node[1] else "false")
    if k == "meta": return "MetaView::parse(r, v)?"
    if k == "bits":
        assert sum(f["size"] for f in node[1]) == 32
        return "r.u32()"
    if k == "buf":
        if node[1] is None: return "{ let n = r.left(); Bytes(r.bytes(n).unwrap_or(&[])) }"
        return "{ let n = r.%s() as i64; if !r.ok() || n < 0 { return None; } Bytes(r.bytes(n as usize)?) }" % node[1]
    if k == "container": return "Elem::decode(r, v, %s)?" % ctx_arg(sc, node)
    if k == "option": return "if r.bool() { Some(%s) } else { None }" % dec_expr(g, node[1], path + "_v", sc)
    if k == "array":
        _, ct, cnt, e = node
        ctx = ctx_arg(sc, e) if e[0] == "container" else "0"
        if cnt is not None: return "Array::parse(r, %d, v, %s)?" % (cnt, ctx)
        return "{ let n = r.%s() as i32; Array::parse(r, n, v, %s)? }" % (ct, ctx)
    raise Exception("dec " + k)

# ------------------------------------------------------------------ encode
def enc_code(g, o, node, path, sc, lvl):
    k = node[0]; i = ind(lvl); L = []
    if k in ("prim", "mapper"):
        t = node[1]; L.append(i + "w.%s(%s as %s);" % (t, o, PRIMS[t]))
    elif k == "str": L.append(i + "w.str(%s.0);" % o)
    elif k == "uuid": L.append(i + "w.bytes(&%s.0);" % o)
    elif k == "slot": L.append(i + "encode_slot(w, &%s);" % o)
    elif k == "pos": L.append(i + "w.u64(pack_pos(%s.x, %s.y, %s.z));" % (o, o, o))
    elif k == "nbt":
        L.append(i + "if !%s.0.is_empty() { w.bytes(%s.0); }%s" % (o, o, " else { w.u8(0); }" if node[1] else ""))
    elif k == "bits": L.append(i + "w.u32(%s);" % o)
    elif k == "buf":
        if node[1] is not None: L.append(i + "w.%s(%s.0.len() as %s);" % (node[1], o, PRIMS[node[1]]))
        L.append(i + "w.bytes(%s.0);" % o)
    elif k == "container": L.append(i + "EncElem::encode(&%s, w, v, %s);" % (o, ctx_arg(sc, node)))
    elif k == "option":
        inner = node[1]
        L.append(i + "if let Some(x) = %s {" % o)
        L.append(ind(lvl + 1) + "w.bool(true);")
        if inner[0] == "container": L.append(ind(lvl + 1) + "EncElem::encode(&x, w, v, %s);" % ctx_arg(sc, inner))
        else: L.extend(enc_code(g, "x", inner, path + "_v", sc, lvl + 1))
        L.append(i + "} else {")
        L.append(ind(lvl + 1) + "w.bool(false);")
        L.append(i + "}")
    elif k == "array":
        _, ct, cnt, e = node
        ctx = ctx_arg(sc, e) if e[0] == "container" else "0"
        if cnt is None: L.append(i + "w.%s(%s.len() as %s);" % (ct, o, PRIMS[ct]))
        L.append(i + "for e in %s.iter() { EncElem::encode(e, w, v, %s); }" % (o, ctx))
    else: raise Exception("enc " + k)
    return L

# ------------------------------------------------------------------ per packet
def emit_side(side, mode, idenum):
    pk = load(side)
    g = Gen(mode)
    chunks = []
    for pname, vers in pk.items():
        name = camel(pname[len("packet_"):])
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
            node = norm(j, types); sc = G.Scope(); L = []
            gen_container(g, node[1], name, sc, S, L, 3, True)
            cases.append((idx, L))
        emit_struct(g, name, S)
        finals = {n: t for n, t in S}
        def fix(line): return resolve_casts(line, finals)
        out = []
        for sname, members, uses_lt in g.structs:
            ltd = "<'a>" if uses_lt else ""
            out.append("#[derive(Clone, Copy, Default, Debug)]\npub struct %s%s {\n%s}\n" % (sname, ltd, "".join("    pub %s: %s,\n" % (n, t) for n, t in members)))
        out.extend(g.funcs)
        out.append("static VAR_%s: [u8; 6] = [%s];" % (name.upper(), ",".join(map(str, var))))
        lt = g.lt_of(name)
        body = "".join("            %d => {\n%s\n            }\n" % (idx, fix("\n".join(L))) for idx, L in cases)
        if mode == "dec":
            if lt:
                out.append("impl<'a> %s%s {\n    pub fn decode(r: &mut Reader<'a>, v: Version) -> Option<Self> {" % (name, lt))
            else:
                out.append("impl %s {\n    pub fn decode(r: &mut Reader<'_>, v: Version) -> Option<Self> {" % name)
            out.append("        let ctx = 0i32;\n        let mut o = Self::default();\n        match VAR_%s[v as usize] {\n%s            _ => return None,\n        }\n        if r.err { None } else { Some(o) }\n    }\n}" % (name.upper(), body))
            out.append("impl<'a> ClientPacket<'a> for %s%s {\n    const ID: PlayCb = PlayCb::%s;\n    fn decode(r: &mut Reader<'a>, v: Version) -> Option<Self> { %s::decode(r, v) }\n}\n" % (name, lt, name, name))
        else:
            out.append("impl%s ServerPacket for %s%s {\n    const ID: PlaySb = PlaySb::%s;\n    fn encode(&self, w: &mut Writer, v: Version) -> bool {\n        let o = self;\n        let ctx = 0i32;\n        match VAR_%s[v as usize] {\n%s            _ => return false,\n        }\n        !w.err\n    }\n}\n" % (lt, name, lt, name, name.upper(), body))
        chunks.append("\n".join(out))
    if mode == "dec":
        cases = "".join("        PlayCb::%s => %s::decode(&mut r, v).is_some(),\n" % (camel(p[7:]), camel(p[7:])) for p in pk)
        chunks.append("/// Decodes (and discards) any clientbound packet by agnostic id - for validation/diagnostics.\npub fn validate(id: PlayCb, r: Reader, v: Version) -> bool {\n    let mut r = r;\n    match id {\n%s    }\n}\n" % cases)
        byname = "".join('        "%s" => %s::decode(r, v).is_some(),\n' % (camel(p[7:]), camel(p[7:])) for p in pk)
        chunks.append("#[doc(hidden)]\npub fn decode_by_name(name: &str, r: &mut Reader, v: Version) -> bool {\n    match name {\n%s        _ => false,\n    }\n}\n" % byname)
    if mode == "dec":
        hdr = ["// GENERATED by tools/gen_packets_rs.py from minecraft-data - do not edit",
               "//! Clientbound Play packets: zero-copy typed decoders (strings, arrays, NBT and metadata are views into the receive buffer).",
               "#![allow(unused_variables, unused_mut, unused_parens, non_camel_case_types, unused_imports, clippy::all)]",
               "use crate::ids::PlayCb;", "use crate::types::Version;", "use crate::wire::*;", "use super::ClientPacket;", ""]
    else:
        hdr = ["// GENERATED by tools/gen_packets_rs.py from minecraft-data - do not edit",
               "//! Serverbound Play packets: typed encoders (arrays are slices).",
               "#![allow(unused_variables, unused_mut, unused_parens, non_camel_case_types, unused_imports, clippy::all)]",
               "use crate::ids::PlaySb;", "use crate::types::Version;", "use crate::wire::*;", "use super::ServerPacket;", ""]
    return "\n".join(hdr) + "\n" + "\n".join(chunks) + "\n"

if __name__ == "__main__":
    open(OUT + "/cb.rs", "w").write(emit_side("toClient", "dec", "PlayCb"))
    open(OUT + "/sb.rs", "w").write(emit_side("toServer", "enc", "PlaySb"))
