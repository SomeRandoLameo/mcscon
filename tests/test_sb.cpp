// Serverbound encoders vs. hand-computed wire bytes (version differences included).
#include <mcscon/packets_sb.h>
#include "check.h"
#include <vector>
using namespace mc;
typedef std::vector<u8> Buf;
template<class P> static Buf enc(const P& p, Version v, bool* ok = 0) {
    u8 buf[512]; Writer w(buf, sizeof buf); bool r = sb::encode(w, p, v); if (ok) *ok = r; return Buf(buf, w.p);
}
static StrView S(const char* s) { StrView v = {s, (u32)strlen(s)}; return v; }

static void test_basic() {
    sb::Chat c = {}; c.message = S("hi"); CHECK((enc(c, Version::V1_12_2) == Buf{2, 'h', 'i'}));
    sb::Settings s = {}; s.locale = S("en"); s.viewDistance = 8; s.chatFlags = 0; s.chatColors = true; s.skinParts = 0x7F; s.mainHand = 1;
    CHECK((enc(s, Version::V1_10) == Buf{2, 'e', 'n', 8, 0, 1, 0x7F, 1}));
    sb::ClientCommand cc = {}; cc.actionId = 0; CHECK((enc(cc, Version::V1_12) == Buf{0}));
    sb::TeleportConfirm t = {}; t.teleportId = 300; CHECK((enc(t, Version::V1_12) == Buf{0xAC, 2}));
    sb::PositionLook pl = {}; pl.x = 1; pl.yaw = 90; pl.onGround = true; Buf b = enc(pl, Version::V1_12_2);
    CHECK(b.size() == 33 && b[0] == 0x3F && b[1] == 0xF0 && b[32] == 1);
}
static void test_versions() {
    sb::KeepAlive k = {}; k.keepAliveId = 0x0102;
    CHECK((enc(k, Version::V1_10) == Buf{0x82, 0x02}));
    CHECK((enc(k, Version::V1_12) == Buf{0x82, 0x02}));
    CHECK((enc(k, Version::V1_12_1) == Buf{0x82, 0x02}));                    // 1.12.1 still VarInt
    CHECK((enc(k, Version::V1_12_2) == Buf{0, 0, 0, 0, 0, 0, 1, 2}));        // 1.12.2: long
    sb::BlockPlace bp = {}; bp.location.x = 1; bp.location.y = 2; bp.location.z = 3; bp.direction = 1; bp.hand = 0; bp.cursorX = 8; bp.cursorY = 4; bp.cursorZ = 0;
    Buf a = enc(bp, Version::V1_10); CHECK(a.size() == 8 + 1 + 1 + 3 && a[10] == 8 && a[11] == 4);
    bp.cursorX = 0.5f; Buf c = enc(bp, Version::V1_11); CHECK(c.size() == 8 + 1 + 1 + 12 && c[10] == 0x3F && c[11] == 0);
    CHECK(c[0] == 0 && c[7] == 3);  // position packing: x<<38|y<<26|z
}
static void test_switches() {
    sb::UseEntity u = {}; u.target = 7; u.mouse = 1;                            // attack: no extra fields
    CHECK((enc(u, Version::V1_12_2) == Buf{7, 1}));
    u.mouse = 0; u.hand = 1; CHECK((enc(u, Version::V1_12_2) == Buf{7, 0, 1}));   // interact: hand
    u.mouse = 2; u.x = 1.0f; u.hand = 0; Buf b = enc(u, Version::V1_12_2); CHECK(b.size() == 2 + 12 + 1 && b[2] == 0x3F);   // interact_at: xyz + hand
    sb::AdvancementTab at = {}; at.action = 0; at.tabId = S("a:b"); CHECK((enc(at, Version::V1_12_2) == Buf{0, 3, 'a', ':', 'b'}));
    at.action = 1; CHECK((enc(at, Version::V1_12_2) == Buf{1}));
    sb::CraftingBookData cb = {}; cb.type = 1; cb.craftingBookOpen = true; cb.craftingFilter = false; CHECK((enc(cb, Version::V1_12_2) == Buf{1, 1, 0}));
    cb.type = 0; cb.displayedRecipe = 5; CHECK((enc(cb, Version::V1_12_2) == Buf{0, 0, 0, 0, 5}));
}
static void test_slots_and_arrays() {
    sb::SetCreativeSlot sc = {}; sc.slot = 36; sc.item.id = 1; sc.item.count = 64; sc.item.damage = 0;
    CHECK((enc(sc, Version::V1_12_2) == Buf{0, 36, 0, 1, 64, 0, 0, 0}));       // id, count, damage, TAG_End
    sc.item.id = -1; CHECK((enc(sc, Version::V1_12_2) == Buf{0, 36, 0xFF, 0xFF}));
    sb::WindowClick wc = {}; wc.windowId = 1; wc.slot = 5; wc.mouseButton = 0; wc.action = 3; wc.mode = 0; wc.item.id = -1;
    CHECK((enc(wc, Version::V1_10) == Buf{1, 0, 5, 0, 0, 3, 0, 0xFF, 0xFF}));
    // 1.12.2 dropped PrepareCraftingGrid (replaced by CraftRecipeRequest); 1.12 has it
    bool ok = true; sb::PrepareCraftingGrid pg = {}; enc(pg, Version::V1_12_2, &ok); CHECK(!ok);
}
int main() { RUN(test_basic); RUN(test_versions); RUN(test_switches); RUN(test_slots_and_arrays); DONE(); }
