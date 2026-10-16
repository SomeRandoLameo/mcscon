# `transport`: byte pipes and porting

The library never touches the network, a clock or a file on its own. The only platform dependency is this trait:

```rust
pub struct Closed;                        // "the connection is closed or failed"

pub trait Transport {
    fn connect(&mut self, host: &str, port: u16) -> bool;
    fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed>;
    fn send(&mut self, buf: &[u8]) -> Result<usize, Closed>;
    fn close(&mut self);
}
```

| Method | Contract |
|--------|----------|
| `connect` | establish the connection; **may block**. Return `true` on success. |
| `recv` | **non-blocking** read into `buf`. `Ok(n > 0)` = bytes read; `Ok(0)` = nothing available *right now*; `Err(Closed)` = closed or failed. An orderly shutdown by the peer must be reported as `Err(Closed)`, not `Ok(0)`. |
| `send` | **non-blocking** write. `Ok(n > 0)` = bytes accepted (may be fewer than `buf.len()`; the library retries the rest later); `Ok(0)` = would block; `Err(Closed)` = failed. |
| `close` | close the connection; must be safe to call twice. |

`&mut T` implements `Transport` for any `T: Transport`, so you can lend a transport to a client and keep using it.

## Built-in: `TcpTransport`

`mcscon::posix::TcpTransport` (feature `std`, default) uses `std::net::TcpStream`. `TcpTransport::new()` (or
`Default`) creates an unconnected transport; `connect` resolves the host name (DNS or IP literal), tries every
resolved address in turn with a blocking connect, then switches the socket to non-blocking I/O with `TCP_NODELAY`. Despite the module name it works on every platform that has `std`.

## Porting to a new platform

Implement the four methods on top of your socket API and pass it to `Client::new` / `Session::new`:

```rust
use mcscon::transport::{Closed, Transport};

struct MySocket { fd: i32 }

impl Transport for MySocket {
    fn connect(&mut self, host: &str, port: u16) -> bool {
        // resolve host, connect (blocking is fine), then put the socket in non-blocking mode
        todo!()
    }
    fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed> {
        // n = my_recv(fd, buf); n > 0 → Ok(n); would-block → Ok(0); n == 0 (peer closed) or error → Err(Closed)
        todo!()
    }
    fn send(&mut self, buf: &[u8]) -> Result<usize, Closed> {
        // n = my_send(fd, buf); would-block → Ok(0); error → Err(Closed)
        todo!()
    }
    fn close(&mut self) { /* my_close(fd) */ }
}
```

Tips:

- The core is `#![no_std]`; build with `--no-default-features` on targets without `std` (then `TcpTransport` and the
  C API are not available, but `Client`, `Session`, all models and packets are).
- Because `connect` may block, run the bot on its own thread/task if the UI must stay responsive.
- Test doubles are easy: a `Transport` backed by two byte vectors lets you script a server (the crate's own tests do this).
- For TLS tunnels, WebSockets and so on, wrap your stream in a `Transport` – the library does not care what is behind it.
