//! A running AirLink device: owns the QUIC endpoint, discovery, trust store,
//! and implements pairing, file transfer and clipboard sharing.

use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddr};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, Weak};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use quinn::{Connection, Endpoint, RecvStream, SendStream};
use serde::Serialize;
use sha2::{Digest, Sha256};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::{broadcast, oneshot, watch};
use tokio::time::timeout;
use tracing::{debug, info, warn};

use crate::discovery::{DiscoveredPeer, Discovery, DiscoveryEvent};
use crate::files::{collect_files, ensure_inside, part_path, sanitize_rel_path, unique_path};
use crate::identity::Identity;
use crate::pairing::{commitment, decode_nonce, new_nonce, sas_code};
use crate::protocol::{
    DeviceInfo, FileMeta, MAX_CLIPBOARD_LEN, Message, check, read_msg, write_msg,
};
use crate::tls;
use crate::trust::{TrustStore, TrustedPeer};

pub const DEFAULT_PORT: u16 = 47820;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const HELLO_TIMEOUT: Duration = Duration::from_secs(10);
/// For a reply that needs no user: clipboard, app messages.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(4);
/// How long a user has to answer a pairing / file prompt.
const PROMPT_TIMEOUT: Duration = Duration::from_secs(120);
/// How long the remote side waits for that answer (prompt + slack).
const REMOTE_PROMPT_TIMEOUT: Duration = Duration::from_secs(150);
const CHUNK: usize = 256 * 1024;
const PROGRESS_INTERVAL: Duration = Duration::from_millis(100);

#[derive(Debug, Clone)]
pub struct NodeConfig {
    pub data_dir: PathBuf,
    pub device_name: String,
    pub platform: String,
    /// Port to listen on. `None` picks a random free port.
    pub port: Option<u16>,
    pub download_dir: PathBuf,
    pub auto_accept_files: bool,
    /// Announce this device over mDNS.
    pub advertise: bool,
    /// Look for other devices over mDNS.
    pub browse: bool,
}

impl NodeConfig {
    /// Sensible per-user defaults; `data_dir` overrides the standard location.
    pub fn with_defaults(data_dir: Option<PathBuf>) -> Result<Self> {
        let data_dir = match data_dir {
            Some(d) => d,
            None => directories::ProjectDirs::from("dev", "AirLink", "AirLink")
                .context("cannot determine config directory")?
                .data_dir()
                .to_path_buf(),
        };
        let download_dir = directories::UserDirs::new()
            .and_then(|u| {
                u.download_dir()
                    .map(|d| d.to_path_buf())
                    .or_else(|| Some(u.home_dir().to_path_buf()))
            })
            .unwrap_or_else(|| PathBuf::from("."))
            .join("AirLink");
        let device_name = read_saved_name(&data_dir).unwrap_or_else(|| {
            hostname::get()
                .ok()
                .and_then(|h| h.into_string().ok())
                .unwrap_or_else(|| "AirLink device".into())
        });
        Ok(Self {
            data_dir,
            device_name,
            platform: std::env::consts::OS.to_owned(),
            port: Some(DEFAULT_PORT),
            download_dir,
            auto_accept_files: false,
            advertise: true,
            browse: true,
        })
    }
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Direction {
    Outgoing,
    Incoming,
}

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub enum Event {
    PeerDiscovered {
        peer: DiscoveredPeer,
    },
    PeerLost {
        id: String,
    },
    /// Answer with [`Node::respond`].
    PairingRequest {
        request_id: u64,
        peer: DeviceInfo,
        code: String,
    },
    PairingCompleted {
        peer: DeviceInfo,
        success: bool,
    },
    /// Answer with [`Node::respond`].
    FileOffer {
        request_id: u64,
        transfer_id: String,
        peer: DeviceInfo,
        files: Vec<FileMeta>,
        total_size: u64,
    },
    /// A prompt (`PairingRequest` / `FileOffer`) is no longer answerable:
    /// the other side gave up, disconnected, or it timed out. Dismiss it.
    RequestCancelled {
        request_id: u64,
    },
    /// Outgoing offer sent; waiting for the receiver to accept.
    TransferPending {
        transfer_id: String,
        peer_id: String,
        file_count: usize,
        bytes_total: u64,
    },
    TransferProgress {
        transfer_id: String,
        peer_id: String,
        direction: Direction,
        bytes_done: u64,
        bytes_total: u64,
    },
    TransferCompleted {
        transfer_id: String,
        peer_id: String,
        direction: Direction,
        files: Vec<PathBuf>,
    },
    TransferFailed {
        transfer_id: String,
        peer_id: String,
        direction: Direction,
        error: String,
    },
    ClipboardReceived {
        peer: DeviceInfo,
        text: String,
        /// A password, as the sender marked it.
        sensitive: bool,
    },
    /// A paired device began a media stream (`name`, e.g. "screen"): take its frames with
    /// [`Node::take_media`].
    MediaStarted {
        peer_id: String,
        name: String,
    },
    MediaStopped {
        peer_id: String,
        name: String,
    },
    /// From a paired device's app (see [`Message::AppMessage`]).
    AppMessage {
        peer: DeviceInfo,
        kind: String,
        body: serde_json::Value,
    },
}

#[derive(Clone)]
pub struct Node {
    inner: Arc<Inner>,
}

struct Inner {
    cfg: NodeConfig,
    info: DeviceInfo,
    endpoint: Endpoint,
    trust: Mutex<TrustStore>,
    events: broadcast::Sender<Event>,
    pending: Mutex<HashMap<u64, oneshot::Sender<bool>>>,
    next_id: AtomicU64,
    discovered: Mutex<HashMap<String, DiscoveredPeer>>,
    connections: Mutex<HashMap<String, (Connection, DeviceInfo)>>,
    discovery: Mutex<Option<Discovery>>,
    /// Outgoing media streams by (peer id, name): the frame waiting to be written.
    media_out: Mutex<HashMap<(String, String), Arc<MediaOut>>>,
    /// The latest frame received on each (peer id, name), not yet taken.
    media_in: Mutex<HashMap<(String, String), Vec<u8>>>,
    /// Transfers running now, by id: true cancels one (`Node::cancel_transfer`).
    transfers: Mutex<HashMap<String, watch::Sender<bool>>>,
}

/// The stream error code with which a side ends a transfer its user cancelled.
const CANCELLED: quinn::VarInt = quinn::VarInt::from_u32(0x63);
/// `TransferFailed`'s error when this device's user cancelled.
pub const TRANSFER_CANCELLED: &str = "cancelled";
/// `TransferFailed`'s error when the other device's user did.
pub const TRANSFER_CANCELLED_BY_PEER: &str = "cancelled by the other device";

/// Whether an error is the other side ending the transfer as cancelled.
fn cancelled_by_peer(e: &anyhow::Error) -> bool {
    e.chain().any(|c| {
        matches!(c.downcast_ref::<quinn::ReadError>(), Some(quinn::ReadError::Reset(code)) if *code == CANCELLED)
            || matches!(c.downcast_ref::<quinn::WriteError>(), Some(quinn::WriteError::Stopped(code)) if *code == CANCELLED)
            || matches!(
                c.downcast_ref::<quinn::ReadExactError>(),
                Some(quinn::ReadExactError::ReadError(quinn::ReadError::Reset(code))) if *code == CANCELLED
            )
    })
}

/// Resolves once the transfer is cancelled (never, if its entry is gone).
async fn until_cancelled(mut rx: watch::Receiver<bool>) {
    if rx.wait_for(|c| *c).await.is_err() {
        std::future::pending::<()>().await;
    }
}

/// One outgoing media stream: frames are written by a task of its own, and a new frame replaces
/// one not yet written (a slow link drops frames instead of falling behind).
struct MediaOut {
    latest: Mutex<Option<Vec<u8>>>,
    wake: tokio::sync::Notify,
    stopped: std::sync::atomic::AtomicBool,
}

/// Largest media frame accepted.
pub const MAX_MEDIA_FRAME: usize = 16 * 1024 * 1024;

impl Node {
    pub async fn start(cfg: NodeConfig) -> Result<Self> {
        std::fs::create_dir_all(&cfg.data_dir)?;
        let identity = Identity::load_or_create(&cfg.data_dir)?;
        let trust = TrustStore::load(cfg.data_dir.join("trusted_peers.json"))?;
        if read_saved_name(&cfg.data_dir).as_deref() != Some(cfg.device_name.as_str()) {
            let json = serde_json::json!({ "device_name": cfg.device_name });
            std::fs::write(
                cfg.data_dir.join(DEVICE_FILE),
                serde_json::to_vec_pretty(&json)?,
            )?;
        }

        let mut endpoint = bind(&identity, cfg.port)?;
        endpoint.set_default_client_config(tls::client_config(&identity)?);
        let port = endpoint.local_addr()?.port();

        let info = DeviceInfo {
            id: identity.device_id().to_owned(),
            name: cfg.device_name.clone(),
            platform: cfg.platform.clone(),
            version: env!("CARGO_PKG_VERSION").to_owned(),
            port,
        };
        info!(id = %info.id, port, "AirLink node started");

        let (events, _) = broadcast::channel(256);
        let inner = Arc::new(Inner {
            cfg,
            info,
            endpoint,
            trust: Mutex::new(trust),
            events,
            pending: Mutex::new(HashMap::new()),
            next_id: AtomicU64::new(1),
            discovered: Mutex::new(HashMap::new()),
            connections: Mutex::new(HashMap::new()),
            discovery: Mutex::new(None),
            media_out: Mutex::new(HashMap::new()),
            media_in: Mutex::new(HashMap::new()),
            transfers: Mutex::new(HashMap::new()),
        });

        tokio::spawn(accept_loop(inner.clone()));

        if inner.cfg.advertise || inner.cfg.browse {
            let weak = Arc::downgrade(&inner);
            match Discovery::start(
                &inner.info,
                inner.cfg.advertise,
                inner.cfg.browse,
                move |ev| on_discovery(&weak, ev),
            ) {
                Ok(d) => *inner.discovery.lock().unwrap() = Some(d),
                Err(e) => warn!("mDNS discovery unavailable: {e:#}"),
            }
        }

        Ok(Self { inner })
    }

