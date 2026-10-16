# `chat` and `nbt`

## `chat_to_text`

Chat messages arrive as JSON text components (`{"text":"hi","extra":[…]}`, `{"translate":"chat.type.text","with":[…]}`).
`chat_to_text` flattens one to plain UTF-8 without allocating.

```rust
pub fn chat_to_text(json: &[u8], out: &mut [u8], strip_formatting: bool) -> usize
```

- Handles `text`, `extra`, `translate` + `with` (common vanilla translation keys such as chat, join/leave, death
  messages, `/say`, `/me`), nested arrays and plain JSON strings.
- `strip_formatting = true` removes `§x` formatting codes.
- Returns the number of bytes written (not counting the NUL); `out` is always NUL-terminated if it is not empty. Output
  longer than `out` is cut off.
- Uses about 512 bytes of stack.

```rust
let mut buf = [0u8; 256];
let n = mcscon::chat::chat_to_text(chat.message.as_bytes(), &mut buf, true);
println!("{}", String::from_utf8_lossy(&buf[..n]));
```

`Session` does this for you and passes the result as the `text` argument of `SessionSink::chat` (up to 511 bytes) along
with the original JSON.

## `nbt`

A zero-copy reader for NBT blobs, such as `Slot::nbt` (item tags), tile entities and some packet fields. The bytes were
validated when the packet was decoded, so iteration cannot run out of bounds. Tag constants: `END`, `BYTE`, `SHORT`,
`INT`, `LONG`, `FLOAT`, `DOUBLE`, `BYTE_ARRAY`, `STRING`, `LIST`, `COMPOUND`, `INT_ARRAY`, `LONG_ARRAY`.

| Item | Description |
|------|-------------|
| `root(Bytes) -> Option<(Compound, Str)>` | the root compound and its name |
| `path(Bytes, "a/b/c") -> Option<Entry>` | look up a tag through nested compounds |
| `Compound::next_entry() -> Option<Entry>` | iterate the entries of a compound |
| `Compound::find(name) -> Option<Entry>` | find an entry by name (a copy of the compound is scanned, the original is not consumed) |
| `List { elem_type, count, .. }` with `next_entry()` | iterate a list |
| `Entry { kind, name, value }` | one tag: `kind` is a tag constant, `name` its name, `value` a `Reader` at the payload |
| `Entry::is_number()`, `as_int() -> i64`, `as_float() -> f64` | numeric tags (byte, short, int, long; float, double) |
| `as_string() -> Str`, `as_compound()`, `as_list()`, `as_bytes() -> Bytes`, `array_length() -> i32` | other payloads |

```rust
// the display name of an item in a SetSlot / SessionSink::slot callback
fn item_name<'a>(item: &mcscon::wire::Slot<'a>) -> Option<mcscon::wire::Str<'a>> {
    let e = mcscon::nbt::path(item.nbt, "display/Name")?;
    Some(e.as_string())
}
```
