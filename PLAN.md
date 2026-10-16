# mcscon – Plan

Ziel: minimale, embeddable Minecraft-Java-Client-Protokollschicht (Vorbild: plushmonkey/mclib), die auf beliebigen
Plattformen (bis hin zum 3DS) läuft. `no_std`, kein Heap im Kern. Der Kern ist **Rust** (Repo-Root, `src/`); für C und
C++ gibt es eine kleine C-API (`include/mcscon.h`, `src/capi.rs`, Feature `capi`) plus einen C++-Wrapper (`include/mcscon.hpp`).

## Designprinzipien
- **Transport-Abstraktion**: `connect/recv/send/close` (Trait `Transport`). Alles andere ist portabel.
- **Kein dynamischer Speicher**: Caller gibt rx/scratch/tx-Puffer. Zu große Frames werden gestreamt verworfen (`PacketTooLarge`).
- **Zero-copy**: Decoder liefern Views (`Str`, `Bytes`, `MetaView`) in den rx-Puffer.
- **Versionen via Daten**: Packet-IDs und Layouts werden aus PrismarineJS/minecraft-data generiert (`tools/`), nie von Hand.
  Neue Version = neue Zeile in `VERS` + ggf. Layout-Unterschiede.
- **Kein Deflate nötig**: Senden nutzt immer "unkomprimiert" (dataLength=0). Nur Inflate (tabellengetrieben).

## Phasen
| # | Inhalt | Status |
|---|--------|--------|
| 0–6 | Analyse, Reader/Writer, Inflate, Client/Framing, Handshake/Status/Login (offline), Keepalive, alle Packets 1.10–1.12.2, Module (NBT, World, Entities, Inventar, Physik, Chat, Session), Real-Server-Tests gegen Vanilla 1.10.2–1.12.2 | fertig |
| 11 | **Rust-Kern** (alles oben in Rust), Rust-Tests mit gemeinsamen Wire-Vektoren (`tests/data/*.bin`) | fertig |
| 12 | Aufräumen: Rust im Root, kleine C-API, je ein identisches Beispiel in Rust, C und C++ (`examples/`) | fertig |
| 7 | Online-Mode: AES-128-CFB8, RSA (PKCS#1 v1.5), SHA-1 + signierter Hex-Digest, Session-Join-Request (HTTPS). RustCrypto-Crates (`no_std`) hinter einem Feature, HTTPS in einem eigenen `std`-Crate | offen |
| 8 | Microsoft/Xbox/Minecraft-Auth (Device-Code-Flow) | offen |
| 9 | Ports: Windows (winsock), 3DS (soc:u), Freestanding-Build-Check | offen |
| 10 | Weitere Protokollversionen (Codegen aus minecraft-data) | später |

## Bekannte Lücken
- Komprimierte Frames müssen komplett in `rx` passen (kein Streaming-Inflate). Chunk-Daten brauchen also größere Puffer.
- `connect()` im Transport ist blockierend; ein nicht-blockierender Variant folgt mit den Ports.
- Die Real-Server-Integrationstests (Vanilla 1.10.2–1.12.2: Szenario, Paket-Coverage, Edge-Cases) wurden beim Aufräumen
  entfernt; sie liegen im Git-Verlauf der C++-Version. Aktuell prüfen `cargo test` und die Beispiele gegen einen echten Server.

## Design-Notizen
- **Inventar**: serverautoritativ ohne Client-Vorhersage. Klicks senden bewusst ein unmögliches „clicked item", wodurch Vanilla mit
  `Transaction(false)` + Vollsync antwortet (1.10–1.12.2 getestet).
- **Physik**: Vanilla-Konstanten (Gravitation 0.08, Drag 0.98, Reibung, Sprung 0.42, Step 0.6, Wasser, Leitern) + exakte
  Kollisionsboxen (`tools/gen_blocks.py`). Vom Server ohne Rubberbanding akzeptiert.
- **Mathe**: `libm` statt Systembibliothek (deterministisch auf allen Plattformen, `no_std`).
- **Rust-API**: statt gespeicherter Funktionszeiger gibt es Sink-Traits (`SessionSink`, ...) und `poll(|link, event| ..)`.

## Codegen (Regenerieren)
```
python3 tools/gen_ids.py <minecraft-data>/data/pc > src/ids.rs
(cd tools && python3 gen_packets_rs.py <minecraft-data>/data/pc ../src/packets)
python3 tools/gen_blocks.py <minecraft-data>/data/pc > src/blocks.rs
python3 tools/gen_vectors.py <minecraft-data>/data/pc tests/data/packet_vectors.bin
python3 tools/gen_testvec.py tests/data/zlib_vectors.bin
```