    pub fn info(&self) -> &DeviceInfo {
        &self.inner.info
    }

    pub fn config(&self) -> &NodeConfig {
        &self.inner.cfg
    }

    pub fn subscribe(&self) -> broadcast::Receiver<Event> {
        self.inner.events.subscribe()
    }

    pub fn trusted_peers(&self) -> Vec<TrustedPeer> {
        self.inner.trust.lock().unwrap().list()
    }

    pub fn discovered_peers(&self) -> Vec<DiscoveredPeer> {
        let mut v: Vec<_> = self
            .inner
            .discovered
            .lock()
            .unwrap()
            .values()
            .cloned()
            .collect();
        v.sort_by(|a, b| a.name.cmp(&b.name));
        v
    }

    pub fn is_trusted(&self, id: &str) -> bool {
        self.inner.trust.lock().unwrap().is_trusted(id)
    }

    /// Marks a paired peer trusted: its file offers no longer ask. False if it is not paired.
    pub fn set_peer_auto_accept(&self, id: &str, on: bool) -> Result<bool> {
        self.inner.trust.lock().unwrap().set_auto_accept(id, on)
    }

    pub fn forget_peer(&self, id: &str) -> Result<bool> {
        if let Some((c, _)) = self.inner.connections.lock().unwrap().remove(id) {
            c.close(0u32.into(), b"forgotten");
        }
        self.inner.trust.lock().unwrap().remove(id)
    }

    /// Trusted peers that are currently reachable (connected or seen via mDNS).
    pub fn reachable_trusted_peers(&self) -> Vec<String> {
        let trust = self.inner.trust.lock().unwrap();
        let disc = self.inner.discovered.lock().unwrap();
        let conns = self.inner.connections.lock().unwrap();
        trust
            .list()
            .into_iter()
            .map(|p| p.id)
            .filter(|id| {
                disc.contains_key(id)
                    || conns
                        .get(id)
                        .is_some_and(|(c, _)| c.close_reason().is_none())
            })
            .collect()
    }

