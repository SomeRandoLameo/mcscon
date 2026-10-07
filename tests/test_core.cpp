#include <mcscon/buffer.h>
#include <mcscon/zlib.h>
#include "check.h"
#include "zlib_vectors.h"
#include <stdlib.h>
#include <initializer_list>
using namespace mc;

static void test_varint() {
    i32 vals[] = {0, 1, 127, 128, 255, 25565, 2097151, 2147483647, -1, -2147483647 - 1};
    for (i32 v : vals) {
        u8 b[8]; Writer w(b, 8); w.varint(v);
        CHECK((size_t)(w.p - b) == varintSize((u32)v));
        Reader r(b, w.p - b); CHECK(r.varint() == v); CHECK(r.ok() && r.left() == 0);
    }
    u8 bad[6] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};   // 6 bytes: too long
    Reader r(bad, 6); r.varint(); CHECK(!r.ok());
    u8 trunc[1] = {0x80}; Reader r2(trunc, 1); r2.varint(); CHECK(!r2.ok());
    u8 b[10]; Writer w(b, 10); w.varlong(-1); Reader r3(b, w.p - b); CHECK(r3.varlong() == -1);
}
static void test_prims() {
    u8 b[64]; Writer w(b, 64);
    w.u16_(0xBEEF); w.i32_(-5); w.i64_(-123456789012345LL); w.f32_(1.5f); w.f64_(-2.25); w.str("héllo"); w.bool_(true);
    Reader r(b, w.p - b);
    CHECK(r.u16_() == 0xBEEF); CHECK(r.i32_() == -5); CHECK(r.i64_() == -123456789012345LL);
    CHECK(r.f32_() == 1.5f); CHECK(r.f64_() == -2.25);
    StrView s = r.str(); CHECK(s.len == 6 && memcmp(s.data, "h\xc3\xa9llo", 6) == 0);
    CHECK(r.bool_()); CHECK(r.ok() && r.left() == 0);
    r.u8_(); CHECK(!r.ok());
    Writer small(b, 2); small.u32_(1); CHECK(small.err);
    // string length lie
    u8 lie[3] = {10, 'a', 'b'}; Reader rl(lie, 3); rl.str(); CHECK(!rl.ok());
}
static void test_pos() {
    i32 xs[] = {0, 1, -1, 33554431, -33554432, 1234, -5678};
    for (i32 x : xs) for (i32 y : {0, 255, 64}) for (i32 z : {0, -1, 33554431, -33554432, 99}) {
        BlockPos p = unpackPos(packPos(x, y, z)); CHECK(p.x == x && p.y == y && p.z == z);
    }
}
static u32 fnv(const u8* d, size_t n) { u32 h = 2166136261u; while (n--) h = (h ^ *d++) * 16777619u; return h; }
static void test_inflate() {
    for (const ZVec& v : zvecs) {
        u8* out = (u8*)malloc(v.rawlen + 1);
        i32 n = zlibInflate(v.z, v.zlen, out, v.rawlen, true);
        printf("    %-6s %zu -> %d\n", v.name, v.zlen, n);
        CHECK(n == (i32)v.rawlen); CHECK(fnv(out, v.rawlen) == v.fnv);
        if (v.rawlen) { CHECK(zlibInflate(v.z, v.zlen, out, v.rawlen - 1) == -1); }   // overflow
        if (v.zlen > 12) {
            CHECK(zlibInflate(v.z, v.zlen / 2, out, v.rawlen) == -1);                 // truncated
            u8* bad = (u8*)malloc(v.zlen); memcpy(bad, v.z, v.zlen); bad[v.zlen - 1] ^= 1;
            CHECK(zlibInflate(bad, v.zlen, out, v.rawlen, true) == -1);               // adler mismatch
            free(bad);
        }
        free(out);
    }
    // fuzz: random corruption must never crash or overrun
    srand(1); u8 out[4096];
    for (int i = 0; i < 20000; ++i) {
        const ZVec& v = zvecs[3 + rand() % 2]; u8 tmp[2048]; size_t n = v.zlen < 2048 ? v.zlen : 2048;
        memcpy(tmp, v.z, n); for (int k = 0; k < 3; ++k) tmp[rand() % n] = (u8)rand();
        zlibInflate(tmp, n, out, sizeof out);
    }
    CHECK(true);
}
int main() { RUN(test_varint); RUN(test_prims); RUN(test_pos); RUN(test_inflate); DONE(); }
