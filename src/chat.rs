//! Flattens a Minecraft chat component (JSON) into plain UTF-8 text: text/extra/translate/with, nested arrays,
//! common translation keys, and optionally strips section-sign (§) formatting codes. No allocation, ~512 B stack.

struct Out<'o> {
    p: &'o mut [u8],
    n: usize,
}
impl<'o> Out<'o> {
    fn put(&mut self, c: u8) {
        if self.n + 1 < self.p.len() {
            self.p[self.n] = c;
            self.n += 1;
        }
    }
}

#[derive(Clone, Copy)]
struct Json<'a> {
    s: &'a [u8],
    p: usize,
    e: usize,
}
impl<'a> Json<'a> {
    fn ws(&mut self) {
        while self.p < self.e && matches!(self.s[self.p], b' ' | b'\n' | b'\t' | b'\r') {
            self.p += 1;
        }
    }
    fn cur(&self) -> Option<u8> {
        if self.p < self.e {
            Some(self.s[self.p])
        } else {
            None
        }
    }
}

/// Reads a JSON string at `j.p` (positioned on the opening quote), feeding decoded UTF-8 bytes to `sink`.
fn read_string(j: &mut Json, mut sink: impl FnMut(u8)) -> bool {
    if j.cur() != Some(b'"') {
        return false;
    }
    j.p += 1;
    while j.p < j.e && j.s[j.p] != b'"' {
        let c = j.s[j.p];
        j.p += 1;
        if c != b'\\' {
            sink(c);
            continue;
        }
        if j.p >= j.e {
            return false;
        }
        let x = j.s[j.p];
        j.p += 1;
        match x {
            b'n' => sink(b'\n'),
            b't' => sink(b'\t'),
            b'r' => sink(b'\r'),
            b'b' | b'f' => {}
            b'u' => {
                if j.e - j.p < 4 {
                    return false;
                }
                let mut cp: u32 = 0;
                for _ in 0..4 {
                    let h = j.s[j.p];
                    j.p += 1;
                    cp = cp * 16 + if h <= b'9' { h.wrapping_sub(b'0') as u32 } else { ((h | 32).wrapping_sub(b'a') as u32).wrapping_add(10) };
                }
                if cp < 0x80 {
                    sink(cp as u8);
                } else if cp < 0x800 {
                    sink((0xC0 | (cp >> 6)) as u8);
                    sink((0x80 | (cp & 63)) as u8);
                } else {
                    sink((0xE0 | (cp >> 12)) as u8);
                    sink((0x80 | ((cp >> 6) & 63)) as u8);
                    sink((0x80 | (cp & 63)) as u8);
                }
            }
            _ => sink(x),
        }
    }
    if j.p >= j.e {
        return false;
    }
    j.p += 1;
    true
}

fn skip_value(j: &mut Json) {
    j.ws();
    let c = match j.cur() {
        Some(c) => c,
        None => return,
    };
    if c == b'"' {
        read_string(j, |_| {});
        return;
    }
    if c == b'{' || c == b'[' {
        let mut depth = 0;
        while j.p < j.e {
            let c = j.s[j.p];
            if c == b'"' {
                read_string(j, |_| {});
                continue;
            }
            j.p += 1;
            if c == b'{' || c == b'[' {
                depth += 1;
            } else if c == b'}' || c == b']' {
                depth -= 1;
                if depth == 0 {
                    return;
                }
            }
        }
        return;
    }
    while j.p < j.e && !matches!(j.s[j.p], b',' | b'}' | b']') {
        j.p += 1;
    }
}

