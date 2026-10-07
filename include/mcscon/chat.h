#pragma once
#include "types.h"
namespace mc {
// Flattens a Minecraft chat component (JSON) into plain UTF-8 text: text/extra/translate/with, nested arrays,
// common translation keys, and optionally strips section-sign (§) formatting codes. No allocation, ~512 B stack.
// Returns the number of bytes written (excluding the NUL; the output is always NUL-terminated if cap > 0).
size_t chatToText(const char* json, size_t len, char* out, size_t cap, bool stripFormatting = true);
} // namespace mc
