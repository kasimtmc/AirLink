//! Short Authentication String pairing (Bluetooth numeric-comparison style).
//! See docs/protocol.md §5.

use sha2::{Digest, Sha256};

pub type Nonce = [u8; 32];

pub fn new_nonce() -> Nonce {
    rand::random()
}

/// Responder's commitment to its nonce, bound to both identities.
pub fn commitment(initiator_id: &str, responder_id: &str, responder_nonce: &Nonce) -> String {
    let mut h = Sha256::new();
    h.update(b"airlink-commit-v1");
    h.update(initiator_id.as_bytes());
    h.update(responder_id.as_bytes());
    h.update(responder_nonce);
    hex::encode(h.finalize())
}

/// 6-digit code shown on both screens.
pub fn sas_code(initiator_id: &str, responder_id: &str, ni: &Nonce, nr: &Nonce) -> String {
    let mut h = Sha256::new();
    h.update(b"airlink-sas-v1");
    h.update(initiator_id.as_bytes());
    h.update(responder_id.as_bytes());
    h.update(ni);
    h.update(nr);
    let d = h.finalize();
    let n = u32::from_be_bytes([d[0], d[1], d[2], d[3]]) % 1_000_000;
    format!("{n:06}")
}

pub fn decode_nonce(s: &str) -> anyhow::Result<Nonce> {
    let v = hex::decode(s)?;
    v.try_into()
        .map_err(|_| anyhow::anyhow!("nonce must be 32 bytes"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn code_is_deterministic_and_six_digits() {
        let (a, b) = (new_nonce(), new_nonce());
        let c1 = sas_code("ida", "idb", &a, &b);
        assert_eq!(c1, sas_code("ida", "idb", &a, &b));
        assert_eq!(c1.len(), 6);
        assert_ne!(commitment("ida", "idb", &b), commitment("idb", "ida", &b));
    }
}
