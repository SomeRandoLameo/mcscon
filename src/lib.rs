//! mcscon: a minimal, embeddable Minecraft: Java Edition **client** protocol layer (1.10 - 1.12.2).
//!
//! * `#![no_std]`, no heap allocation, no panics on malformed input: you hand in the buffers.
//! * Zero-copy decoding: strings, NBT, arrays and entity metadata are views into the receive buffer.
//! * Layered: use only [`client::Client`] + the typed packets in [`packets`], or add [`world`], [`entities`],
//!   [`inventory`], [`physics`] and the ready-made [`session::Session`].
#![cfg_attr(not(any(test, feature = "std")), no_std)]
#![allow(clippy::too_many_arguments, clippy::needless_range_loop, clippy::collapsible_else_if)]

#[cfg(feature = "std")]
extern crate std;

pub mod types;
pub mod wire;
pub mod inflate;
pub mod ids;
pub mod transport;
pub mod client;
pub mod packets;
pub mod nbt;
pub mod arena;
pub mod blocks;
pub mod world;
pub mod entities;
pub mod inventory;
pub mod physics;
pub mod chat;
pub mod session;
#[cfg(feature = "std")]
pub mod posix;

#[cfg(feature = "capi")]
pub mod capi;

pub use types::{Error, State, Version};

/// Compiler barrier: tells the optimizer that `r` may be read or written by code it cannot see. Used right before
/// user callbacks run (the C++ front end legitimately sends packets or reads state from inside its callbacks while
/// Rust holds `&mut` references to the same objects). Zero runtime cost.
#[inline(always)]
pub(crate) fn escape<T: ?Sized>(r: &mut T) {
    core::hint::black_box(r as *mut T as *mut u8);
}
