#include <mcscon/client.h>
#include <mcscon/zlib.h>

namespace mc {

static const size_t kHdr = 6;   // max frame header we prepend: 5 (length) + 1 (zero data-length)

Client::Client(const Transport& t, const Buffers& b, Version v)
    : t_(t), b_(b), version_(v), state_(State::Closed), packetFn_(0), errorFn_(0), user_(0),
      rh_(0), rt_(0), th_(0), tt_(0), skip_(0), threshold_(-1), autoKeepAlive_(true), building_(0) {}

void Client::fail(Error e, i32 info) {
    if (errorFn_) errorFn_(user_, e, info);
}

// ---------------------------------------------------------------- sending

bool Client::flush() {
    while (th_ < tt_) {
        int n = t_.send(t_.ctx, b_.tx + th_, tt_ - th_);
        if (n < 0) { fail(Error::Transport); disconnect(); return false; }
        if (n == 0) break;
        th_ += (size_t)n;
    }
    if (th_ == tt_) th_ = tt_ = 0;
    return true;
}

Writer Client::begin(u8 wireId) {
    if (th_ == tt_) th_ = tt_ = 0;
    else if (tt_ + kHdr + 8 > b_.txCap && th_ > 0) {        // compact
        memmove(b_.tx, b_.tx + th_, tt_ - th_); tt_ -= th_; th_ = 0;
    }
    if (tt_ + kHdr >= b_.txCap) { building_ = 0; return Writer(); }
    building_ = b_.tx + tt_;
    Writer w(building_ + kHdr, b_.txCap - tt_ - kHdr);
    w.varint(wireId);
    return w;
}

Writer Client::begin(ids::PlaySb id) {
    const ids::Tables& t = ids::PlaySb_tables[tableIndex(version_)];
    u8 wire = (u8)id < t.a2wLen ? t.a2w[(u8)id] : 0xFF;
    if (wire == 0xFF) { building_ = 0; return Writer(); }
    return begin(wire);
}

bool Client::commit(Writer& w) {
    u8* start = building_; building_ = 0;
    if (!start || w.err || state_ == State::Closed) { if (start) fail(Error::Overflow); return false; }
    size_t inner = (size_t)(w.p - (start + kHdr));
    bool comp = threshold_ >= 0;
    size_t lenField = inner + (comp ? 1 : 0);
    size_t hlen = varintSize((u32)lenField) + (comp ? 1 : 0);
    Writer h(start + kHdr - hlen, hlen);
    h.varint((i32)lenField); if (comp) h.u8_(0);
    memmove(start, start + kHdr - hlen, hlen + inner);
    tt_ += hlen + inner;
    return flush();
}

bool Client::sendRaw(u8 wireId, const void* payload, size_t len) {
    Writer w = begin(wireId); w.bytes(payload, len); return commit(w);
}

bool Client::handshake(const char* host, u16 port, i32 next) {
    if (!t_.connect(t_.ctx, host, port)) { fail(Error::Transport); return false; }
    rh_ = rt_ = th_ = tt_ = skip_ = 0; threshold_ = -1;
    state_ = State::Handshake;
    Writer w = begin((u8)0x00);   // Handshake SetProtocol
    w.varint(protocolNumber(version_)); w.str(host); w.u16_(port); w.varint(next);
    if (!commit(w)) return false;
    state_ = next == 1 ? State::Status : State::Login;
    return true;
}

bool Client::connectLogin(const char* host, u16 port, const char* username) {
    if (!handshake(host, port, 2)) return false;
    Writer w = begin((u8)0x00);   // LoginStart
    w.str(username);
    return commit(w);
}

bool Client::connectStatus(const char* host, u16 port) {
    if (!handshake(host, port, 1)) return false;
    return sendRaw(0x00, 0, 0);   // status request
}

void Client::sendStatusPing(i64 token) {
    Writer w = begin((u8)0x01); w.i64_(token); commit(w);
}

void Client::disconnect() {
    if (state_ != State::Closed) { state_ = State::Closed; t_.close(t_.ctx); }
}

// ---------------------------------------------------------------- receiving

void Client::internalLogin(const Packet& p) {
    Reader r = p.r;
    switch ((ids::LoginCb)p.id) {
    case ids::LoginCb::Compress: { i32 th = r.varint(); if (r.ok()) threshold_ = th; else fail(Error::Protocol); break; }
    case ids::LoginCb::Success: state_ = State::Play; break;
    case ids::LoginCb::EncryptionBegin: fail(Error::EncryptionRequired); break;
    case ids::LoginCb::Disconnect: fail(Error::Disconnected); break;
    default: break;
    }
}

void Client::dispatch(const u8* body, size_t len) {
    Reader r(body, len);
    i32 wire = r.varint();
    if (!r.ok()) { fail(Error::Protocol); return; }
    const ids::Tables* tab = 0;
    int ti = tableIndex(version_);
    switch (state_) {
    case State::Status: tab = &ids::StatusCb_tables[ti]; break;
    case State::Login:  tab = &ids::LoginCb_tables[ti]; break;
    case State::Play:   tab = &ids::PlayCb_tables[ti]; break;
    default: return;
    }
    u8 id = (wire >= 0 && wire < tab->w2aLen) ? tab->w2a[wire] : 0xFF;
    Packet p; p.state = state_; p.id = id; p.wireId = wire; p.version = version_; p.r = r;
    if (state_ == State::Login) internalLogin(p);
    else if (state_ == State::Play) {
        if (id == (u8)ids::PlayCb::KeepAlive && autoKeepAlive_) {
            u8 echo[10]; size_t n = r.left() < sizeof echo ? r.left() : sizeof echo;
            memcpy(echo, r.p, n);
            const ids::Tables& st = ids::PlaySb_tables[ti];
            sendRaw(st.a2w[(u8)ids::PlaySb::KeepAlive], echo, n);
        } else if (id == (u8)ids::PlayCb::KickDisconnect) {
            fail(Error::Disconnected);
        }
    }
    if (packetFn_) packetFn_(user_, p);
    if (state_ == State::Play && id == (u8)ids::PlayCb::KickDisconnect) disconnect();
    // Online-mode servers (encryption request) are not supported yet, and a login disconnect ends the connection: close cleanly.
    if (p.state == State::Login && (id == (u8)ids::LoginCb::EncryptionBegin || id == (u8)ids::LoginCb::Disconnect)) disconnect();
}

int Client::poll() {
    if (state_ == State::Closed) return -1;
    if (!flush()) return -1;

    // refill rx
    if (rh_ == rt_) rh_ = rt_ = 0;
    else if (rt_ == b_.rxCap && rh_ > 0) { memmove(b_.rx, b_.rx + rh_, rt_ - rh_); rt_ -= rh_; rh_ = 0; }
    if (rt_ < b_.rxCap) {
        int n = t_.recv(t_.ctx, b_.rx + rt_, b_.rxCap - rt_);
        if (n < 0) { fail(Error::Transport); disconnect(); return -1; }
        rt_ += (size_t)n;
    }

    int count = 0;
    while (state_ != State::Closed && rh_ < rt_) {
        if (skip_) {                                  // discarding an oversized frame
            size_t n = rt_ - rh_; if (n > skip_) n = skip_;
            rh_ += n; skip_ -= n; continue;
        }
        Reader r(b_.rx + rh_, rt_ - rh_);
        i32 len = r.varint();
        if (!r.ok()) {
            if (rt_ - rh_ >= 5) { fail(Error::Protocol); disconnect(); return -1; }
            break;                                    // length prefix incomplete
        }
        if (len <= 0) { fail(Error::Protocol); disconnect(); return -1; }
        size_t hdr = (size_t)(r.p - (b_.rx + rh_));
        size_t avail = rt_ - rh_ - hdr;
        if ((size_t)len > avail) {
            if ((size_t)len + hdr > b_.rxCap) {       // can never fit: stream it away
                fail(Error::PacketTooLarge, len);
                skip_ = (size_t)len - avail; rh_ = rt_; break;
            }
            // make room for the rest of this frame
            if (rh_ > 0 && rh_ + hdr + len > b_.rxCap) { memmove(b_.rx, b_.rx + rh_, rt_ - rh_); rt_ -= rh_; rh_ = 0; }
            break;
        }
        const u8* body = r.p; size_t blen = (size_t)len;
        rh_ += hdr + blen;                            // consume before dispatch (handler may send)
        if (threshold_ >= 0) {
            Reader br(body, blen);
            i32 dlen = br.varint();
            if (!br.ok() || dlen < 0) { fail(Error::Protocol); disconnect(); return -1; }
            body = br.p; blen = br.left();
            if (dlen > 0) {
                if (!b_.scratch || (size_t)dlen > b_.scratchCap) { fail(Error::PacketTooLarge, dlen); continue; }
                i32 n = zlibInflate(body, blen, b_.scratch, (size_t)dlen);
                if (n != dlen) { fail(Error::Inflate); disconnect(); return -1; }
                body = b_.scratch; blen = (size_t)n;
            }
        }
        dispatch(body, blen);
        ++count;
    }
    return state_ == State::Closed ? -1 : count;
}

} // namespace mc