    /// Finds a trusted or discovered peer by id prefix or (case-insensitive) name.
    pub fn find_peer(&self, query: &str) -> Option<String> {
        let q = query.to_lowercase();
        let matches = |id: &str, name: &str| id.starts_with(&q) || name.to_lowercase() == q;
        let trust = self.inner.trust.lock().unwrap().list();
        if let Some(p) = trust.iter().find(|p| matches(&p.id, &p.name)) {
            return Some(p.id.clone());
        }
        let disc = self.inner.discovered.lock().unwrap();
        disc.values()
            .find(|p| matches(&p.id, &p.name))
            .map(|p| p.id.clone())
    }

    /// Answers a `PairingRequest` or `FileOffer` event. Returns false if the
    /// request is unknown or already expired.
    pub fn respond(&self, request_id: u64, accept: bool) -> bool {
        match self.inner.pending.lock().unwrap().remove(&request_id) {
            Some(tx) => tx.send(accept).is_ok(),
            None => false,
        }
    }

    /// Starts pairing with the device at `addr`. Show [`OutgoingPairing::code`]
    /// to the user, then call [`OutgoingPairing::confirm`].
    pub async fn start_pairing(&self, addr: SocketAddr) -> Result<OutgoingPairing> {
        let (conn, peer) = self.inner.connect_addr(addr).await?;
        self.inner.adopt(&conn, &peer, addr);
        if peer.id == self.inner.info.id {
            bail!("cannot pair with this device itself");
        }
        self.pair_over(conn, peer, addr).await
    }

    /// Pairs by device id, resolving its address via discovery.
    pub async fn start_pairing_with(&self, peer_id: &str) -> Result<OutgoingPairing> {
        let (conn, peer, addr) = self
            .inner
            .connect_any(self.inner.resolve(peer_id), Some(peer_id))
            .await?;
        self.pair_over(conn, peer, addr).await
    }

    async fn pair_over(
        &self,
        conn: Connection,
        peer: DeviceInfo,
        addr: SocketAddr,
    ) -> Result<OutgoingPairing> {
        let inner = &self.inner;
        let (mut send, mut recv) = conn.open_bi().await?;
        write_msg(&mut send, &Message::PairRequest).await?;
        let Message::PairCommit {
            commitment: their_commit,
        } = check(read_msg(&mut recv).await?)?
        else {
            bail!("unexpected reply to pair_request");
        };
        let na = new_nonce();
        write_msg(
            &mut send,
            &Message::PairNonce {
                nonce: hex::encode(na),
            },
        )
        .await?;
        let Message::PairReveal { nonce } = check(read_msg(&mut recv).await?)? else {
            bail!("unexpected reply to pair_nonce");
        };
        let nb = decode_nonce(&nonce)?;
        if commitment(&inner.info.id, &peer.id, &nb) != their_commit {
            bail!("pairing commitment mismatch, possible man-in-the-middle; aborting");
        }
        let code = sas_code(&inner.info.id, &peer.id, &na, &nb);
        Ok(OutgoingPairing {
            inner: inner.clone(),
            conn,
            peer,
            addr,
            code,
            send,
            recv,
        })
    }

    /// Sends files and/or directories to a trusted peer. Resolves when the
    /// receiver has verified everything. Returns the transfer id.
    pub async fn send_files(&self, peer_id: &str, paths: &[PathBuf]) -> Result<String> {
        let transfer_id = hex::encode(rand::random::<[u8; 8]>());
        let res = self.inner.send_files(peer_id, paths, &transfer_id).await;
        match &res {
            Ok(files) => self.inner.emit(Event::TransferCompleted {
                transfer_id: transfer_id.clone(),
                peer_id: peer_id.to_owned(),
                direction: Direction::Outgoing,
                files: files.clone(),
            }),
            Err(e) => self.inner.emit(Event::TransferFailed {
                transfer_id: transfer_id.clone(),
                peer_id: peer_id.to_owned(),
                direction: Direction::Outgoing,
                error: format!("{e:#}"),
            }),
        }
        res.map(|_| transfer_id)
    }

    /// Cancels a running transfer, either way: it fails here with `TRANSFER_CANCELLED` and on the
    /// other device with `TRANSFER_CANCELLED_BY_PEER`; files not complete yet are not kept. False
    /// if no transfer with that id is running.
    pub fn cancel_transfer(&self, transfer_id: &str) -> bool {
        match self.inner.transfers.lock().unwrap().get(transfer_id) {
            Some(tx) => tx.send(true).is_ok(),
            None => false,
        }
    }

    pub async fn send_clipboard(&self, peer_id: &str, text: &str) -> Result<()> {
        self.send_clipboard_marked(peer_id, text, false).await
    }

    /// As `send_clipboard`, `sensitive` marking a password (the receiver hides it).
    pub async fn send_clipboard_marked(&self, peer_id: &str, text: &str, sensitive: bool) -> Result<()> {
        if text.len() > MAX_CLIPBOARD_LEN {
            bail!("clipboard text too large");
        }
        let msg = Message::Clipboard {
            text: text.to_owned(),
            sensitive,
        };
        match self.inner.request(peer_id, &msg).await? {
            Message::Ack => Ok(()),
            m => bail!("unexpected reply: {m:?}"),
        }
    }

    /// Sends an app message to a paired device; resolves once it was received.
    pub async fn send_app_message(
        &self,
        peer_id: &str,
        kind: &str,
        body: serde_json::Value,
    ) -> Result<()> {
        if kind.is_empty() || body.to_string().len() > crate::protocol::MAX_APP_MESSAGE_LEN {
            bail!("app message is empty or too large");
        }
        let msg = Message::AppMessage {
            kind: kind.to_owned(),
            body,
        };
        match self.inner.request(peer_id, &msg).await? {
            Message::Ack => Ok(()),
            m => bail!("unexpected reply: {m:?}"),
        }
    }

