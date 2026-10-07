#pragma once
#include "buffer.h"
#include "ids_gen.h"
#include "transport.h"

namespace mc {

// Caller-provided memory. Nothing is ever allocated by the library.
//  rx      : receive window. Must hold the largest *wire* frame you want to see.
//  scratch : inflate target; only used when the server enables compression. Must hold the largest
//            *decompressed* packet you want to see (may be null for uncompressed-only use).
//  tx      : outgoing queue.
// Frames that do not fit are skipped (streamed away) and reported as Error::PacketTooLarge.
struct Buffers { u8* rx; size_t rxCap; u8* scratch; size_t scratchCap; u8* tx; size_t txCap; };

// One received packet. `id` is the version-agnostic id; cast it to the enum of `state`:
//   Status -> ids::StatusCb, Login -> ids::LoginCb, Play -> ids::PlayCb.
// `r` is positioned at the payload and is only valid inside the callback.
struct Packet { State state; u8 id; i32 wireId; Version version; Reader r; };

typedef void (*PacketFn)(void* user, const Packet& pkt);
typedef void (*ErrorFn)(void* user, Error err, i32 info);

class Client {
public:
    Client(const Transport& t, const Buffers& b, Version v);

    void onPacket(PacketFn fn, void* user) { packetFn_ = fn; user_ = user; }
    void onError(ErrorFn fn) { errorFn_ = fn; }
    // Reply to server keep-alives automatically (default on).
    void autoKeepAlive(bool on) { autoKeepAlive_ = on; }

    // Offline-mode login. Returns false if the transport could not connect.
    bool connectLogin(const char* host, u16 port, const char* username);
    // Server list ping: after this, expect StatusCb::ServerInfo, then call sendStatusPing().
    bool connectStatus(const char* host, u16 port);
    void sendStatusPing(i64 token);
    void disconnect();

    // Pump: flush tx, read the transport, dispatch every complete packet.
    // Returns the number of packets dispatched, or -1 when the connection is closed.
    int poll();

    State state() const { return state_; }
    Version version() const { return version_; }
    i32 compressionThreshold() const { return threshold_; }

    // --- sending (Play state) ---
    // Build in place: w = begin(id); w.varint(..); ...; commit(w).
    Writer begin(u8 wireId);
    bool commit(Writer& w);
    // Version-agnostic serverbound ids; returns Writer in error state if the packet does not exist.
    Writer begin(ids::PlaySb id);
    bool sendRaw(u8 wireId, const void* payload, size_t len);

private:
    friend struct ClientImpl;
    bool handshake(const char* host, u16 port, i32 next);
    bool flush();
    void fail(Error e, i32 info = 0);
    void dispatch(const u8* body, size_t len);
    void internalLogin(const Packet& p);

    Transport t_; Buffers b_; Version version_; State state_;
    PacketFn packetFn_; ErrorFn errorFn_; void* user_;
    size_t rh_, rt_, th_, tt_;       // rx head/tail, tx head/tail
    size_t skip_;                    // bytes of an oversized frame still to discard
    i32 threshold_;                  // <0: compression off
    bool autoKeepAlive_;
    u8* building_;                   // start of the frame under construction (null if none)
};

} // namespace mc
