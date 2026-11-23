//! AirLink core library.
//!
//! - [`Node`]: a running device (discovery, pairing, transfer, clipboard)
//! - [`protocol`]: wire format, see `docs/protocol.md`

pub mod discovery;
pub mod files;
pub mod identity;
pub mod node;
pub mod pairing;
pub mod protocol;
pub mod tls;
pub mod trust;

pub use discovery::DiscoveredPeer;
pub use identity::short_id;
pub use node::{
    DEFAULT_PORT, Direction, Event, Node, NodeConfig, OutgoingPairing, read_saved_name,
};
pub use protocol::{DeviceInfo, FileMeta};
pub use trust::TrustedPeer;
