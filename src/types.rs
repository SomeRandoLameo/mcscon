/// Supported protocol versions. 1.12 and 1.12.1 differ in id tables, 1.12.1/1.12.2 in keep-alive width.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub enum Version {
    V1_10,
    V1_11,
    V1_11_2,
    V1_12,
    V1_12_1,
    V1_12_2,
}

impl Version {
    pub const ALL: [Version; 6] = [Version::V1_10, Version::V1_11, Version::V1_11_2, Version::V1_12, Version::V1_12_1, Version::V1_12_2];

    pub fn protocol_number(self) -> i32 {
        const N: [i32; 6] = [210, 315, 316, 335, 338, 340];
        N[self as usize]
    }
    /// Index into the generated id tables (V1_10, V1_11, V1_12, V1_12_2).
    pub fn table_index(self) -> usize {
        const T: [u8; 6] = [0, 1, 1, 2, 3, 3];
        T[self as usize] as usize
    }
    pub fn from_u8(v: u8) -> Option<Version> {
        Self::ALL.get(v as usize).copied()
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Handshake,
    Status,
    Login,
    Play,
    Closed,
}

impl State {
    pub fn from_u8(v: u8) -> State {
        match v {
            0 => State::Handshake,
            1 => State::Status,
            2 => State::Login,
            3 => State::Play,
            _ => State::Closed,
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Error {
    None,
    Transport,
    Protocol,
    Overflow,
    PacketTooLarge,
    Inflate,
    Disconnected,
    /// The server is in online mode (encryption request); not supported yet.
    EncryptionRequired,
    Backpressure,
}
