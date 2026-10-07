#include <mcscon/chat.h>
#include "check.h"
#include <string.h>
#include <stdlib.h>
using namespace mc;
static bool is(const char* json, const char* want, bool strip = true) {
    char b[256]; size_t n = chatToText(json, strlen(json), b, sizeof b, strip);
    if (strcmp(b, want) || n != strlen(want)) { printf("    got '%s' want '%s'\n", b, want); return false; }
    return true;
}
static void test_chat() {
    CHECK(is("{\"text\":\"hello\"}", "hello"));
    CHECK(is("\"plain\"", "plain"));
    CHECK(is("{\"text\":\"a\",\"extra\":[{\"text\":\"b\",\"bold\":true},\"c\",[{\"text\":\"d\"}]]}", "abcd"));
    CHECK(is("{\"translate\":\"chat.type.text\",\"with\":[{\"text\":\"Bob\",\"insertion\":\"Bob\"},\"hi there\"]}", "<Bob> hi there"));
    CHECK(is("{\"translate\":\"multiplayer.player.joined\",\"with\":[{\"text\":\"X\"}],\"color\":\"yellow\"}", "X joined the game"));
    CHECK(is("{\"translate\":\"some.unknown.key\",\"with\":[\"a\",{\"text\":\"b\"}]}", "some.unknown.key[a,b]"));
    CHECK(is("{\"text\":\"\\u00a7cred \\u00e4\\u20ac \\\"q\\\"\"}", "red \xc3\xa4\xe2\x82\xac \"q\""));
    CHECK(is("{\"text\":\"\xc2\xa7" "cX\"}", "\xc2\xa7" "cX", false));
    CHECK(is("{\"text\":\"\xc2\xa7" "cX\"}", "X", true));
    CHECK(is("{\"text\":\"x\",\"hoverEvent\":{\"action\":\"show_text\",\"value\":{\"text\":\"ignored\"}},\"extra\":[]}", "x"));
    CHECK(is("{broken", ""));
    char tiny[4]; CHECK(chatToText("{\"text\":\"abcdefgh\"}", 19, tiny, sizeof tiny) == 3 && !strcmp(tiny, "abc"));   // truncation is safe
    for (int i = 0; i < 20000; ++i) { char junk[64], o[32]; for (auto& c : junk) c = "{}[]\",:\\u0text"[rand() % 15]; chatToText(junk, sizeof junk, o, sizeof o); }
}
int main() { RUN(test_chat); DONE(); }
