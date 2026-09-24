//! voxelbit wallet. Today it tracks the price; it is laid out to become the
//! wallet, which is why amounts are integer satoshis from the start (units)
//! and why it is its own process rather than part of the game: a wallet's
//! keys should not share an address space with a renderer, its mods and its
//! driver-level plugins.

pub mod app;
pub mod cross;
pub mod feed;
pub mod mark;
pub mod units;

/// voxelbit's own face, the one the game and the website set every word in.
pub const PX3: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../game/3x3-pixel.otf"));