const TR: &[(&str, &str)] = &[
    ("chat.type.text", "<%s> %s"),
    ("chat.type.emote", "* %s %s"),
    ("chat.type.announcement", "[%s] %s"),
    ("chat.type.admin", "[%s: %s]"),
    ("chat.type.team.text", "%s <%s> %s"),
    ("commands.message.display.incoming", "%s whispers to you: %s"),
    ("multiplayer.player.joined", "%s joined the game"),
    ("multiplayer.player.left", "%s left the game"),
    ("death.attack.player", "%s was slain by %s"),
    ("death.attack.mob", "%s was slain by %s"),
    ("death.attack.generic", "%s died"),
    ("death.attack.fall", "%s hit the ground too hard"),
    ("death.attack.lava", "%s tried to swim in lava"),
    ("death.attack.drown", "%s drowned"),
    ("death.attack.inFire", "%s went up in flames"),
    ("death.attack.arrow", "%s was shot by %s"),
    ("death.attack.outOfWorld", "%s fell out of the world"),
    ("multiplayer.disconnect.kicked", "Kicked by an operator"),
    ("multiplayer.disconnect.banned", "You are banned from this server"),
    ("multiplayer.disconnect.server_shutdown", "Server closed"),
    ("disconnect.spam", "Kicked for spamming"),
    ("disconnect.timeout", "Timed out"),
    ("disconnect.closed", "Connection closed"),
    ("disconnect.lost", "Connection Lost"),
    ("multiplayer.disconnect.idling", "You have been idle for too long!"),
    ("multiplayer.disconnect.not_whitelisted", "You are not white-listed on this server!"),
];

fn render_object(j: &mut Json, o: &mut Out, depth: i32) -> bool {
    j.p += 1; // '{'
    let mut tr_key = [0u8; 64];
    let mut tr_len = 0usize;
    let mut with: Option<(usize, usize)> = None;
    let mut extra: Option<(usize, usize)> = None;
    loop {
        j.ws();
        let c = match j.cur() {
            Some(c) => c,
            None => return false,
        };
        if c == b'}' {
            j.p += 1;
            break;
        }
        if c == b',' {
            j.p += 1;
            continue;
        }
        let mut key = [0u8; 16];
        let mut kn = 0;
        if !read_string(j, |c| {
            if kn + 1 < key.len() {
                key[kn] = c;
                kn += 1;
            }
        }) {
            return false;
        }
        let key = &key[..kn];
        j.ws();
        if j.cur() != Some(b':') {
            return false;
        }
        j.p += 1;
        j.ws();
        let is_str = j.cur() == Some(b'"');
        if key == b"text" && is_str {
            read_string(j, |c| o.put(c)); // raw to keep UTF-8 intact; formatting stripped below
        } else if key == b"translate" && is_str {
            read_string(j, |c| {
                if tr_len + 1 < tr_key.len() {
                    tr_key[tr_len] = c;
                    tr_len += 1;
                }
            });
        } else if key == b"with" {
            let s = j.p;
            skip_value(j);
            with = Some((s, j.p));
        } else if key == b"extra" {
            let s = j.p;
            skip_value(j);
            extra = Some((s, j.p));
        } else {
            skip_value(j);
        }
    }
    if tr_len > 0 {
        let tk = &tr_key[..tr_len];
        let fmt = TR.iter().find(|(k, _)| k.as_bytes() == tk).map(|(_, f)| *f);
        // collect args (raw ranges)
        let mut args = [(0usize, 0usize); 4];
        let mut na = 0;
        if let Some((ws, we)) = with {
            if j.s.get(ws) == Some(&b'[') {
                let mut w = Json { s: j.s, p: ws + 1, e: we };
                while na < 4 {
                    w.ws();
                    match w.cur() {
                        None | Some(b']') => break,
                        Some(b',') => {
                            w.p += 1;
                            continue;
                        }
                        _ => {}
                    }
                    let s = w.p;
                    skip_value(&mut w);
                    args[na] = (s, w.p);
                    na += 1;
                }
            }
        }
        match fmt {
            None => {
                // unknown key: "key[arg, arg]"
                for &c in tk {
                    o.put(c);
                }
                for i in 0..na {
                    o.put(if i > 0 { b',' } else { b'[' });
                    let mut a = Json { s: j.s, p: args[i].0, e: args[i].1 };
                    render(&mut a, o, depth + 1);
                }
                if na > 0 {
                    o.put(b']');
                }
            }
            Some(fmt) => {
                let f = fmt.as_bytes();
                let mut ai = 0;
                let mut i = 0;
                while i < f.len() {
                    if f[i] == b'%' && i + 1 < f.len() && f[i + 1] == b's' {
                        if ai < na {
                            let mut a = Json { s: j.s, p: args[ai].0, e: args[ai].1 };
                            ai += 1;
                            render(&mut a, o, depth + 1);
                        }
                        i += 1;
                    } else {
                        o.put(f[i]);
                    }
                    i += 1;
                }
            }
        }
    }
    if let Some((es, ee)) = extra {
        if j.s.get(es) == Some(&b'[') {
            let mut x = Json { s: j.s, p: es, e: ee };
            render(&mut x, o, depth + 1);
        }
    }
    true
}

