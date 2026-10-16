//! The only thing a port has to implement: a byte pipe. It can be backed by BSD sockets, winsock, 3DS soc:u,
//! lwIP, a TLS tunnel, a WebSocket, a test pipe, ...

/// The connection is closed or failed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Closed;

pub trait Transport {
    /// Establish the connection (may block). Return true on success.
    fn connect(&mut self, host: &str, port: u16) -> bool;
    /// Non-blocking read. `Ok(n > 0)` bytes read, `Ok(0)` nothing available right now, `Err` closed or failed.
    fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed>;
    /// Non-blocking write. `Ok(n > 0)` bytes accepted (may be partial), `Ok(0)` would block, `Err` failed.
    fn send(&mut self, buf: &[u8]) -> Result<usize, Closed>;
    fn close(&mut self);
}

impl<T: Transport + ?Sized> Transport for &mut T {
    fn connect(&mut self, host: &str, port: u16) -> bool {
        (**self).connect(host, port)
    }
    fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed> {
        (**self).recv(buf)
    }
    fn send(&mut self, buf: &[u8]) -> Result<usize, Closed> {
        (**self).send(buf)
    }
    fn close(&mut self) {
        (**self).close()
    }
}
