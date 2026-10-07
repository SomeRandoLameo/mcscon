# mcscon – Plan

Ziel: minimale, embeddable C++ Minecraft-Java-Client-Protokollschicht (Vorbild: plushmonkey/mclib), die auf
beliebigen Plattformen (bis hin zum 3DS) läuft. Keine curl/OpenSSL, kein Heap, keine Exceptions/RTTI, C++11.

## Designprinzipien
- **Transport-Abstraktion**: 4 Funktionspointer (`connect/recv/send/close`) in `transport.h`. Alles andere ist portabel.
- **Kein dynamischer Speicher**: Caller gibt rx/scratch/tx-Puffer. Zu große Frames werden gestreamt verworfen (`PacketTooLarge`).
- **Zero-copy**: Decoder liefern `StrView` in den rx-Puffer.
- **Versionen via Daten**: Packet-IDs werden aus PrismarineJS/minecraft-data generiert (`tools/gen_ids.py`), nie von Hand.
  Neue Version = neue Zeile in `VERS` + ggf. Layout-Unterschiede in `packets.h`.
- **Kein Deflate nötig**: Senden nutzt immer "unkomprimiert" (dataLength=0), das ist protokollkonform. Nur Inflate (~150 Zeilen).

## Phasen
| # | Inhalt | Status |
|---|--------|--------|
| 0 | Referenz mclib + minecraft-data analysiert, Architektur | fertig |
| 1 | Reader/Writer (VarInt, Strings, Position), zlib-Inflate, Tests + Fuzz | fertig |
| 2 | Transport, Framing, Kompression, Handshake/Status/Login (offline), Keepalive, ID-Tabellen 1.10–1.12.2 | fertig |
| 3 | Typisierte Play-Pakete (Kern-Set) + Sende-Helfer; POSIX-Transport; Loopback-TCP-Test; CLI-Beispiel | fertig |
| 4 | Vollständige Packet-Abdeckung 1.10–1.12.2: alle 80 Clientbound-Decoder + 34 Serverbound-Encoder, generiert (`tools/gen_packets.py`), Versions-Layouts zur Laufzeit gewählt; Verifikation durch unabhängigen Python-Referenz-Encoder (11.208 Zufallspakete, exakter Byte-Verbrauch, Negativkontrolle, Fuzz) + handgerechnete Encoder-Bytes | fertig |
| 5 | Optionale Module (`libmcscon_modules`): NBT-Reader, Chunk-/Weltmodell (Sektionen paletted im Arena-Speicher, optional Licht/Biome), exakte Kollisionsshapes pro Blockstate, Entity-Tracker + Tab-Liste, Inventar/Fenster/Klicks, Chat-JSON→Text, Vanilla-Physik, `Session` (Spielerzustand, Teleport-/Respawn-/Resourcepack-Automatik). 55 Unit-Checks + Szenario-Test gegen echte Server (34 Checks × 3 Versionen) | fertig |
| 6 | Real-Server-Test gegen Vanilla 1.10.2/1.11.2/1.12.2 (nur 127.0.0.1, `testserver/`, `tests/integ_server.cpp`): Status, Login, Kompression, Join, Position, Chat-Echo, Keepalive; alle empfangenen Pakete dekodieren | fertig |
| 7 | Online-Mode: AES-128-CFB8, RSA (PKCS#1 v1.5), SHA-1 (eigenes, minimales Crypto), Session-Join-Request über Transport-Abstraktion (HTTPS/TLS als Port-Aufgabe) | offen |
| 8 | Microsoft/Xbox/Minecraft-Auth (Device-Code-Flow) | offen |
| 9 | Ports: Windows (winsock), 3DS (soc:u), Freestanding-Build-Check | offen |
| 10 | Weitere Protokollversionen (generischer Ansatz: Codegen aus minecraft-data) | später |

## Bekannte Lücken
- Inflate ist bit-seriell (klein, dafür langsamer als zlib). Bei Bedarf Lookup-Tabellen nachrüsten.
- Komprimierte Frames müssen komplett in `rx` passen (kein Streaming-Inflate). Chunk-Daten brauchen also größere Puffer.
- `connect()` im Transport ist blockierend; ein nicht-blockierender Variant folgt mit den Ports.

## Codegen (Regenerieren)
```
python3 tools/gen_ids.py <minecraft-data>/data/pc > include/mcscon/ids_gen.h
python3 tools/gen_packets.py <minecraft-data>/data/pc include/mcscon
python3 tools/gen_vectors.py <minecraft-data>/data/pc > tests/packet_vectors.h
```
Clientbound = Decoder (`cb::X`, Arrays/Metadata als Zero-Copy-Views, vorab validiert), Serverbound = Encoder (`sb::X`, Arrays als `Span`).
Nutzung: `cb::Chat m; if (pkt::read(packet, m)) ...` / `sb::Chat m = {}; pkt::send(client, m);`

## Integrationstest gegen echte Server (nur lokal)
```
testserver/run.sh 1.12.2            # bindet nur an 127.0.0.1 (Ports 25610/25611/25612 für 1.10.2/1.11.2/1.12.2)
build/integ_server 25612 1.12.2     # MCSCON_SECS=40 für Keepalive-Test
```

## Phase-5-Design-Notizen
- **Speicher**: `StaticStorage<Columns, ArenaBytes, Entities, Players>` (alles in einem Objekt, kein Heap). Beispiel 81 Spalten ≈ 400 KB Arena; echter Flat-Welt-Test belegt ~170 KB.
- **Inventar**: serverautoritativ ohne Client-Vorhersage. Klicks senden bewusst ein unmögliches „clicked item", wodurch Vanilla mit `Transaction(false)` + Vollsync antwortet (1.10–1.12.2 getestet). Details in `src/inventory.cpp`.
- **Physik**: Vanilla-Konstanten (Gravitation 0.08, Drag 0.98, Reibung, Sprung 0.42, Step 0.6, Wasser, Leitern) + exakte Kollisionsboxen (`tools/gen_blocks.py`). Vom Server ohne Rubberbanding akzeptiert.
- **Tests**: `tools/integ_session.py <1.10.2|1.11.2|1.12.2>` startet den lokalen Server (frische Welt), steuert ihn per Konsole und prüft Welt/Entities/Inventar/Chat/Tod/Respawn/Physik.
## Weitere Generatoren
```
python3 tools/gen_blocks.py <minecraft-data>/data/pc > include/mcscon/blocks_gen.h
```

## Verifikation (alles gegen echte lokale Vanilla-Server 1.10.2, 1.11, 1.11.2, 1.12, 1.12.1, 1.12.2 – nur 127.0.0.1)
```
python3 tools/verify_all.py            # Build + ctest + je Version: Szenario, Paket-Coverage, Edge-Cases (+ Online-Mode-Server)
python3 tools/integ_session.py 1.12.2  # 34 Checks: Welt, Physik, Entities, Inventar, Fenster, Chat, Tod/Respawn
python3 tools/integ_coverage.py 1.12.2 # zwei Bots, ~100 Konsolenbefehle, jedes clientbound-Paket wird dekodiert
python3 tools/join_stress.py 1.12.2 25612 6   # wiederholte Joins (Keepalive-Fenster)
```
Bekannt/erklärt:
- Server-Log `moved too quickly!` nach `/tp` über ~10 Blöcke ist normales Vanilla-Verhalten (gleiche Bestätigungsfolge wie beim Vanilla-Client); der Bot landet exakt auf dem Ziel.
- Nicht auslösbar mit Vanilla ohne Plugin: `PlayerlistHeader`, `Entity` (nur per Vektor-Tests abgedeckt).
- Blöcke außerhalb des Weltrands sind für Interaktionen gesperrt (Testszenario setzt den Rand zurück).
