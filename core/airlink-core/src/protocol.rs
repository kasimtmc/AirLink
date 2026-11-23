//! Wire messages and framing: `[u32 BE length][JSON]`.

use anyhow::{Result, bail};
use serde::{Deserialize, Serialize};

pub const PROTOCOL_VERSION: u32 = 1;
pub const MAX_HEADER_LEN: usize = 8 * 1024 * 1024;
pub const MAX_CLIPBOARD_LEN: usize = 1024 * 1024;
/// An app message's body, as JSON text.
pub const MAX_APP_MESSAGE_LEN: usize = 1024 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DeviceInfo {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub version: String,
    pub port: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct FileMeta {
    /// Relative path using `/` separators.
    pub path: String,
    pub size: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Message {
    Hello {
        device: DeviceInfo,
    },

    PairRequest,
    PairCommit {
        commitment: String,
    },
    PairNonce {
        nonce: String,
    },
    PairReveal {
        nonce: String,
    },
    PairConfirm {
        accepted: bool,
    },

    FileOffer {
        transfer_id: String,
        files: Vec<FileMeta>,
    },
    FileOfferResponse {
        accepted: bool,
    },
    FileDone {
        sha256: Vec<String>,
    },
    FileAck {
        ok: bool,
        error: Option<String>,
    },

    Clipboard {
        text: String,
    },

    /// A message between the two devices' apps, opaque to the core: `kind` names it
    /// ("notification.posted", "sms.send"…), `body` is its JSON. Features beyond files and the
    /// clipboard ride on it.
    AppMessage {
        kind: String,
        body: serde_json::Value,
    },

    Ack,
    Error {
        message: String,
    },

    /// Any message type this version does not understand.
    #[serde(other)]
    Unknown,
}

impl Message {
    pub fn error(msg: impl Into<String>) -> Self {
        Message::Error {
            message: msg.into(),
        }
    }
}

pub async fn write_msg(send: &mut quinn::SendStream, msg: &Message) -> Result<()> {
    let body = serde_json::to_vec(msg)?;
    if body.len() > MAX_HEADER_LEN {
        bail!("message too large ({} bytes)", body.len());
    }
    let mut frame = Vec::with_capacity(4 + body.len());
    frame.extend_from_slice(&(body.len() as u32).to_be_bytes());
    frame.extend_from_slice(&body);
    send.write_all(&frame).await?;
    Ok(())
}

pub async fn read_msg(recv: &mut quinn::RecvStream) -> Result<Message> {
    let mut len = [0u8; 4];
    recv.read_exact(&mut len).await?;
    let len = u32::from_be_bytes(len) as usize;
    if len > MAX_HEADER_LEN {
        bail!("incoming message too large ({len} bytes)");
    }
    let mut body = vec![0u8; len];
    recv.read_exact(&mut body).await?;
    Ok(serde_json::from_slice(&body)?)
}

/// Converts a peer-sent `Error` into an `Err`, passes other messages through.
pub fn check(msg: Message) -> Result<Message> {
    match msg {
        Message::Error { message } => bail!("peer error: {message}"),
        m => Ok(m),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn json_shape() {
        let m = Message::PairConfirm { accepted: true };
        assert_eq!(
            serde_json::to_string(&m).unwrap(),
            r#"{"type":"pair_confirm","accepted":true}"#
        );
        let unknown: Message = serde_json::from_str(r#"{"type":"sms.send","to":"x"}"#).unwrap();
        assert_eq!(unknown, Message::Unknown);
    }
}