    /// Sends a frame on the media stream `name` to a paired device. Returns at once: a task writes
    /// it, and a frame not yet written when the next comes is dropped (the latest wins).
    pub fn send_media(&self, peer_id: &str, name: &str, frame: Vec<u8>) -> Result<()> {
        if frame.len() > MAX_MEDIA_FRAME {
            bail!("media frame too large");
        }
        let key = (peer_id.to_owned(), name.to_owned());
        let out = {
            let mut map = self.inner.media_out.lock().unwrap();
            if let Some(o) = map.get(&key) {
                o.clone()
            } else {
                let o = Arc::new(MediaOut {
                    latest: Mutex::new(None),
                    wake: tokio::sync::Notify::new(),
                    stopped: std::sync::atomic::AtomicBool::new(false),
                });
                map.insert(key.clone(), o.clone());
                tokio::spawn(media_writer(self.inner.clone(), key, o.clone()));
                o
            }
        };
        *out.latest.lock().unwrap() = Some(frame);
        out.wake.notify_one();
        Ok(())
    }

    /// Ends the media stream `name` to a device (it hears MediaStopped).
    pub fn stop_media(&self, peer_id: &str, name: &str) {
        if let Some(o) = self
            .inner
            .media_out
            .lock()
            .unwrap()
            .remove(&(peer_id.to_owned(), name.to_owned()))
        {
            o.stopped.store(true, Ordering::Relaxed);
            o.wake.notify_one();
        }
    }

    /// The latest frame received on media stream `name` from `peer_id`, if a new one came since
    /// the last call.
    pub fn take_media(&self, peer_id: &str, name: &str) -> Option<Vec<u8>> {
        self.inner
            .media_in
            .lock()
            .unwrap()
            .remove(&(peer_id.to_owned(), name.to_owned()))
    }

    pub async fn shutdown(&self) {
        if let Some(d) = self.inner.discovery.lock().unwrap().take() {
            d.shutdown();
        }
        self.inner.endpoint.close(0u32.into(), b"shutdown");
        let _ = timeout(Duration::from_secs(2), self.inner.endpoint.wait_idle()).await;
    }
}

/// An in-progress outgoing pairing, waiting for the local user's decision.
pub struct OutgoingPairing {
    inner: Arc<Inner>,
    conn: Connection,
    peer: DeviceInfo,
    addr: SocketAddr,
    code: String,
    send: SendStream,
    recv: RecvStream,
}

impl OutgoingPairing {
    pub fn code(&self) -> &str {
        &self.code
    }

    pub fn peer(&self) -> &DeviceInfo {
        &self.peer
    }

    /// Sends the local decision and waits for the remote one. Returns true
    /// if both sides accepted and the peer is now trusted.
    pub async fn confirm(mut self, accept: bool) -> Result<bool> {
        let res: Result<bool> = async {
            write_msg(&mut self.send, &Message::PairConfirm { accepted: accept }).await?;
            let remote =
                match check(timeout(REMOTE_PROMPT_TIMEOUT, read_msg(&mut self.recv)).await??)? {
                    Message::PairConfirm { accepted } => accepted,
                    m => bail!("unexpected reply: {m:?}"),
                };
            let _ = self.send.finish();
            Ok(accept && remote)
        }
        .await;
        let success = *res.as_ref().unwrap_or(&false);
        self.inner.finish_pairing(&self.peer, self.addr, success);
        if success {
            self.inner
                .connections
                .lock()
                .unwrap()
                .insert(self.peer.id.clone(), (self.conn, self.peer));
        }
        res
    }
}

const DEVICE_FILE: &str = "device.json";

/// The device name saved by a previous run, if any.
pub fn read_saved_name(data_dir: &std::path::Path) -> Option<String> {
    let bytes = std::fs::read(data_dir.join(DEVICE_FILE)).ok()?;
    let v: serde_json::Value = serde_json::from_slice(&bytes).ok()?;
    v.get("device_name")?.as_str().map(str::to_owned)
}

fn bind(identity: &Identity, port: Option<u16>) -> Result<Endpoint> {
    let server = tls::server_config(identity)?;
    let addr = |p| SocketAddr::from((Ipv4Addr::UNSPECIFIED, p));
    if let Some(p) = port {
        match Endpoint::server(server.clone(), addr(p)) {
            Ok(e) => return Ok(e),
            Err(e) => warn!("port {p} unavailable ({e}), using a random port"),
        }
    }
    Ok(Endpoint::server(server, addr(0))?)
}

fn on_discovery(weak: &Weak<Inner>, ev: DiscoveryEvent) {
    let Some(inner) = weak.upgrade() else { return };
    match ev {
        DiscoveryEvent::Found(peer) => {
            // mDNS reports the same device once per interface, each time with a
            // partial address set: merge addresses, and only announce devices
            // that are new or whose visible details changed.
            let announce = {
                let mut map = inner.discovered.lock().unwrap();
                match map.get_mut(&peer.id) {
                    Some(known) => {
                        for a in &peer.addrs {
                            if !known.addrs.contains(a) {
                                known.addrs.push(*a);
                            }
                        }
                        known.addrs.sort_by_key(|a| (a.ip().is_loopback(), *a));
                        let renamed = known.name != peer.name || known.platform != peer.platform;
                        known.name = peer.name;
                        known.platform = peer.platform;
                        renamed.then(|| known.clone())
                    }
                    None => {
                        map.insert(peer.id.clone(), peer.clone());
                        Some(peer)
                    }
                }
            };
            if let Some(peer) = announce {
                inner.emit(Event::PeerDiscovered { peer });
            }
        }
        DiscoveryEvent::Lost(id) => {
            if inner.discovered.lock().unwrap().remove(&id).is_some() {
                inner.emit(Event::PeerLost { id });
            }
        }
    }
}

async fn accept_loop(inner: Arc<Inner>) {
    while let Some(incoming) = inner.endpoint.accept().await {
        let inner = inner.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_connection(inner, incoming).await {
                debug!("incoming connection ended: {e:#}");
            }
        });
    }
}

