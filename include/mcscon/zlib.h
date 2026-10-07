#pragma once
#include "types.h"
namespace mc {
// Inflate a zlib stream (RFC1950, no preset dictionary). Needs ~2.4KB stack, no heap.
// Returns decompressed size, or -1 on corrupt data / output overflow.
// The Adler-32 trailer is verified only if `verify` is set.
i32 zlibInflate(const u8* in, size_t inLen, u8* out, size_t outCap, bool verify = false);
} // namespace mc
