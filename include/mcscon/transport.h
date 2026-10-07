#pragma once
#include "types.h"
namespace mc {

// The only thing a port has to implement: a byte pipe. Plain C-style function table so it can be
// backed by BSD sockets, winsock, 3DS soc:u, lwIP, a TLS tunnel, a WebSocket, a test pipe, ...
struct Transport {
    void* ctx;
    // Establish the connection (may block). Return true on success.
    bool (*connect)(void* ctx, const char* host, u16 port);
    // Non-blocking read. >0 bytes read, 0 nothing available right now, <0 closed or failed.
    int  (*recv)(void* ctx, u8* buf, size_t cap);
    // Non-blocking write. >0 bytes accepted (may be partial), 0 would block, <0 failed.
    int  (*send)(void* ctx, const u8* buf, size_t len);
    void (*close)(void* ctx);
};

} // namespace mc