async fn handle_connection(inner: Arc<Inner>, incoming: quinn::Incoming) -> Result<()> {
    let conn = incoming.await?;
    let peer_id = tls::peer_device_id(&conn)?;
    let addr = conn.remote_address();

    let (mut send, mut recv) = timeout(HELLO_TIMEOUT, conn.accept_bi()).await??;
    let Message::Hello { device } = check(read_msg(&mut recv).await?)? else {
        bail!("expected hello");
    };
    write_msg(
        &mut send,
        &Message::Hello {
            device: inner.info.clone(),
        },
    )
    .await?;
    send.finish()?;
    let peer = DeviceInfo {
        id: peer_id,
        ..device
    };
    debug!(peer = %peer.name, %addr, "connection accepted");

    if inner.trust.lock().unwrap().is_trusted(&peer.id) {
        inner.on_trusted_contact(&peer, addr);
        inner
            .connections
            .lock()
            .unwrap()
            .insert(peer.id.clone(), (conn.clone(), peer.clone()));
    }

    serve_streams(inner, conn, peer, addr).await;
    Ok(())
}

/// Handles requests the peer opens on `conn` until it closes. Runs for every
/// connection, whichever side opened it: both sides cache and reuse
/// connections, so either may send requests over it.
async fn serve_streams(inner: Arc<Inner>, conn: Connection, peer: DeviceInfo, addr: SocketAddr) {
    // Media comes on unidirectional streams.
    {
        let (inner, conn, peer) = (inner.clone(), conn.clone(), peer.clone());
        tokio::spawn(async move {
            while let Ok(recv) = conn.accept_uni().await {
                let (inner, peer) = (inner.clone(), peer.clone());
                tokio::spawn(async move {
                    if let Err(e) = receive_media(inner, peer, recv).await {
                        debug!("media stream ended: {e:#}");
                    }
                });
            }
        });
    }
    while let Ok((send, recv)) = conn.accept_bi().await {
        let inner = inner.clone();
        let conn = conn.clone();
        let peer = peer.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_stream(inner, conn, peer, addr, send, recv).await {
                debug!("stream failed: {e:#}");
            }
        });
    }
}

/// A media stream: `media { name }`, then frames, each `[u32 BE length][bytes]`, until it ends.
async fn receive_media(inner: Arc<Inner>, peer: DeviceInfo, mut recv: RecvStream) -> Result<()> {
    let Message::Media { name } = read_msg(&mut recv).await? else {
        bail!("expected a media header");
    };
    if !inner.trust.lock().unwrap().is_trusted(&peer.id) {
        bail!("media from an unpaired device");
    }
    let key = (peer.id.clone(), name.clone());
    inner.emit(Event::MediaStarted {
        peer_id: peer.id.clone(),
        name: name.clone(),
    });
    let result: Result<()> = async {
        loop {
            let mut len = [0u8; 4];
            match recv.read_exact(&mut len).await {
                Ok(()) => {}
                Err(quinn::ReadExactError::FinishedEarly(0)) => return Ok(()), // the sender ended it
                Err(e) => return Err(e.into()),
            }
            let len = u32::from_be_bytes(len) as usize;
            if len > MAX_MEDIA_FRAME {
                bail!("media frame too large ({len} bytes)");
            }
            let mut frame = vec![0u8; len];
            recv.read_exact(&mut frame).await?;
            inner.media_in.lock().unwrap().insert(key.clone(), frame);
        }
    }
    .await;
    inner.media_in.lock().unwrap().remove(&key);
    inner.emit(Event::MediaStopped {
        peer_id: peer.id,
        name,
    });
    result
}

/// Writes an outgoing media stream's frames as they come, reconnecting if its connection drops.
async fn media_writer(inner: Arc<Inner>, key: (String, String), out: Arc<MediaOut>) {
    let mut stream: Option<SendStream> = None;
    loop {
        out.wake.notified().await;
        if out.stopped.load(Ordering::Relaxed) {
            if let Some(mut s) = stream.take() {
                let _ = s.finish();
            }
            return;
        }
        let Some(frame) = out.latest.lock().unwrap().take() else {
            continue;
        };
        let written: Result<()> = async {
            if stream.is_none() {
                let (conn, _) = inner.connection_to(&key.0).await?;
                let mut s = conn.open_uni().await?;
                write_msg(
                    &mut s,
                    &Message::Media {
                        name: key.1.clone(),
                    },
                )
                .await?;
                stream = Some(s);
            }
            let s = stream.as_mut().unwrap();
            s.write_all(&(frame.len() as u32).to_be_bytes()).await?;
            s.write_all(&frame).await?;
            Ok(())
        }
        .await;
        if let Err(e) = written {
            debug!("media frame not sent ({e:#}); a new stream next time");
            stream = None;
            inner.connections.lock().unwrap().remove(&key.0);
        }
    }
}

async fn handle_stream(
    inner: Arc<Inner>,
    conn: Connection,
    peer: DeviceInfo,
    addr: SocketAddr,
    mut send: SendStream,
    mut recv: RecvStream,
) -> Result<()> {
    let msg = read_msg(&mut recv).await?;
    if msg == Message::PairRequest {
        let res = pair_responder(&inner, &conn, &peer, addr, &mut send, &mut recv).await;
        if res.is_err() {
            inner.finish_pairing(&peer, addr, false);
        }
        return res;
    }
    if !inner.trust.lock().unwrap().is_trusted(&peer.id) {
        write_msg(&mut send, &Message::error("not_paired")).await?;
        send.finish()?;
        return Ok(());
    }
    match msg {
        Message::FileOffer { transfer_id, files } => {
            let res = receive_files(&inner, &peer, &transfer_id, files, &mut send, &mut recv).await;
            match res {
                Ok(Some(saved)) => inner.emit(Event::TransferCompleted {
                    transfer_id,
                    peer_id: peer.id.clone(),
                    direction: Direction::Incoming,
                    files: saved,
                }),
                Ok(None) => {}
                Err(e) => {
                    inner.emit(Event::TransferFailed {
                        transfer_id,
                        peer_id: peer.id.clone(),
                        direction: Direction::Incoming,
                        error: format!("{e:#}"),
                    });
                    return Err(e);
                }
            }
        }
        Message::Clipboard { text, sensitive } => {
            if text.len() > MAX_CLIPBOARD_LEN {
                write_msg(&mut send, &Message::error("too_large")).await?;
            } else {
                inner.emit(Event::ClipboardReceived {
                    peer: peer.clone(),
                    text,
                    sensitive,
                });
                write_msg(&mut send, &Message::Ack).await?;
            }
        }
        Message::AppMessage { kind, body } => {
            inner.emit(Event::AppMessage {
                peer: peer.clone(),
                kind,
                body,
            });
            write_msg(&mut send, &Message::Ack).await?;
        }
        _ => write_msg(&mut send, &Message::error("unsupported")).await?,
    }
    send.finish()?;
    Ok(())
}

