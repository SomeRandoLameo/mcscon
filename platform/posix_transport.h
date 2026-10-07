// BSD-socket Transport (Linux, macOS, BSD, Android, 3DS via devkitARM newlib/soc: define MCSCON_3DS_SOC).
#pragma once
#include <mcscon/transport.h>
namespace mc {
struct PosixSocket { int fd; };
// `s` must outlive the Transport.
Transport makePosixTransport(PosixSocket* s);
} // namespace mc