fn render(j: &mut Json, o: &mut Out, depth: i32) -> bool {
    if depth > 12 {
        return false;
    }
    j.ws();
    let c = match j.cur() {
        Some(c) => c,
        None => return false,
    };
    if c == b'"' {
        return read_string(j, |c| o.put(c));
    }
    if c == b'{' {
        return render_object(j, o, depth);
    }
    if c == b'[' {
        j.p += 1;
        loop {
            j.ws();
            match j.cur() {
                None => return false,
                Some(b']') => {
                    j.p += 1;
                    return true;
                }
                Some(b',') => {
                    j.p += 1;
                    continue;
                }
                _ => {}
            }
            let before = j.p;
            if !render(j, o, depth + 1) || j.p == before {
                return false;
            }
        }
    }
    skip_value(j); // numbers / booleans
    true
}

/// Flattens the chat component `json` into `out`. Returns the number of bytes written (excluding the NUL;
/// the output is always NUL-terminated if `out` is not empty).
pub fn chat_to_text(json: &[u8], out: &mut [u8], strip_formatting: bool) -> usize {
    if out.is_empty() {
        return 0;
    }
    let n = {
        let mut o = Out { p: out, n: 0 };
        let mut j = Json { s: json, p: 0, e: json.len() };
        render(&mut j, &mut o, 0);
        o.n
    };
    out[n] = 0;
    if strip_formatting {
        // codes can be split across components: strip in a second in-place pass
        let mut w = 0;
        let mut r = 0;
        while r < n {
            if out[r] == 0xC2 && r + 2 < n && out[r + 1] == 0xA7 {
                r += 3;
                continue;
            }
            out[w] = out[r];
            w += 1;
            r += 1;
        }
        out[w] = 0;
        return w;
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;

    fn flat(json: &str, strip: bool) -> std::string::String {
        let mut out = [0u8; 256];
        let n = chat_to_text(json.as_bytes(), &mut out, strip);
        std::string::String::from_utf8_lossy(&out[..n]).into_owned()
    }

    #[test]
    fn plain_and_extra() {
        assert_eq!(flat(r#"{"text":"Hello "," extra":1,"extra":[{"text":"World"},"!"]}"#, true), "Hello World!");
        assert_eq!(flat(r#""just a string""#, true), "just a string");
        assert_eq!(flat(r#"["a",{"text":"b"},"c"]"#, true), "abc");
    }

    #[test]
    fn translations() {
        assert_eq!(flat(r#"{"translate":"chat.type.text","with":[{"text":"Bob"},"hi there"]}"#, true), "<Bob> hi there");
        assert_eq!(flat(r#"{"translate":"multiplayer.player.joined","with":["Alice"]}"#, true), "Alice joined the game");
        assert_eq!(flat(r#"{"translate":"some.unknown.key","with":["x","y"]}"#, true), "some.unknown.key[x,y]");
    }

    #[test]
    fn escapes_and_formatting() {
        assert_eq!(flat(r#"{"text":"line\nbreak \u00e9\u4e2d"}"#, true), "line\nbreak é中");
        assert_eq!(flat("{\"text\":\"\u{a7}cRed \u{a7}rReset\"}", true), "Red Reset");
        assert_eq!(flat("{\"text\":\"\u{a7}cRed\"}", false), "\u{a7}cRed");
    }

    #[test]
    fn malformed_input_does_not_panic() {
        for s in ["", "{", "[", "\"abc", "{\"text\":", "{\"extra\":[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]}", "{\"text\":\"\\u12\"}"] {
            let _ = flat(s, true);
        }
        let mut tiny = [0u8; 4];
        let n = chat_to_text(b"\"abcdefgh\"", &mut tiny, true);
        assert_eq!(n, 3);
        assert_eq!(chat_to_text(b"\"x\"", &mut [], true), 0);
    }
}