async fn pair_responder(
    inner: &Arc<Inner>,
    conn: &Connection,
    peer: &DeviceInfo,
    addr: SocketAddr,
    send: &mut SendStream,
    recv: &mut RecvStream,
) -> Result<()> {
    if peer.id == inner.info.id {
        bail!("refusing to pair with self");
    }
    let me = &inner.info.id;
    let nb = new_nonce();
    write_msg(
        send,
        &Message::PairCommit {
            commitment: commitment(&peer.id, me, &nb),
        },
    )
    .await?;
    let Message::PairNonce { nonce } = check(read_msg(recv).await?)? else {
        bail!("expected pair_nonce");
    };
    let na = decode_nonce(&nonce)?;
    write_msg(
        send,
        &Message::PairReveal {
            nonce: hex::encode(nb),
        },
    )
    .await?;
    let code = sas_code(&peer.id, me, &na, &nb);

    let (request_id, rx) = inner.register_prompt();
    inner.emit(Event::PairingRequest {
        request_id,
        peer: peer.clone(),
        code,
    });
    let local = inner.await_prompt(request_id, rx, send).await;

    write_msg(send, &Message::PairConfirm { accepted: local }).await?;
    let remote = match check(timeout(REMOTE_PROMPT_TIMEOUT, read_msg(recv)).await??)? {
        Message::PairConfirm { accepted } => accepted,
        m => bail!("unexpected message: {m:?}"),
    };
    send.finish()?;
    let paired = local && remote;
    inner.finish_pairing(peer, addr, paired);
    if paired {
        inner
            .connections
            .lock()
            .unwrap()
            .insert(peer.id.clone(), (conn.clone(), peer.clone()));
    }
    Ok(())
}

/// Returns the saved paths, or `None` if the offer was declined.
async fn receive_files(
    inner: &Arc<Inner>,
    peer: &DeviceInfo,
    transfer_id: &str,
    files: Vec<FileMeta>,
    send: &mut SendStream,
    recv: &mut RecvStream,
) -> Result<Option<Vec<PathBuf>>> {
    let base = inner.cfg.download_dir.clone();
    let mut rel_paths = Vec::with_capacity(files.len());
    for f in &files {
        match sanitize_rel_path(&f.path) {
            Some(p) => rel_paths.push(p),
            None => {
                write_msg(send, &Message::error("invalid_path")).await?;
                bail!("peer sent invalid path {:?}", f.path);
            }
        }
    }
    let total: u64 = files.iter().map(|f| f.size).sum();

    let trusted = inner.trust.lock().unwrap().auto_accepts(&peer.id);
    let accepted = if inner.cfg.auto_accept_files || trusted {
        true
    } else {
        let (request_id, rx) = inner.register_prompt();
        inner.emit(Event::FileOffer {
            request_id,
            transfer_id: transfer_id.to_owned(),
            peer: peer.clone(),
            files: files.clone(),
            total_size: total,
        });
        inner.await_prompt(request_id, rx, send).await
    };
    write_msg(send, &Message::FileOfferResponse { accepted }).await?;
    if !accepted {
        return Ok(None);
    }

    tokio::fs::create_dir_all(&base).await?;
    let mut progress = Progress::new(inner, transfer_id, &peer.id, Direction::Incoming, total);
    let mut written: Vec<(PathBuf, PathBuf, String)> = Vec::new(); // (part, target, sha256)

    let cancel = inner.track_transfer(transfer_id);
    let work = async {
        let mut buf = vec![0u8; CHUNK];
        for (meta, rel) in files.iter().zip(&rel_paths) {
            let target = unique_path(base.join(rel));
            ensure_inside(&base, &target)?;
            if let Some(dir) = target.parent() {
                tokio::fs::create_dir_all(dir).await?;
            }
            let part = part_path(&target);
            let mut out = tokio::io::BufWriter::new(tokio::fs::File::create(&part).await?);
            written.push((part, target, String::new()));
            let mut hasher = Sha256::new();
            let mut remaining = meta.size;
            while remaining > 0 {
                let want = remaining.min(CHUNK as u64) as usize;
                let n = recv
                    .read(&mut buf[..want])
                    .await?
                    .ok_or_else(|| anyhow!("stream ended early"))?;
                hasher.update(&buf[..n]);
                out.write_all(&buf[..n]).await?;
                remaining -= n as u64;
                progress.add(n as u64);
            }
            out.flush().await?;
            written.last_mut().unwrap().2 = hex::encode(hasher.finalize());
        }
        progress.finish();
        let Message::FileDone { sha256 } = check(read_msg(recv).await?)? else {
            bail!("expected file_done");
        };
        if sha256.len() != written.len() || sha256.iter().zip(&written).any(|(a, (_, _, b))| a != b)
        {
            write_msg(
                send,
                &Message::FileAck {
                    ok: false,
                    error: Some("checksum mismatch".into()),
                },
            )
            .await?;
            bail!("checksum mismatch");
        }
        Ok(())
    };
    let result: Result<()> = tokio::select! {
        r = work => r.map_err(|e| if cancelled_by_peer(&e) { anyhow!(TRANSFER_CANCELLED_BY_PEER) } else { e }),
        () = until_cancelled(cancel) => {
            Err(anyhow!(TRANSFER_CANCELLED))
        }
    };
    inner.untrack_transfer(transfer_id);
    if result.as_ref().is_err_and(|e| e.to_string() == TRANSFER_CANCELLED) {
        let _ = send.reset(CANCELLED);
        let _ = recv.stop(CANCELLED);
    }

    if let Err(e) = result {
        for (part, _, _) in &written {
            let _ = tokio::fs::remove_file(part).await;
        }
        return Err(e);
    }

    let mut saved = Vec::with_capacity(written.len());
    for (part, target, _) in written {
        let target = unique_path(target);
        tokio::fs::rename(&part, &target).await?;
        saved.push(target);
    }
    write_msg(
        send,
        &Message::FileAck {
            ok: true,
            error: None,
        },
    )
    .await?;
    Ok(Some(saved))
}

