//! Blocking-connect, non-blocking-I/O TCP transport on `std::net` (the Rust counterpart of platform/posix_transport.cpp).
use crate::transport::{Closed, Transport};
use std::io::{ErrorKind, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};

#[derive(Default)]
pub struct TcpTransport {
    stream: Option<TcpStream>,
}

impl TcpTransport {
    pub fn new() -> Self {
        Self::default()
    }
}

impl Transport for TcpTransport {
    fn connect(&mut self, host: &str, port: u16) -> bool {
        let addrs = match (host, port).to_socket_addrs() {
            Ok(a) => a,
            Err(_) => return false,
        };
        for a in addrs {
            if let Ok(s) = TcpStream::connect(a) {
                let _ = s.set_nodelay(true);
                if s.set_nonblocking(true).is_err() {
                    continue;
                }
                self.stream = Some(s);
                return true;
            }
        }
        false
    }
    fn recv(&mut self, buf: &mut [u8]) -> Result<usize, Closed> {
        let s = self.stream.as_mut().ok_or(Closed)?;
        match s.read(buf) {
            Ok(0) => Err(Closed), // orderly close
            Ok(n) => Ok(n),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => Ok(0),
            Err(_) => Err(Closed),
        }
    }
    fn send(&mut self, buf: &[u8]) -> Result<usize, Closed> {
        let s = self.stream.as_mut().ok_or(Closed)?;
        match s.write(buf) {
            Ok(n) => Ok(n),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted) => Ok(0),
            Err(_) => Err(Closed),
        }
    }
    fn close(&mut self) {
        self.stream = None;
    }
}
