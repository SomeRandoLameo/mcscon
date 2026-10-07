#include <mcscon/chat.h>
#include <string.h>

namespace mc {
namespace {

struct Out {
    char* p; size_t cap, n; bool strip; bool skipNext;
    void put(char c) {
        if (strip) { if (skipNext) { skipNext = false; return; } }
        if (n + 1 < cap) p[n++] = c;
    }
    void putRaw(char c) { if (n + 1 < cap) p[n++] = c; }
    void text(const char* s, size_t l) {
        for (size_t i = 0; i < l; ++i) {
            if (strip && (unsigned char)s[i] == 0xC2 && i + 1 < l && (unsigned char)s[i + 1] == 0xA7) { ++i; skipNext = true; continue; }   // "§"
            put(s[i]);
        }
    }
};

struct Json {
    const char* p; const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p; }
};

// Reads a JSON string at p (positioned on the opening quote), writing decoded UTF-8 via `sink`.
template<class F> bool readString(Json& j, F sink) {
    if (j.p >= j.e || *j.p != '"') return false;
    ++j.p;
    while (j.p < j.e && *j.p != '"') {
        char c = *j.p++;
        if (c != '\\') { sink(c); continue; }
        if (j.p >= j.e) return false;
        char x = *j.p++;
        switch (x) {
        case 'n': sink('\n'); break; case 't': sink('\t'); break; case 'r': sink('\r'); break;
        case 'b': case 'f': break;
        case 'u': {
            if (j.e - j.p < 4) return false;
            unsigned cp = 0; for (int i = 0; i < 4; ++i) { char h = *j.p++; cp = cp * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
            if (cp < 0x80) sink((char)cp);
            else if (cp < 0x800) { sink((char)(0xC0 | (cp >> 6))); sink((char)(0x80 | (cp & 63))); }
            else { sink((char)(0xE0 | (cp >> 12))); sink((char)(0x80 | ((cp >> 6) & 63))); sink((char)(0x80 | (cp & 63))); }
            break;
        }
        default: sink(x);
        }
    }
    if (j.p >= j.e) return false;
    ++j.p; return true;
}

void skipValue(Json& j) {
    j.ws(); if (j.p >= j.e) return;
    if (*j.p == '"') { readString(j, [](char) {}); return; }
    if (*j.p == '{' || *j.p == '[') {
        int depth = 0;
        while (j.p < j.e) {
            char c = *j.p;
            if (c == '"') { readString(j, [](char) {}); continue; }
            ++j.p;
            if (c == '{' || c == '[') ++depth; else if (c == '}' || c == ']') { if (--depth == 0) return; }
        }
        return;
    }
    while (j.p < j.e && *j.p != ',' && *j.p != '}' && *j.p != ']') ++j.p;
}

struct Tr { const char* key; const char* fmt; };
const Tr kTr[] = {
    {"chat.type.text", "<%s> %s"}, {"chat.type.emote", "* %s %s"}, {"chat.type.announcement", "[%s] %s"}, {"chat.type.admin", "[%s: %s]"},
    {"chat.type.team.text", "%s <%s> %s"}, {"commands.message.display.incoming", "%s whispers to you: %s"},
    {"multiplayer.player.joined", "%s joined the game"}, {"multiplayer.player.left", "%s left the game"},
    {"death.attack.player", "%s was slain by %s"}, {"death.attack.mob", "%s was slain by %s"}, {"death.attack.generic", "%s died"},
    {"death.attack.fall", "%s hit the ground too hard"}, {"death.attack.lava", "%s tried to swim in lava"}, {"death.attack.drown", "%s drowned"},
    {"death.attack.inFire", "%s went up in flames"}, {"death.attack.arrow", "%s was shot by %s"}, {"death.attack.outOfWorld", "%s fell out of the world"},
    {"multiplayer.disconnect.kicked", "Kicked by an operator"}, {"multiplayer.disconnect.banned", "You are banned from this server"},
    {"multiplayer.disconnect.server_shutdown", "Server closed"}, {"disconnect.spam", "Kicked for spamming"},
    {"disconnect.timeout", "Timed out"}, {"disconnect.closed", "Connection closed"}, {"disconnect.lost", "Connection Lost"},
    {"multiplayer.disconnect.idling", "You have been idle for too long!"}, {"multiplayer.disconnect.not_whitelisted", "You are not white-listed on this server!"},
};

bool render(Json& j, Out& o, int depth);

// object -> output
bool renderObject(Json& j, Out& o, int depth) {
    ++j.p;   // '{'
    char trKey[64] = {0}; size_t trLen = 0; Json with = {0, 0}, extra = {0, 0};
    for (;;) {
        j.ws(); if (j.p >= j.e) return false;
        if (*j.p == '}') { ++j.p; break; }
        if (*j.p == ',') { ++j.p; continue; }
        char key[16]; size_t kn = 0;
        if (!readString(j, [&](char c) { if (kn + 1 < sizeof key) key[kn++] = c; })) return false;
        key[kn] = 0; j.ws(); if (j.p >= j.e || *j.p != ':') return false; ++j.p; j.ws();
        if (!strcmp(key, "text") && *j.p == '"') { readString(j, [&](char c) { o.putRaw(c); }); }   // raw to keep UTF-8 intact; formatting stripped below
        else if (!strcmp(key, "translate") && *j.p == '"') { readString(j, [&](char c) { if (trLen + 1 < sizeof trKey) trKey[trLen++] = c; }); }
        else if (!strcmp(key, "with")) { with.p = j.p; skipValue(j); with.e = j.p; }
        else if (!strcmp(key, "extra")) { extra.p = j.p; skipValue(j); extra.e = j.p; }
        else skipValue(j);
    }
    if (trLen) {
        const char* fmt = 0; for (const Tr& t : kTr) if (!strcmp(t.key, trKey)) { fmt = t.fmt; break; }
        // collect args (raw ranges)
        Json args[4]; int na = 0;
        if (with.p && *with.p == '[') {
            Json w = with; ++w.p;
            while (na < 4) { w.ws(); if (w.p >= w.e || *w.p == ']') break; if (*w.p == ',') { ++w.p; continue; } args[na].p = w.p; skipValue(w); args[na].e = w.p; ++na; }
        }
        if (!fmt) {                                          // unknown key: "key[arg, arg]"
            for (const char* k = trKey; *k; ++k) o.put(*k);
            for (int i = 0; i < na; ++i) { o.put(i ? ',' : '['); Json a = args[i]; render(a, o, depth + 1); }
            if (na) o.put(']');
        } else {
            int ai = 0;
            for (const char* f = fmt; *f; ++f) {
                if (f[0] == '%' && f[1] == 's') { if (ai < na) { Json a = args[ai++]; render(a, o, depth + 1); } ++f; }
                else o.put(*f);
            }
        }
    }
    if (extra.p && *extra.p == '[') { Json x = extra; render(x, o, depth + 1); }
    return true;
}

bool render(Json& j, Out& o, int depth) {
    if (depth > 12) return false;
    j.ws(); if (j.p >= j.e) return false;
    if (*j.p == '"') { return readString(j, [&](char c) { o.putRaw(c); }); }
    if (*j.p == '{') return renderObject(j, o, depth);
    if (*j.p == '[') {
        ++j.p;
        for (;;) { j.ws(); if (j.p >= j.e) return false; if (*j.p == ']') { ++j.p; return true; } if (*j.p == ',') { ++j.p; continue; } const char* before = j.p; if (!render(j, o, depth + 1) || j.p == before) return false; }
    }
    skipValue(j); return true;   // numbers / booleans
}

} // namespace

size_t chatToText(const char* json, size_t len, char* out, size_t cap, bool strip) {
    if (!cap) return 0;
    Out o = {out, cap, 0, false, false};
    Json j = {json, json + len};
    // Render into `out`, then strip formatting codes in a second in-place pass (codes can be split across components).
    render(j, o, 0);
    out[o.n] = 0;
    if (strip) {
        size_t w = 0;
        for (size_t r = 0; r < o.n; ++r) {
            if ((unsigned char)out[r] == 0xC2 && r + 2 < o.n + 0 && (unsigned char)out[r + 1] == 0xA7) { r += 2; continue; }
            out[w++] = out[r];
        }
        out[w] = 0; return w;
    }
    return o.n;
}

} // namespace mc