impl Inner {
    fn track_transfer(&self, id: &str) -> watch::Receiver<bool> {
        let (tx, rx) = watch::channel(false);
        self.transfers.lock().unwrap().insert(id.to_owned(), tx);
        rx
    }

    fn untrack_transfer(&self, id: &str) {
        self.transfers.lock().unwrap().remove(id);
    }

    fn emit(&self, e: Event) {
        let _ = self.events.send(e);
    }

    fn register_prompt(&self) -> (u64, oneshot::Receiver<bool>) {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, tx);
        (id, rx)
    }

    /// Waits for the local answer to a prompt. Gives up (and emits
    /// `RequestCancelled`) on timeout or when the remote side stops listening
    /// on `send` (it cancelled, timed out, or the connection dropped).
    async fn await_prompt(&self, id: u64, rx: oneshot::Receiver<bool>, send: &SendStream) -> bool {
        let answer = tokio::select! {
            r = timeout(PROMPT_TIMEOUT, rx) => r.ok().and_then(Result::ok),
            _ = send.stopped() => None,
        };
        let unanswered = self.pending.lock().unwrap().remove(&id).is_some();
        if answer.is_none() && unanswered {
            self.emit(Event::RequestCancelled { request_id: id });
        }
        answer.unwrap_or(false)
    }

    fn finish_pairing(&self, peer: &DeviceInfo, addr: SocketAddr, success: bool) {
        if success {
            let stored_addr = SocketAddr::new(addr.ip(), peer.port);
            if let Err(e) = self.trust.lock().unwrap().add(peer, Some(stored_addr)) {
                warn!("failed to save trusted peer: {e:#}");
            }
        }
        self.emit(Event::PairingCompleted {
            peer: peer.clone(),
            success,
        });
    }

    fn on_trusted_contact(&self, peer: &DeviceInfo, addr: SocketAddr) {
        let stored_addr = SocketAddr::new(addr.ip(), peer.port);
        if let Err(e) = self.trust.lock().unwrap().touch(peer, stored_addr) {
            warn!("failed to update trusted peer: {e:#}");
        }
    }

    /// Candidate addresses for a device: mDNS results first, then last known.
    fn resolve(&self, peer_id: &str) -> Vec<SocketAddr> {
        let mut addrs = Vec::new();
        if let Some(p) = self.discovered.lock().unwrap().get(peer_id) {
            addrs.extend(p.addrs.iter().copied());
        }
        if let Some(a) = self
            .trust
            .lock()
            .unwrap()
            .get(peer_id)
            .and_then(|p| p.last_addr)
            && !addrs.contains(&a)
        {
            addrs.push(a);
        }
        addrs
    }

    /// Starts serving the peer's requests on an outgoing connection we keep.
    fn adopt(self: &Arc<Self>, conn: &Connection, peer: &DeviceInfo, addr: SocketAddr) {
        tokio::spawn(serve_streams(
            self.clone(),
            conn.clone(),
            peer.clone(),
            addr,
        ));
    }

    /// Opens a connection and exchanges hellos. Does not check trust.
    async fn connect_addr(&self, addr: SocketAddr) -> Result<(Connection, DeviceInfo)> {
        let connecting = self.endpoint.connect(addr, tls::SERVER_NAME)?;
        let conn = timeout(CONNECT_TIMEOUT, connecting)
            .await
            .map_err(|_| anyhow!("timed out connecting to {addr}"))??;
        let peer_id = tls::peer_device_id(&conn)?;
        let (mut send, mut recv) = conn.open_bi().await?;
        write_msg(
            &mut send,
            &Message::Hello {
                device: self.info.clone(),
            },
        )
        .await?;
        send.finish()?;
        let Message::Hello { device } = check(timeout(HELLO_TIMEOUT, read_msg(&mut recv)).await??)?
        else {
            bail!("expected hello from {addr}");
        };
        Ok((
            conn,
            DeviceInfo {
                id: peer_id,
                ..device
            },
        ))
    }

    /// Tries all addresses concurrently; returns the first that answers as
    /// `expect` (or any other device if `expect` is None).
    async fn connect_any(
        self: &Arc<Self>,
        addrs: Vec<SocketAddr>,
        expect: Option<&str>,
    ) -> Result<(Connection, DeviceInfo, SocketAddr)> {
        let mut set = tokio::task::JoinSet::new();
        for addr in addrs {
            let me = self.clone();
            set.spawn(async move { (addr, me.connect_addr(addr).await) });
        }
        let mut last = anyhow!("device is offline (no known address)");
        while let Some(joined) = set.join_next().await {
            let (addr, res) = joined?;
            match res {
                Ok((conn, peer))
                    if peer.id != self.info.id && expect.is_none_or(|e| e == peer.id) =>
                {
                    set.abort_all();
                    self.adopt(&conn, &peer, addr);
                    return Ok((conn, peer, addr));
                }
                Ok((conn, _)) => {
                    conn.close(0u32.into(), b"wrong device");
                    last = anyhow!("a different device answered at {addr}");
                }
                Err(e) => last = e,
            }
        }
        Err(last)
    }

    /// Returns a live, authenticated connection to a trusted peer.
    /// One message and its reply over a new stream to a paired device. A cached connection can be
    /// dead without our knowing yet (the peer crashed, slept or lost Wi-Fi: QUIC only notices at
    /// its idle timeout): if the exchange fails over one, it is dropped and tried once more over a
    /// fresh connection.
    async fn request(self: &Arc<Self>, peer_id: &str, msg: &Message) -> Result<Message> {
        let cached = self
            .connections
            .lock()
            .unwrap()
            .get(peer_id)
            .is_some_and(|(c, _)| c.close_reason().is_none());
        match self.request_once(peer_id, msg).await {
            Err(e) if cached && !format!("{e:#}").starts_with("peer error") => {
                debug!("request over cached connection failed ({e:#}); retrying over a new one");
                if let Some((c, _)) = self.connections.lock().unwrap().remove(peer_id) {
                    c.close(0u32.into(), b"stale");
                }
                self.request_once(peer_id, msg).await
            }
            other => other,
        }
    }

    async fn request_once(self: &Arc<Self>, peer_id: &str, msg: &Message) -> Result<Message> {
        let (conn, _) = self.connection_to(peer_id).await?;
        let (mut send, mut recv) = conn.open_bi().await?;
        write_msg(&mut send, msg).await?;
        send.finish()?;
        // A live peer answers at once; waiting the whole idle timeout would hold up the retry.
        check(
            timeout(REQUEST_TIMEOUT, read_msg(&mut recv))
                .await
                .map_err(|_| anyhow!("no answer"))??,
        )
    }

    async fn connection_to(self: &Arc<Self>, peer_id: &str) -> Result<(Connection, DeviceInfo)> {
        if !self.trust.lock().unwrap().is_trusted(peer_id) {
            bail!("device {peer_id} is not paired");
        }
        if let Some((c, p)) = self.connections.lock().unwrap().get(peer_id)
            && c.close_reason().is_none()
        {
            return Ok((c.clone(), p.clone()));
        }
        let (conn, peer, addr) = self
            .connect_any(self.resolve(peer_id), Some(peer_id))
            .await?;
        self.on_trusted_contact(&peer, addr);
        self.connections
            .lock()
            .unwrap()
            .insert(peer.id.clone(), (conn.clone(), peer.clone()));
        Ok((conn, peer))
    }

    async fn send_files(
        self: &Arc<Self>,
        peer_id: &str,
        paths: &[PathBuf],
        transfer_id: &str,
    ) -> Result<Vec<PathBuf>> {
        let files = tokio::task::spawn_blocking({
            let paths = paths.to_vec();
            move || collect_files(&paths)
        })
        .await??;
        if files.is_empty() {
            bail!("nothing to send");
        }
        let (conn, _) = self.connection_to(peer_id).await?;
        let (mut send, mut recv) = conn.open_bi().await?;
        let metas: Vec<FileMeta> = files.iter().map(|(_, m)| m.clone()).collect();
        let total: u64 = metas.iter().map(|m| m.size).sum();
        write_msg(
            &mut send,
            &Message::FileOffer {
                transfer_id: transfer_id.to_owned(),
                files: metas,
            },
        )
        .await?;
        self.emit(Event::TransferPending {
            transfer_id: transfer_id.to_owned(),
            peer_id: peer_id.to_owned(),
            file_count: files.len(),
            bytes_total: total,
        });

        // Cancellable from here on, while waiting for the answer too.
        let cancel = self.track_transfer(transfer_id);
        let work = async {
            match check(timeout(REMOTE_PROMPT_TIMEOUT, read_msg(&mut recv)).await??)? {
                Message::FileOfferResponse { accepted: true } => {}
                Message::FileOfferResponse { accepted: false } => {
                    bail!("the receiver declined the transfer")
                }
                m => bail!("unexpected reply: {m:?}"),
            }

            let mut progress = Progress::new(self, transfer_id, peer_id, Direction::Outgoing, total);
            let mut hashes = Vec::with_capacity(files.len());
            let mut buf = vec![0u8; CHUNK];
            for (path, meta) in &files {
                let mut file = tokio::fs::File::open(path).await?.take(meta.size);
                let mut hasher = Sha256::new();
                let mut sent = 0u64;
                loop {
                    let n = file.read(&mut buf).await?;
                    if n == 0 {
                        break;
                    }
                    hasher.update(&buf[..n]);
                    send.write_all(&buf[..n]).await?;
                    sent += n as u64;
                    progress.add(n as u64);
                }
                if sent != meta.size {
                    bail!("{} changed while sending", path.display());
                }
                hashes.push(hex::encode(hasher.finalize()));
            }
            progress.finish();
            write_msg(&mut send, &Message::FileDone { sha256: hashes }).await?;
            send.finish()?;

            match check(timeout(Duration::from_secs(60), read_msg(&mut recv)).await??)? {
                Message::FileAck { ok: true, .. } => Ok(files.iter().map(|(p, _)| p.clone()).collect()),
                Message::FileAck { ok: false, error } => {
                    bail!("receiver rejected: {}", error.unwrap_or_default())
                }
                m => bail!("unexpected reply: {m:?}"),
            }
        };
        let outcome = tokio::select! {
            r = work => Some(r),
            () = until_cancelled(cancel) => None,
        };
        self.untrack_transfer(transfer_id);
        match outcome {
            Some(Ok(sent)) => Ok(sent),
            Some(Err(e)) if cancelled_by_peer(&e) => bail!(TRANSFER_CANCELLED_BY_PEER),
            Some(Err(e)) => Err(e),
            None => {
                let _ = send.reset(CANCELLED);
                let _ = recv.stop(CANCELLED);
                bail!(TRANSFER_CANCELLED)
            }
        }
    }
}

/// Throttled progress reporting.
struct Progress<'a> {
    inner: &'a Inner,
    transfer_id: String,
    peer_id: String,
    direction: Direction,
    done: u64,
    total: u64,
    last: Instant,
}

impl<'a> Progress<'a> {
    fn new(
        inner: &'a Inner,
        transfer_id: &str,
        peer_id: &str,
        direction: Direction,
        total: u64,
    ) -> Self {
        let p = Self {
            inner,
            transfer_id: transfer_id.to_owned(),
            peer_id: peer_id.to_owned(),
            direction,
            done: 0,
            total,
            last: Instant::now(),
        };
        p.emit();
        p
    }

    fn add(&mut self, n: u64) {
        self.done += n;
        if self.last.elapsed() >= PROGRESS_INTERVAL {
            self.last = Instant::now();
            self.emit();
        }
    }

    fn finish(&mut self) {
        self.emit();
    }

    fn emit(&self) {
        self.inner.emit(Event::TransferProgress {
            transfer_id: self.transfer_id.clone(),
            peer_id: self.peer_id.clone(),
            direction: self.direction,
            bytes_done: self.done,
            bytes_total: self.total,
        });
    }
}
