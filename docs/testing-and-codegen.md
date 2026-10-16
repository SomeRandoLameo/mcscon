# Testing and code generation

## Running the tests

```
cargo test
```

No network and no server needed.

| Test | What it checks |
|------|----------------|
| unit tests in `src/` | inflate (stored block, garbage rejected, output overflow), wire (VarInt round trip, reader overrun, position packing, string limits), the arena (alloc/release/coalescing), the client with a scripted transport (login flow and framing, switch to Play, keep-alive echo, oversized frames skipped), chat flattening (plain/extra, translations, escapes, malformed input), NBT lookup and paths |
| `tests/vectors.rs` | the wire vectors in `tests/data/`: **every** clientbound decoder must consume each reference packet to the last byte, a negative control (flipped layouts must fail), truncation and corruption fuzzing, and zlib vectors (inflate output verified by hash) |
| `tests/sb.rs` | every serverbound encoder produces the exact bytes hand-computed from the protocol, for each version |
| `tests/world.rs` | chunk decoding, block/multi-block changes, unload, memory exhaustion degrades gracefully |
| `tests/modules.rs` | physics (including shapes, water and ladders), entity tracker, tab list, inventory and windows, and a `Session` scenario |

### The C and C++ examples

```bash
make -C examples        # builds libmcscon.a (feature capi), bot_c (C99, -Wall -Wextra) and bot_cpp (C++11)
```

This checks that `include/mcscon.h` and `include/mcscon.hpp` compile cleanly against the current library and that the
library links into a C and a C++ program. Running them needs a server (below).

### Lints and API docs

```bash
cargo clippy --all-targets --features capi
cargo doc --no-deps --features capi --open
cargo build --no-default-features      # checks that the core still builds as no_std
```

### Wire vectors

`tests/data/packet_vectors.bin` contains random but valid clientbound packets (several per packet type and version)
produced by an **independent** Python encoder (`tools/gen_vectors.py`) that walks the raw minecraft-data schema. The Rust
decoders are generated from the same schema by a different program, so agreement between the two is meaningful.
`tests/data/zlib_vectors.bin` holds zlib streams (stored, fixed, dynamic, large) with hashes of their plaintext.

### Trying against a real server

Start a vanilla 1.10 – 1.12.2 server with `online-mode=false` and run the examples:

```bash
cargo run --release --example bot -- 127.0.0.1 25565
examples/build/bot_c 127.0.0.1 25565
examples/build/bot_cpp 127.0.0.1 25565
```

All three should print the chat, say hello, walk for three seconds and print a position that differs from the spawn
point. The earlier real-server integration suite (scenario, packet
coverage and edge-case runs against vanilla 1.10.2 – 1.12.2) lives in the git history.

## Code generation

Large parts of the library are generated from [PrismarineJS/minecraft-data](https://github.com/PrismarineJS/minecraft-data)
so that no packet id or layout is typed by hand. The generated files are checked in; regenerate them when you add a
protocol version or fix a generator. You need Python 3 and a checkout of minecraft-data (`MD=<checkout>/data/pc`).

| Command | Writes |
|---------|--------|
| `python3 tools/gen_ids.py $MD > src/ids.rs` | packet id enums and wire-id tables per version |
| `(cd tools && python3 gen_packets_rs.py $MD ../src/packets)` | `src/packets/cb.rs`, `src/packets/sb.rs` (all packet structs, decoders, encoders) |
| `python3 tools/gen_blocks.py $MD > src/blocks.rs` | block flags, collision shapes, names |
| `python3 tools/gen_vectors.py $MD tests/data/packet_vectors.bin` | the packet test vectors |
| `python3 tools/gen_testvec.py tests/data/zlib_vectors.bin` | the zlib test vectors |
| `python3 tools/gen_docs.py > docs/packet-reference.md` | this documentation's packet reference (reads the generated Rust files, no minecraft-data needed) |

Everything else in `docs/` is written by hand; when you change a public API, update the matching page (the
[index](README.md) lists which page covers which module, and the C/C++ pages mirror `include/`).

`tools/gen_packets.py` is the shared schema parser and layout merger imported by `gen_packets_rs.py`.

### Adding a protocol version

1. Add a row to `VERS` in `tools/gen_ids.py` and the matching entries in the packet generator (`tools/gen_packets.py`).
2. Add the variant to `Version` (`src/types.rs`), its protocol number and `table_index`.
3. Regenerate, extend the test vectors, run `cargo test`, and test with a real server of that version.
