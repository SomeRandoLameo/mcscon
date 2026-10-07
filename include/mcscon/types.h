// mcscon - minimal embeddable Minecraft Java client protocol layer.
// Freestanding-friendly: no exceptions, no RTTI, no heap, no <string>/<vector>.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {
typedef uint8_t u8;   typedef int8_t i8;
typedef uint16_t u16; typedef int16_t i16;
typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t u64; typedef int64_t i64;

// Supported protocol versions. 1.12 and 1.12.1 differ in id tables, 1.12.1/1.12.2 in keepalive width.
enum class Version : u8 { V1_10, V1_11, V1_11_2, V1_12, V1_12_1, V1_12_2 };

inline i32 protocolNumber(Version v) {
    static const i32 n[] = {210, 315, 316, 335, 338, 340};
    return n[(int)v];
}
// index into the generated id tables (V1_10, V1_11, V1_12, V1_12_2)
inline int tableIndex(Version v) {
    static const u8 t[] = {0, 1, 1, 2, 3, 3};
    return t[(int)v];
}

enum class State : u8 { Handshake, Status, Login, Play, Closed };

enum class Error : u8 {
    None, Transport, Protocol, Overflow, PacketTooLarge, Inflate, Disconnected,
    EncryptionRequired,  // server is in online mode; not supported yet
    Backpressure
};
} // namespace mc
